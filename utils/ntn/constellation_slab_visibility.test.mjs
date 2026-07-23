import assert from 'node:assert/strict';
import test from 'node:test';

import {createWalkerDelta, resolveOrbitModel} from './constellation_audit_core.mjs';
import {createCatalogGeometry} from './constellation_epoch.mjs';
import {
  createSlabVisibilityContext,
  createSlabVisibilityState,
  mergeSlabVisibilityRecords,
  processNextVisibilitySlab,
  restoreSlabVisibilityState,
  runSlabVisibilityAudit
} from './constellation_slab_visibility.mjs';
import {auditPairVisibility} from './constellation_visibility.mjs';

const HOUR_US = 3_600_000_000;

function tinyInputs() {
  const satellites = createWalkerDelta({
    planes: 1,
    satellitesPerPlane: 2,
    phaseFactor: 0,
    altitudeKm: 500,
    inclinationDeg: 25,
    raanOffsetDeg: 0,
    phaseOffsetDeg: 0
  });
  const cells = [
    {id: 'G000001', lat: 0, lon: 0, childMask: 127},
    {id: 'G000002', lat: 8, lon: 18, childMask: 127},
    {id: 'G000003', lat: -12, lon: 70, childMask: 127},
    {id: 'G000004', lat: 30, lon: -110, childMask: 127},
    {id: 'G000005', lat: -45, lon: 145, childMask: 127}
  ];
  return {satellites, geometry: createCatalogGeometry(cells, {latitudeBucketDeg: 2})};
}

function makeContext(overrides = {}) {
  const {satellites, geometry} = tinyInputs();
  return createSlabVisibilityContext({
    satellites,
    catalogGeometry: geometry,
    orbitModel: resolveOrbitModel({altitudeKm: 500}),
    boundaryPaddingUs: 120_000_000,
    toleranceUs: 1_000,
    scanStepUs: 60_000_000,
    ...overrides
  });
}

function makeState(context, overrides = {}) {
  return createSlabVisibilityState({
    context,
    auditStartTimeUs: 0,
    auditEndTimeUs: 2 * HOUR_US,
    slabDurationUs: 10 * 60_000_000,
    ...overrides
  });
}

function byPair(items) {
  const grouped = new Map();
  for (const item of items) {
    const key = `${item.positionId}/${item.satelliteId}`;
    const values = grouped.get(key) ?? [];
    values.push({startTimeUs: item.startTimeUs, endTimeUs: item.endTimeUs});
    grouped.set(key, values);
  }
  return grouped;
}

function assertIntervalsNear(actual, expected, toleranceUs) {
  assert.equal(actual.size, expected.size);
  for (const [key, expectedIntervals] of expected) {
    const actualIntervals = actual.get(key);
    assert.ok(actualIntervals, `missing intervals for ${key}`);
    assert.equal(actualIntervals.length, expectedIntervals.length, `interval count for ${key}`);
    for (let index = 0; index < expectedIntervals.length; index += 1) {
      assert.ok(
        Math.abs(actualIntervals[index].startTimeUs - expectedIntervals[index].startTimeUs) <= toleranceUs,
        `start time for ${key}[${index}]`
      );
      assert.ok(
        Math.abs(actualIntervals[index].endTimeUs - expectedIntervals[index].endTimeUs) <= toleranceUs,
        `end time for ${key}[${index}]`
      );
    }
  }
}

function bruteForce(context, startTimeUs, endTimeUs) {
  const entryIntervals = [];
  const releaseIntervals = [];
  for (const compiled of context.satellites) {
    for (const position of context.catalogGeometry.positions) {
      const result = auditPairVisibility({
        satellite: compiled.definition,
        observerUnit: position.observerUnit,
        startTimeUs,
        endTimeUs,
        boundaryPaddingUs: context.boundaryPaddingUs,
        orbitModel: context.orbitModel,
        entryElevationDeg: context.entryElevationDeg,
        releaseElevationDeg: context.releaseElevationDeg,
        toleranceUs: context.toleranceUs,
        scanStepUs: context.scanStepUs,
        marginTolerance: context.marginTolerance,
        maxEvaluations: context.maxEvaluations
      });
      for (const interval of result.entryIntervals) {
        entryIntervals.push({positionId: position.positionId, satelliteId: compiled.definition.id, ...interval});
      }
      for (const interval of result.releaseIntervals) {
        releaseIntervals.push({positionId: position.positionId, satelliteId: compiled.definition.id, ...interval});
      }
    }
  }
  return {entryIntervals, releaseIntervals};
}

