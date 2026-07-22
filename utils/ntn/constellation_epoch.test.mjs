import assert from 'node:assert/strict';
import test from 'node:test';

import {planAssignmentEpoch} from './constellation_assignment.mjs';
import {
  centralAngleForElevation,
  createWalkerDelta,
  resolveOrbitModel
} from './constellation_audit_core.mjs';
import {
  auditEpochSnapshot,
  createCatalogGeometry,
  createEpochSnapshot
} from './constellation_epoch.mjs';

function satellite(overrides = {}) {
  return createWalkerDelta({
    planes: 1,
    satellitesPerPlane: 1,
    phaseFactor: 0,
    altitudeKm: 500,
    inclinationDeg: 0,
    raanOffsetDeg: 0,
    phaseOffsetDeg: 0,
    ...overrides
  })[0];
}

function positionId(index) {
  return `G${String(index + 1).padStart(6, '0')}`;
}

test('catalog geometry is latitude-bucketed and independent of input order', () => {
  const cells = [
    {id: 'G000003', lat: 10, lon: 20, childMask: 7},
    {id: 'G000001', lat: -10, lon: 0, childMask: 1},
    {id: 'G000002', lat: 0, lon: 10, childMask: 3}
  ];
  const first = createCatalogGeometry({cells}, {latitudeBucketDeg: 5});
  const second = createCatalogGeometry([...cells].reverse(), {latitudeBucketDeg: 5});

  assert.deepEqual(first, second);
  assert.deepEqual(first.positions.map((position) => position.positionId), [
    'G000001', 'G000002', 'G000003'
  ]);
  assert.equal(first.bucketCount, 36);
  assert.equal(first.buckets.flat().length, 3);
  assert.throws(
    () => createCatalogGeometry([...cells, cells[0]]),
    /duplicate position/
  );
});

test('snapshot includes both threshold boundaries and separates 42 from 45 degrees', () => {
  const model = resolveOrbitModel({altitudeKm: 500});
  const entryLongitude = centralAngleForElevation(45, model) * 180 / Math.PI;
  const releaseLongitude = centralAngleForElevation(42, model) * 180 / Math.PI;
  const geometry = createCatalogGeometry([
    {id: 'G000004', lat: 0, lon: releaseLongitude + 0.001, childMask: 1},
    {id: 'G000002', lat: 0, lon: (entryLongitude + releaseLongitude) / 2, childMask: 1},
    {id: 'G000001', lat: 0, lon: entryLongitude, childMask: 1},
    {id: 'G000003', lat: 0, lon: releaseLongitude, childMask: 1}
  ]);
  const snapshot = createEpochSnapshot({
    timeUs: 0,
    satellites: [satellite()],
    catalogGeometry: geometry,
    orbitModel: model
  });

  const inventory = new Map(snapshot.completeCandidateSets.map((entry) => [entry.positionId, entry.candidates]));
  const eligible = new Map(snapshot.assignmentCandidateSets.map((entry) => [entry.positionId, entry.candidates]));
  assert.equal(inventory.get('G000001').length, 1, '45-degree boundary belongs to complete inventory');
  assert.equal(eligible.get('G000001').length, 1, '45-degree boundary is eligible to enter');
  assert.equal(inventory.get('G000002').length, 1);
  assert.equal(eligible.get('G000002').length, 0, '42..45 degree candidate cannot become a new owner');
  assert.equal(inventory.get('G000003').length, 1, '42-degree boundary remains visible');
  assert.equal(eligible.get('G000003').length, 0);
  assert.equal(inventory.get('G000004').length, 0);
  assert.deepEqual(snapshot.satelliteCounts, [{
    satelliteId: 'P01-S01',
    rawVisibleCount: 3,
    assignmentCandidateCount: 1
  }]);
});

