import {createHash} from 'node:crypto';

import {
  centralAngleForElevation,
  createSatellitePropagator,
  resolveOrbitModel
} from './constellation_audit_core.mjs';
import {auditPairVisibility} from './constellation_visibility.mjs';

const STATE_SCHEMA_VERSION = 'ntn-slab-visibility-state-v1';
const DEFAULT_ENTRY_ELEVATION_DEG = 45;
const DEFAULT_RELEASE_ELEVATION_DEG = 42;
const DEFAULT_BOUNDARY_PADDING_US = 120_000_000;
const DEFAULT_ROOT_TOLERANCE_US = 1_000;
const DEFAULT_SCAN_STEP_US = 60_000_000;
const DEFAULT_MARGIN_TOLERANCE = 1e-12;
const DEFAULT_MAX_EVALUATIONS = 1_000_000;
const RADIANS_TO_DEGREES = 180 / Math.PI;
const SCREENING_ROUNDING_MARGIN = 64 * Number.EPSILON;

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

function positiveInteger(value, context, maximum = Number.MAX_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value <= 0 || value > maximum) {
    fail(`${context} must be a positive safe integer no greater than ${maximum}`);
  }
  return value;
}

function integerTime(value, context) {
  if (!Number.isSafeInteger(value)) fail(`${context} must be integer microseconds`);
  return value;
}

