const TWO_PI = 2 * Math.PI;

export const DEFAULT_EARTH_RADIUS_KM = 6378.137;
export const DEFAULT_EARTH_MU_KM3_S2 = 398600.4418;
export const DEFAULT_EARTH_ROTATION_RAD_PER_SECOND = 7.2921159e-5;

const DEFAULT_ALTITUDE_KM = 500;
const DEFAULT_ROOT_TOLERANCE_US = 1_000;
const MAX_ROOT_TOLERANCE_US = 10_000;
const DEFAULT_SCAN_STEP_US = 60_000_000;
const DEFAULT_MARGIN_TOLERANCE = 1e-12;
const DEFAULT_MAX_EVALUATIONS = 1_000_000;

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

function safeInteger(value, context) {
  if (!Number.isSafeInteger(value)) fail(`${context} must be a safe integer number of microseconds`);
  return value;
}

function toRadians(value) {
  return value * Math.PI / 180;
}

function normalizeDegrees(value) {
  const normalized = ((value % 360) + 360) % 360;
  return Object.is(normalized, -0) ? 0 : normalized;
}

function freezeVector(vector) {
  return Object.freeze(vector);
}

function assertUnitVector(value, context) {
  if (!Array.isArray(value) || value.length !== 3 || value.some((component) => !Number.isFinite(component))) {
    fail(`${context} must be a finite three-component unit vector`);
  }
  const norm = Math.hypot(value[0], value[1], value[2]);
  if (Math.abs(norm - 1) > 1e-9) fail(`${context} must have unit length`);
  return value;
}

/**
 * Resolves the circular, spherical-Earth orbit model used by the existing NTN
 * planning prototype. The returned angular-rate bound is conservative for a
 * fixed ground unit vector and an ECEF satellite unit vector.
 */
export function resolveOrbitModel(config = {}) {
  assertExactKeys(config, 'orbitModel', [], [
    'earthRadiusKm',
    'earthMuKm3S2',
    'earthRotationRadPerSecond',
    'altitudeKm'
  ]);
  const earthRadiusKm = finiteNumber(
    config.earthRadiusKm ?? DEFAULT_EARTH_RADIUS_KM,
    'orbitModel.earthRadiusKm',
    Number.MIN_VALUE
  );
  const earthMuKm3S2 = finiteNumber(
    config.earthMuKm3S2 ?? DEFAULT_EARTH_MU_KM3_S2,
    'orbitModel.earthMuKm3S2',
    Number.MIN_VALUE
  );
  const earthRotationRadPerSecond = finiteNumber(
    config.earthRotationRadPerSecond ?? DEFAULT_EARTH_ROTATION_RAD_PER_SECOND,
    'orbitModel.earthRotationRadPerSecond',
    0
  );
  const altitudeKm = finiteNumber(config.altitudeKm ?? DEFAULT_ALTITUDE_KM, 'orbitModel.altitudeKm', Number.MIN_VALUE);
  const orbitRadiusKm = earthRadiusKm + altitudeKm;
  const meanMotionRadPerSecond = Math.sqrt(earthMuKm3S2 / orbitRadiusKm ** 3);
  const orbitPeriodSeconds = TWO_PI / meanMotionRadPerSecond;
  return Object.freeze({
    earthRadiusKm,
    earthMuKm3S2,
    earthRotationRadPerSecond,
    altitudeKm,
    orbitRadiusKm,
    meanMotionRadPerSecond,
    orbitPeriodSeconds,
    conservativeAngularRateRadPerSecond: meanMotionRadPerSecond + earthRotationRadPerSecond
  });
}

function assertResolvedOrbitModel(model) {
  assertExactKeys(model, 'resolvedOrbitModel', [
    'earthRadiusKm',
    'earthMuKm3S2',
    'earthRotationRadPerSecond',
    'altitudeKm',
    'orbitRadiusKm',
    'meanMotionRadPerSecond',
    'orbitPeriodSeconds',
    'conservativeAngularRateRadPerSecond'
  ]);
  const canonical = resolveOrbitModel({
    earthRadiusKm: model.earthRadiusKm,
    earthMuKm3S2: model.earthMuKm3S2,
    earthRotationRadPerSecond: model.earthRotationRadPerSecond,
    altitudeKm: model.altitudeKm
  });
  for (const key of Object.keys(canonical)) {
    if (!Number.isFinite(model[key]) || Math.abs(model[key] - canonical[key]) > Math.max(1, Math.abs(canonical[key])) * 1e-12) {
      fail(`resolvedOrbitModel.${key} is inconsistent with the circular orbit model`);
    }
  }
  return model;
}

