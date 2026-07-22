import assert from 'node:assert/strict';
import test from 'node:test';

import {
  buildHysteresisIntervals,
  centralAngleForElevation,
  createWalkerDelta,
  findThresholdCrossings,
  groundUnit,
  resolveOrbitModel,
  satelliteUnitEcef,
  visibilityMargin
} from './constellation_audit_core.mjs';

function search(marginAtTimeUs, overrides = {}) {
  return findThresholdCrossings({
    marginAtTimeUs,
    startTimeUs: 0,
    endTimeUs: 4_000_000,
    angularRateBoundRadPerSecond: 1,
    scanStepUs: 4_000_000,
    toleranceUs: 1_000,
    ...overrides
  });
}

function result(crossings, ambiguous = []) {
  return {crossings, ambiguous, toleranceUs: 1_000, evaluations: 0};
}

test('strict Walker scenario parsing is deterministic and keeps F explicit', () => {
  const scenario = {
    planes: 3,
    satellitesPerPlane: 4,
    phaseFactor: 2,
    altitudeKm: 600,
    inclinationDeg: 70,
    raanOffsetDeg: 7,
    phaseOffsetDeg: 11
  };
  const first = createWalkerDelta(scenario);
  const second = createWalkerDelta({...scenario});
  assert.deepEqual(first, second);
  assert.equal(first.length, 12);
  assert.deepEqual(first[0], {
    id: 'P01-S01', plane: 1, slot: 1, raanDeg: 7, phaseDeg: 11, altitudeKm: 600, inclinationDeg: 70
  });
  assert.equal(first[4].raanDeg, 127);
  assert.equal(first[4].phaseDeg, 71);
  assert.throws(() => createWalkerDelta({...scenario, phaseFactor: 3}), /phaseFactor/);
  assert.throws(() => createWalkerDelta({...scenario, unexpected: true}), /unknown field/);
  assert.throws(() => createWalkerDelta({planes: 3, satellitesPerPlane: 4}), /phaseFactor/);
});

test('circular orbit helpers match the existing spherical-Earth geometry', () => {
  const model = resolveOrbitModel({altitudeKm: 500});
  assert.ok(Math.abs(model.orbitPeriodSeconds / 60 - 94.6) < 0.1);
  assert.ok(model.conservativeAngularRateRadPerSecond > model.meanMotionRadPerSecond);
  const satellite = createWalkerDelta({
    planes: 1, satellitesPerPlane: 1, phaseFactor: 0,
    altitudeKm: 500, inclinationDeg: 0, raanOffsetDeg: 0, phaseOffsetDeg: 0
  })[0];
  assert.deepEqual(satelliteUnitEcef(satellite, 0, model), [1, 0, 0]);
  const observer = groundUnit({id: 'G000001', lat: 0, lon: 0});
  assert.deepEqual(observer, [1, 0, 0]);
  const enterAngle = centralAngleForElevation(45, model);
  const releaseAngle = centralAngleForElevation(42, model);
  assert.ok(releaseAngle > enterAngle);
  assert.ok(visibilityMargin([1, 0, 0], observer, enterAngle) > 0);
  assert.throws(() => groundUnit({lat: 91, lon: 0}), /groundPoint.lat/);
  assert.throws(() => resolveOrbitModel({altitudeKm: 500, unexpected: 1}), /unknown field/);
});

test('dot-margin root search finds conservative upward and downward crossings within one millisecond', () => {
  const upward = search((timeUs) => (timeUs - 2_000_000) / 10_000_000, {
    angularRateBoundRadPerSecond: 0.1
  });
  assert.deepEqual(upward.ambiguous, []);
  assert.equal(upward.crossings.length, 1);
  assert.equal(upward.crossings[0].direction, 'up');
  assert.ok(upward.crossings[0].bracketEndUs - upward.crossings[0].bracketStartUs <= 1_000);
  assert.equal(upward.crossings[0].timeUs, 2_000_000);

  const downward = search((timeUs) => (2_000_000 - timeUs) / 10_000_000, {
    angularRateBoundRadPerSecond: 0.1
  });
  assert.deepEqual(downward.ambiguous, []);
  assert.equal(downward.crossings.length, 1);
  assert.equal(downward.crossings[0].direction, 'down');
  assert.equal(downward.crossings[0].timeUs, 2_000_000);
});

