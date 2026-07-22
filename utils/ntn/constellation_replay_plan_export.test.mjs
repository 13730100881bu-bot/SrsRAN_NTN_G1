import assert from 'node:assert/strict';
import test from 'node:test';

import {createCatalogGeometry} from './constellation_epoch.mjs';
import {
  ConstellationReplayPlanExportError,
  exportDryRunSatellitePlanFromReplay
} from './constellation_replay_plan_export.mjs';
import {ACCESS_PROFILE_V1, ACCESS_PROFILE_V1_HASH, validatePlanV2} from './versioned_position_plan_v2.mjs';

const HASH_A = `sha256:${'a'.repeat(64)}`;
const HASH_B = `sha256:${'b'.repeat(64)}`;
const EPOCH_UNIX_MS = 1_767_225_600_000;

const identityRegistry = {
  registry_version: 'mc-ntn-onboard-cell-registry-v1',
  satellites: [
    {
      satellite_id: 'P01-S01',
      cells: [
        {bank: 1, nci: '0x34AFAE80D', pci: 101},
        {bank: 0, nci: '0xBE3CBAECA', pci: 100}
      ]
    },
    {
      satellite_id: 'P01-S02',
      cells: [
        {bank: 0, nci: '0x000000003', pci: 102},
        {bank: 1, nci: '0x000000004', pci: 103}
      ]
    }
  ]
};

const scenario = {
  orbit_epoch_unix_ms: EPOCH_UNIX_MS,
  satellite_capacity: 256,
  cell_capacity: 128
};

const planningContext = {
  planningRunId: 'global-f1-seven-day-v1',
  catalog: {id: 'global-land-l1-v1', sha256: HASH_A},
  identityRegistry: {version: identityRegistry.registry_version, sha256: HASH_B},
  accessProfile: {id: ACCESS_PROFILE_V1.id, sha256: ACCESS_PROFILE_V1_HASH},
  catalogVersion: 1,
  scheduleVersion: 17,
  validFromUnixMs: EPOCH_UNIX_MS - 1_000,
  validUntilUnixMs: EPOCH_UNIX_MS + 10_000,
  activationEpochUnixMs: EPOCH_UNIX_MS
};

function catalog(count) {
  return createCatalogGeometry(Array.from({length: count}, (_, index) => ({
    id: `G${String(index + 1).padStart(6, '0')}`,
    lat: -20 + index * 0.01,
    lon: 30 + index * 0.01,
    childMask: index % 2 === 0 ? 127 : 1
  })));
}

function visible(positionIndex, startTimeUs = 0, endTimeUs = 100_000, threshold = 'release', satelliteId = 'P01-S01') {
  return {
    record_type: 'visibility_interval',
    threshold,
    start_time_us: startTimeUs,
    end_time_us: endTimeUs,
    position_id: `G${String(positionIndex + 1).padStart(6, '0')}`,
    satellite_id: satelliteId
  };
}

function assigned(positionIndex, bank = positionIndex % 2, startTimeUs = 0, endTimeUs = 100_000, satelliteId = 'P01-S01') {
  const cell = identityRegistry.satellites.find((item) => item.satellite_id === satelliteId).cells
    .find((item) => item.bank === bank);
  return {
    record_type: 'assignment_interval',
    start_time_us: startTimeUs,
    end_time_us: endTimeUs,
    position_id: `G${String(positionIndex + 1).padStart(6, '0')}`,
    satellite_id: satelliteId,
    cell_bank: bank,
    nci: cell.nci,
    pci: cell.pci
  };
}

function exportAt({
  timeUs = 0,
  catalogGeometry = catalog(2),
  visibilityIntervalRecords = [visible(0), visible(1)],
  assignmentIntervalRecords = [],
  satelliteId = 'P01-S01',
  registry = identityRegistry,
  replayScenario = scenario
} = {}) {
  return exportDryRunSatellitePlanFromReplay({
    timeUs,
    satelliteId,
    catalogGeometry,
    identityRegistry: registry,
    scenario: replayScenario,
    planningContext,
    visibilityIntervalRecords,
    assignmentIntervalRecords
  });
}