/** Creates a deterministic Walker Delta satellite catalog. */
export function createWalkerDelta(config) {
  assertExactKeys(config, 'walker', ['planes', 'satellitesPerPlane', 'phaseFactor'], [
    'altitudeKm',
    'inclinationDeg',
    'raanOffsetDeg',
    'phaseOffsetDeg'
  ]);
  const planes = positiveInteger(config.planes, 'walker.planes');
  const satellitesPerPlane = positiveInteger(config.satellitesPerPlane, 'walker.satellitesPerPlane');
  if (!Number.isSafeInteger(config.phaseFactor) || config.phaseFactor < 0 || config.phaseFactor >= planes) {
    fail('walker.phaseFactor must be a safe integer in 0..planes-1');
  }
  const altitudeKm = finiteNumber(config.altitudeKm ?? DEFAULT_ALTITUDE_KM, 'walker.altitudeKm', Number.MIN_VALUE);
  const inclinationDeg = finiteNumber(config.inclinationDeg ?? 60, 'walker.inclinationDeg', 0, 180);
  const raanOffsetDeg = finiteNumber(config.raanOffsetDeg ?? 0, 'walker.raanOffsetDeg');
  const phaseOffsetDeg = finiteNumber(config.phaseOffsetDeg ?? 0, 'walker.phaseOffsetDeg');
  if (planes * satellitesPerPlane > Number.MAX_SAFE_INTEGER) fail('walker satellite count exceeds safe integer range');

  const planeDigits = Math.max(2, String(planes).length);
  const slotDigits = Math.max(2, String(satellitesPerPlane).length);
  const definitions = [];
  for (let planeIndex = 0; planeIndex < planes; planeIndex += 1) {
    for (let slotIndex = 0; slotIndex < satellitesPerPlane; slotIndex += 1) {
      definitions.push(Object.freeze({
        id: `P${String(planeIndex + 1).padStart(planeDigits, '0')}-S${String(slotIndex + 1).padStart(slotDigits, '0')}`,
        plane: planeIndex + 1,
        slot: slotIndex + 1,
        raanDeg: normalizeDegrees(raanOffsetDeg + planeIndex * 360 / planes),
        phaseDeg: normalizeDegrees(
          phaseOffsetDeg + slotIndex * 360 / satellitesPerPlane
          + planeIndex * config.phaseFactor * 360 / (planes * satellitesPerPlane)
        ),
        altitudeKm,
        inclinationDeg
      }));
    }
  }
  return Object.freeze(definitions);
}

function assertSatelliteDefinition(definition) {
  assertExactKeys(definition, 'satellite', [
    'id', 'plane', 'slot', 'raanDeg', 'phaseDeg', 'altitudeKm', 'inclinationDeg'
  ]);
  if (typeof definition.id !== 'string' || !/^P\d{2,}-S\d{2,}$/.test(definition.id)) {
    fail('satellite.id must use Pxx-Syy form');
  }
  positiveInteger(definition.plane, 'satellite.plane');
  positiveInteger(definition.slot, 'satellite.slot');
  finiteNumber(definition.raanDeg, 'satellite.raanDeg', 0, 360);
  finiteNumber(definition.phaseDeg, 'satellite.phaseDeg', 0, 360);
  finiteNumber(definition.altitudeKm, 'satellite.altitudeKm', Number.MIN_VALUE);
  finiteNumber(definition.inclinationDeg, 'satellite.inclinationDeg', 0, 180);
  return definition;
}

