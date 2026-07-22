import assert from 'node:assert/strict';
import test from 'node:test';

import {replayAssignmentTimeline} from './constellation_timeline.mjs';
import {
  createStreamingAssignmentSession,
  replayStreamingAssignmentTimeline,
  resumeStreamingAssignmentTimeline
} from './constellation_streaming_assignment.mjs';

const registry = {
  satellites: [
    {satellite_id: 'P01-S01', cells: [{bank: 0, nci: '0x000000001', pci: 1}, {bank: 1, nci: '0x000000002', pci: 2}]},
    {satellite_id: 'P01-S02', cells: [{bank: 0, nci: '0x000000003', pci: 3}, {bank: 1, nci: '0x000000004', pci: 4}]}
  ]
};

function runStreaming(overrides = {}) {
  return replayStreamingAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 40,
    positionIds: ['G000001', 'G000002'],
    initialVisibility: [
      {positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true, score: 50},
      {positionId: 'G000002', satelliteId: 'P01-S02', aboveEntry: true, aboveRelease: true, score: 50}
    ],
    events: [],
    registry,
    satelliteCapacity: 2,
    cellCapacity: 1,
    ...overrides
  });
}

function runExisting(overrides = {}) {
  return replayAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 40,
    positionIds: ['G000001', 'G000002'],
    initialVisibility: [
      {positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true, score: 50},
      {positionId: 'G000002', satelliteId: 'P01-S02', aboveEntry: true, aboveRelease: true, score: 50}
    ],
    events: [],
    registry,
    satelliteCapacity: 2,
    cellCapacity: 1,
    ...overrides
  });
}

test('incremental epochs and final assignments match the existing small-scenario replay', () => {
  const events = [
    {timeUs: 30, kind: 'release_exit', positionId: 'G000002', satelliteId: 'P01-S02'},
    {timeUs: 10, kind: 'entry_exit', positionId: 'G000001', satelliteId: 'P01-S01'},
    {timeUs: 20, kind: 'entry_enter', positionId: 'G000001', satelliteId: 'P01-S02', score: 60},
    {timeUs: 30, kind: 'entry_enter', positionId: 'G000002', satelliteId: 'P01-S01', score: 55},
    {timeUs: 30, kind: 'release_exit', positionId: 'G000001', satelliteId: 'P01-S01'}
  ];
  const streaming = runStreaming({events});
  const existing = runExisting({events});

  assert.deepEqual(
    streaming.epochs.map(({timeUs, success, failureCounts}) => ({timeUs, success, failureCounts})),
    existing.epochs.filter(({timeUs}) => streaming.epochs.some((epoch) => epoch.timeUs === timeUs))
      .map(({timeUs, success, failureCounts}) => ({timeUs, success, failureCounts}))
  );
  assert.equal(streaming.metrics.fastPathGroups, 2);
  assert.equal(streaming.metrics.matchingRepairGroups, 1);
  assert.equal(streaming.metrics.globalFallbackGroups, 0);
  assert.equal(streaming.metrics.componentFallbackGroups, 1);
  assert.deepEqual(streaming.intervals, existing.intervals);
  assert.deepEqual(streaming.finalAssignments, existing.finalPlan.assignments);
  assert.deepEqual(streaming.finalPlan.failureCounts, existing.finalPlan.failureCounts);
  assert.deepEqual(streaming.finalVisibility, existing.finalVisibility);
});

test('an augmenting path moves an incumbent only when needed for maximum coverage', () => {
  const result = replayStreamingAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 20,
    positionIds: ['G000001', 'G000002'],
    initialVisibility: [
      {positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true, score: 50}
    ],
    events: [
      {timeUs: 10, kind: 'entry_enter', positionId: 'G000001', satelliteId: 'P01-S02', score: 50},
      {timeUs: 10, kind: 'entry_enter', positionId: 'G000002', satelliteId: 'P01-S01', score: 50}
    ],
    registry,
    satelliteCapacity: 1,
    cellCapacity: 1
  });

  assert.equal(result.finalPlan.success, true);
  assert.deepEqual(result.finalAssignments.map(({positionId, satelliteId}) => ({positionId, satelliteId})), [
    {positionId: 'G000001', satelliteId: 'P01-S02'},
    {positionId: 'G000002', satelliteId: 'P01-S01'}
  ]);
  assert.equal(result.ownerChangeCount, 1);
  assert.equal(result.metrics.augmentingPaths, 1);
  assert.equal(result.metrics.globalFallbackGroups, 0);
});

