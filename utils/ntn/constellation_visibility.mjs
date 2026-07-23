import {
  centralAngleForElevation,
  createSatellitePropagator,
  findThresholdCrossings,
  resolveOrbitModel
} from './constellation_audit_core.mjs';

// marginTolerance controls root isolation, but it is too wide to decide which
// half-open slab owns a crossing that lands within one tolerance bracket of a
// slab boundary. At the boundary itself, only values indistinguishable from
// floating-point roundoff remain unresolved; every other sign can assign the
// crossing deterministically to the left or right slab.
const BOUNDARY_ROUNDOFF_MARGIN = 64 * Number.EPSILON;

function fail(message) {
  throw new Error(message);
}

function compareEvent(left, right) {
  const rank = {release_exit: 0, entry_exit: 1, release_enter: 2, entry_enter: 3};
  return left.timeUs - right.timeUs || rank[left.kind] - rank[right.kind];
}

function assertTime(value, context) {
  if (!Number.isSafeInteger(value)) fail(`${context} must be integer microseconds`);
}

function intersectingAmbiguity(items, startTimeUs, endTimeUs) {
  return items.filter((item) => item.endTimeUs > startTimeUs && item.startTimeUs < endTimeUs);
}

/** Builds raw half-open intervals for one threshold from a complete crossing search. */
export function buildAboveThresholdIntervals({
  startTimeUs,
  endTimeUs,
  searchStartTimeUs = startTimeUs,
  searchEndTimeUs = endTimeUs,
  crossings,
  initiallyAbove
}) {
  for (const [value, context] of [
    [startTimeUs, 'startTimeUs'], [endTimeUs, 'endTimeUs'],
    [searchStartTimeUs, 'searchStartTimeUs'], [searchEndTimeUs, 'searchEndTimeUs']
  ]) assertTime(value, context);
  if (searchStartTimeUs > startTimeUs || searchEndTimeUs < endTimeUs || endTimeUs <= startTimeUs) {
    fail('search range must cover a non-empty audit range');
  }
  if (!Array.isArray(crossings) || typeof initiallyAbove !== 'boolean') fail('crossings/initiallyAbove are invalid');
  const ordered = [...crossings].sort((left, right) => left.timeUs - right.timeUs
    || (left.direction === right.direction ? 0 : left.direction === 'down' ? -1 : 1));
  let above = initiallyAbove;
  let openedAtUs = above ? searchStartTimeUs : null;
  const raw = [];
  for (const crossing of ordered) {
    assertTime(crossing.timeUs, 'crossing.timeUs');
    if (crossing.timeUs < searchStartTimeUs || crossing.timeUs > searchEndTimeUs) {
      fail('crossing lies outside search range');
    }
    if (crossing.direction === 'up') {
      if (!above) {
        above = true;
        openedAtUs = crossing.timeUs;
      }
    } else if (crossing.direction === 'down') {
      if (above) {
        if (crossing.timeUs > openedAtUs) raw.push({startTimeUs: openedAtUs, endTimeUs: crossing.timeUs});
        above = false;
        openedAtUs = null;
      }
    } else fail('crossing.direction must be up or down');
  }
  if (above && searchEndTimeUs > openedAtUs) raw.push({startTimeUs: openedAtUs, endTimeUs: searchEndTimeUs});
  return raw.map((interval) => ({
    startTimeUs: Math.max(startTimeUs, interval.startTimeUs),
    endTimeUs: Math.min(endTimeUs, interval.endTimeUs)
  })).filter((interval) => interval.endTimeUs > interval.startTimeUs);
}

/**
 * Audits one satellite/position pair and returns separate >=45 and >=42
 * intervals. Padding supplies both sides of the audit range so a root exactly
 * on a shard boundary is resolved once and then cropped deterministically.
 */