test('[start,end) boundaries select release visibility and assignments exactly', () => {
  const geometry = catalog(2);
  const visibilityIntervalRecords = [
    visible(0, 0, 10_000, 'release'),
    visible(0, 2_000, 8_000, 'entry'),
    visible(1, 10_000, 20_000, 'release')
  ];
  const assignmentIntervalRecords = [assigned(0, 0, 2_000, 10_000)];

  const atStart = exportAt({timeUs: 0, catalogGeometry: geometry, visibilityIntervalRecords, assignmentIntervalRecords});
  assert.deepEqual(atStart.plan.visible_l1_positions.map(({position_id}) => position_id), ['G000001']);
  assert.deepEqual(atStart.assignment_sidecar.assigned_l1_positions, []);

  const atAssignmentStart = exportAt({
    timeUs: 2_000,
    catalogGeometry: geometry,
    visibilityIntervalRecords,
    assignmentIntervalRecords
  });
  assert.deepEqual(atAssignmentStart.assignment_sidecar.assigned_l1_positions.map(({position_id}) => position_id), ['G000001']);

  const atFirstEnd = exportAt({
    timeUs: 10_000,
    catalogGeometry: geometry,
    visibilityIntervalRecords,
    assignmentIntervalRecords
  });
  assert.deepEqual(atFirstEnd.plan.visible_l1_positions.map(({position_id}) => position_id), ['G000002']);
  assert.deepEqual(atFirstEnd.assignment_sidecar.assigned_l1_positions, []);

  const atFinalEnd = exportAt({
    timeUs: 20_000,
    catalogGeometry: geometry,
    visibilityIntervalRecords,
    assignmentIntervalRecords
  });
  assert.deepEqual(atFinalEnd.plan.visible_l1_positions, []);
});

test('257 visible positions stay complete while 256 assignments use the two registry cells', () => {
  const geometry = catalog(257);
  const visibilityIntervalRecords = Array.from({length: 257}, (_, index) => [
    visible(index),
    visible(index, 0, 100_000, 'entry')
  ]).flat();
  const assignmentIntervalRecords = Array.from({length: 256}, (_, index) => assigned(index));
  const exported = exportAt({
    timeUs: 1_000,
    catalogGeometry: geometry,
    visibilityIntervalRecords: [...visibilityIntervalRecords].reverse(),
    assignmentIntervalRecords: [...assignmentIntervalRecords].reverse()
  });

  assert.equal(exported.plan.visible_l1_positions.length, 257);
  assert.equal(exported.assignment_sidecar.assigned_l1_positions.length, 256);
  assert.deepEqual(exported.assignment_sidecar.visible_but_not_assigned_position_ids, ['G000257']);
  assert.deepEqual(exported.assignment_sidecar.onboard_cells, [
    {bank: 0, nci: Number(0xBE3CBAECAn), pci: 100},
    {bank: 1, nci: Number(0x34AFAE80Dn), pci: 101}
  ]);
  assert.equal(exported.assignment_sidecar.assigned_l1_positions.filter(({cell_bank}) => cell_bank === 0).length, 128);
  assert.equal(exported.assignment_sidecar.assigned_l1_positions.filter(({cell_bank}) => cell_bank === 1).length, 128);
  assert.equal(validatePlanV2(exported.plan).contentHash, exported.plan.content_hash);
  assert.equal(exported.plan_validation.content_hash, exported.plan.content_hash);
});