test('slab engine is equivalent to brute-force pair visibility on a tiny scenario', () => {
  const context = makeContext();
  const state = makeState(context);
  const result = runSlabVisibilityAudit({context, state});
  const brute = bruteForce(context, state.auditStartTimeUs, state.auditEndTimeUs);

  assert.equal(result.complete, true);
  assert.equal(result.ambiguous.length, 0);
  assertIntervalsNear(byPair(result.entryIntervals), byPair(brute.entryIntervals), context.toleranceUs);
  assertIntervalsNear(byPair(result.releaseIntervals), byPair(brute.releaseIntervals), context.toleranceUs);
  for (let index = 1; index < result.events.length; index += 1) {
    const previous = result.events[index - 1];
    const current = result.events[index];
    assert.ok(previous.timeUs <= current.timeUs);
    assert.notDeepEqual(
      [previous.timeUs, previous.kind, previous.positionId, previous.satelliteId],
      [current.timeUs, current.kind, current.positionId, current.satelliteId],
      'half-open slabs must not duplicate a boundary event'
    );
  }
});

test('JSON checkpoint resume produces byte-for-byte identical records and metrics', () => {
  const context = makeContext();
  const initial = makeState(context, {slabDurationUs: 60_000_000});
  const uninterrupted = runSlabVisibilityAudit({context, state: initial});
  const firstPart = runSlabVisibilityAudit({context, state: initial, maxSlabs: 1});
  assert.equal(firstPart.complete, false);
  assert.ok(firstPart.state.openPairs.length > 0);
  const restored = restoreSlabVisibilityState(context, JSON.parse(JSON.stringify(firstPart.state)));
  const secondPart = runSlabVisibilityAudit({context, state: restored});
  const combined = mergeSlabVisibilityRecords([firstPart, secondPart]);

  assert.deepEqual(combined.initialStates, uninterrupted.initialStates);
  assert.deepEqual(combined.events, uninterrupted.events);
  assert.deepEqual(combined.entryIntervals, uninterrupted.entryIntervals);
  assert.deepEqual(combined.releaseIntervals, uninterrupted.releaseIntervals);
  assert.deepEqual(combined.ambiguous, uninterrupted.ambiguous);
  assert.deepEqual(secondPart.metrics, uninterrupted.metrics);
  assert.throws(
    () => restoreSlabVisibilityState(makeContext({entryElevationDeg: 46}), JSON.parse(JSON.stringify(firstPart.state))),
    /context hash mismatch/
  );
  const corrupt = JSON.parse(JSON.stringify(firstPart.state));
  corrupt.openPairs[0].releaseStartTimeUs = corrupt.openPairs[0].entryStartTimeUs + 1;
  assert.throws(() => restoreSlabVisibilityState(context, corrupt), /entry before release/);
});

test('satellite shards merge to the same deterministic evidence as one shard', () => {
  const fullContext = makeContext();
  const full = runSlabVisibilityAudit({context: fullContext, state: makeState(fullContext)});
  const leftContext = makeContext({satelliteShard: {index: 0, count: 2}});
  const rightContext = makeContext({satelliteShard: {index: 1, count: 2}});
  const left = runSlabVisibilityAudit({context: leftContext, state: makeState(leftContext)});
  const right = runSlabVisibilityAudit({context: rightContext, state: makeState(rightContext)});
  const merged = mergeSlabVisibilityRecords([right, left]);

  assert.deepEqual(merged.initialStates, full.initialStates);
  assert.deepEqual(merged.events, full.events);
  assert.deepEqual(merged.entryIntervals, full.entryIntervals);
  assert.deepEqual(merged.releaseIntervals, full.releaseIntervals);
  assert.deepEqual(merged.ambiguous, full.ambiguous);
});

