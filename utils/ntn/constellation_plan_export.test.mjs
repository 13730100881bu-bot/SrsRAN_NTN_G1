import assert from 'node:assert/strict';
import test from 'node:test';

import {
  ConstellationPlanExportError,
  exportDryRunSatellitePlan
} from './constellation_plan_export.mjs';
import {ACCESS_PROFILE_V1, ACCESS_PROFILE_V1_HASH, validatePlanV2} from './versioned_position_plan_v2.mjs';

const HASH_A = `sha256:${'a'.repeat(64)}`;
const HASH_B = `sha256:${'b'.repeat(64)}`;

const registry = {
  registry_version: 'mc-ntn-onboard-cell-registry-v1',
  satellites: [{
    satellite_id: 'P01-S01',
    cells: [
      {bank: 0, nci: '0xBE3CBAECA', pci: 0},
      {bank: 1, nci: '0x34AFAE80D', pci: 1}
    ]
  }]
};

const planningContext = {
  planningRunId: 'global-f1-seven-day-v1',
  catalog: {id: 'global-land-l1-v1', sha256: HASH_A},
  identityRegistry: {version: registry.registry_version, sha256: HASH_B},
  accessProfile: {id: ACCESS_PROFILE_V1.id, sha256: ACCESS_PROFILE_V1_HASH},
  catalogVersion: 1,
  scheduleVersion: 17,
  validFromUnixMs: 1767225600000,
  validUntilUnixMs: 1767225664000,
  activationEpochUnixMs: 1767225600000
};

function position(index) {
  return {
    id: `G${String(index + 1).padStart(6, '0')}`,
    lat: -20 + index * 0.01,
    lon: 30 + index * 0.01,
    childMask: index % 2 === 0 ? 127 : 1
  };
}

function assignment(index, bank = index % 2) {
  const identity = registry.satellites[0].cells[bank];
  return {
    positionId: position(index).id,
    satelliteId: 'P01-S01',
    cellBank: bank,
    nci: identity.nci,
    pci: identity.pci
  };
}

function exportPlan(overrides = {}) {
  return exportDryRunSatellitePlan({
    satelliteId: 'P01-S01',
    planningTimeUnixMs: planningContext.validFromUnixMs,
    visibleInventory: [position(1), position(0)],
    assignments: [assignment(1), assignment(0)],
    identityRegistry: registry,
    planningContext,
    ...overrides
  });
}

test('P01-S01 export is an explicitly non-runtime schema-v2 candidate plan', () => {
  const exported = exportPlan();

  assert.equal(exported.mode, 'dry_run');
  assert.equal(exported.plan_role, 'candidate_only');
  assert.equal(exported.runtime_activation_claimed, false);
  assert.equal(exported.assignment_sidecar.runtime_activation_claimed, false);
  assert.equal(exported.plan.satellite_id, 'P01-S01');
  assert.deepEqual(exported.plan.visible_l1_positions.map((entry) => entry.position_id), ['G000001', 'G000002']);
  assert.equal(validatePlanV2(exported.plan).contentHash, exported.plan.content_hash);
  assert.equal(exported.plan_validation.content_hash, exported.plan.content_hash);
});

test('257 visible positions remain in candidate inventory while assignment stays in the sidecar', () => {
  const visibleInventory = Array.from({length: 257}, (_, index) => position(index));
  const assignments = Array.from({length: 256}, (_, index) => assignment(index));
  const exported = exportPlan({visibleInventory: visibleInventory.reverse(), assignments: assignments.reverse()});

  assert.equal(exported.plan.visible_l1_positions.length, 257);
  assert.equal(exported.assignment_sidecar.assigned_l1_positions.length, 256);
  assert.deepEqual(exported.assignment_sidecar.visible_but_not_assigned_position_ids, ['G000257']);
  assert.equal(validatePlanV2(exported.plan).contentHash, exported.plan.content_hash);
});

test('input ordering does not change the plan or deterministic assignment sidecar', () => {
  const forward = exportPlan({
    visibleInventory: [position(0), position(1)],
    assignments: [assignment(0), assignment(1)]
  });
  const reverse = exportPlan({
    visibleInventory: [position(1), position(0)],
    assignments: [assignment(1), assignment(0)]
  });

  assert.deepEqual(reverse.plan, forward.plan);
  assert.deepEqual(reverse.assignment_sidecar, forward.assignment_sidecar);
});

test('cell identities always come from the registry and mismatches fail closed', () => {
  const exported = exportPlan({
    assignments: [{positionId: 'G000001', satelliteId: 'P01-S01', cellBank: 1}]
  });
  assert.deepEqual(exported.plan.onboard_cells, [
    {nci: Number(0x34AFAE80Dn), pci: 1},
    {nci: Number(0xBE3CBAECAn), pci: 0}
  ]);
  assert.deepEqual(exported.assignment_sidecar.assigned_l1_positions, [{
    position_id: 'G000001',
    cell_bank: 1,
    nci: Number(0x34AFAE80Dn),
    pci: 1
  }]);

  assert.throws(() => exportPlan({
    assignments: [{
      positionId: 'G000001',
      satelliteId: 'P01-S01',
      cellBank: 0,
      nci: registry.satellites[0].cells[1].nci,
      pci: 0
    }]
  }), (error) => error instanceof ConstellationPlanExportError && error.code === 'identity_mismatch');
});

test('an assignment cannot refer to a position outside the complete visible inventory', () => {
  assert.throws(() => exportPlan({
    assignments: [assignment(2)]
  }), (error) => error instanceof ConstellationPlanExportError && error.code === 'assignment_not_visible');
});