test('unordered interval evidence produces identical plans and hashes', () => {
  const geometry = catalog(3);
  const visibilityIntervalRecords = [
    visible(2), visible(0), visible(1),
    visible(2, 0, 100_000, 'entry'),
    visible(0, 0, 100_000, 'entry'),
    visible(1, 0, 100_000, 'entry')
  ];
  const assignmentIntervalRecords = [assigned(2), assigned(0), assigned(1)];
  const forward = exportAt({catalogGeometry: geometry, visibilityIntervalRecords, assignmentIntervalRecords});
  const reverse = exportAt({
    catalogGeometry: geometry,
    visibilityIntervalRecords: [...visibilityIntervalRecords].reverse(),
    assignmentIntervalRecords: [...assignmentIntervalRecords].reverse()
  });

  assert.deepEqual(reverse, forward);
  assert.equal(reverse.plan.content_hash, forward.plan.content_hash);
  assert.equal(reverse.assignment_sidecar.content_hash, forward.assignment_sidecar.content_hash);
});

test('a pre-roll acquisition proves an incumbent carried through the audit boundary', () => {
  const exported = exportAt({
    timeUs: 7_000,
    catalogGeometry: catalog(1),
    visibilityIntervalRecords: [
      visible(0, 0, 10_000, 'release'),
      visible(0, 1_000, 4_000, 'entry')
    ],
    assignmentIntervalRecords: [
      {...assigned(0, 0, 2_000, 5_000), window: 'initialization'},
      {...assigned(0, 0, 5_000, 9_000), window: 'audit'}
    ]
  });

  assert.deepEqual(exported.assignment_sidecar.assigned_l1_positions.map(({position_id}) => position_id), ['G000001']);
});

test('overlap, invisibility, unknown references and identity mismatches fail closed', () => {
  const geometry = catalog(2);
  const cases = [
    {
      code: 'overlapping_visibility',
      visibilityIntervalRecords: [visible(0, 0, 10_000), visible(0, 9_999, 20_000)]
    },
    {
      code: 'overlapping_assignment',
      visibilityIntervalRecords: [visible(0, 0, 20_000), visible(0, 0, 20_000, 'release', 'P01-S02')],
      assignmentIntervalRecords: [assigned(0, 0, 0, 10_000), assigned(0, 0, 9_999, 20_000, 'P01-S02')]
    },
    {
      code: 'assignment_not_visible',
      visibilityIntervalRecords: [visible(0, 0, 10_000), visible(0, 0, 10_000, 'entry')],
      assignmentIntervalRecords: [assigned(0, 0, 0, 10_001)]
    },
    {
      code: 'assignment_without_entry',
      visibilityIntervalRecords: [visible(0, 0, 10_000)],
      assignmentIntervalRecords: [assigned(0, 0, 1_000, 2_000)]
    },
    {
      code: 'entry_not_release_visible',
      visibilityIntervalRecords: [visible(0, 0, 10_000), visible(0, 9_000, 11_000, 'entry')]
    },
    {
      code: 'unknown_position',
      visibilityIntervalRecords: [visible(2)]
    },
    {
      code: 'unknown_identity',
      visibilityIntervalRecords: [visible(0, 0, 10_000, 'release', 'P09-S09')]
    },
    {
      code: 'identity_mismatch',
      visibilityIntervalRecords: [visible(0)],
      assignmentIntervalRecords: [{...assigned(0), nci: '0x000000004'}]
    }
  ];

  for (const {code, ...overrides} of cases) {
    assert.throws(
      () => exportAt({catalogGeometry: geometry, ...overrides}),
      (error) => error instanceof ConstellationReplayPlanExportError && error.code === code,
      code
    );
  }
});

test('identity is selected only from the requested satellite registry entry', () => {
  const exported = exportAt({
    satelliteId: 'P01-S02',
    visibilityIntervalRecords: [
      visible(0, 0, 100_000, 'release', 'P01-S02'),
      visible(0, 0, 100_000, 'entry', 'P01-S02')
    ],
    assignmentIntervalRecords: [assigned(0, 1, 0, 100_000, 'P01-S02')]
  });

  assert.deepEqual(exported.assignment_sidecar.assigned_l1_positions, [{
    position_id: 'G000001',
    cell_bank: 1,
    nci: 4,
    pci: 103
  }]);
  assert.deepEqual(exported.assignment_sidecar.onboard_cells, [
    {bank: 0, nci: 3, pci: 102},
    {bank: 1, nci: 4, pci: 103}
  ]);
});