test('a crossing inside the left padding cannot corrupt the next slab initial state', () => {
  const satellites = createWalkerDelta({
    planes: 2,
    satellitesPerPlane: 2,
    phaseFactor: 1,
    altitudeKm: 500,
    inclinationDeg: 60,
    raanOffsetDeg: 16.435087313875556,
    phaseOffsetDeg: 331.78478700108826
  }).filter(({id}) => id === 'P01-S02');
  const geometry = createCatalogGeometry([{
    id: 'G000001',
    lat: -49.572140914388,
    lon: -135.61191405169666,
    childMask: 127
  }]);
  const context = createSlabVisibilityContext({
    satellites,
    catalogGeometry: geometry,
    boundaryPaddingUs: 60_000_000,
    toleranceUs: 10_000,
    scanStepUs: 60_000_000,
    screeningStepUs: 30_000_000
  });
  const state = createSlabVisibilityState({
    context,
    auditStartTimeUs: 0,
    auditEndTimeUs: 1_800_000_000,
    slabDurationUs: 120_000_000
  });
  const result = runSlabVisibilityAudit({context, state});
  const brute = bruteForce(context, state.auditStartTimeUs, state.auditEndTimeUs);

  assert.deepEqual(result.entryIntervals, brute.entryIntervals);
  assert.deepEqual(result.releaseIntervals, brute.releaseIntervals);
  assert.equal(result.metrics.pairAudits, 2, 'only the two slabs containing crossings need pair root searches');
});

test('first slab uses canonical padded-root state at a crossing one microsecond before the boundary', () => {
  const satellites = [{
    id: 'P13-S29',
    plane: 13,
    slot: 29,
    raanDeg: 102.85714285714289,
    phaseDeg: 121.22448979591837,
    altitudeKm: 500,
    inclinationDeg: 60
  }];
  const geometry = createCatalogGeometry([{
    id: 'G000612',
    lat: 55.36329,
    lon: -126.33245,
    childMask: 127
  }]);
  const context = createSlabVisibilityContext({
    satellites,
    catalogGeometry: geometry,
    boundaryPaddingUs: 120_000_000,
    toleranceUs: 1_000,
    scanStepUs: 30_000_000
  });
  const initial = createSlabVisibilityState({
    context,
    auditStartTimeUs: -6_000_000_000,
    auditEndTimeUs: -5_760_000_000,
    slabDurationUs: 120_000_000
  });
  const first = processNextVisibilitySlab({context, state: initial});
  assert.deepEqual(first.initialStates, [{
    positionId: 'G000612',
    satelliteId: 'P13-S29',
    aboveEntry: true,
    aboveRelease: true
  }]);
  assert.equal(first.ambiguous.length, 0);
  assert.equal(first.metrics.exact, true);
  assert.equal(first.state.openPairs.length, 0);
  const restored = restoreSlabVisibilityState(context, JSON.parse(JSON.stringify(first.state)));
  assert.doesNotThrow(() => processNextVisibilitySlab({context, state: restored}));
});

test('first slab preserves nested thresholds when a release entry is ambiguous only at the padding edge', () => {
  const context = createSlabVisibilityContext({
    satellites: [{
      id: 'P25-S22',
      plane: 25,
      slot: 22,
      raanDeg: 205.71428571428578,
      phaseDeg: 92.44897959183675,
      altitudeKm: 500,
      inclinationDeg: 60
    }],
    catalogGeometry: createCatalogGeometry([{
      id: 'G001776',
      lat: 53.02613,
      lon: -75.86035,
      childMask: 127
    }]),
    boundaryPaddingUs: 120_000_000,
    toleranceUs: 1_000,
    scanStepUs: 30_000_000
  });
  const initial = createSlabVisibilityState({
    context,
    auditStartTimeUs: -6_000_000_000,
    auditEndTimeUs: -5_760_000_000,
    slabDurationUs: 120_000_000
  });
  const first = processNextVisibilitySlab({context, state: initial});
  assert.deepEqual(first.initialStates, [{
    positionId: 'G001776',
    satelliteId: 'P25-S22',
    aboveEntry: true,
    aboveRelease: true
  }]);
  assert.equal(first.ambiguous.length, 0);
  const restored = restoreSlabVisibilityState(context, JSON.parse(JSON.stringify(first.state)));
  assert.doesNotThrow(() => processNextVisibilitySlab({context, state: restored}));
});