export function auditPairVisibility({
  satellite,
  observerUnit,
  startTimeUs,
  endTimeUs,
  boundaryPaddingUs,
  orbitModel = undefined,
  entryElevationDeg = 45,
  releaseElevationDeg = 42,
  toleranceUs = 1_000,
  scanStepUs = 60_000_000,
  marginTolerance = 1e-12,
  maxEvaluations = 1_000_000
}) {
  assertTime(startTimeUs, 'startTimeUs');
  assertTime(endTimeUs, 'endTimeUs');
  assertTime(boundaryPaddingUs, 'boundaryPaddingUs');
  if (endTimeUs <= startTimeUs || boundaryPaddingUs <= 0) fail('audit range and boundary padding are invalid');
  if (!Array.isArray(observerUnit) || observerUnit.length !== 3) fail('observerUnit must have three components');
  if (!(releaseElevationDeg < entryElevationDeg)) fail('releaseElevationDeg must be below entryElevationDeg');
  const model = orbitModel ?? resolveOrbitModel({altitudeKm: satellite.altitudeKm});
  const propagate = createSatellitePropagator(satellite, model);
  const searchStartTimeUs = startTimeUs - boundaryPaddingUs;
  const searchEndTimeUs = endTimeUs + boundaryPaddingUs;
  assertTime(searchStartTimeUs, 'searchStartTimeUs');
  assertTime(searchEndTimeUs, 'searchEndTimeUs');

  const searchThreshold = (elevationDeg) => {
    const centralAngleRad = centralAngleForElevation(elevationDeg, model);
    const minimumDot = Math.cos(centralAngleRad);
    const marginAtTimeUs = (timeUs) => {
      const unit = propagate(timeUs);
      return unit[0] * observerUnit[0] + unit[1] * observerUnit[1] + unit[2] * observerUnit[2] - minimumDot;
    };
    const search = findThresholdCrossings({
      marginAtTimeUs,
      startTimeUs: searchStartTimeUs,
      endTimeUs: searchEndTimeUs,
      angularRateBoundRadPerSecond: model.conservativeAngularRateRadPerSecond,
      centralAngleRad,
      toleranceUs,
      scanStepUs,
      marginTolerance,
      maxEvaluations
    });
    const boundaryAmbiguous = [];
    let startBoundaryUnresolved = false;
    const centralCrossings = [];
    for (const crossing of search.crossings) {
      let crossingTimeUs = crossing.timeUs;
      let unresolved = false;
      for (const [boundary, name] of [[startTimeUs, 'start'], [endTimeUs, 'end']]) {
        if (crossing.bracketStartUs > boundary || crossing.bracketEndUs < boundary) continue;
        const boundaryMargin = marginAtTimeUs(boundary);
        if (Math.abs(boundaryMargin) <= BOUNDARY_ROUNDOFF_MARGIN) {
          const ambiguityStart = name === 'start'
            ? startTimeUs
            : Math.max(startTimeUs, endTimeUs - toleranceUs);
          const ambiguityEnd = name === 'start'
            ? Math.min(endTimeUs, startTimeUs + toleranceUs)
            : endTimeUs;
          boundaryAmbiguous.push({
            startTimeUs: ambiguityStart,
            endTimeUs: Math.max(ambiguityStart + 1, ambiguityEnd),
            reason: `root_bracket_crosses_audit_${name}`
          });
          startBoundaryUnresolved ||= name === 'start';
          unresolved = true;
          break;
        }
        const boundaryIsBeforeCrossing = crossing.direction === 'down'
          ? boundaryMargin > 0
          : boundaryMargin < 0;
        if (name === 'start') {
          crossingTimeUs = boundaryIsBeforeCrossing ? startTimeUs : startTimeUs - 1;
        } else {
          crossingTimeUs = boundaryIsBeforeCrossing ? endTimeUs : endTimeUs - 1;
        }
      }
      if (!unresolved && crossingTimeUs >= startTimeUs && crossingTimeUs < endTimeUs) {
        centralCrossings.push({...crossing, timeUs: crossingTimeUs});
      }
    }
    // Padding exists only to resolve roots close to the central boundaries. It
    // must not make the central state depend on an uncertain root at the outer
    // padding boundary. Anchor [start,end) directly at its actual start value
    // and replay only crossings that belong to that half-open interval.
    const initialAboveAtStart = marginAtTimeUs(startTimeUs) >= 0;
    // If the margin is non-zero, its sign and the crossing direction assign a
    // straddling root to exactly one adjacent half-open interval. Only a margin
    // inside the configured numerical tolerance remains ambiguous.
    const initialAbove = !startBoundaryUnresolved && initialAboveAtStart;
    const intervals = buildAboveThresholdIntervals({
      startTimeUs,
      endTimeUs,
      crossings: centralCrossings,
      initiallyAbove: initialAbove
    });
    return {
      intervals,
      crossings: centralCrossings,
      initialAbove,
      ambiguous: [
        ...intersectingAmbiguity(search.ambiguous, startTimeUs, endTimeUs),
        ...boundaryAmbiguous
      ],
      evaluations: search.evaluations
    };
  };

  const entry = searchThreshold(entryElevationDeg);
  const release = searchThreshold(releaseElevationDeg);
  const events = [
    ...entry.crossings.map((crossing) => ({
      timeUs: crossing.timeUs,
      kind: crossing.direction === 'up' ? 'entry_enter' : 'entry_exit'
    })),
    ...release.crossings.map((crossing) => ({
      timeUs: crossing.timeUs,
      kind: crossing.direction === 'up' ? 'release_enter' : 'release_exit'
    }))
  ].sort(compareEvent);
  const initialEntry = entry.initialAbove;
  const initialRelease = release.initialAbove;
  if (initialEntry && !initialRelease) {
    fail('visibility threshold nesting is inconsistent at the audit start');
  }
  if (entry.ambiguous.length === 0 && release.ambiguous.length === 0) {
    for (const interval of entry.intervals) {
      if (!release.intervals.some((candidate) =>
        candidate.startTimeUs <= interval.startTimeUs && candidate.endTimeUs >= interval.endTimeUs)) {
        fail('visibility threshold nesting is inconsistent inside the audit range');
      }
    }
  }
  return {
    entryIntervals: entry.intervals,
    releaseIntervals: release.intervals,
    events,
    initialEntry,
    initialRelease,
    ambiguous: [
      ...entry.ambiguous.map((item) => ({...item, threshold: 'entry'})),
      ...release.ambiguous.map((item) => ({...item, threshold: 'release'}))
    ],
    exact: entry.ambiguous.length === 0 && release.ambiguous.length === 0,
    evaluations: entry.evaluations + release.evaluations
  };
}
