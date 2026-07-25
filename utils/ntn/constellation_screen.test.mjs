import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import test from 'node:test';

import {
  centralAngleForElevation,
  createWalkerDelta,
  resolveOrbitModel,
  satelliteUnitEcef
} from './constellation_audit_core.mjs';
import {createCatalogGeometry} from './constellation_epoch.mjs';
import {
  parseArguments,
  runScreen,
  screenEpoch,
  validateScreenManifest
} from './constellation_screen.mjs';

function manifest(overrides = {}) {
  return {
    schema_version: 1,
    screen_id: 'test-screen',
    altitude_km: 500,
    entry_elevation_deg: 45,
    release_elevation_deg: 42,
    duration_seconds: 120,
    step_seconds: 120,
    satellite_capacity: 256,
    cell_capacity: 128,
    scenarios: [{
      id: 'one-satellite',
      inclination_deg: 0,
      planes: 1,
      satellites_per_plane: 1,
      phase_factor: 0
    }],
    ...overrides
  };
}

function cell(id, lat = 0, lon = 0) {
  return {id, lat, lon, childMask: 1};
}

function catalogWithHash(cells, version = 'test-catalog-v1') {
  const sha256 = createHash('sha256').update(JSON.stringify(cells)).digest('hex');
  return {
    catalog: {
      metadata: {version, integrity: {sha256}},
      cells
    },
    version,
    sha256
  };
}

function overheadSatellites(count) {
  const definition = createWalkerDelta({
    altitudeKm: 500,
    inclinationDeg: 0,
    planes: 1,
    satellitesPerPlane: 1,
    phaseFactor: 0
  })[0];
  return Array.from({length: count}, (_, index) => ({
    ...definition,
    id: `P01-S${String(index + 1).padStart(2, '0')}`
  }));
}

test('screen manifest is exact-keyed and validates scenario identity', () => {
  assert.equal(validateScreenManifest(manifest()).screenId, 'test-screen');
  assert.throws(
    () => validateScreenManifest({...manifest(), surprise: true}),
    /unknown field 'manifest\.surprise'/
  );
  assert.throws(
    () => validateScreenManifest(manifest({
      scenarios: [
        manifest().scenarios[0],
        {...manifest().scenarios[0]}
      ]
    })),
    /duplicate scenario id/
  );
  assert.throws(
    () => validateScreenManifest(manifest({
      scenarios: [{...manifest().scenarios[0], phase_factor: 1}]
    })),
    /phase_factor/
  );
  assert.throws(
    () => validateScreenManifest(manifest({satellite_capacity: 257, cell_capacity: 128})),
    /two cell capacities/
  );
});

test('sampled epoch keeps visibility candidates separate from one-owner assignment', () => {
  const geometry = createCatalogGeometry({cells: [cell('G000001')]});
  const epoch = screenEpoch({
    timeUs: 0,
    satellites: overheadSatellites(2),
    catalogGeometry: geometry,
    entryElevationDeg: 45,
    releaseElevationDeg: 42
  });

  assert.equal(epoch.uncoveredL1, 0);
  assert.equal(epoch.minimumEntryCandidateCount, 2);
  assert.equal(epoch.maximumReleaseVisibleL1PerSatellite, 1);
  assert.equal(epoch.maximumAssignedL1PerSatellite, 1);
  assert.equal([...epoch.owners].filter((owner) => owner >= 0).length, 1);
});

test('an incumbent remains eligible between the 45 degree entry and 42 degree release thresholds', () => {
  const satellites = overheadSatellites(1);
  const orbitModel = resolveOrbitModel({altitudeKm: 500});
  const satelliteUnit = satelliteUnitEcef(satellites[0], 0, orbitModel);
  const satelliteLongitudeDeg = Math.atan2(satelliteUnit[1], satelliteUnit[0]) * 180 / Math.PI;
  const offsetDeg = centralAngleForElevation(43, orbitModel) * 180 / Math.PI;
  const longitudeDeg = ((satelliteLongitudeDeg + offsetDeg + 540) % 360) - 180;
  const geometry = createCatalogGeometry({cells: [cell('G000001', 0, longitudeDeg)]});

  const withoutIncumbent = screenEpoch({
    timeUs: 0,
    satellites,
    catalogGeometry: geometry,
    orbitModel,
    entryElevationDeg: 45,
    releaseElevationDeg: 42
  });
  const withIncumbent = screenEpoch({
    timeUs: 0,
    satellites,
    catalogGeometry: geometry,
    orbitModel,
    entryElevationDeg: 45,
    releaseElevationDeg: 42,
    previousOwners: new Int32Array([0])
  });

  assert.equal(withoutIncumbent.uncoveredL1, 1);
  assert.equal(withIncumbent.uncoveredL1, 0);
  assert.equal(withIncumbent.minimumEntryCandidateCount, 0);
  assert.equal(withIncumbent.minimumEligibleCandidateCount, 1);
  assert.equal(withIncumbent.owners[0], 0);
});