test('snapshot is deterministic when catalog and satellite inputs are reversed', () => {
  const satellites = createWalkerDelta({
    planes: 2,
    satellitesPerPlane: 2,
    phaseFactor: 1,
    altitudeKm: 500,
    inclinationDeg: 60,
    raanOffsetDeg: 0,
    phaseOffsetDeg: 0
  });
  const cells = Array.from({length: 20}, (_, index) => ({
    id: positionId(index),
    lat: -10 + index,
    lon: -30 + index * 3,
    childMask: 127
  }));
  const first = createEpochSnapshot({
    timeUs: 12_345_678,
    satellites,
    catalogGeometry: createCatalogGeometry(cells)
  });
  const second = createEpochSnapshot({
    timeUs: 12_345_678,
    satellites: [...satellites].reverse(),
    catalogGeometry: createCatalogGeometry([...cells].reverse())
  });

  assert.deepEqual(second, first);
  assert.deepEqual(auditEpochSnapshot(first), auditEpochSnapshot(second));
});

test('all 257 visible L1 remain in inventory and capacity failure is reported later', () => {
  const cells = Array.from({length: 257}, (_, index) => ({
    id: positionId(index),
    lat: 0,
    lon: 0,
    childMask: 127
  }));
  const geometry = createCatalogGeometry(cells);
  const walkerSatellite = satellite();
  const snapshot = createEpochSnapshot({
    timeUs: 0,
    satellites: [walkerSatellite],
    catalogGeometry: geometry
  });
  const audit = auditEpochSnapshot(snapshot, {
    catalogGeometry: geometry,
    satellites: [walkerSatellite]
  });

  assert.equal(snapshot.completeCandidateSets.length, 257);
  assert.equal(snapshot.assignmentCandidateSets.length, 257);
  assert.equal(audit.positionCount, 257);
  assert.equal(audit.inventoryCandidateCount, 257);
  assert.equal(snapshot.satelliteCounts[0].rawVisibleCount, 257);

  const allocation = planAssignmentEpoch({
    completeCandidateSets: snapshot.completeCandidateSets,
    assignmentCandidateSets: snapshot.assignmentCandidateSets,
    registry: {
      satellites: [{
        satellite_id: 'P01-S01',
        cells: [
          {bank: 0, nci: '0x000000001', pci: 1},
          {bank: 1, nci: '0x000000002', pci: 2}
        ]
      }]
    },
    satelliteCapacity: 256,
    cellCapacity: 128
  });
  assert.equal(allocation.assignments.length, 256);
  assert.deepEqual(allocation.failureCounts, {
    no_visible_candidate: 0,
    schedule_overflow: 1,
    cell_partition_overflow: 0
  });
});

test('snapshot audit rejects cropped inventory and inconsistent per-satellite counts', () => {
  const geometry = createCatalogGeometry([
    {id: 'G000001', lat: 0, lon: 0, childMask: 1},
    {id: 'G000002', lat: 0, lon: 1, childMask: 1}
  ]);
  const walkerSatellite = satellite();
  const snapshot = createEpochSnapshot({timeUs: 0, satellites: [walkerSatellite], catalogGeometry: geometry});
  const cropped = structuredClone(snapshot);
  cropped.completeCandidateSets.pop();
  cropped.assignmentCandidateSets.pop();
  assert.throws(
    () => auditEpochSnapshot(cropped, {catalogGeometry: geometry, satellites: [walkerSatellite]}),
    /cropped/
  );

  const mismatched = structuredClone(snapshot);
  mismatched.satelliteCounts[0].rawVisibleCount -= 1;
  assert.throws(() => auditEpochSnapshot(mismatched), /count mismatch/);
});

test('snapshot rejects non-integer time and inverted thresholds', () => {
  const geometry = createCatalogGeometry([]);
  assert.throws(
    () => createEpochSnapshot({timeUs: 0.5, satellites: [], catalogGeometry: geometry}),
    /safe integer/
  );
  assert.throws(
    () => createEpochSnapshot({
      timeUs: 0,
      satellites: [],
      catalogGeometry: geometry,
      entryElevationDeg: 42,
      releaseElevationDeg: 45
    }),
    /lower than entryElevationDeg/
  );
});