/** Propagates one circular orbit and returns an ECEF unit vector at integer microsecond time. */
export function satelliteUnitEcef(definition, timeUs, orbitModel = undefined) {
  assertSatelliteDefinition(definition);
  safeInteger(timeUs, 'timeUs');
  const model = orbitModel === undefined
    ? resolveOrbitModel({altitudeKm: definition.altitudeKm})
    : assertResolvedOrbitModel(orbitModel);
  if (Math.abs(model.altitudeKm - definition.altitudeKm) > 1e-9) {
    fail('satellite altitude does not match resolvedOrbitModel.altitudeKm');
  }

  const seconds = timeUs / 1_000_000;
  const inclination = toRadians(definition.inclinationDeg);
  const raan = toRadians(definition.raanDeg);
  const argument = toRadians(definition.phaseDeg) + seconds * model.meanMotionRadPerSecond;
  const cosRaan = Math.cos(raan);
  const sinRaan = Math.sin(raan);
  const cosArgument = Math.cos(argument);
  const sinArgument = Math.sin(argument);
  const cosInclination = Math.cos(inclination);
  const sinInclination = Math.sin(inclination);
  const xEci = cosRaan * cosArgument - sinRaan * sinArgument * cosInclination;
  const yEci = sinRaan * cosArgument + cosRaan * sinArgument * cosInclination;
  const z = sinArgument * sinInclination;
  const earthAngle = model.earthRotationRadPerSecond * seconds;
  const cosEarth = Math.cos(earthAngle);
  const sinEarth = Math.sin(earthAngle);
  return freezeVector([
    cosEarth * xEci + sinEarth * yEci,
    -sinEarth * xEci + cosEarth * yEci,
    z
  ]);
}

/** Returns the spherical-Earth ground unit vector for a catalog position. */
export function groundUnit(point) {
  assertExactKeys(point, 'groundPoint', ['lat', 'lon'], ['id', 'name']);
  const lat = toRadians(finiteNumber(point.lat, 'groundPoint.lat', -90, 90));
  const lon = toRadians(finiteNumber(point.lon, 'groundPoint.lon', -180, 180));
  return freezeVector([
    Math.cos(lat) * Math.cos(lon),
    Math.cos(lat) * Math.sin(lon),
    Math.sin(lat)
  ]);
}

/** Converts an elevation threshold to its Earth-centred coverage half-angle. */
export function centralAngleForElevation(elevationDeg, orbitModel = resolveOrbitModel()) {
  finiteNumber(elevationDeg, 'elevationDeg', 0, 90);
  const model = assertResolvedOrbitModel(orbitModel);
  const elevation = toRadians(elevationDeg);
  const cosineArgument = Math.max(-1, Math.min(1, model.earthRadiusKm / model.orbitRadiusKm * Math.cos(elevation)));
  return Math.max(0, Math.acos(cosineArgument) - elevation);
}

/** Positive means the satellite is inside the supplied central-angle threshold. */
export function visibilityMargin(satelliteUnit, observerUnit, centralAngleRad) {
  assertUnitVector(satelliteUnit, 'satelliteUnit');
  assertUnitVector(observerUnit, 'observerUnit');
  finiteNumber(centralAngleRad, 'centralAngleRad', 0, Math.PI);
  const dot = satelliteUnit[0] * observerUnit[0]
    + satelliteUnit[1] * observerUnit[1]
    + satelliteUnit[2] * observerUnit[2];
  return dot - Math.cos(centralAngleRad);
}

function crossingResult(crossings, ambiguous, toleranceUs, evaluations) {
  return Object.freeze({
    crossings: Object.freeze(crossings.map((item) => Object.freeze(item))),
    ambiguous: Object.freeze(ambiguous.map((item) => Object.freeze(item))),
    toleranceUs,
    evaluations
  });
}

/**
 * Finds all threshold crossings of a continuous dot-margin function.
 *
 * The caller supplies a valid angular-rate bound. Midpoint Lipschitz bounds
 * conservatively discard intervals that cannot contain a root. A same-sign
 * unresolved region is reported as ambiguous instead of being silently
 * treated as a crossing; this includes threshold tangencies.
 */
