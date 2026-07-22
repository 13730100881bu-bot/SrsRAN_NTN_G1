import assert from 'node:assert/strict';
import test from 'node:test';

import {
  assignmentFailureReason,
  matchCandidatesToCapacity,
  partitionAssignmentsToTwoCells,
  planAssignmentEpoch
} from './constellation_assignment.mjs';

const registry = {
  satellites: [
    {
      satellite_id: 'SAT-A',
      cells: [
        {bank: 0, nci: '0x000000101', pci: 11},
        {bank: 1, nci: '0x000000102', pci: 12}
      ]
    },
    {
      satellite_id: 'SAT-B',
      cells: [
        {bank: 0, nci: '0x000000201', pci: 21},
        {bank: 1, nci: '0x000000202', pci: 22}
      ]
    }
  ]
};

function positionId(index) {
  return `G${String(index + 1).padStart(6, '0')}`;
}

function candidateSets(count, satelliteIds = ['SAT-A']) {
  return Array.from({length: count}, (_, index) => ({
    positionId: positionId(index),
    candidates: satelliteIds.map((satelliteId, satelliteIndex) => ({
      satelliteId,
      elevationDeg: 60 - satelliteIndex
    }))
  }));
}

function plan(count, overrides = {}) {
  const complete = overrides.completeCandidateSets ?? candidateSets(count);
  const eligible = overrides.assignmentCandidateSets ?? structuredClone(complete);
  return planAssignmentEpoch({
    registry,
    ...overrides,
    completeCandidateSets: complete,
    assignmentCandidateSets: eligible
  });
}

test('maximum-cardinality matching follows an augmenting path that a greedy pass misses', () => {
  const matched = matchCandidatesToCapacity([
    {
      positionId: 'FLEX',
      candidates: [
        {satelliteId: 'SAT-A', elevationDeg: 60},
        {satelliteId: 'SAT-B', elevationDeg: 50}
      ]
    },
    {positionId: 'ONLY-A', candidates: [{satelliteId: 'SAT-A', elevationDeg: 55}]}
  ], {satelliteCapacity: 1});

  assert.deepEqual([...matched], [['FLEX', 'SAT-B'], ['ONLY-A', 'SAT-A']]);
});

test('matching and epoch output are deterministic under input reordering', () => {
  const complete = candidateSets(10, ['SAT-B', 'SAT-A']);
  const reversed = [...complete].reverse().map((entry) => ({
    ...entry,
    candidates: [...entry.candidates].reverse()
  }));
  const first = planAssignmentEpoch({
    completeCandidateSets: complete,
    assignmentCandidateSets: complete,
    registry,
    satelliteCapacity: 5,
    cellCapacity: 3
  });
  const second = planAssignmentEpoch({
    completeCandidateSets: reversed,
    assignmentCandidateSets: reversed,
    registry,
    satelliteCapacity: 5,
    cellCapacity: 3
  });

  assert.deepEqual(second.assignments, first.assignments);
  assert.deepEqual([...second.candidateInventoryByPositionId], [...first.candidateInventoryByPositionId]);
});

test('feasible previous satellite owners and cell banks remain sticky', () => {
  const complete = [
    {positionId: 'G000001', candidates: [{satelliteId: 'SAT-A'}, {satelliteId: 'SAT-B'}]},
    {positionId: 'G000002', candidates: [{satelliteId: 'SAT-A'}]},
    {positionId: 'G000003', candidates: [{satelliteId: 'SAT-A'}]}
  ];
  const previousAssignments = new Map([
    ['G000001', {satelliteId: 'SAT-B', cellBank: 1}],
    ['G000002', {satelliteId: 'SAT-A', cellBank: 1}],
    ['G000003', {satelliteId: 'SAT-A', cellBank: 0}]
  ]);
  const result = planAssignmentEpoch({
    completeCandidateSets: complete,
    assignmentCandidateSets: complete,
    registry,
    previousAssignments,
    satelliteCapacity: 2,
    cellCapacity: 1
  });

  assert.equal(result.assignmentByPositionId.get('G000001').satelliteId, 'SAT-B');
  assert.equal(result.assignmentByPositionId.get('G000001').cellBank, 1);
  assert.equal(result.assignmentByPositionId.get('G000002').cellBank, 1);
  assert.equal(result.assignmentByPositionId.get('G000003').cellBank, 0);
});