test('global path selection avoids the A/B/C/D/E sticky local optimum', () => {
  const satelliteIds = ['A', 'B', 'C', 'D', 'E'];
  const localRegistry = {
    satellites: satelliteIds.map((satellite_id, index) => ({
      satellite_id,
      cells: [
        {bank: 0, nci: String(index * 2 + 1), pci: index * 2},
        {bank: 1, nci: String(index * 2 + 2), pci: index * 2 + 1}
      ]
    }))
  };
  const graph = {
    A: ['A', 'C', 'E'],
    B: ['A', 'B', 'E'],
    C: ['C', 'D'],
    D: ['A', 'C', 'D'],
    E: ['D', 'E']
  };
  const previousAssignments = new Map([
    ['A', {satelliteId: 'E', cellBank: 0}],
    ['C', {satelliteId: 'D', cellBank: 0}],
    ['D', {satelliteId: 'D', cellBank: 0}],
    ['E', {satelliteId: 'D', cellBank: 0}]
  ]);
  const result = replayStreamingAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 1,
    positionIds: Object.keys(graph),
    initialVisibility: Object.entries(graph).flatMap(([positionId, candidates]) => candidates.map((satelliteId) => ({
      positionId,
      satelliteId,
      aboveEntry: true,
      aboveRelease: true
    }))),
    registry: localRegistry,
    initialAssignments: previousAssignments,
    satelliteCapacity: 1,
    cellCapacity: 1
  });
  const retained = result.finalAssignments.filter((assignment) => {
    return previousAssignments.get(assignment.positionId)?.satelliteId === assignment.satelliteId;
  });

  assert.equal(result.finalAssignments.length, 5);
  assert.equal(retained.length, 2);
});

test('release-only visibility retains an owner but cannot acquire a new owner', () => {
  const retained = replayStreamingAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 20,
    positionIds: ['G000001'],
    initialVisibility: [
      {positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true}
    ],
    events: [
      {timeUs: 10, kind: 'entry_exit', positionId: 'G000001', satelliteId: 'P01-S01'}
    ],
    registry,
    satelliteCapacity: 1,
    cellCapacity: 1
  });
  assert.equal(retained.finalAssignments[0].satelliteId, 'P01-S01');

  const notAcquired = replayStreamingAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 20,
    positionIds: ['G000001'],
    initialVisibility: [
      {positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: false, aboveRelease: true}
    ],
    registry,
    satelliteCapacity: 1,
    cellCapacity: 1
  });
  assert.deepEqual(notAcquired.finalAssignments, []);
  assert.equal(notAcquired.finalPlan.failureCounts.no_visible_candidate, 1);
});

test('satellite and two-cell capacity remain explicit without deriving identity', () => {
  const result = replayStreamingAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 10,
    positionIds: ['G000003', 'G000001', 'G000002'],
    initialVisibility: ['G000003', 'G000001', 'G000002'].map((positionId) => ({
      positionId,
      satelliteId: 'P01-S01',
      aboveEntry: true,
      aboveRelease: true
    })),
    registry,
    satelliteCapacity: 2,
    cellCapacity: 1
  });

  assert.equal(result.finalPlan.failureCounts.schedule_overflow, 1);
  assert.equal(result.finalPlan.failureCounts.cell_partition_overflow, 0);
  assert.deepEqual(result.finalAssignments.map(({positionId, nci, pci}) => ({positionId, nci, pci})), [
    {positionId: 'G000001', nci: '0x000000001', pci: 1},
    {positionId: 'G000002', nci: '0x000000002', pci: 2}
  ]);
});

test('257 visible positions keep complete inventory while assigning only 256', () => {
  const positionIds = Array.from({length: 257}, (_, index) => `G${String(index + 1).padStart(6, '0')}`);
  const result = replayStreamingAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 10,
    positionIds,
    initialVisibility: positionIds.map((positionId) => ({
      positionId,
      satelliteId: 'P01-S01',
      aboveEntry: true,
      aboveRelease: true
    })),
    registry,
    satelliteCapacity: 256,
    cellCapacity: 128
  });

  assert.equal(result.finalPlan.candidateInventory.length, 257);
  assert.equal(result.finalAssignments.length, 256);
  assert.equal(result.finalPlan.failureCounts.schedule_overflow, 1);
  assert.deepEqual(result.finalPlan.cellAllocations[0].cells.map((cell) => cell.assignedPositionIds.length), [128, 128]);
});