export function findThresholdCrossings(options) {
  assertExactKeys(options, 'crossingSearch', [
    'marginAtTimeUs',
    'startTimeUs',
    'endTimeUs',
    'angularRateBoundRadPerSecond'
  ], [
    'toleranceUs',
    'scanStepUs',
    'marginTolerance',
    'maxEvaluations'
  ]);
  if (typeof options.marginAtTimeUs !== 'function') fail('crossingSearch.marginAtTimeUs must be a function');
  const startTimeUs = safeInteger(options.startTimeUs, 'crossingSearch.startTimeUs');
  const endTimeUs = safeInteger(options.endTimeUs, 'crossingSearch.endTimeUs');
  if (endTimeUs <= startTimeUs) fail('crossingSearch.endTimeUs must be greater than startTimeUs');
  const angularRateBound = finiteNumber(
    options.angularRateBoundRadPerSecond,
    'crossingSearch.angularRateBoundRadPerSecond',
    Number.MIN_VALUE
  );
  const toleranceUs = positiveInteger(
    options.toleranceUs ?? DEFAULT_ROOT_TOLERANCE_US,
    'crossingSearch.toleranceUs',
    MAX_ROOT_TOLERANCE_US
  );
  const scanStepUs = positiveInteger(options.scanStepUs ?? DEFAULT_SCAN_STEP_US, 'crossingSearch.scanStepUs');
  if (scanStepUs < toleranceUs) fail('crossingSearch.scanStepUs must be at least toleranceUs');
  const marginTolerance = finiteNumber(
    options.marginTolerance ?? DEFAULT_MARGIN_TOLERANCE,
    'crossingSearch.marginTolerance',
    0
  );
  const maxEvaluations = positiveInteger(
    options.maxEvaluations ?? DEFAULT_MAX_EVALUATIONS,
    'crossingSearch.maxEvaluations'
  );

  let evaluations = 0;
  const sampleCache = new Map();
  const evaluate = (timeUs) => {
    const cached = sampleCache.get(timeUs);
    if (cached !== undefined) return cached;
    if (evaluations >= maxEvaluations) fail(`crossing search exceeded maxEvaluations=${maxEvaluations}`);
    const margin = options.marginAtTimeUs(timeUs);
    if (!Number.isFinite(margin)) fail(`marginAtTimeUs(${timeUs}) returned a non-finite value`);
    evaluations += 1;
    sampleCache.set(timeUs, margin);
    return margin;
  };

  const partitions = [];
  const emitPartition = (state, start, end) => {
    const previous = partitions.at(-1);
    if (previous && previous.state === state && previous.endTimeUs === start) {
      previous.endTimeUs = end;
      return;
    }
    partitions.push({state, startTimeUs: start, endTimeUs: end});
  };

  const visit = (leftTimeUs, rightTimeUs) => {
    const midpointUs = leftTimeUs + Math.floor((rightTimeUs - leftTimeUs) / 2);
    const leftMargin = evaluate(leftTimeUs);
    const rightMargin = evaluate(rightTimeUs);
    const midpointMargin = evaluate(midpointUs);
    const radius = angularRateBound * ((rightTimeUs - leftTimeUs) / 2_000_000);
    if (midpointMargin - radius > marginTolerance) {
      emitPartition('above', leftTimeUs, rightTimeUs);
      return;
    }
    if (midpointMargin + radius < -marginTolerance) {
      emitPartition('below', leftTimeUs, rightTimeUs);
      return;
    }
    if (Math.abs(leftMargin) <= marginTolerance
        && Math.abs(midpointMargin) <= marginTolerance
        && Math.abs(rightMargin) <= marginTolerance) {
      emitPartition('uncertain', leftTimeUs, rightTimeUs);
      return;
    }
    if (rightTimeUs - leftTimeUs <= toleranceUs || midpointUs === leftTimeUs || midpointUs === rightTimeUs) {
      emitPartition('uncertain', leftTimeUs, rightTimeUs);
      return;
    }
    visit(leftTimeUs, midpointUs);
    visit(midpointUs, rightTimeUs);
  };

  for (let segmentStartUs = startTimeUs; segmentStartUs < endTimeUs;) {
    const segmentEndUs = Math.min(endTimeUs, segmentStartUs + scanStepUs);
    visit(segmentStartUs, segmentEndUs);
    segmentStartUs = segmentEndUs;
  }

  const refine = (leftTimeUs, rightTimeUs, leftState, rightState) => {
    let left = leftTimeUs;
    let right = rightTimeUs;
    let leftMargin = evaluate(left);
    let rightMargin = evaluate(right);
    if (leftMargin === 0) return {left, right: left};
    if (rightMargin === 0) return {left: right, right};
    if (Math.sign(leftMargin) !== (leftState === 'above' ? 1 : -1)
        || Math.sign(rightMargin) !== (rightState === 'above' ? 1 : -1)) {
      return null;
    }
    while (right - left > toleranceUs) {
      const midpoint = left + Math.floor((right - left) / 2);
      if (midpoint === left || midpoint === right) break;
      const midpointMargin = evaluate(midpoint);
      if (midpointMargin === 0) return {left: midpoint, right: midpoint};
      if (Math.sign(midpointMargin) === Math.sign(leftMargin)) {
        left = midpoint;
        leftMargin = midpointMargin;
      } else {
        right = midpoint;
        rightMargin = midpointMargin;
      }
    }
    return {left, right};
  };

  const crossings = [];
  const ambiguous = [];
  for (let index = 0; index < partitions.length; index += 1) {
    const partition = partitions[index];
    if (partition.state !== 'uncertain') continue;
    const leftState = partitions[index - 1]?.state;
    const rightState = partitions[index + 1]?.state;
    if ((leftState === 'above' || leftState === 'below')
        && (rightState === 'above' || rightState === 'below')
        && leftState !== rightState) {
      const bracket = refine(partition.startTimeUs, partition.endTimeUs, leftState, rightState);
      if (bracket !== null) {
        const direction = leftState === 'below' ? 'up' : 'down';
        crossings.push({
          timeUs: bracket.left === bracket.right
            ? bracket.left
            : direction === 'up' ? bracket.right : bracket.left,
          bracketStartUs: bracket.left,
          bracketEndUs: bracket.right,
          direction
        });
        continue;
      }
    }
    ambiguous.push({
      startTimeUs: partition.startTimeUs,
      endTimeUs: partition.endTimeUs,
      reason: leftState === undefined || rightState === undefined ? 'range_boundary' : 'tangent_or_unresolved'
    });
  }

  return crossingResult(crossings, ambiguous, toleranceUs, evaluations);
}