test('maximum coverage also keeps the largest feasible set of incumbent owners', () => {
  const candidate = (satelliteId) => ({satelliteId});
  const matched = matchCandidatesToCapacity([
    {positionId: 'P0', candidates: [candidate('C')]},
    {positionId: 'P1', candidates: [candidate('A'), candidate('B'), candidate('C')]},
    {positionId: 'P3', candidates: [candidate('A')]},
    {positionId: 'P4', candidates: [candidate('A'), candidate('B'), candidate('C')]}
  ], {
    satelliteCapacity: 1,
    previousAssignments: new Map([['P4', {satelliteId: 'C', cellBank: 0}]])
  });

  assert.equal(matched.size, 3);
  assert.equal(matched.get('P4'), 'C');
  assert.equal([...matched].filter(([positionId, satelliteId]) => positionId === 'P4' && satelliteId === 'C').length, 1);
});

test('retention optimization avoids the A/B/C/D/E cap-one local optimum', () => {
  const candidates = (...satelliteIds) => satelliteIds.map((satelliteId) => ({satelliteId}));
  const previousAssignments = new Map([
    ['A', {satelliteId: 'E', cellBank: 0}],
    ['C', {satelliteId: 'D', cellBank: 0}],
    ['D', {satelliteId: 'D', cellBank: 0}],
    ['E', {satelliteId: 'D', cellBank: 0}]
  ]);
  const matched = matchCandidatesToCapacity([
    {positionId: 'A', candidates: candidates('A', 'C', 'E')},
    {positionId: 'B', candidates: candidates('A', 'B', 'E')},
    {positionId: 'C', candidates: candidates('C', 'D')},
    {positionId: 'D', candidates: candidates('A', 'C', 'D')},
    {positionId: 'E', candidates: candidates('D', 'E')}
  ], {satelliteCapacity: 1, previousAssignments});

  const retained = [...matched].filter(([positionId, satelliteId]) => {
    return previousAssignments.get(positionId)?.satelliteId === satelliteId;
  });
  assert.equal(matched.size, 5);
  assert.equal(retained.length, 2);
});

test('small exhaustive graphs confirm maximum coverage then maximum incumbent retention', () => {
  let seed = 0x51f15e;
  const random = () => {
    seed = (1664525 * seed + 1013904223) >>> 0;
    return seed / 2 ** 32;
  };
  const satellites = ['A', 'B', 'C'];
  for (let sample = 0; sample < 80; sample += 1) {
    const sets = Array.from({length: 5}, (_, index) => ({
      positionId: `P${index}`,
      candidates: satellites.filter(() => random() > 0.35).map((satelliteId) => ({satelliteId}))
    }));
    const previousAssignments = new Map(sets.flatMap((entry) => {
      if (entry.candidates.length === 0 || random() < 0.25) return [];
      const candidate = entry.candidates[Math.floor(random() * entry.candidates.length)];
      return [[entry.positionId, {satelliteId: candidate.satelliteId, cellBank: 0}]];
    }));
    let optimum = {matched: -1, retained: -1};
    const loads = new Map(satellites.map((satelliteId) => [satelliteId, 0]));
    const visit = (index, matched, retained) => {
      if (index === sets.length) {
        if (matched > optimum.matched || (matched === optimum.matched && retained > optimum.retained)) {
          optimum = {matched, retained};
        }
        return;
      }
      visit(index + 1, matched, retained);
      const entry = sets[index];
      for (const candidate of entry.candidates) {
        if (loads.get(candidate.satelliteId) >= 1) continue;
        loads.set(candidate.satelliteId, 1);
        visit(
          index + 1,
          matched + 1,
          retained + Number(previousAssignments.get(entry.positionId)?.satelliteId === candidate.satelliteId)
        );
        loads.set(candidate.satelliteId, 0);
      }
    };
    visit(0, 0, 0);
    const actual = matchCandidatesToCapacity(sets, {satelliteCapacity: 1, previousAssignments});
    const retained = [...actual].filter(([positionId, satelliteId]) => {
      return previousAssignments.get(positionId)?.satelliteId === satelliteId;
    }).length;
    assert.deepEqual({matched: actual.size, retained}, optimum);
  }
});