function compareText(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

const EVENT_RANK = Object.freeze({
  release_exit: 0,
  entry_exit: 1,
  release_enter: 2,
  entry_enter: 3
});

function compareEvent(left, right) {
  return left.timeUs - right.timeUs
    || EVENT_RANK[left.kind] - EVENT_RANK[right.kind]
    || compareText(left.positionId, right.positionId)
    || compareText(left.satelliteId, right.satelliteId);
}

function compareInterval(left, right) {
  return left.startTimeUs - right.startTimeUs
    || left.endTimeUs - right.endTimeUs
    || compareText(left.positionId, right.positionId)
    || compareText(left.satelliteId, right.satelliteId);
}

function compareAmbiguity(left, right) {
  return left.startTimeUs - right.startTimeUs
    || left.endTimeUs - right.endTimeUs
    || compareText(left.threshold, right.threshold)
    || compareText(left.positionId, right.positionId)
    || compareText(left.satelliteId, right.satelliteId)
    || compareText(left.reason, right.reason);
}

function freezeRecords(items) {
  return Object.freeze(items.map((item) => Object.freeze(item)));
}

function validateCatalogGeometry(geometry) {
  if (!isPlainObject(geometry) || !Array.isArray(geometry.positions) || !Array.isArray(geometry.buckets)) {
    fail('catalogGeometry must be created by createCatalogGeometry');
  }
  finiteNumber(geometry.latitudeBucketDeg, 'catalogGeometry.latitudeBucketDeg', Number.MIN_VALUE, 180);
  positiveInteger(geometry.bucketCount, 'catalogGeometry.bucketCount');
  if (geometry.buckets.length !== geometry.bucketCount) fail('catalogGeometry bucket count is inconsistent');
  const known = new Set();
  let previousPositionId;
  for (const [index, position] of geometry.positions.entries()) {
    if (!isPlainObject(position) || typeof position.positionId !== 'string' ||
        !Number.isFinite(position.latitudeDeg) || !Number.isFinite(position.longitudeDeg) ||
        !Array.isArray(position.observerUnit) || position.observerUnit.length !== 3) {
      fail(`catalogGeometry.positions[${index}] is invalid`);
    }
    if (previousPositionId !== undefined && compareText(previousPositionId, position.positionId) >= 0) {
      fail('catalogGeometry.positions must be sorted by unique positionId');
    }
    previousPositionId = position.positionId;
    known.add(position);
  }
  let bucketItems = 0;
  for (const [bucketIndex, bucket] of geometry.buckets.entries()) {
    if (!Array.isArray(bucket)) fail(`catalogGeometry.buckets[${bucketIndex}] must be an array`);
    for (const position of bucket) {
      if (!known.has(position) || position.bucketIndex !== bucketIndex) {
        fail(`catalogGeometry.buckets[${bucketIndex}] references an inconsistent position`);
      }
      bucketItems += 1;
    }
  }
  if (bucketItems !== geometry.positions.length) fail('catalogGeometry buckets do not cover the position inventory once');
  return geometry;
}

function normalizeSatellites(satellites) {
  if (!Array.isArray(satellites)) fail('satellites must be an array');
  const ordered = [...satellites].sort((left, right) => compareText(left?.id, right?.id));
  let previousId;
  for (const [index, satellite] of ordered.entries()) {
    if (!isPlainObject(satellite) || typeof satellite.id !== 'string' || satellite.id.length === 0) {
      fail(`satellites[${index}] is invalid`);
    }
    if (previousId !== undefined && compareText(previousId, satellite.id) >= 0) {
      fail(`satellites contains duplicate satellite '${satellite.id}'`);
    }
    previousId = satellite.id;
  }
  return ordered;
}

function normalizeShard(shard) {
  if (shard === undefined) return {index: 0, count: 1};
  assertExactKeys(shard, 'satelliteShard', ['index', 'count']);
  positiveInteger(shard.count, 'satelliteShard.count');
  if (!Number.isSafeInteger(shard.index) || shard.index < 0 || shard.index >= shard.count) {
    fail('satelliteShard.index must be an integer in 0..count-1');
  }
  return {index: shard.index, count: shard.count};
}

function stableContextHash(serializable) {
  return `sha256:${createHash('sha256').update(JSON.stringify(serializable)).digest('hex')}`;
}

function orbitFingerprint(model) {
  return {
    earthRadiusKm: model.earthRadiusKm,
    earthMuKm3S2: model.earthMuKm3S2,
    earthRotationRadPerSecond: model.earthRotationRadPerSecond,
    altitudeKm: model.altitudeKm,
    orbitRadiusKm: model.orbitRadiusKm,
    meanMotionRadPerSecond: model.meanMotionRadPerSecond,
    orbitPeriodSeconds: model.orbitPeriodSeconds,
    conservativeAngularRateRadPerSecond: model.conservativeAngularRateRadPerSecond
  };
}

/**
 * Creates the immutable computation context for one satellite shard. Sharding
 * uses the sorted satellite index modulo shard count, which is deterministic
 * and keeps Walker planes reasonably balanced without changing satellite IDs.
 */
export function createSlabVisibilityContext(options) {
  assertExactKeys(options, 'slabVisibilityContext', ['satellites', 'catalogGeometry'], [
    'orbitModel', 'entryElevationDeg', 'releaseElevationDeg', 'boundaryPaddingUs',
    'toleranceUs', 'scanStepUs', 'screeningStepUs', 'marginTolerance', 'maxEvaluations',
    'satelliteShard'
  ]);
  const geometry = validateCatalogGeometry(options.catalogGeometry);
  const allSatellites = normalizeSatellites(options.satellites);
  const shard = normalizeShard(options.satelliteShard);
  const satellites = allSatellites.filter((_, index) => index % shard.count === shard.index);
  const orbitModel = options.orbitModel ?? resolveOrbitModel({
    altitudeKm: allSatellites[0]?.altitudeKm ?? 500
  });
  const entryElevationDeg = finiteNumber(
    options.entryElevationDeg ?? DEFAULT_ENTRY_ELEVATION_DEG,
    'slabVisibilityContext.entryElevationDeg', 0, 90
  );
  const releaseElevationDeg = finiteNumber(
    options.releaseElevationDeg ?? DEFAULT_RELEASE_ELEVATION_DEG,
    'slabVisibilityContext.releaseElevationDeg', 0, 90
  );
  if (releaseElevationDeg >= entryElevationDeg) {
    fail('slabVisibilityContext.releaseElevationDeg must be below entryElevationDeg');
  }
  const boundaryPaddingUs = positiveInteger(
    options.boundaryPaddingUs ?? DEFAULT_BOUNDARY_PADDING_US,
    'slabVisibilityContext.boundaryPaddingUs'
  );
  const toleranceUs = positiveInteger(
    options.toleranceUs ?? DEFAULT_ROOT_TOLERANCE_US,
    'slabVisibilityContext.toleranceUs', 10_000
  );
  const scanStepUs = positiveInteger(
    options.scanStepUs ?? DEFAULT_SCAN_STEP_US,
    'slabVisibilityContext.scanStepUs'
  );
  if (scanStepUs < toleranceUs) fail('slabVisibilityContext.scanStepUs must be at least toleranceUs');
  const screeningStepUs = positiveInteger(
    options.screeningStepUs ?? scanStepUs,
    'slabVisibilityContext.screeningStepUs'
  );
  const marginTolerance = finiteNumber(
    options.marginTolerance ?? DEFAULT_MARGIN_TOLERANCE,
    'slabVisibilityContext.marginTolerance', 0, 1e-6
  );
  const maxEvaluations = positiveInteger(
    options.maxEvaluations ?? DEFAULT_MAX_EVALUATIONS,
    'slabVisibilityContext.maxEvaluations'
  );
  const entryAngleRad = centralAngleForElevation(entryElevationDeg, orbitModel);
  const releaseAngleRad = centralAngleForElevation(releaseElevationDeg, orbitModel);
  const compiledSatellites = satellites.map((definition) => Object.freeze({
    definition,
    propagate: createSatellitePropagator(definition, orbitModel)
  }));
  const fingerprintInput = {
    model: 'ntn-slab-visibility-context-v1',
    orbitModel: orbitFingerprint(orbitModel),
    entryElevationDeg,
    releaseElevationDeg,
    boundaryPaddingUs,
    toleranceUs,
    scanStepUs,
    screeningStepUs,
    marginTolerance,
    maxEvaluations,
    shard,
    latitudeBucketDeg: geometry.latitudeBucketDeg,
    satellites: satellites.map(({id, plane, slot, raanDeg, phaseDeg, altitudeKm, inclinationDeg}) => ({
      id, plane, slot, raanDeg, phaseDeg, altitudeKm, inclinationDeg
    })),
    positions: geometry.positions.map(({positionId, latitudeDeg, longitudeDeg}) => ({
      positionId, latitudeDeg, longitudeDeg
    }))
  };
  return Object.freeze({
    contextHash: stableContextHash(fingerprintInput),
    catalogGeometry: geometry,
    satellites: Object.freeze(compiledSatellites),
    satelliteIds: Object.freeze(satellites.map(({id}) => id)),
    orbitModel,
    entryElevationDeg,
    releaseElevationDeg,
    boundaryPaddingUs,
    toleranceUs,
    scanStepUs,
    screeningStepUs,
    marginTolerance,
    maxEvaluations,
    entryMinimumDot: Math.cos(entryAngleRad),
    releaseMinimumDot: Math.cos(releaseAngleRad),
    releaseAngleRad,
    satelliteShard: Object.freeze(shard)
  });
}

function assertContext(context) {
  if (!isPlainObject(context) || typeof context.contextHash !== 'string' ||
      !Array.isArray(context.satellites) || !Array.isArray(context.satelliteIds) ||
      !isPlainObject(context.catalogGeometry)) {
    fail('context must be created by createSlabVisibilityContext');
  }
  return context;
}

function emptyMetrics() {
  return {
    exact: true,
    slabsProcessed: 0,
    satelliteSlabsProcessed: 0,
    potentialPairs: 0,
    screeningSegments: 0,
    latitudeBucketCandidates: 0,
    sweptCapCandidates: 0,
    pairAudits: 0,
    thresholdEvaluations: 0,
    ambiguousRanges: 0,
    entryIntervalsEmitted: 0,
    releaseIntervalsEmitted: 0
  };
}

function freezeMetrics(metrics) {
  return Object.freeze({...metrics});
}

function validateMetrics(metrics) {
  assertExactKeys(metrics, 'slabVisibilityState.metrics', [
    'exact', 'slabsProcessed', 'satelliteSlabsProcessed', 'potentialPairs',
    'screeningSegments', 'latitudeBucketCandidates', 'sweptCapCandidates', 'pairAudits',
    'thresholdEvaluations', 'ambiguousRanges', 'entryIntervalsEmitted',
    'releaseIntervalsEmitted'
  ]);
  if (typeof metrics.exact !== 'boolean') fail('slabVisibilityState.metrics.exact must be boolean');
  for (const [key, value] of Object.entries(metrics)) {
    if (key !== 'exact' && (!Number.isSafeInteger(value) || value < 0)) {
      fail(`slabVisibilityState.metrics.${key} must be a non-negative safe integer`);
    }
  }
  return {...metrics};
}

function pairKey(positionId, satelliteId) {
  return `${positionId}\u0000${satelliteId}`;
}

function compareOpenPair(left, right) {
  return compareText(left.positionId, right.positionId) || compareText(left.satelliteId, right.satelliteId);
}

function normalizeOpenPairs(openPairs, state) {
  if (!Array.isArray(openPairs)) fail('slabVisibilityState.openPairs must be an array');
  const satelliteIds = new Set(state.satelliteIds);
  const positionIds = new Set(state.positionIds);
  const normalized = openPairs.map((pair, index) => {
    assertExactKeys(pair, `slabVisibilityState.openPairs[${index}]`, [
      'positionId', 'satelliteId', 'entryStartTimeUs', 'releaseStartTimeUs'
    ]);
    if (!positionIds.has(pair.positionId) || !satelliteIds.has(pair.satelliteId)) {
      fail(`slabVisibilityState.openPairs[${index}] is outside the context inventory`);
    }
    for (const field of ['entryStartTimeUs', 'releaseStartTimeUs']) {
      if (pair[field] !== null) {
        integerTime(pair[field], `slabVisibilityState.openPairs[${index}].${field}`);
        if (pair[field] < state.auditStartTimeUs || pair[field] > state.nextSlabStartTimeUs) {
          fail(`slabVisibilityState.openPairs[${index}].${field} is outside processed time`);
        }
      }
    }
    if (pair.entryStartTimeUs === null && pair.releaseStartTimeUs === null) {
      fail(`slabVisibilityState.openPairs[${index}] has no open threshold`);
    }
    if (pair.entryStartTimeUs !== null && pair.releaseStartTimeUs === null) {
      fail(`slabVisibilityState.openPairs[${index}] is above entry but not release threshold`);
    }
    if (pair.entryStartTimeUs !== null && pair.releaseStartTimeUs > pair.entryStartTimeUs) {
      fail(`slabVisibilityState.openPairs[${index}] opens entry before release`);
    }
    return {...pair};
  }).sort(compareOpenPair);
  for (let index = 1; index < normalized.length; index += 1) {
    if (compareOpenPair(normalized[index - 1], normalized[index]) === 0) {
      fail('slabVisibilityState.openPairs contains a duplicate pair');
    }
  }
  return normalized;
}

function freezeState(state) {
  return Object.freeze({
    ...state,
    satelliteIds: Object.freeze([...state.satelliteIds]),
    positionIds: Object.freeze([...state.positionIds]),
    openPairs: freezeRecords(state.openPairs),
    metrics: freezeMetrics(state.metrics)
  });
}

/** Creates a JSON-safe checkpoint at the beginning of an audit. */
export function createSlabVisibilityState(options) {
  assertExactKeys(options, 'slabVisibilityStateOptions', [
    'context', 'auditStartTimeUs', 'auditEndTimeUs', 'slabDurationUs'
  ]);
  const context = assertContext(options.context);
  const auditStartTimeUs = integerTime(options.auditStartTimeUs, 'auditStartTimeUs');
  const auditEndTimeUs = integerTime(options.auditEndTimeUs, 'auditEndTimeUs');
  if (auditEndTimeUs <= auditStartTimeUs) fail('auditEndTimeUs must be greater than auditStartTimeUs');
  const slabDurationUs = positiveInteger(options.slabDurationUs, 'slabDurationUs');
  if (slabDurationUs % context.scanStepUs !== 0) {
    fail('slabDurationUs must be an exact multiple of context.scanStepUs for stable boundary roots');
  }
  return freezeState({
    schemaVersion: STATE_SCHEMA_VERSION,
    contextHash: context.contextHash,
    auditStartTimeUs,
    auditEndTimeUs,
    slabDurationUs,
    nextSlabStartTimeUs: auditStartTimeUs,
    satelliteIds: context.satelliteIds,
    positionIds: context.catalogGeometry.positions.map(({positionId}) => positionId),
    openPairs: [],
    metrics: emptyMetrics()
  });
}

/** Validates and freezes a state restored from JSON. */
export function restoreSlabVisibilityState(contextValue, serializedState) {
  const context = assertContext(contextValue);
  assertExactKeys(serializedState, 'slabVisibilityState', [
    'schemaVersion', 'contextHash', 'auditStartTimeUs', 'auditEndTimeUs',
    'slabDurationUs', 'nextSlabStartTimeUs', 'satelliteIds', 'positionIds',
    'openPairs', 'metrics'
  ]);
  if (serializedState.schemaVersion !== STATE_SCHEMA_VERSION) fail('unsupported slab visibility state schema');
  if (serializedState.contextHash !== context.contextHash) fail('slab visibility state context hash mismatch');
  const auditStartTimeUs = integerTime(serializedState.auditStartTimeUs, 'slabVisibilityState.auditStartTimeUs');
  const auditEndTimeUs = integerTime(serializedState.auditEndTimeUs, 'slabVisibilityState.auditEndTimeUs');
  const nextSlabStartTimeUs = integerTime(
    serializedState.nextSlabStartTimeUs,
    'slabVisibilityState.nextSlabStartTimeUs'
  );
  const slabDurationUs = positiveInteger(serializedState.slabDurationUs, 'slabVisibilityState.slabDurationUs');
  if (slabDurationUs % context.scanStepUs !== 0) {
    fail('slab visibility state slabDurationUs is not aligned to context.scanStepUs');
  }
  if (auditEndTimeUs <= auditStartTimeUs || nextSlabStartTimeUs < auditStartTimeUs ||
      nextSlabStartTimeUs > auditEndTimeUs) {
    fail('slab visibility state has an invalid audit range');
  }
  const satelliteIds = context.satelliteIds;
  const positionIds = context.catalogGeometry.positions.map(({positionId}) => positionId);
  if (!Array.isArray(serializedState.satelliteIds) ||
      serializedState.satelliteIds.length !== satelliteIds.length ||
      serializedState.satelliteIds.some((id, index) => id !== satelliteIds[index])) {
    fail('slab visibility state satellite shard mismatch');
  }
  if (!Array.isArray(serializedState.positionIds) ||
      serializedState.positionIds.length !== positionIds.length ||
      serializedState.positionIds.some((id, index) => id !== positionIds[index])) {
    fail('slab visibility state catalog mismatch');
  }
  const state = {
    schemaVersion: STATE_SCHEMA_VERSION,
    contextHash: context.contextHash,
    auditStartTimeUs,
    auditEndTimeUs,
    slabDurationUs,
    nextSlabStartTimeUs,
    satelliteIds,
    positionIds,
    openPairs: [],
    metrics: validateMetrics(serializedState.metrics)
  };
  state.openPairs = normalizeOpenPairs(serializedState.openPairs, state);
  if (nextSlabStartTimeUs === auditEndTimeUs && state.openPairs.length !== 0) {
    fail('completed slab visibility state must not contain open pairs');
  }
  return freezeState(state);
}

function bucketRange(geometry, centreLatitudeDeg, angularRadiusRad) {
  if (angularRadiusRad >= Math.PI) return {first: 0, last: geometry.bucketCount - 1};
  const radiusDeg = angularRadiusRad * RADIANS_TO_DEGREES;
  const minimumLatitudeDeg = Math.max(-90, centreLatitudeDeg - radiusDeg);
  const maximumLatitudeDeg = Math.min(90, centreLatitudeDeg + radiusDeg);
  return {
    first: Math.max(0, Math.floor((minimumLatitudeDeg + 90) / geometry.latitudeBucketDeg)),
    last: Math.min(
      geometry.bucketCount - 1,
      Math.floor((maximumLatitudeDeg + 90) / geometry.latitudeBucketDeg)
    )
  };
}

function subSatelliteLatitudeDeg(unit) {
  return Math.asin(Math.max(-1, Math.min(1, unit[2]))) * RADIANS_TO_DEGREES;
}

function dot(left, right) {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

function sampleThresholds(context, compiledSatellite, observerUnit, timeUs) {
  const satelliteUnit = compiledSatellite.propagate(timeUs);
  const value = dot(satelliteUnit, observerUnit);
  const entryMargin = value - context.entryMinimumDot;
  const releaseMargin = value - context.releaseMinimumDot;
  return {
    entryAbove: entryMargin >= -context.marginTolerance,
    releaseAbove: releaseMargin >= -context.marginTolerance,
    entryMargin,
    releaseMargin
  };
}

function screenedPositions(context, compiledSatellite, slabStartTimeUs, slabEndTimeUs, deltaMetrics) {
  // Screening covers only the central half-open slab. Padding is root-search
  // context; a pair that is below 42 degrees throughout the central slab
  // cannot contribute a central crossing or an open interval.
  const searchStartTimeUs = slabStartTimeUs;
  const searchEndTimeUs = slabEndTimeUs;
  const candidatesById = new Map();
  for (let segmentStartTimeUs = searchStartTimeUs; segmentStartTimeUs < searchEndTimeUs;) {
    const segmentEndTimeUs = Math.min(searchEndTimeUs, segmentStartTimeUs + context.screeningStepUs);
    const midpointUs = segmentStartTimeUs + Math.floor((segmentEndTimeUs - segmentStartTimeUs) / 2);
    const halfWidthSeconds = Math.max(
      midpointUs - segmentStartTimeUs,
      segmentEndTimeUs - midpointUs
    ) / 1_000_000;
    const sweptRadiusRad = Math.min(
      Math.PI,
      context.releaseAngleRad + context.orbitModel.conservativeAngularRateRadPerSecond * halfWidthSeconds
        + SCREENING_ROUNDING_MARGIN
    );
    const satelliteUnit = compiledSatellite.propagate(midpointUs);
    const range = bucketRange(
      context.catalogGeometry,
      subSatelliteLatitudeDeg(satelliteUnit),
      sweptRadiusRad
    );
    const minimumDot = sweptRadiusRad >= Math.PI ? -1 : Math.cos(sweptRadiusRad);
    deltaMetrics.screeningSegments += 1;
    for (let bucketIndex = range.first; bucketIndex <= range.last; bucketIndex += 1) {
      const bucket = context.catalogGeometry.buckets[bucketIndex];
      deltaMetrics.latitudeBucketCandidates += bucket.length;
      for (const position of bucket) {
        if (dot(satelliteUnit, position.observerUnit) + context.marginTolerance + SCREENING_ROUNDING_MARGIN < minimumDot) {
          continue;
        }
        candidatesById.set(position.positionId, position);
      }
    }
    segmentStartTimeUs = segmentEndTimeUs;
  }
  const candidates = [...candidatesById.values()];
  candidates.sort((left, right) => compareText(left.positionId, right.positionId));
  deltaMetrics.sweptCapCandidates += candidates.length;
  return candidates;
}

function mutableOpenMap(state) {
  return new Map(state.openPairs.map((pair) => [pairKey(pair.positionId, pair.satelliteId), {...pair}]));
}

function ensurePair(openMap, positionId, satelliteId) {
  const key = pairKey(positionId, satelliteId);
  let pair = openMap.get(key);
  if (pair === undefined) {
    pair = {positionId, satelliteId, entryStartTimeUs: null, releaseStartTimeUs: null};
    openMap.set(key, pair);
  }
  return pair;
}

function applyThresholdEvent(openMap, event, entryIntervals, releaseIntervals) {
  const pair = ensurePair(openMap, event.positionId, event.satelliteId);
  const isEntry = event.kind.startsWith('entry_');
  const field = isEntry ? 'entryStartTimeUs' : 'releaseStartTimeUs';
  const output = isEntry ? entryIntervals : releaseIntervals;
  if (event.kind.endsWith('_enter')) {
    if (pair[field] === null) pair[field] = event.timeUs;
  } else if (pair[field] !== null) {
    if (event.timeUs > pair[field]) {
      output.push({
        positionId: pair.positionId,
        satelliteId: pair.satelliteId,
        startTimeUs: pair[field],
        endTimeUs: event.timeUs
      });
    }
    pair[field] = null;
  }
  if (pair.entryStartTimeUs === null && pair.releaseStartTimeUs === null) {
    openMap.delete(pairKey(pair.positionId, pair.satelliteId));
  }
}

function initializeFirstSlabPair(openMap, initial, positionId, satelliteId, auditStartTimeUs) {
  if (!initial.entryAbove && !initial.releaseAbove) return;
  const pair = ensurePair(openMap, positionId, satelliteId);
  if (initial.entryAbove) pair.entryStartTimeUs = auditStartTimeUs;
  if (initial.releaseAbove) pair.releaseStartTimeUs = auditStartTimeUs;
  if (pair.entryStartTimeUs !== null && pair.releaseStartTimeUs === null) {
    fail(`visibility thresholds are inconsistent for '${positionId}/${satelliteId}'`);
  }
}

function verifyBoundaryContinuity(
  openMap,
  result,
  initial,
  marginTolerance,
  positionId,
  satelliteId,
  slabStartTimeUs
) {
  if (!result.exact) return;
  const existing = openMap.get(pairKey(positionId, satelliteId));
  const entry = existing?.entryStartTimeUs !== null && existing?.entryStartTimeUs !== undefined;
  const release = existing?.releaseStartTimeUs !== null && existing?.releaseStartTimeUs !== undefined;
  // The carried state represents the instant just before start-boundary events
  // are applied. A crossing whose root bracket straddles the boundary is owned
  // by exactly one adjacent slab and may be normalized to slabStartTimeUs, so
  // applying it here would compare a post-event state with a pre-event sample.
  const entryMismatch = Math.abs(initial.entryMargin) > marginTolerance && entry !== result.initialEntry;
  const releaseMismatch = Math.abs(initial.releaseMargin) > marginTolerance && release !== result.initialRelease;
  if (entryMismatch || releaseMismatch) {
    fail(`slab boundary continuity mismatch for '${positionId}/${satelliteId}' at ${slabStartTimeUs}`);
  }
}

function addMetrics(target, delta) {
  const combined = {...target, exact: target.exact && delta.exact};
  for (const [key, value] of Object.entries(delta)) {
    if (key === 'exact') continue;
    combined[key] += value;
    if (!Number.isSafeInteger(combined[key])) fail(`slab visibility metric '${key}' exceeds safe integer range`);
  }
  return combined;
}

/**
 * Processes exactly one half-open time slab and returns a new checkpoint. Root
 * searches include padding on both sides, but only crossings in [start,end)
 * are applied, so adjacent slabs cannot duplicate a boundary event.
 */
export function processNextVisibilitySlab(options) {
  assertExactKeys(options, 'processNextVisibilitySlab', ['context', 'state']);
  const context = assertContext(options.context);
  const state = restoreSlabVisibilityState(context, options.state);
  if (state.nextSlabStartTimeUs >= state.auditEndTimeUs) fail('slab visibility audit is already complete');
  const slabStartTimeUs = state.nextSlabStartTimeUs;
  const slabEndTimeUs = Math.min(state.auditEndTimeUs, slabStartTimeUs + state.slabDurationUs);
  const firstSlab = slabStartTimeUs === state.auditStartTimeUs;
  const finalSlab = slabEndTimeUs === state.auditEndTimeUs;
  const deltaMetrics = {
    ...emptyMetrics(),
    slabsProcessed: 1,
    satelliteSlabsProcessed: context.satellites.length,
    potentialPairs: context.satellites.length * context.catalogGeometry.positions.length
  };
  if (!Number.isSafeInteger(deltaMetrics.potentialPairs)) fail('slab potential pair count exceeds safe integer range');
  const openMap = mutableOpenMap(state);
  const events = [];
  const initialStates = [];
  const ambiguous = [];

  for (const compiledSatellite of context.satellites) {
    const satelliteId = compiledSatellite.definition.id;
    const positions = screenedPositions(
      context,
      compiledSatellite,
      slabStartTimeUs,
      slabEndTimeUs,
      deltaMetrics
    );
    const candidateIds = new Set(positions.map(({positionId}) => positionId));
    for (const openPair of openMap.values()) {
      if (openPair.satelliteId === satelliteId && !candidateIds.has(openPair.positionId)) {
        fail(`open pair '${openPair.positionId}/${satelliteId}' escaped conservative swept-cap screening`);
      }
    }
    for (const position of positions) {
      const initial = sampleThresholds(
        context,
        compiledSatellite,
        position.observerUnit,
        slabStartTimeUs
      );
      const result = auditPairVisibility({
        satellite: compiledSatellite.definition,
        observerUnit: position.observerUnit,
        startTimeUs: slabStartTimeUs,
        endTimeUs: slabEndTimeUs,
        boundaryPaddingUs: context.boundaryPaddingUs,
        orbitModel: context.orbitModel,
        entryElevationDeg: context.entryElevationDeg,
        releaseElevationDeg: context.releaseElevationDeg,
        toleranceUs: context.toleranceUs,
        scanStepUs: context.scanStepUs,
        marginTolerance: context.marginTolerance,
        maxEvaluations: context.maxEvaluations
      });
      deltaMetrics.pairAudits += 1;
      deltaMetrics.thresholdEvaluations += result.evaluations;
      if (!Number.isSafeInteger(deltaMetrics.thresholdEvaluations)) {
        fail('slab threshold evaluation count exceeds safe integer range');
      }
      if (firstSlab) {
        const canonicalInitial = {
          entryAbove: result.initialEntry,
          releaseAbove: result.initialRelease
        };
        initializeFirstSlabPair(
          openMap,
          canonicalInitial,
          position.positionId,
          satelliteId,
          state.auditStartTimeUs
        );
        if (canonicalInitial.entryAbove || canonicalInitial.releaseAbove) {
          initialStates.push({
            positionId: position.positionId,
            satelliteId,
            aboveEntry: canonicalInitial.entryAbove,
            aboveRelease: canonicalInitial.releaseAbove
          });
        }
      } else {
        verifyBoundaryContinuity(
          openMap,
          result,
          initial,
          context.marginTolerance,
          position.positionId,
          satelliteId,
          slabStartTimeUs
        );
      }
      for (const event of result.events) {
        events.push({...event, positionId: position.positionId, satelliteId});
      }
      for (const item of result.ambiguous) {
        ambiguous.push({...item, positionId: position.positionId, satelliteId});
      }
    }
  }

  events.sort(compareEvent);
  ambiguous.sort(compareAmbiguity);
  deltaMetrics.ambiguousRanges = ambiguous.length;
  deltaMetrics.exact = ambiguous.length === 0;
  const entryIntervals = [];
  const releaseIntervals = [];
  for (const event of events) applyThresholdEvent(openMap, event, entryIntervals, releaseIntervals);

  if (finalSlab) {
    for (const pair of [...openMap.values()].sort(compareOpenPair)) {
      if (pair.entryStartTimeUs !== null && slabEndTimeUs > pair.entryStartTimeUs) {
        entryIntervals.push({
          positionId: pair.positionId,
          satelliteId: pair.satelliteId,
          startTimeUs: pair.entryStartTimeUs,
          endTimeUs: slabEndTimeUs
        });
      }
      if (pair.releaseStartTimeUs !== null && slabEndTimeUs > pair.releaseStartTimeUs) {
        releaseIntervals.push({
          positionId: pair.positionId,
          satelliteId: pair.satelliteId,
          startTimeUs: pair.releaseStartTimeUs,
          endTimeUs: slabEndTimeUs
        });
      }
    }
    openMap.clear();
  }
  entryIntervals.sort(compareInterval);
  releaseIntervals.sort(compareInterval);
  deltaMetrics.entryIntervalsEmitted = entryIntervals.length;
  deltaMetrics.releaseIntervalsEmitted = releaseIntervals.length;
  const nextMetrics = addMetrics(state.metrics, deltaMetrics);
  const nextState = freezeState({
    ...state,
    nextSlabStartTimeUs: slabEndTimeUs,
    openPairs: [...openMap.values()].sort(compareOpenPair),
    metrics: nextMetrics
  });
  return Object.freeze({
    slabStartTimeUs,
    slabEndTimeUs,
    complete: finalSlab,
    initialStates: freezeRecords(initialStates.sort(compareOpenPair)),
    events: freezeRecords(events),
    entryIntervals: freezeRecords(entryIntervals),
    releaseIntervals: freezeRecords(releaseIntervals),
    ambiguous: freezeRecords(ambiguous),
    metrics: freezeMetrics(deltaMetrics),
    state: nextState
  });
}

/** Runs a bounded number of slabs; maxSlabs makes cooperative checkpointing straightforward. */
export function runSlabVisibilityAudit(options) {
  assertExactKeys(options, 'runSlabVisibilityAudit', ['context', 'state'], ['maxSlabs']);
  const context = assertContext(options.context);
  let state = restoreSlabVisibilityState(context, options.state);
  const maxSlabs = options.maxSlabs === undefined
    ? Number.MAX_SAFE_INTEGER
    : positiveInteger(options.maxSlabs, 'runSlabVisibilityAudit.maxSlabs');
  const events = [];
  const initialStates = [];
  const entryIntervals = [];
  const releaseIntervals = [];
  const ambiguous = [];
  let processed = 0;
  while (state.nextSlabStartTimeUs < state.auditEndTimeUs && processed < maxSlabs) {
    const result = processNextVisibilitySlab({context, state});
    state = result.state;
    initialStates.push(...result.initialStates);
    events.push(...result.events);
    entryIntervals.push(...result.entryIntervals);
    releaseIntervals.push(...result.releaseIntervals);
    ambiguous.push(...result.ambiguous);
    processed += 1;
  }
  events.sort(compareEvent);
  entryIntervals.sort(compareInterval);
  releaseIntervals.sort(compareInterval);
  ambiguous.sort(compareAmbiguity);
  return Object.freeze({
    complete: state.nextSlabStartTimeUs === state.auditEndTimeUs,
    slabsProcessed: processed,
    initialStates: freezeRecords(initialStates.sort(compareOpenPair)),
    events: freezeRecords(events),
    entryIntervals: freezeRecords(entryIntervals),
    releaseIntervals: freezeRecords(releaseIntervals),
    ambiguous: freezeRecords(ambiguous),
    metrics: state.metrics,
    state
  });
}

/** Deterministically combines disjoint satellite-shard or checkpoint outputs. */
export function mergeSlabVisibilityRecords(outputs) {
  if (!Array.isArray(outputs)) fail('outputs must be an array');
  const events = [];
  const initialStates = [];
  const entryIntervals = [];
  const releaseIntervals = [];
  const ambiguous = [];
  for (const [index, output] of outputs.entries()) {
    if (!isPlainObject(output) || !Array.isArray(output.initialStates) || !Array.isArray(output.events) ||
        !Array.isArray(output.entryIntervals) || !Array.isArray(output.releaseIntervals) ||
        !Array.isArray(output.ambiguous)) {
      fail(`outputs[${index}] is invalid`);
    }
    initialStates.push(...output.initialStates);
    events.push(...output.events);
    entryIntervals.push(...output.entryIntervals);
    releaseIntervals.push(...output.releaseIntervals);
    ambiguous.push(...output.ambiguous);
  }
  events.sort(compareEvent);
  entryIntervals.sort(compareInterval);
  releaseIntervals.sort(compareInterval);
  ambiguous.sort(compareAmbiguity);
  return Object.freeze({
    initialStates: freezeRecords(initialStates.sort(compareOpenPair)),
    events: freezeRecords(events),
    entryIntervals: freezeRecords(entryIntervals),
    releaseIntervals: freezeRecords(releaseIntervals),
    ambiguous: freezeRecords(ambiguous)
  });
}