function validateCrossingSearchResult(value, context) {
  assertExactKeys(value, context, ['crossings', 'ambiguous', 'toleranceUs', 'evaluations']);
  if (!Array.isArray(value.crossings) || !Array.isArray(value.ambiguous)) {
    fail(`${context}.crossings and ambiguous must be arrays`);
  }
  positiveInteger(value.toleranceUs, `${context}.toleranceUs`, MAX_ROOT_TOLERANCE_US);
  if (!Number.isSafeInteger(value.evaluations) || value.evaluations < 0) fail(`${context}.evaluations must be non-negative`);
  const crossings = value.crossings.map((crossing, index) => {
    assertExactKeys(crossing, `${context}.crossings[${index}]`, [
      'timeUs', 'bracketStartUs', 'bracketEndUs', 'direction'
    ]);
    const timeUs = safeInteger(crossing.timeUs, `${context}.crossings[${index}].timeUs`);
    const bracketStartUs = safeInteger(crossing.bracketStartUs, `${context}.crossings[${index}].bracketStartUs`);
    const bracketEndUs = safeInteger(crossing.bracketEndUs, `${context}.crossings[${index}].bracketEndUs`);
    if (bracketEndUs < bracketStartUs || timeUs < bracketStartUs || timeUs > bracketEndUs) {
      fail(`${context}.crossings[${index}] has an invalid bracket`);
    }
    if (crossing.direction !== 'up' && crossing.direction !== 'down') {
      fail(`${context}.crossings[${index}].direction must be up or down`);
    }
    return {timeUs, bracketStartUs, bracketEndUs, direction: crossing.direction};
  });
  const ambiguous = value.ambiguous.map((range, index) => {
    assertExactKeys(range, `${context}.ambiguous[${index}]`, ['startTimeUs', 'endTimeUs', 'reason']);
    const startTimeUs = safeInteger(range.startTimeUs, `${context}.ambiguous[${index}].startTimeUs`);
    const endTimeUs = safeInteger(range.endTimeUs, `${context}.ambiguous[${index}].endTimeUs`);
    if (endTimeUs < startTimeUs || typeof range.reason !== 'string' || range.reason.length === 0) {
      fail(`${context}.ambiguous[${index}] is invalid`);
    }
    return {startTimeUs, endTimeUs, reason: range.reason};
  });
  return {crossings, ambiguous};
}