test('complete visible inventory is not cropped when it exceeds the planning bound', () => {
  const geometry = createCatalogGeometry({
    cells: [cell('G000001'), cell('G000002', 0, 0.01), cell('G000003', 0, -0.01)]
  });
  const epoch = screenEpoch({
    timeUs: 0,
    satellites: overheadSatellites(1),
    catalogGeometry: geometry,
    entryElevationDeg: 45,
    releaseElevationDeg: 42,
    satelliteCapacity: 2,
    cellCapacity: 1
  });

  assert.equal(epoch.maximumReleaseVisibleL1PerSatellite, 3);
  assert.equal(epoch.maximumAssignedL1PerSatellite, 3);
  assert.equal(epoch.capacitySafeByInventoryBound, false);
  assert.equal(epoch.scheduleOverflow, true);
});

test('screen report is deterministic and cannot select a sampled candidate', () => {
  const input = {
    manifest: manifest(),
    catalog: {cells: [cell('G000001')]}
  };
  const first = runScreen(input);
  const second = runScreen(input);

  assert.deepEqual(first, second);
  assert.equal(first.exact, false);
  assert.equal(first.selectedScenario, null);
  assert.equal(first.selection_eligible, false);
  assert.equal(first.catalog_version, null);
  assert.equal(first.catalog_content_hash, null);
  assert.equal(first.catalog_hash_verified, false);
  assert.equal(first.scenarios[0].coverage_passed, true);
  assert.equal(first.scenarios[0].assignment_passed, true);
  assert.equal(first.scenarios[0].maximum_assigned_l1_per_satellite, 1);
  assert.equal(first.smallest_passing_sampled_candidate.id, 'one-satellite');
});

test('catalog content and planning-context hashes are verified before screening', () => {
  const cells = [cell('G000001')];
  const input = catalogWithHash(cells);
  const boundManifest = manifest({
    catalog_version: input.version,
    catalog_content_hash: input.sha256
  });

  const report = runScreen({manifest: boundManifest, catalog: input.catalog});
  assert.equal(report.catalog_version, input.version);
  assert.equal(report.catalog_content_hash, input.sha256);
  assert.equal(report.catalog_hash_verified, true);

  const tampered = structuredClone(input.catalog);
  tampered.cells[0].lat = 1;
  assert.throws(
    () => runScreen({manifest: boundManifest, catalog: tampered}),
    /catalog content hash does not match/
  );
  assert.throws(
    () => runScreen({
      manifest: {...boundManifest, catalog_version: 'other-catalog'},
      catalog: input.catalog
    }),
    /catalog version mismatch/
  );
  assert.throws(
    () => validateScreenManifest({...manifest(), catalog_version: input.version}),
    /must be provided together/
  );
});

test('uncovered positions remain explicit failures', () => {
  const report = runScreen({
    manifest: manifest(),
    catalog: {cells: [cell('G000001', 0, 180)]}
  });

  assert.equal(report.scenarios[0].coverage_passed, false);
  assert.equal(report.scenarios[0].assignment_passed, false);
  assert.equal(report.scenarios[0].maximum_uncovered_l1, 1);
  assert.equal(report.scenarios[0].first_failure.uncoveredL1, 1);
  assert.equal(report.smallest_passing_sampled_candidate, null);
});

test('CLI arguments reject unknown options and preserve explicit paths', () => {
  const parsed = parseArguments([
    '--manifest', 'scenario.json',
    '--catalog', 'catalog.json',
    '--output', 'screen.json'
  ]);
  assert.match(parsed.manifestPath, /scenario\.json$/);
  assert.match(parsed.catalogPath, /catalog\.json$/);
  assert.match(parsed.outputPath, /screen\.json$/);
  assert.throws(() => parseArguments(['--unknown']), /unknown argument/);
});