test('a threshold touch and a root at the search boundary remain ambiguous', () => {
  const tangent = search((timeUs) => -Math.abs((timeUs - 2_000_000) / 1_000_000));
  assert.deepEqual(tangent.crossings, []);
  assert.ok(tangent.ambiguous.some(({reason}) => reason === 'tangent_or_unresolved'));

  const boundary = search((timeUs) => timeUs / 1_000_000, {
    angularRateBoundRadPerSecond: 1
  });
  assert.deepEqual(boundary.crossings, []);
  assert.ok(boundary.ambiguous.some(({reason}) => reason === 'range_boundary'));
});

test('45-up entry and 42-down release produce half-open intervals at exact boundaries', () => {
  const enter = result([
    {timeUs: 1_000_000, bracketStartUs: 1_000_000, bracketEndUs: 1_000_000, direction: 'up'},
    {timeUs: 4_000_000, bracketStartUs: 4_000_000, bracketEndUs: 4_000_000, direction: 'down'}
  ]);
  const release = result([
    {timeUs: 500_000, bracketStartUs: 500_000, bracketEndUs: 500_000, direction: 'up'},
    {timeUs: 5_000_000, bracketStartUs: 5_000_000, bracketEndUs: 5_000_000, direction: 'down'}
  ]);
  const built = buildHysteresisIntervals({
    startTimeUs: 1_000_000,
    endTimeUs: 5_000_000,
    enterCrossings: enter,
    releaseCrossings: release
  });
  assert.deepEqual(built.intervals, [{startTimeUs: 1_000_000, endTimeUs: 5_000_000}]);
  assert.equal(built.ignoredCrossings, 2);
  assert.equal(built.exact, true);
  assert.equal(built.finalHeld, false);

  const initiallyHeld = buildHysteresisIntervals({
    startTimeUs: 1_000_000,
    endTimeUs: 5_000_000,
    enterCrossings: result([]),
    releaseCrossings: result([
      {timeUs: 5_000_000, bracketStartUs: 5_000_000, bracketEndUs: 5_000_000, direction: 'down'}
    ]),
    initiallyHeld: true
  });
  assert.deepEqual(initiallyHeld.intervals, [{startTimeUs: 1_000_000, endTimeUs: 5_000_000}]);
});

test('hysteresis output is deterministic for unordered and duplicated crossing input', () => {
  const entryItems = [
    {timeUs: 7_000_000, bracketStartUs: 7_000_000, bracketEndUs: 7_000_000, direction: 'up'},
    {timeUs: 1_000_000, bracketStartUs: 1_000_000, bracketEndUs: 1_000_000, direction: 'up'},
    {timeUs: 1_000_000, bracketStartUs: 1_000_000, bracketEndUs: 1_000_000, direction: 'up'}
  ];
  const releaseItems = [
    {timeUs: 9_000_000, bracketStartUs: 9_000_000, bracketEndUs: 9_000_000, direction: 'down'},
    {timeUs: 3_000_000, bracketStartUs: 3_000_000, bracketEndUs: 3_000_000, direction: 'down'}
  ];
  const build = (entries, releases) => buildHysteresisIntervals({
    startTimeUs: 0,
    endTimeUs: 10_000_000,
    enterCrossings: result(entries),
    releaseCrossings: result(releases)
  });
  const forward = build(entryItems, releaseItems);
  const reverse = build([...entryItems].reverse(), [...releaseItems].reverse());
  assert.deepEqual(forward, reverse);
  assert.deepEqual(forward.intervals, [
    {startTimeUs: 1_000_000, endTimeUs: 3_000_000},
    {startTimeUs: 7_000_000, endTimeUs: 9_000_000}
  ]);
});

test('invalid timing and non-finite margins fail closed', () => {
  assert.throws(() => search(() => 1, {toleranceUs: 10_001}), /toleranceUs/);
  assert.throws(() => search(() => Number.NaN), /non-finite/);
  assert.throws(() => findThresholdCrossings({
    marginAtTimeUs: () => 1,
    startTimeUs: 0.5,
    endTimeUs: 10,
    angularRateBoundRadPerSecond: 1
  }), /safe integer/);
});