/**
 * Builds half-open held intervals from complete entry- and release-threshold
 * crossing results. Entry uses only an upward 45-degree crossing; release uses
 * only a downward 42-degree crossing. Input ordering has no effect.
 */
export function buildHysteresisIntervals(options) {
  assertExactKeys(options, 'hysteresis', [
    'startTimeUs',
    'endTimeUs',
    'enterCrossings',
    'releaseCrossings'
  ], ['initiallyHeld']);
  const startTimeUs = safeInteger(options.startTimeUs, 'hysteresis.startTimeUs');
  const endTimeUs = safeInteger(options.endTimeUs, 'hysteresis.endTimeUs');
  if (endTimeUs <= startTimeUs) fail('hysteresis.endTimeUs must be greater than startTimeUs');
  if (options.initiallyHeld !== undefined && typeof options.initiallyHeld !== 'boolean') {
    fail('hysteresis.initiallyHeld must be boolean');
  }
  const enter = validateCrossingSearchResult(options.enterCrossings, 'hysteresis.enterCrossings');
  const release = validateCrossingSearchResult(options.releaseCrossings, 'hysteresis.releaseCrossings');

  const events = [];
  let ignoredCrossings = 0;
  for (const crossing of enter.crossings) {
    if (crossing.direction === 'up') events.push({...crossing, kind: 'enter'});
    else ignoredCrossings += 1;
  }
  for (const crossing of release.crossings) {
    if (crossing.direction === 'down') events.push({...crossing, kind: 'release'});
    else ignoredCrossings += 1;
  }
  events.sort((left, right) => left.timeUs - right.timeUs
    || (left.kind === right.kind ? 0 : left.kind === 'release' ? -1 : 1)
    || left.bracketStartUs - right.bracketStartUs
    || left.bracketEndUs - right.bracketEndUs);

  let held = options.initiallyHeld ?? false;
  let heldFromUs = held ? Number.NEGATIVE_INFINITY : null;
  const intervals = [];
  let previousEventKey = '';
  for (const event of events) {
    if (event.timeUs > endTimeUs) break;
    const eventKey = `${event.kind}:${event.timeUs}`;
    if (eventKey === previousEventKey) continue;
    previousEventKey = eventKey;
    if (event.kind === 'release') {
      if (!held) continue;
      const intervalStartUs = Math.max(startTimeUs, heldFromUs);
      const intervalEndUs = Math.min(endTimeUs, event.timeUs);
      if (intervalEndUs > intervalStartUs) intervals.push({startTimeUs: intervalStartUs, endTimeUs: intervalEndUs});
      held = false;
      heldFromUs = null;
    } else if (!held) {
      held = true;
      heldFromUs = event.timeUs;
    }
  }
  if (held) {
    const intervalStartUs = Math.max(startTimeUs, heldFromUs);
    if (endTimeUs > intervalStartUs) intervals.push({startTimeUs: intervalStartUs, endTimeUs});
  }

  const ambiguity = [
    ...enter.ambiguous.map((range) => ({...range, threshold: 'enter'})),
    ...release.ambiguous.map((range) => ({...range, threshold: 'release'}))
  ].sort((left, right) => left.startTimeUs - right.startTimeUs
    || left.endTimeUs - right.endTimeUs
    || left.threshold.localeCompare(right.threshold)
    || left.reason.localeCompare(right.reason));

  return Object.freeze({
    intervals: Object.freeze(intervals.map((interval) => Object.freeze(interval))),
    ambiguous: Object.freeze(ambiguity.map((range) => Object.freeze(range))),
    exact: ambiguity.length === 0,
    finalHeld: held,
    ignoredCrossings
  });
}
