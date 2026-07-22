import assert from 'node:assert/strict';
import test from 'node:test';

import {replayAssignmentTimeline} from './constellation_timeline.mjs';

const registry = {
  satellites: [
    {satellite_id: 'P01-S01', cells: [{bank: 0, nci: '0x000000001', pci: 1}, {bank: 1, nci: '0x000000002', pci: 2}]},
    {satellite_id: 'P01-S02', cells: [{bank: 0, nci: '0x000000003', pci: 3}, {bank: 1, nci: '0x000000004', pci: 4}]}
  ]
};

function run(overrides = {}) {
  return replayAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 30,
    positionIds: ['G000001'],
    initialVisibility: [{
      positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true, score: 50
    }],
    events: [],
    registry,
    satelliteCapacity: 2,
    cellCapacity: 1,
    ...overrides
  });
}

test('an incumbent stays between the entry and release thresholds', () => {
  const result = run({events: [
    {timeUs: 10, kind: 'entry_exit', positionId: 'G000001', satelliteId: 'P01-S01'},
    {timeUs: 20, kind: 'release_exit', positionId: 'G000001', satelliteId: 'P01-S01'}
  ]});
  assert.deepEqual(result.intervals.map(({satelliteId, startTimeUs, endTimeUs}) => ({satelliteId, startTimeUs, endTimeUs})), [
    {satelliteId: 'P01-S01', startTimeUs: 0, endTimeUs: 20}
  ]);
  assert.equal(result.finalPlan.failureCounts.no_visible_candidate, 1);
});

test('a new owner is selected only after reaching the entry threshold', () => {
  const result = run({events: [
    {timeUs: 10, kind: 'release_enter', positionId: 'G000001', satelliteId: 'P01-S02', score: 60},
    {timeUs: 15, kind: 'entry_enter', positionId: 'G000001', satelliteId: 'P01-S02', score: 60},
    {timeUs: 20, kind: 'release_exit', positionId: 'G000001', satelliteId: 'P01-S01'}
  ]});
  assert.deepEqual(result.intervals.map(({satelliteId, startTimeUs, endTimeUs}) => ({satelliteId, startTimeUs, endTimeUs})), [
    {satelliteId: 'P01-S01', startTimeUs: 0, endTimeUs: 20},
    {satelliteId: 'P01-S02', startTimeUs: 20, endTimeUs: 30}
  ]);
  assert.equal(result.ownerChangeCount, 1);
});

test('same-time exits are applied before entries and input order cannot change the result', () => {
  const events = [
    {timeUs: 20, kind: 'entry_enter', positionId: 'G000001', satelliteId: 'P01-S02', score: 60},
    {timeUs: 20, kind: 'release_exit', positionId: 'G000001', satelliteId: 'P01-S01'}
  ];
  assert.deepEqual(run({events}).intervals, run({events: [...events].reverse()}).intervals);
  assert.equal(run({events}).finalPlan.success, true);
});

test('a split replay has the same final assignment as one continuous replay', () => {
  const events = [
    {timeUs: 10, kind: 'entry_enter', positionId: 'G000001', satelliteId: 'P01-S02', score: 60},
    {timeUs: 20, kind: 'release_exit', positionId: 'G000001', satelliteId: 'P01-S01'}
  ];
  const full = run({events});
  const first = run({endTimeUs: 15, events: events.filter(({timeUs}) => timeUs <= 15)});
  const second = run({
    startTimeUs: 15,
    initialVisibility: first.finalVisibility,
    initialAssignments: first.finalAssignments,
    events: events.filter(({timeUs}) => timeUs >= 15)
  });
  assert.deepEqual([...second.finalAssignments], [...full.finalAssignments]);
});

test('capacity failures remain distinct from missing visibility', () => {
  const result = replayAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 10,
    positionIds: ['G000001', 'G000002', 'G000003'],
    initialVisibility: ['G000001', 'G000002', 'G000003'].map((positionId) => ({
      positionId, satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true
    })),
    registry,
    satelliteCapacity: 2,
    cellCapacity: 1
  });
  assert.equal(result.finalPlan.failureCounts.schedule_overflow, 1);
  assert.equal(result.finalPlan.failureCounts.no_visible_candidate, 0);
});