test('a sub-microsecond release crossing at a slab boundary is owned exactly once', () => {
  const satellites = createWalkerDelta({
    planes: 42,
    satellitesPerPlane: 84,
    phaseFactor: 1,
    altitudeKm: 500,
    inclinationDeg: 60,
    raanOffsetDeg: 0,
    phaseOffsetDeg: 0
  }).filter(({id}) => id === 'P30-S01');
  const context = createSlabVisibilityContext({
    satellites,
    catalogGeometry: createCatalogGeometry([{
      id: 'G013976',
      lat: 29.19678,
      lon: 2.16495,
      childMask: 127
    }]),
    orbitModel: resolveOrbitModel({altitudeKm: 500}),
    boundaryPaddingUs: 120_000_000,
    toleranceUs: 1_000,
    scanStepUs: 30_000_000,
    screeningStepUs: 30_000_000,
    marginTolerance: 1e-12
  });
  const initial = createSlabVisibilityState({
    context,
    auditStartTimeUs: 409_200_000_000,
    auditEndTimeUs: 409_440_000_000,
    slabDurationUs: 120_000_000
  });

  const left = processNextVisibilitySlab({context, state: initial});
  assert.equal(left.ambiguous.length, 0);
  assert.equal(left.state.openPairs.length, 1);

  const right = processNextVisibilitySlab({context, state: left.state});
  assert.equal(right.complete, true);
  assert.equal(right.metrics.exact, true);
  assert.equal(right.ambiguous.length, 0);
  assert.equal(right.state.openPairs.length, 0);
  assert.deepEqual(
    right.events.filter(({kind}) => kind === 'release_exit').map(({timeUs}) => timeUs),
    [409_320_000_000]
  );
});

test('swept-cap screening benchmark skips remote pairs without losing open state', () => {
  const satellites = createWalkerDelta({
    planes: 1,
    satellitesPerPlane: 1,
    phaseFactor: 0,
    altitudeKm: 500,
    inclinationDeg: 0,
    raanOffsetDeg: 0,
    phaseOffsetDeg: 0
  });
  const cells = Array.from({length: 720}, (_, index) => ({
    id: `G${String(index + 1).padStart(6, '0')}`,
    lat: -80 + (index % 17) * 10,
    lon: -180 + (index % 72) * 5,
    childMask: 127
  }));
  const context = createSlabVisibilityContext({
    satellites,
    catalogGeometry: createCatalogGeometry(cells, {latitudeBucketDeg: 1}),
    boundaryPaddingUs: 10_000_000,
    toleranceUs: 10_000,
    scanStepUs: 30_000_000
  });
  const state = createSlabVisibilityState({
    context,
    auditStartTimeUs: 0,
    auditEndTimeUs: 60_000_000,
    slabDurationUs: 60_000_000
  });
  const result = processNextVisibilitySlab({context, state});

  assert.equal(result.complete, true);
  assert.equal(result.metrics.potentialPairs, 720);
  assert.ok(result.metrics.latitudeBucketCandidates < result.metrics.potentialPairs / 2);
  assert.ok(result.metrics.sweptCapCandidates <= result.metrics.latitudeBucketCandidates);
  assert.ok(result.metrics.pairAudits < result.metrics.potentialPairs / 20);
  assert.equal(result.state.openPairs.length, 0);
});

test('invalid shard, timing and completed-state reuse fail closed', () => {
  assert.throws(() => makeContext({satelliteShard: {index: 2, count: 2}}), /index/);
  const context = makeContext();
  assert.throws(
    () => createSlabVisibilityState({context, auditStartTimeUs: 1, auditEndTimeUs: 1, slabDurationUs: 1}),
    /greater/
  );
  assert.throws(
    () => createSlabVisibilityState({
      context,
      auditStartTimeUs: 0,
      auditEndTimeUs: 100_000_000,
      slabDurationUs: 90_000_000
    }),
    /exact multiple/
  );
  const completed = runSlabVisibilityAudit({context, state: makeState(context)}).state;
  assert.throws(() => processNextVisibilitySlab({context, state: completed}), /already complete/);
});