test('event order is normalized and the result is JSON-safe and deterministic', () => {
  const events = [
    {timeUs: 20, kind: 'entry_enter', positionId: 'G000001', satelliteId: 'P01-S02', score: 60},
    {timeUs: 20, kind: 'release_exit', positionId: 'G000001', satelliteId: 'P01-S01'}
  ];
  const forward = runStreaming({
    positionIds: ['G000002', 'G000001'],
    initialVisibility: [
      {positionId: 'G000002', satelliteId: 'P01-S02', aboveEntry: true, aboveRelease: true, score: 50},
      {positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true, score: 50}
    ],
    events
  });
  const reverse = runStreaming({events: [...events].reverse()});

  assert.deepEqual(forward, reverse);
  assert.deepEqual(JSON.parse(JSON.stringify(forward)), forward);
});

test('large runs of non-owner visibility events use the fast path', () => {
  const events = Array.from({length: 1000}, (_, index) => ({
    timeUs: index + 1,
    kind: index % 2 === 0 ? 'release_enter' : 'release_exit',
    positionId: 'G000001',
    satelliteId: 'P01-S02'
  }));
  const input = {
    startTimeUs: 0,
    endTimeUs: 1001,
    positionIds: ['G000001'],
    initialVisibility: [
      {positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true}
    ],
    events,
    registry,
    satelliteCapacity: 1,
    cellCapacity: 1
  };
  const streaming = replayStreamingAssignmentTimeline(input);
  const existing = replayAssignmentTimeline(input);

  assert.equal(streaming.epochs.length, 1);
  assert.equal(streaming.metrics.eventGroups, 1000);
  assert.equal(streaming.metrics.fastPathGroups, 1000);
  assert.equal(streaming.metrics.matchingRepairGroups, 0);
  assert.equal(streaming.metrics.augmentingSearches, 0);
  assert.deepEqual(streaming.finalAssignments, existing.finalPlan.assignments);
  assert.deepEqual(streaming.finalVisibility, existing.finalVisibility);
  assert.deepEqual(streaming.finalPlan.failureCounts, existing.finalPlan.failureCounts);
});

test('a bounded JSON checkpoint resumes to the same combined records and final state', () => {
  const events = [
    {timeUs: 10, kind: 'entry_exit', positionId: 'G000001', satelliteId: 'P01-S01'},
    {timeUs: 20, kind: 'entry_enter', positionId: 'G000001', satelliteId: 'P01-S02', score: 60},
    {timeUs: 30, kind: 'release_exit', positionId: 'G000001', satelliteId: 'P01-S01'}
  ];
  const full = runStreaming({events});
  const first = runStreaming({
    endTimeUs: 15,
    events: events.filter(({timeUs}) => timeUs < 15),
    finalize: false
  });
  const resumed = resumeStreamingAssignmentTimeline({
    checkpoint: JSON.parse(JSON.stringify(first.checkpoint)),
    endTimeUs: 40,
    events: events.filter(({timeUs}) => timeUs >= 15)
  });

  assert.deepEqual([...first.intervals, ...resumed.intervals], full.intervals);
  assert.deepEqual([...first.epochs, ...resumed.epochs], full.epochs);
  assert.deepEqual(resumed.finalPlan, full.finalPlan);
  assert.deepEqual(resumed.finalAssignments, full.finalAssignments);
  assert.deepEqual(resumed.finalVisibility, full.finalVisibility);
  assert.deepEqual(resumed.checkpoint, full.checkpoint);
  assert.equal(resumed.ownerChangeCount, full.ownerChangeCount);
  assert.equal(Object.hasOwn(first.checkpoint, 'closedIntervals'), false);
  assert.equal(Object.hasOwn(first.checkpoint, 'epochs'), false);
  assert.deepEqual(JSON.parse(JSON.stringify(first.checkpoint)), first.checkpoint);
});