test('sticky cell partition never derives identity outside the supplied registry', () => {
  const previousAssignments = new Map([['G000001', {satelliteId: 'SAT-A', cellBank: 1}]]);
  const allocation = partitionAssignmentsToTwoCells({
    satelliteId: 'SAT-A',
    positionIds: ['G000002', 'G000001'],
    registryCells: registry.satellites[0].cells,
    previousAssignments,
    cellCapacity: 2
  });

  assert.deepEqual(allocation.cells.map(({nci, pci}) => ({nci, pci})), [
    {nci: '0x000000101', pci: 11},
    {nci: '0x000000102', pci: 12}
  ]);
  assert.deepEqual(allocation.cells[1].assignedPositionIds, ['G000001']);
});

test('0, 1, 128 and 256 positions preserve complete inventory and fit two cells', () => {
  const expectedCellCounts = new Map([
    [0, [0, 0]],
    [1, [1, 0]],
    [128, [64, 64]],
    [256, [128, 128]]
  ]);

  for (const count of [0, 1, 128, 256]) {
    const result = plan(count);
    assert.equal(result.success, true, `count=${count}`);
    assert.equal(result.candidateInventoryByPositionId.size, count, `count=${count}`);
    assert.equal(result.completeInventoryBySatellite.get('SAT-A').length, count, `count=${count}`);
    assert.equal(result.assignments.length, count, `count=${count}`);
    assert.deepEqual(
      result.cellAllocationsBySatellite.get('SAT-A').cells.map((cell) => cell.assignedPositionIds.length),
      expectedCellCounts.get(count),
      `count=${count}`
    );
    assert.equal(result.assignments.every((assignment) =>
      assignment.nci === '0x000000101' || assignment.nci === '0x000000102'), true);
  }
});

test('257 positions retain all inventory and report schedule overflow without clipping', () => {
  const result = plan(257);

  assert.equal(result.success, false);
  assert.equal(result.failureReason, assignmentFailureReason.scheduleOverflow);
  assert.equal(result.candidateInventoryByPositionId.size, 257);
  assert.equal(result.completeInventoryBySatellite.get('SAT-A').length, 257);
  assert.equal(result.satelliteMatchesByPositionId.size, 256);
  assert.equal(result.assignments.length, 256);
  assert.deepEqual(result.cellAllocationsBySatellite.get('SAT-A').cells.map((cell) => cell.assignedPositionIds.length),
    [128, 128]);
  assert.deepEqual(result.failures, [
    {positionId: 'G000257', reason: assignmentFailureReason.scheduleOverflow}
  ]);
});

test('complete inventory remains distinct from current assignment eligibility', () => {
  const complete = [{
    positionId: 'G000001',
    candidates: [{satelliteId: 'SAT-A'}, {satelliteId: 'SAT-B'}]
  }];
  const result = plan(1, {
    completeCandidateSets: complete,
    assignmentCandidateSets: [{positionId: 'G000001', candidates: []}]
  });

  assert.equal(result.candidateInventoryByPositionId.get('G000001').length, 2);
  assert.equal(result.completeInventoryBySatellite.get('SAT-A').length, 1);
  assert.equal(result.completeInventoryBySatellite.get('SAT-B').length, 1);
  assert.deepEqual(result.failures, [
    {positionId: 'G000001', reason: assignmentFailureReason.noVisibleCandidate}
  ]);
});

test('cell partition overflow is separate from satellite schedule capacity', () => {
  const result = plan(257, {satelliteCapacity: 257, cellCapacity: 128});

  assert.equal(result.satelliteMatchesByPositionId.size, 257);
  assert.equal(result.assignments.length, 256);
  assert.equal(result.candidateInventoryByPositionId.size, 257);
  assert.equal(result.failureReason, assignmentFailureReason.cellPartitionOverflow);
  assert.deepEqual(result.failures, [
    {positionId: 'G000257', reason: assignmentFailureReason.cellPartitionOverflow}
  ]);
});
