import assert from 'node:assert/strict';
import test from 'node:test';

import {createWalkerDelta, groundUnit, resolveOrbitModel} from './constellation_audit_core.mjs';
import {auditPairVisibility, buildAboveThresholdIntervals} from './constellation_visibility.mjs';

test('raw threshold intervals are half-open and cropped from padded search context', () => {
  const intervals = buildAboveThresholdIntervals({
    startTimeUs: 10,
    endTimeUs: 90,
    searchStartTimeUs: 0,
    searchEndTimeUs: 100,
    initiallyAbove: false,
    crossings: [
      {timeUs: 5, direction: 'up'},
      {timeUs: 40, direction: 'down'},
      {timeUs: 70, direction: 'up'},
      {timeUs: 95, direction: 'down'}
    ]
  });
  assert.deepEqual(intervals, [{startTimeUs: 10, endTimeUs: 40}, {startTimeUs: 70, endTimeUs: 90}]);
});

test('orbit pair emits separate entry and release intervals with exit-before-entry ordering', () => {
  const satellite = createWalkerDelta({
    planes: 1, satellitesPerPlane: 1, phaseFactor: 0,
    altitudeKm: 500, inclinationDeg: 0, raanOffsetDeg: 0, phaseOffsetDeg: 0
  })[0];
  const model = resolveOrbitModel({altitudeKm: 500});
  const result = auditPairVisibility({
    satellite,
    observerUnit: groundUnit({lat: 0, lon: 0}),
    startTimeUs: 0,
    endTimeUs: 8_000_000_000,
    boundaryPaddingUs: 120_000_000,
    orbitModel: model,
    toleranceUs: 10_000,
    scanStepUs: 60_000_000
  });
  assert.ok(result.entryIntervals.length > 0);
  assert.ok(result.releaseIntervals.length >= result.entryIntervals.length);
  for (const interval of result.entryIntervals) {
    assert.ok(result.releaseIntervals.some((release) =>
      release.startTimeUs <= interval.startTimeUs && release.endTimeUs >= interval.endTimeUs));
  }
  for (let index = 1; index < result.events.length; index += 1) {
    assert.ok(result.events[index - 1].timeUs <= result.events[index].timeUs);
  }
});

test('padded-boundary ambiguity cannot invert 45-degree and 42-degree initial visibility', () => {
  const satellite = {
    id: 'P25-S22',
    plane: 25,
    slot: 22,
    raanDeg: 205.71428571428578,
    phaseDeg: 92.44897959183675,
    altitudeKm: 500,
    inclinationDeg: 60
  };
  const startTimeUs = -6_000_000_000;
  const result = auditPairVisibility({
    satellite,
    observerUnit: groundUnit({lat: 53.02613, lon: -75.86035}),
    startTimeUs,
    endTimeUs: startTimeUs + 120_000_000,
    boundaryPaddingUs: 120_000_000,
    orbitModel: resolveOrbitModel({altitudeKm: 500}),
    entryElevationDeg: 45,
    releaseElevationDeg: 42,
    toleranceUs: 1_000,
    scanStepUs: 30_000_000,
    marginTolerance: 1e-12,
    maxEvaluations: 1_000_000
  });

  assert.equal(result.exact, true);
  assert.equal(result.initialEntry, true);
  assert.equal(result.initialRelease, true);
  assert.equal(result.ambiguous.length, 0);
  assert.equal(result.entryIntervals[0].startTimeUs, startTimeUs);
  assert.equal(result.releaseIntervals[0].startTimeUs, startTimeUs);
  assert.ok(result.releaseIntervals[0].endTimeUs > result.entryIntervals[0].endTimeUs);
});

test('sub-second grazing pass is resolved as two crossings while a nearby miss stays below', () => {
  const model = resolveOrbitModel({altitudeKm: 500});
  const satellites = createWalkerDelta({
    planes: 42,
    satellitesPerPlane: 84,
    phaseFactor: 1,
    altitudeKm: 500,
    inclinationDeg: 60,
    raanOffsetDeg: 0,
    phaseOffsetDeg: 0
  });
  const audit = (satelliteId, observer, threshold) => auditPairVisibility({
    satellite: satellites.find(({id}) => id === satelliteId),
    observerUnit: groundUnit(observer),
    startTimeUs: -5_880_000_000,
    endTimeUs: -5_760_000_000,
    boundaryPaddingUs: 120_000_000,
    orbitModel: model,
    entryElevationDeg: threshold === 'entry' ? 45 : 46,
    releaseElevationDeg: threshold === 'entry' ? 42 : 42,
    toleranceUs: 1_000,
    scanStepUs: 30_000_000,
    marginTolerance: 1e-12,
    maxEvaluations: 1_000_000
  });

  const grazing = audit('P35-S78', {lat: -28.75233, lon: -68.92308}, 'release');
  assert.equal(grazing.exact, true);
  assert.equal(grazing.ambiguous.length, 0);
  assert.equal(grazing.releaseIntervals.length, 1);
  assert.ok(grazing.releaseIntervals[0].endTimeUs - grazing.releaseIntervals[0].startTimeUs < 1_000_000);
  assert.deepEqual(grazing.events.filter(({kind}) => kind.startsWith('release_')).map(({kind}) => kind), [
    'release_enter', 'release_exit'
  ]);

  const nearMiss = audit('P09-S36', {lat: 30.5986, lon: -112.89199}, 'entry');
  assert.equal(nearMiss.exact, true);
  assert.equal(nearMiss.ambiguous.length, 0);
  assert.equal(nearMiss.entryIntervals.length, 0);
  assert.ok(grazing.evaluations < 20_000);
  assert.ok(nearMiss.evaluations < 5_000);
});

test('invalid threshold and insufficiently typed time inputs fail closed', () => {
  const satellite = createWalkerDelta({
    planes: 1, satellitesPerPlane: 1, phaseFactor: 0,
    altitudeKm: 500, inclinationDeg: 0
  })[0];
  assert.throws(() => auditPairVisibility({
    satellite,
    observerUnit: [1, 0, 0],
    startTimeUs: 0.5,
    endTimeUs: 10,
    boundaryPaddingUs: 1
  }), /integer microseconds/);
  assert.throws(() => auditPairVisibility({
    satellite,
    observerUnit: [1, 0, 0],
    startTimeUs: 0,
    endTimeUs: 10,
    boundaryPaddingUs: 1,
    entryElevationDeg: 42,
    releaseElevationDeg: 45
  }), /releaseElevationDeg/);
});