test('one live session consumes async event groups across slabs without checkpoint restoration', async () => {
  const events = [
    {timeUs: 10, kind: 'entry_exit', positionId: 'G000001', satelliteId: 'P01-S01'},
    {timeUs: 20, kind: 'entry_enter', positionId: 'G000001', satelliteId: 'P01-S02', score: 60},
    {timeUs: 30, kind: 'release_exit', positionId: 'G000001', satelliteId: 'P01-S01'}
  ];
  const full = runStreaming({events});
  const session = createStreamingAssignmentSession({
    startTimeUs: 0,
    positionIds: ['G000001', 'G000002'],
    initialVisibility: [
      {positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true, score: 50},
      {positionId: 'G000002', satelliteId: 'P01-S02', aboveEntry: true, aboveRelease: true, score: 50}
    ],
    registry,
    satelliteCapacity: 2,
    cellCapacity: 1
  });
  const groups = (items) => (async function* () {
    for (const item of items) yield [item];
  })();
  const first = await session.advanceEventGroups({
    endTimeUs: 20,
    eventGroups: groups(events.filter(({timeUs}) => timeUs < 20)),
    finalize: false
  });
  const second = await session.advanceEventGroups({
    endTimeUs: 40,
    eventGroups: groups(events.filter(({timeUs}) => timeUs >= 20)),
    finalize: true
  });

  assert.deepEqual([...first.intervals, ...second.intervals], full.intervals);
  assert.deepEqual([...first.epochs, ...second.epochs], full.epochs);
  assert.deepEqual(second.finalAssignments, full.finalAssignments);
  assert.deepEqual(second.finalPlan.failureCounts, full.finalPlan.failureCounts);
  assert.deepEqual(second.checkpoint, full.checkpoint);
});

test('start-boundary events report the sticky pre-roll owner change and counters', () => {
  const initialAssignments = new Map([['G000001', {
    positionId: 'G000001',
    satelliteId: 'P01-S01',
    cellBank: 0,
    nci: '0x000000001',
    pci: 1
  }]]);
  const input = {
    startTimeUs: 0,
    endTimeUs: 10,
    positionIds: ['G000001'],
    initialVisibility: [
      {positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true}
    ],
    initialAssignments,
    events: [
      {timeUs: 0, kind: 'entry_enter', positionId: 'G000001', satelliteId: 'P01-S02'},
      {timeUs: 0, kind: 'release_exit', positionId: 'G000001', satelliteId: 'P01-S01'}
    ],
    registry,
    satelliteCapacity: 1,
    cellCapacity: 1
  };
  const streaming = replayStreamingAssignmentTimeline(input);
  const existing = replayAssignmentTimeline(input);

  assert.deepEqual(streaming.epochs[0].assignmentChanges, existing.transitions.map((transition) => ({
    positionId: transition.positionId,
    before: transition.before,
    after: transition.after
  })));
  assert.equal(streaming.ownerChangeCount, existing.ownerChangeCount);
  assert.equal(streaming.handoverCount, existing.handoverCount);
  assert.equal(streaming.releaseCount, existing.releaseCount);
  assert.equal(streaming.acquisitionCount, existing.acquisitionCount);
  assert.equal(streaming.ownerChangeCount, 1);
  assert.equal(streaming.handoverCount, 1);
});

test('checkpoint size is tied to live state rather than elapsed event history', () => {
  let result = replayStreamingAssignmentTimeline({
    startTimeUs: 0,
    endTimeUs: 10,
    positionIds: ['G000001'],
    initialVisibility: [
      {positionId: 'G000001', satelliteId: 'P01-S01', aboveEntry: true, aboveRelease: true}
    ],
    registry,
    satelliteCapacity: 1,
    cellCapacity: 1,
    finalize: false
  });
  for (let chunk = 1; chunk <= 20; chunk += 1) {
    const oldSatelliteId = chunk % 2 === 1 ? 'P01-S01' : 'P01-S02';
    const newSatelliteId = chunk % 2 === 1 ? 'P01-S02' : 'P01-S01';
    result = resumeStreamingAssignmentTimeline({
      checkpoint: JSON.parse(JSON.stringify(result.checkpoint)),
      endTimeUs: (chunk + 1) * 10,
      events: [
        {timeUs: chunk * 10, kind: 'release_exit', positionId: 'G000001', satelliteId: oldSatelliteId},
        {timeUs: chunk * 10, kind: 'entry_enter', positionId: 'G000001', satelliteId: newSatelliteId}
      ],
      finalize: false
    });
    assert.equal(result.intervals.length, 1);
    assert.equal(result.checkpoint.assignments.length, 1);
    assert.equal(result.checkpoint.openIntervals.length, 1);
    assert.equal(result.checkpoint.lastOwners.length, 1);
    assert.equal(Object.hasOwn(result.checkpoint, 'closedIntervals'), false);
    assert.equal(Object.hasOwn(result.checkpoint, 'epochs'), false);
  }
  assert.equal(result.checkpoint.counters.ownerChangeCount, 20);
});
