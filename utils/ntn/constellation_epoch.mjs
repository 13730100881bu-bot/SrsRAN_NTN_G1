import {
  centralAngleForElevation,
  groundUnit,
  resolveOrbitModel,
  satelliteUnitEcef
} from './constellation_audit_core.mjs';

const DEFAULT_ENTRY_ELEVATION_DEG = 45;
const DEFAULT_RELEASE_ELEVATION_DEG = 42;
const DEFAULT_LATITUDE_BUCKET_DEG = 1;
const DEFAULT_MARGIN_TOLERANCE = 1e-12;
const RADIANS_TO_DEGREES = 180 / Math.PI;

function fail(message) {
  throw new TypeError(message);
}

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function assertExactKeys(value, context, required, optional = []) {
  if (!isPlainObject(value)) fail(`${context} must be an object`);
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) fail(`unknown field '${context}.${key}'`);
  }
  for (const key of required) {
    if (!Object.hasOwn(value, key)) fail(`missing field '${context}.${key}'`);
  }
}

function finiteNumber(value, context, minimum = -Infinity, maximum = Infinity) {
  if (!Number.isFinite(value) || value < minimum || value > maximum) {
    fail(`${context} must be a finite number in ${minimum}..${maximum}`);
  }
  return value;
}

function compareText(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function freezeArray(items) {
  return Object.freeze(items);
}

function normalizePosition(cell, index) {
  if (!isPlainObject(cell)) fail(`catalog.cells[${index}] must be an object`);
  const positionId = cell.id ?? cell.positionId ?? cell.position_id;
  const latitudeDeg = cell.lat ?? cell.latitudeDeg ?? cell.latitude_deg;
  const longitudeDeg = cell.lon ?? cell.longitudeDeg ?? cell.longitude_deg;
  const childMask = cell.childMask ?? cell.child_mask;
  if (typeof positionId !== 'string' || !/^G\d{6}$/.test(positionId)) {
    fail(`catalog.cells[${index}] id must use G###### form`);
  }
  finiteNumber(latitudeDeg, `catalog.cells[${index}].lat`, -90, 90);
  finiteNumber(longitudeDeg, `catalog.cells[${index}].lon`, -180, 180);
  if (childMask !== undefined && (!Number.isInteger(childMask) || childMask < 1 || childMask > 127)) {
    fail(`catalog.cells[${index}].childMask must be an integer in 1..127`);
  }
  return {
    positionId,
    latitudeDeg: Object.is(latitudeDeg, -0) ? 0 : latitudeDeg,
    longitudeDeg: Object.is(longitudeDeg, -0) ? 0 : longitudeDeg,
    ...(childMask === undefined ? {} : {childMask})
  };
}

function catalogCells(catalog) {
  if (Array.isArray(catalog)) return catalog;
  if (!isPlainObject(catalog) || !Array.isArray(catalog.cells)) {
    fail('catalog must be an array or an object with a cells array');
  }
  return catalog.cells;
}

/**
 * Precomputes ground unit vectors and groups catalog positions into latitude
 * buckets. A spherical cap cannot contain a point whose latitude differs from
 * the cap centre by more than its angular radius, so a snapshot can safely skip
 * all non-intersecting buckets without losing any visible position.
 */
export function createCatalogGeometry(catalog, options = {}) {
  assertExactKeys(options, 'catalogGeometryOptions', [], ['latitudeBucketDeg']);
  const latitudeBucketDeg = finiteNumber(
    options.latitudeBucketDeg ?? DEFAULT_LATITUDE_BUCKET_DEG,
    'catalogGeometryOptions.latitudeBucketDeg',
    Number.MIN_VALUE,
    180
  );
  const bucketCount = Math.ceil(180 / latitudeBucketDeg);
  const cells = catalogCells(catalog);
  const seen = new Set();
  const normalized = cells.map((cell, index) => normalizePosition(cell, index));
  normalized.sort((left, right) => compareText(left.positionId, right.positionId));
  const buckets = Array.from({length: bucketCount}, () => []);
  const positions = normalized.map((position, positionIndex) => {
    if (seen.has(position.positionId)) fail(`catalog contains duplicate position '${position.positionId}'`);
    seen.add(position.positionId);
    const observerUnit = groundUnit({
      id: position.positionId,
      lat: position.latitudeDeg,
      lon: position.longitudeDeg
    });
    const bucketIndex = Math.min(
      bucketCount - 1,
      Math.floor((position.latitudeDeg + 90) / latitudeBucketDeg)
    );
    const geometry = Object.freeze({...position, observerUnit, bucketIndex, positionIndex});
    buckets[bucketIndex].push(geometry);
    return geometry;
  });
  const frozenBuckets = buckets.map((bucket) => freezeArray(bucket));
  return Object.freeze({
    latitudeBucketDeg,
    bucketCount,
    positions: freezeArray(positions),
    buckets: freezeArray(frozenBuckets)
  });
}

function assertCatalogGeometry(geometry) {
  if (!isPlainObject(geometry) || !Array.isArray(geometry.positions) || !Array.isArray(geometry.buckets)) {
    fail('catalogGeometry must be created by createCatalogGeometry');
  }
  finiteNumber(geometry.latitudeBucketDeg, 'catalogGeometry.latitudeBucketDeg', Number.MIN_VALUE, 180);
  if (!Number.isSafeInteger(geometry.bucketCount) || geometry.bucketCount <= 0 ||
      geometry.buckets.length !== geometry.bucketCount) {
    fail('catalogGeometry bucket count is invalid');
  }
  return geometry;
}

function normalizeSatellites(satellites) {
  if (!Array.isArray(satellites)) fail('satellites must be an array');
  const ordered = [...satellites].sort((left, right) => compareText(left?.id, right?.id));
  const seen = new Set();
  for (const [index, satellite] of ordered.entries()) {
    if (!isPlainObject(satellite) || typeof satellite.id !== 'string' || satellite.id.length === 0) {
      fail(`satellites[${index}] must contain a non-empty id`);
    }
    if (seen.has(satellite.id)) fail(`satellites contains duplicate satellite '${satellite.id}'`);
    seen.add(satellite.id);
  }
  return ordered;
}

function subSatelliteLatitudeDeg(unit) {
  return Math.asin(Math.max(-1, Math.min(1, unit[2]))) * RADIANS_TO_DEGREES;
}

function bucketRange(geometry, centreLatitudeDeg, angularRadiusRad) {
  const radiusDeg = angularRadiusRad * RADIANS_TO_DEGREES;
  const minimumLatitudeDeg = Math.max(-90, centreLatitudeDeg - radiusDeg);
  const maximumLatitudeDeg = Math.min(90, centreLatitudeDeg + radiusDeg);
  const first = Math.max(0, Math.floor((minimumLatitudeDeg + 90) / geometry.latitudeBucketDeg));
  const last = Math.min(
    geometry.bucketCount - 1,
    Math.floor((maximumLatitudeDeg + 90) / geometry.latitudeBucketDeg)
  );
  return {first, last};
}

function elevationDeg(dot, orbitModel) {
  const numerator = orbitModel.orbitRadiusKm * dot - orbitModel.earthRadiusKm;
  const distance = Math.sqrt(
    orbitModel.orbitRadiusKm ** 2 + orbitModel.earthRadiusKm ** 2 -
      2 * orbitModel.orbitRadiusKm * orbitModel.earthRadiusKm * dot
  );
  const sine = distance === 0 ? 1 : Math.max(-1, Math.min(1, numerator / distance));
  const result = Math.asin(sine) * RADIANS_TO_DEGREES;
  return Object.is(result, -0) ? 0 : result;
}

function unitDot(left, right) {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

function freezeCandidateSet(positionId, candidates) {
  return Object.freeze({positionId, candidates: freezeArray(candidates)});
}

function freezeCount(satelliteId, rawVisibleCount, assignmentCandidateCount) {
  return Object.freeze({satelliteId, rawVisibleCount, assignmentCandidateCount});
}

/**
 * Computes one deterministic constellation epoch.
 *
 * completeCandidateSets always contains every catalog L1. Its candidates are
 * the complete >= release-threshold inventory and are never capacity-cropped.
 * assignmentCandidateSets uses the stricter entry threshold. Both arrays can be
 * passed directly to planAssignmentEpoch.
 */
export function createEpochSnapshot(options) {
  assertExactKeys(options, 'epochSnapshot', ['timeUs', 'satellites', 'catalogGeometry'], [
    'orbitModel',
    'entryElevationDeg',
    'releaseElevationDeg',
    'marginTolerance'
  ]);
  if (!Number.isSafeInteger(options.timeUs)) fail('epochSnapshot.timeUs must be a safe integer');
  const geometry = assertCatalogGeometry(options.catalogGeometry);
  const satellites = normalizeSatellites(options.satellites);
  const orbitModel = options.orbitModel ?? resolveOrbitModel({
    altitudeKm: satellites[0]?.altitudeKm ?? 500
  });
  const entryElevationDeg = finiteNumber(
    options.entryElevationDeg ?? DEFAULT_ENTRY_ELEVATION_DEG,
    'epochSnapshot.entryElevationDeg',
    0,
    90
  );
  const releaseElevationDeg = finiteNumber(
    options.releaseElevationDeg ?? DEFAULT_RELEASE_ELEVATION_DEG,
    'epochSnapshot.releaseElevationDeg',
    0,
    90
  );
  if (releaseElevationDeg >= entryElevationDeg) {
    fail('epochSnapshot.releaseElevationDeg must be lower than entryElevationDeg');
  }
  const marginTolerance = finiteNumber(
    options.marginTolerance ?? DEFAULT_MARGIN_TOLERANCE,
    'epochSnapshot.marginTolerance',
    0,
    1e-6
  );
  const entryAngleRad = centralAngleForElevation(entryElevationDeg, orbitModel);
  const releaseAngleRad = centralAngleForElevation(releaseElevationDeg, orbitModel);
  const entryMinimumDot = Math.cos(entryAngleRad);
  const releaseMinimumDot = Math.cos(releaseAngleRad);
  const releaseCandidatesByPosition = Array.from({length: geometry.positions.length}, () => []);
  const entryCandidatesByPosition = Array.from({length: geometry.positions.length}, () => []);
  const satelliteCounts = [];

  for (const satellite of satellites) {
    const satelliteUnit = satelliteUnitEcef(satellite, options.timeUs, orbitModel);
    const range = bucketRange(geometry, subSatelliteLatitudeDeg(satelliteUnit), releaseAngleRad);
    let rawVisibleCount = 0;
    let assignmentCandidateCount = 0;
    for (let bucketIndex = range.first; bucketIndex <= range.last; bucketIndex += 1) {
      for (const position of geometry.buckets[bucketIndex]) {
        const dot = unitDot(satelliteUnit, position.observerUnit);
        if (dot - releaseMinimumDot < -marginTolerance) continue;
        const candidate = Object.freeze({
          satelliteId: satellite.id,
          elevationDeg: elevationDeg(dot, orbitModel)
        });
        const index = position.positionIndex;
        releaseCandidatesByPosition[index].push(candidate);
        rawVisibleCount += 1;
        if (dot - entryMinimumDot >= -marginTolerance) {
          entryCandidatesByPosition[index].push(candidate);
          assignmentCandidateCount += 1;
        }
      }
    }
    satelliteCounts.push(freezeCount(satellite.id, rawVisibleCount, assignmentCandidateCount));
  }

  const completeCandidateSets = geometry.positions.map((position, index) =>
    freezeCandidateSet(position.positionId, releaseCandidatesByPosition[index]));
  const assignmentCandidateSets = geometry.positions.map((position, index) =>
    freezeCandidateSet(position.positionId, entryCandidatesByPosition[index]));
  const snapshot = Object.freeze({
    timeUs: options.timeUs,
    entryElevationDeg,
    releaseElevationDeg,
    completeCandidateSets: freezeArray(completeCandidateSets),
    assignmentCandidateSets: freezeArray(assignmentCandidateSets),
    satelliteCounts: freezeArray(satelliteCounts)
  });
  auditEpochSnapshot(snapshot, {catalogGeometry: geometry, satellites});
  return snapshot;
}

function assertCandidateSets(candidateSets, context) {
  if (!Array.isArray(candidateSets)) fail(`${context} must be an array`);
  let previousPositionId;
  const byPosition = new Map();
  for (const [index, candidateSet] of candidateSets.entries()) {
    if (!isPlainObject(candidateSet) || typeof candidateSet.positionId !== 'string' ||
        !Array.isArray(candidateSet.candidates)) {
      fail(`${context}[${index}] is invalid`);
    }
    if (previousPositionId !== undefined && compareText(previousPositionId, candidateSet.positionId) >= 0) {
      fail(`${context} must be sorted by unique positionId`);
    }
    previousPositionId = candidateSet.positionId;
    const seenSatellites = new Set();
    let previousSatelliteId;
    for (const [candidateIndex, candidate] of candidateSet.candidates.entries()) {
      if (!isPlainObject(candidate) || typeof candidate.satelliteId !== 'string' ||
          !Number.isFinite(candidate.elevationDeg)) {
        fail(`${context}[${index}].candidates[${candidateIndex}] is invalid`);
      }
      if (seenSatellites.has(candidate.satelliteId) ||
          (previousSatelliteId !== undefined && compareText(previousSatelliteId, candidate.satelliteId) >= 0)) {
        fail(`${context}[${index}].candidates must be sorted by unique satelliteId`);
      }
      seenSatellites.add(candidate.satelliteId);
      previousSatelliteId = candidate.satelliteId;
    }
    byPosition.set(candidateSet.positionId, candidateSet);
  }
  return byPosition;
}

/**
 * Verifies that a snapshot is complete, deterministic and internally
 * consistent. It returns compact counts suitable for evidence summaries and
 * throws instead of accepting a cropped or mismatched inventory.
 */
export function auditEpochSnapshot(snapshot, expected = {}) {
  if (!isPlainObject(expected)) fail('expected must be an object');
  for (const key of Object.keys(expected)) {
    if (key !== 'catalogGeometry' && key !== 'satellites') fail(`unknown field 'expected.${key}'`);
  }
  if (!isPlainObject(snapshot) || !Number.isSafeInteger(snapshot.timeUs)) {
    fail('snapshot must contain an integer timeUs');
  }
  finiteNumber(snapshot.entryElevationDeg, 'snapshot.entryElevationDeg', 0, 90);
  finiteNumber(snapshot.releaseElevationDeg, 'snapshot.releaseElevationDeg', 0, 90);
  if (snapshot.releaseElevationDeg >= snapshot.entryElevationDeg) {
    fail('snapshot.releaseElevationDeg must be lower than entryElevationDeg');
  }
  const completeByPosition = assertCandidateSets(snapshot.completeCandidateSets, 'snapshot.completeCandidateSets');
  const eligibleByPosition = assertCandidateSets(snapshot.assignmentCandidateSets, 'snapshot.assignmentCandidateSets');
  if (completeByPosition.size !== eligibleByPosition.size ||
      [...completeByPosition.keys()].some((positionId) => !eligibleByPosition.has(positionId))) {
    fail('snapshot candidate sets must describe the same complete position inventory');
  }
  if (expected.catalogGeometry !== undefined) {
    const geometry = assertCatalogGeometry(expected.catalogGeometry);
    const expectedIds = geometry.positions.map((position) => position.positionId);
    if (expectedIds.length !== completeByPosition.size ||
        expectedIds.some((positionId, index) => positionId !== snapshot.completeCandidateSets[index].positionId)) {
      fail('snapshot inventory was cropped or does not match catalogGeometry');
    }
  }
  const releaseCounts = new Map();
  const entryCounts = new Map();
  let inventoryCandidateCount = 0;
  let assignmentCandidateCount = 0;
  for (const [positionId, complete] of completeByPosition) {
    const completeSatellites = new Map(
      complete.candidates.map((candidate) => [candidate.satelliteId, candidate])
    );
    inventoryCandidateCount += complete.candidates.length;
    for (const candidate of complete.candidates) {
      if (candidate.elevationDeg + 1e-6 < snapshot.releaseElevationDeg) {
        fail(`complete candidate '${positionId}/${candidate.satelliteId}' is below the release threshold`);
      }
      releaseCounts.set(candidate.satelliteId, (releaseCounts.get(candidate.satelliteId) ?? 0) + 1);
    }
    const eligible = eligibleByPosition.get(positionId);
    assignmentCandidateCount += eligible.candidates.length;
    for (const candidate of eligible.candidates) {
      const inventoryCandidate = completeSatellites.get(candidate.satelliteId);
      if (inventoryCandidate === undefined) {
        fail(`assignment candidate '${positionId}/${candidate.satelliteId}' is absent from complete inventory`);
      }
      if (candidate.elevationDeg + 1e-6 < snapshot.entryElevationDeg) {
        fail(`assignment candidate '${positionId}/${candidate.satelliteId}' is below the entry threshold`);
      }
      if (Math.abs(candidate.elevationDeg - inventoryCandidate.elevationDeg) > 1e-12) {
        fail(`candidate elevation mismatch for '${positionId}/${candidate.satelliteId}'`);
      }
      entryCounts.set(candidate.satelliteId, (entryCounts.get(candidate.satelliteId) ?? 0) + 1);
    }
  }
  if (!Array.isArray(snapshot.satelliteCounts)) fail('snapshot.satelliteCounts must be an array');
  let previousSatelliteId;
  for (const [index, count] of snapshot.satelliteCounts.entries()) {
    if (!isPlainObject(count) || typeof count.satelliteId !== 'string' ||
        !Number.isSafeInteger(count.rawVisibleCount) || count.rawVisibleCount < 0 ||
        !Number.isSafeInteger(count.assignmentCandidateCount) || count.assignmentCandidateCount < 0) {
      fail(`snapshot.satelliteCounts[${index}] is invalid`);
    }
    if (previousSatelliteId !== undefined && compareText(previousSatelliteId, count.satelliteId) >= 0) {
      fail('snapshot.satelliteCounts must be sorted by unique satelliteId');
    }
    if ((releaseCounts.get(count.satelliteId) ?? 0) !== count.rawVisibleCount ||
        (entryCounts.get(count.satelliteId) ?? 0) !== count.assignmentCandidateCount) {
      fail(`snapshot count mismatch for satellite '${count.satelliteId}'`);
    }
    if (count.assignmentCandidateCount > count.rawVisibleCount) {
      fail(`snapshot assignment count exceeds raw visibility for satellite '${count.satelliteId}'`);
    }
    releaseCounts.delete(count.satelliteId);
    entryCounts.delete(count.satelliteId);
    previousSatelliteId = count.satelliteId;
  }
  if (releaseCounts.size !== 0 || entryCounts.size !== 0) {
    fail('snapshot.satelliteCounts omits a referenced satellite');
  }

  if (expected.satellites !== undefined) {
    const satelliteIds = normalizeSatellites(expected.satellites).map((satellite) => satellite.id);
    if (satelliteIds.length !== snapshot.satelliteCounts.length ||
        satelliteIds.some((satelliteId, index) => satelliteId !== snapshot.satelliteCounts[index].satelliteId)) {
      fail('snapshot satelliteCounts does not match satellites');
    }
  }

  return Object.freeze({
    success: true,
    positionCount: completeByPosition.size,
    satelliteCount: snapshot.satelliteCounts.length,
    inventoryCandidateCount,
    assignmentCandidateCount,
    maximumRawVisibleCount: snapshot.satelliteCounts.reduce(
      (maximum, count) => Math.max(maximum, count.rawVisibleCount), 0),
    maximumAssignmentCandidateCount: snapshot.satelliteCounts.reduce(
      (maximum, count) => Math.max(maximum, count.assignmentCandidateCount), 0)
  });
}
