import assert from 'node:assert/strict';
import {mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {gzipSync} from 'node:zlib';

import {
  computePlannerSourceHash,
  finalizeSlabVisibilityRun,
  runSlabVisibilityStream
} from './constellation_audit.mjs';
import {
  canonicalJson,
  readSlabJournalEntries,
  readMergedEvidenceChunks,
  scenarioInputHash,
  sha256Bytes,
  writeNdjsonGzipChunk
} from './constellation_audit_io.mjs';
import {
  exportDryRunSatellitePlanFromReplay
} from './constellation_replay_plan_export.mjs';
import {
  runStreamingAssignmentLedger,
  StreamingAssignmentLedgerError
} from './constellation_streaming_assignment_ledger.mjs';
import {
  ACCESS_PROFILE_V1,
  ACCESS_PROFILE_V1_HASH,
  validatePlanV2
} from './versioned_position_plan_v2.mjs';

const INPUT_HASH = `sha256:${'1'.repeat(64)}`;
const SOURCE_HASH = `sha256:${'2'.repeat(64)}`;
const OTHER_SOURCE_HASH = `sha256:${'3'.repeat(64)}`;

const registry = {
  registry_version: 'mc-ntn-onboard-cell-registry-v1',
  satellites: [
    {
      satellite_id: 'P01-S01',
      cells: [
        {bank: 0, nci: '0x000000001', pci: 1},
        {bank: 1, nci: '0x000000002', pci: 2}
      ]
    },
    {
      satellite_id: 'P01-S02',
      cells: [
        {bank: 0, nci: '0x000000003', pci: 3},
        {bank: 1, nci: '0x000000004', pci: 4}
      ]
    }
  ]
};

function positionId(index) {
  return `G${String(index + 1).padStart(6, '0')}`;
}

function initialState(position_id, satellite_id) {
  return {
    record_type: 'visibility_initial_state',
    kind: 'initial_state',
    window: 'initialization',
    time_us: -10,
    position_id,
    satellite_id,
    above_entry: true,
    above_release: true
  };
}

function event(time_us, kind, position_id, satellite_id) {
  return {
    record_type: 'visibility_event',
    window: time_us < 0 ? 'initialization' : 'audit',
    time_us,
    kind,
    threshold: kind.startsWith('entry_') ? 'entry' : 'release',
    position_id,
    satellite_id
  };
}

function interval(threshold, start_time_us, end_time_us, position_id, satellite_id, window) {
  return {
    record_type: 'visibility_interval',
    threshold,
    start_time_us,
    end_time_us,
    position_id,
    satellite_id,
    window
  };
}

function shardLabel(index, count) {
  return `shard-${String(index).padStart(5, '0')}-of-${String(count).padStart(5, '0')}`;
}

async function writeLedger(runDirectory, recordsByShardAndPart, slabs = [[-10, 0], [0, 10]]) {
  const shardCount = recordsByShardAndPart.length;
  return Promise.all(recordsByShardAndPart.map(async (recordsByPart, shardIndex) => {
    assert.equal(recordsByPart.length, slabs.length);
    const entries = [];
    for (let partIndex = 0; partIndex < slabs.length; partIndex += 1) {
      const eventChunk = await writeNdjsonGzipChunk({
        runDirectory,
        chunkIndex: partIndex,
        relativeDirectory: `slab-events/${shardLabel(shardIndex, shardCount)}`,
        records: recordsByPart[partIndex].filter(({record_type: type}) => type !== 'visibility_interval')
      });
      const intervalChunk = await writeNdjsonGzipChunk({
        runDirectory,
        chunkIndex: partIndex,
        relativeDirectory: `slab-intervals/${shardLabel(shardIndex, shardCount)}`,
        records: recordsByPart[partIndex].filter(({record_type: type}) => type === 'visibility_interval')
      });
      entries.push({
        schema_version: 2,
        input_hash: INPUT_HASH,
        part_index: partIndex,
        shard_index: shardIndex,
        shard_count: shardCount,
        slab_start_time_us: slabs[partIndex][0],
        slab_end_time_us: slabs[partIndex][1],
        event_chunk: eventChunk,
        interval_chunk: intervalChunk
      });
    }
    return entries;
  }));
}

function fixedGzipRecords(records) {
  const text = records.length === 0 ? '' : `${records.map(canonicalJson).join('\n')}\n`;
  const uncompressed = Buffer.from(text, 'utf8');
  const compressed = gzipSync(uncompressed, {level: 9});
  compressed.writeUInt32LE(0, 4);
  compressed[8] = 2;
  compressed[9] = 255;
  return {compressed, uncompressed};
}

async function writeProducerFixture(t) {
  const root = await mkdtemp(join(tmpdir(), 'ntn-stream-producer-'));
  t.after(() => rm(root, {recursive: true, force: true}));
  const cells = [{id: 'G000001', lat: 0, lon: 0, childMask: 127}];
  const catalog = {
    metadata: {
      version: 'tiny-global-v1',
      counts: {l1: cells.length},
      integrity: {
        algorithm: 'SHA-256',
        scope: 'UTF-8 JSON.stringify(cells)',
        sha256: sha256Bytes(JSON.stringify(cells))
      }
    },
    cells
  };
  const identityRegistry = {
    registry_version: 'tiny-registry-v1',
    satellites: [{
      satellite_id: 'P01-S01',
      cells: [
        {bank: 0, nci: '0x000000001', pci: 1},
        {bank: 1, nci: '0x000000002', pci: 2}
      ]
    }]
  };
  const catalogBytes = Buffer.from(JSON.stringify(catalog), 'utf8');
  const registryBytes = Buffer.from(JSON.stringify(identityRegistry), 'utf8');
  const catalogPath = join(root, 'catalog.json');
  const registryPath = join(root, 'registry.json');
  await writeFile(catalogPath, catalogBytes);
  await writeFile(registryPath, registryBytes);
  const startUnixMs = Date.UTC(2026, 0, 1);
  const sourceHash = await computePlannerSourceHash();
  const manifest = {
    schema_version: 1,
    audit_kind: 'ntn_constellation_planning_evidence',
    run_id: 'tiny-stream-producer-consumer',
    engine: {
      name: 'ntn-headless-planner',
      version: '1.0.0',
      source_revision: 'test-worktree',
      source_sha256: sourceHash
    },
    scenario: {
      scenario_id: 'tiny-f1',
      altitude_km: 500,
      inclination_deg: 0,
      planes: 1,
      satellites_per_plane: 1,
      phase_factor: 0,
      raan_offset_deg: 0,
      phase_offset_deg: 0,
      orbit_epoch_unix_ms: startUnixMs,
      start_unix_ms: startUnixMs,
      duration_ms: 120000,
      pre_roll_ms: 6000000,
      entry_elevation_deg: 45,
      exit_elevation_deg: 42,
      root_tolerance_ms: 10,
      scan_step_ms: 60000,
      slab_duration_ms: 120000,
      slab_boundary_padding_ms: 120000,
      slab_scan_step_ms: 30000,
      earth_radius_km: 6378.137,
      earth_mu_km3_s2: 398600.4418,
      earth_rotation_rad_per_second: 7.2921159e-5,
      margin_tolerance: 1e-12,
      max_evaluations: 1000000,
      satellite_shard_count: 1,
      positions_per_chunk: 1,
      satellite_capacity: 256,
      cell_capacity: 128
    },
    inputs: {
      catalog: {
        id: catalog.metadata.version,
        sha256: catalog.metadata.integrity.sha256,
        file_sha256: sha256Bytes(catalogBytes)
      },
      identity_registry: {
        version: identityRegistry.registry_version,
        sha256: sha256Bytes(registryBytes)
      },
      access_profile: {id: ACCESS_PROFILE_V1.id, sha256: ACCESS_PROFILE_V1_HASH}
    }
  };
  const scenarioPath = join(root, 'scenario.json');
  await writeFile(scenarioPath, JSON.stringify(manifest), 'utf8');
  return {
    manifest,
    identityRegistry,
    options: {
      scenarioPath,
      catalogPath,
      registryPath,
      outputRoot: join(root, 'outputs')
    }
  };
}

function commonOptions(runDirectory, journals, positionIds, overrides = {}) {
  return {
    runDirectory,
    journals,
    inputHash: INPUT_HASH,
    sourceHash: SOURCE_HASH,
    positionIds,
    registry,
    simulationStartTimeUs: -10,
    auditStartTimeUs: 0,
    auditEndTimeUs: 10,
    satelliteCapacity: 256,
    cellCapacity: 128,
    ...overrides
  };
}

async function allOutputRecords(result, runDirectory) {
  const records = [];
  for (const chunk of result.chunks) {
    records.push(...await readMergedEvidenceChunks({runDirectory, chunks: [chunk]}));
  }
  return records;
}

function steadyRecords(positionIds) {
  return [[
    positionIds.map((id) => initialState(id, 'P01-S01')),
    positionIds.flatMap((id) => [
      interval('entry', -10, 0, id, 'P01-S01', 'initialization'),
      interval('release', -10, 0, id, 'P01-S01', 'initialization'),
      interval('entry', 0, 10, id, 'P01-S01', 'audit'),
      interval('release', 0, 10, id, 'P01-S01', 'audit')
    ])
  ]];
}

test('unordered shards, start-boundary exits and resume produce identical evidence hashes', async () => {
  const fullDirectory = await mkdtemp(join(tmpdir(), 'ntn-stream-full-'));
  const resumedDirectory = await mkdtemp(join(tmpdir(), 'ntn-stream-resume-'));
  const records = [
    [
      [initialState('G000001', 'P01-S01')],
      [
        event(0, 'release_exit', 'G000001', 'P01-S01'),
        interval('entry', -10, 0, 'G000001', 'P01-S01', 'initialization'),
        interval('release', -10, 0, 'G000001', 'P01-S01', 'initialization')
      ]
    ],
    [
      [],
      [
        event(0, 'entry_enter', 'G000001', 'P01-S02'),
        interval('entry', 0, 10, 'G000001', 'P01-S02', 'audit'),
        interval('release', 0, 10, 'G000001', 'P01-S02', 'audit')
      ]
    ]
  ];
  const fullJournals = await writeLedger(fullDirectory, records);
  const resumeJournals = await writeLedger(resumedDirectory, records);
  const full = await runStreamingAssignmentLedger(commonOptions(
    fullDirectory,
    [...fullJournals].reverse(),
    ['G000001']
  ));
  const partial = await runStreamingAssignmentLedger(commonOptions(
    resumedDirectory,
    resumeJournals,
    ['G000001'],
    {maxParts: 1}
  ));
  assert.equal(partial.complete, false);
  assert.equal(partial.chunks.length, 0);
  const resumed = await runStreamingAssignmentLedger(commonOptions(
    resumedDirectory,
    [...resumeJournals].reverse(),
    ['G000001'],
    {resume: true}
  ));

  assert.equal(full.outputHash, resumed.outputHash);
  assert.deepEqual(full.chunks, resumed.chunks);
  assert.deepEqual(full.checkpoint, resumed.checkpoint);
  assert.deepEqual(full.finalAssignments, resumed.finalAssignments);
  assert.equal(full.stickyStart.assignments[0].satelliteId, 'P01-S01');
  assert.equal(resumed.stickyStart.assignments[0].satelliteId, 'P01-S01');
  assert.equal(full.finalAssignments[0].satelliteId, 'P01-S02');
  assert.equal(full.metrics.owner_change_count, 1);
  assert.equal(full.metrics.handover_count, 1);

  const output = await allOutputRecords(full, fullDirectory);
  const provenance = output.find(({record_type: type}) => type === 'assignment_initialization');
  const transition = output.find(({record_type: type}) => type === 'assignment_transition');
  assert.equal(provenance.initialization_method, 'pre_roll_event_replay');
  assert.equal(provenance.input_hash, INPUT_HASH);
  assert.equal(provenance.source_hash, SOURCE_HASH);
  assert.equal(transition.kind, 'handover');
  assert.equal(transition.before.satelliteId, 'P01-S01');
  assert.equal(transition.after.satelliteId, 'P01-S02');
  assert.deepEqual(full.metrics.raw_visible_peak_by_satellite, [
    {satellite_id: 'P01-S01', count: 0},
    {satellite_id: 'P01-S02', count: 1}
  ]);
});

test('resume rejects a changed planner source hash', async () => {
  const runDirectory = await mkdtemp(join(tmpdir(), 'ntn-stream-source-'));
  const journals = await writeLedger(runDirectory, steadyRecords(['G000001']));
  await runStreamingAssignmentLedger(commonOptions(runDirectory, journals, ['G000001'], {maxParts: 1}));

  await assert.rejects(
    runStreamingAssignmentLedger(commonOptions(runDirectory, journals, ['G000001'], {
      sourceHash: OTHER_SOURCE_HASH,
      resume: true
    })),
    (error) => error instanceof StreamingAssignmentLedgerError && error.code === 'checkpoint_mismatch'
  );
});

test('a rehashed shard with non-monotonic event order is rejected', async () => {
  const runDirectory = await mkdtemp(join(tmpdir(), 'ntn-stream-order-'));
  const journals = await writeLedger(runDirectory, steadyRecords(['G000001', 'G000002']));
  const chunk = journals[0][0].event_chunk;
  const {compressed, uncompressed} = fixedGzipRecords([
    initialState('G000002', 'P01-S01'),
    initialState('G000001', 'P01-S01')
  ]);
  await writeFile(join(runDirectory, ...chunk.path.split('/')), compressed);
  journals[0][0].event_chunk = {
    ...chunk,
    sha256: sha256Bytes(compressed),
    size_bytes: compressed.length,
    record_count: 2,
    uncompressed_bytes: uncompressed.length
  };

  await assert.rejects(
    runStreamingAssignmentLedger(commonOptions(
      runDirectory,
      journals,
      ['G000001', 'G000002']
    )),
    (error) => error instanceof StreamingAssignmentLedgerError &&
      error.code === 'invalid_evidence' &&
      error.message.includes('canonical event order')
  );
});

test('missing and truncated shard chunks fail closed', async () => {
  for (const damage of ['missing', 'truncated']) {
    const runDirectory = await mkdtemp(join(tmpdir(), `ntn-stream-${damage}-`));
    const journals = await writeLedger(runDirectory, steadyRecords(['G000001']));
    const path = join(runDirectory, ...journals[0][0].event_chunk.path.split('/'));
    if (damage === 'missing') {
      await rm(path);
    } else {
      const bytes = await readFile(path);
      await writeFile(path, bytes.subarray(0, bytes.length - 5));
    }
    await assert.rejects(
      runStreamingAssignmentLedger(commonOptions(runDirectory, journals, ['G000001'])),
      (error) => error instanceof StreamingAssignmentLedgerError && error.code === 'invalid_evidence',
      damage
    );
  }
});

test('the real slab visibility producer feeds the streaming assignment ledger', async (t) => {
  const fixture = await writeProducerFixture(t);
  const produced = await runSlabVisibilityStream({
    ...fixture.options,
    shardIndex: 0,
    shardCount: 1
  });
  const finalized = await finalizeSlabVisibilityRun({...fixture.options, shardCount: 1});
  const inputHash = scenarioInputHash(fixture.manifest);
  const journals = [await readSlabJournalEntries({
    runDirectory: produced.runDirectory,
    relativeDirectory: 'slab-journal/shard-00000-of-00001',
    expectedInputHash: inputHash
  })];
  const assigned = await runStreamingAssignmentLedger({
    runDirectory: produced.runDirectory,
    journals,
    inputHash,
    sourceHash: fixture.manifest.engine.source_sha256,
    positionIds: ['G000001'],
    registry: fixture.identityRegistry,
    simulationStartTimeUs: -6_000_000_000,
    auditStartTimeUs: 0,
    auditEndTimeUs: 120_000_000,
    satelliteCapacity: 256,
    cellCapacity: 128
  });

  assert.equal(produced.shardComplete, true);
  assert.equal(produced.geometryStreamComplete, false);
  assert.equal(finalized.geometryStreamComplete, true);
  assert.equal(finalized.evidenceVerified, true);
  assert.equal(assigned.complete, true);
  assert.equal(assigned.nextInputPartIndex, journals[0].length);
  assert.equal(assigned.recordCounts.initialization_record_count, 1);
  assert.ok(assigned.chunks.length > 0);
  assert.ok(assigned.finalCandidateInventory.length <= 1);
  assert.ok(assigned.finalAssignments.length <= 1);
  const output = await allOutputRecords(assigned, produced.runDirectory);
  const initialization = output.find(({record_type: type}) => type === 'assignment_initialization');
  assert.equal(initialization.input_hash, inputHash);
  assert.equal(initialization.source_hash, fixture.manifest.engine.source_sha256);
});

test('a pre-roll owner carried below entry threshold remains exportable at audit start', async () => {
  const runDirectory = await mkdtemp(join(tmpdir(), 'ntn-stream-carry-in-'));
  const initialInvisible = {
    ...initialState('G000001', 'P01-S01'),
    above_entry: false,
    above_release: false
  };
  const journals = await writeLedger(runDirectory, [[
    [
      initialInvisible,
      event(-9, 'release_enter', 'G000001', 'P01-S01'),
      event(-8, 'entry_enter', 'G000001', 'P01-S01'),
      event(-2, 'entry_exit', 'G000001', 'P01-S01'),
      interval('release', -9, 0, 'G000001', 'P01-S01', 'initialization'),
      interval('entry', -8, -2, 'G000001', 'P01-S01', 'initialization')
    ],
    [interval('release', 0, 10, 'G000001', 'P01-S01', 'audit')]
  ]]);
  const result = await runStreamingAssignmentLedger(commonOptions(
    runDirectory,
    journals,
    ['G000001']
  ));
  const output = await allOutputRecords(result, runDirectory);
  const assignmentRecords = output.filter(({record_type: type}) => type === 'assignment_interval');
  assert.deepEqual(assignmentRecords.map(({start_time_us, end_time_us}) => [start_time_us, end_time_us]), [
    [-8, 0],
    [0, 10]
  ]);
  assert.deepEqual(assignmentRecords.map(({window}) => window), ['initialization', 'audit']);

  const visibilityRecords = [];
  for (let partIndex = 0; partIndex < journals[0].length; partIndex += 1) {
    visibilityRecords.push(...await readMergedEvidenceChunks({
      runDirectory,
      chunks: [journals[0][partIndex].interval_chunk]
    }));
  }
  const epochUnixMs = 1_767_225_600_000;
  const exported = exportDryRunSatellitePlanFromReplay({
    timeUs: 5,
    satelliteId: 'P01-S01',
    catalogGeometry: {
      positions: [{
        positionId: 'G000001',
        latitudeDeg: 0,
        longitudeDeg: 0,
        childMask: 127
      }]
    },
    identityRegistry: registry,
    scenario: {orbit_epoch_unix_ms: epochUnixMs, satellite_capacity: 256, cell_capacity: 128},
    planningContext: {
      planningRunId: 'streaming-carry-in-test',
      catalog: {id: 'global-land-l1-v1', sha256: `sha256:${'a'.repeat(64)}`},
      identityRegistry: {version: registry.registry_version, sha256: `sha256:${'b'.repeat(64)}`},
      accessProfile: {id: ACCESS_PROFILE_V1.id, sha256: ACCESS_PROFILE_V1_HASH},
      catalogVersion: 1,
      scheduleVersion: 1,
      validFromUnixMs: epochUnixMs,
      validUntilUnixMs: epochUnixMs + 1000,
      activationEpochUnixMs: epochUnixMs
    },
    visibilityIntervalRecords: visibilityRecords,
    assignmentIntervalRecords: assignmentRecords
  });

  assert.deepEqual(
    exported.assignment_sidecar.assigned_l1_positions.map(({position_id}) => position_id),
    ['G000001']
  );
});

test('large irrelevant entry events do not rescan or mutate the raw release inventory', async () => {
  const runDirectory = await mkdtemp(join(tmpdir(), 'ntn-stream-raw-fast-'));
  const positionIds = Array.from({length: 2000}, (_, index) => positionId(index));
  const journals = await writeLedger(runDirectory, [[
    [initialState('G000001', 'P01-S01')],
    [
      ...positionIds.map((id) => event(1, 'entry_exit', id, 'P01-S02')),
      interval('entry', -10, 0, 'G000001', 'P01-S01', 'initialization'),
      interval('release', -10, 0, 'G000001', 'P01-S01', 'initialization'),
      interval('entry', 0, 10, 'G000001', 'P01-S01', 'audit'),
      interval('release', 0, 10, 'G000001', 'P01-S01', 'audit')
    ]
  ]]);
  const result = await runStreamingAssignmentLedger(commonOptions(runDirectory, journals, positionIds));

  assert.equal(result.metrics.raw_visibility_event_count, 2000);
  assert.equal(result.metrics.raw_visibility_pair_mutation_count, 0);
  assert.ok(result.metrics.raw_visibility_peak_observation_count <= registry.satellites.length * 2);
  assert.equal(result.metrics.raw_visible_peak_by_satellite[0].count, 1);
  assert.equal(result.metrics.assignment_metric_transition_count, 0);
  assert.ok(result.metrics.assignment_peak_observation_count <= registry.satellites.length * 3);
});

test('failure duration remains exact above Number.MAX_SAFE_INTEGER', async () => {
  const runDirectory = await mkdtemp(join(tmpdir(), 'ntn-stream-duration-'));
  const auditEndTimeUs = 5_000_000_000_000_000;
  const journals = await writeLedger(runDirectory, [[[], []]], [[-10, 0], [0, auditEndTimeUs]]);
  const result = await runStreamingAssignmentLedger(commonOptions(
    runDirectory,
    journals,
    ['G000001', 'G000002'],
    {auditEndTimeUs}
  ));

  assert.equal(
    result.metrics.failure_duration_us_by_reason.no_visible_candidate,
    '10000000000000000'
  );
  assert.ok(BigInt(result.metrics.failure_duration_us_by_reason.no_visible_candidate) > BigInt(Number.MAX_SAFE_INTEGER));
});

for (const count of [0, 1, 128, 256, 257]) {
  test(`${count} visible L1 positions keep complete inventory and explicit capacity outcome`, async () => {
    const runDirectory = await mkdtemp(join(tmpdir(), `ntn-stream-${count}-`));
    const positionIds = Array.from({length: count}, (_, index) => positionId(index));
    const journals = await writeLedger(runDirectory, steadyRecords(positionIds));
    const result = await runStreamingAssignmentLedger(commonOptions(runDirectory, journals, positionIds));

    assert.equal(result.complete, true);
    assert.equal(result.finalCandidateInventory.length, count);
    assert.equal(result.finalAssignments.length, Math.min(count, 256));
    assert.equal(result.finalPlan.failure_counts.schedule_overflow, count === 257 ? 1 : 0);
    assert.equal(result.finalPlan.failure_counts.no_visible_candidate, 0);
    assert.equal(result.finalPlan.failure_counts.cell_partition_overflow, 0);
    assert.equal(result.metrics.raw_visible_peak_by_satellite[0].count, count);
    assert.equal(result.metrics.assigned_peak_by_satellite[0].count, Math.min(count, 256));
    assert.equal(result.metrics.failure_peak_counts.schedule_overflow, count === 257 ? 1 : 0);
    if (count === 257) {
      const output = await allOutputRecords(result, runDirectory);
      const failures = output.filter(({record_type: type}) => type === 'unassigned_interval');
      assert.deepEqual(failures, [{
        record_type: 'unassigned_interval',
        start_time_us: 0,
        end_time_us: 10,
        position_id: 'G000257',
        reason: 'schedule_overflow'
      }]);
      assert.deepEqual(
        result.finalAssignments.reduce((counts, assignment) => {
          counts[assignment.cellBank] += 1;
          return counts;
        }, [0, 0]),
        [128, 128]
      );
      assert.equal(result.metrics.failure_duration_us_by_reason.schedule_overflow, '10');
    }
  });
}

test('sticky audit start feeds the strict schema-v2 replay plan exporter', async () => {
  const runDirectory = await mkdtemp(join(tmpdir(), 'ntn-stream-export-'));
  const positionIds = ['G000001'];
  const journals = await writeLedger(runDirectory, steadyRecords(positionIds));
  const result = await runStreamingAssignmentLedger(commonOptions(runDirectory, journals, positionIds));
  const assignmentRecords = (await allOutputRecords(result, runDirectory))
    .filter(({record_type: type}) => type === 'assignment_interval');
  const visibilityRecords = [];
  for (let partIndex = 0; partIndex < journals[0].length; partIndex += 1) {
    visibilityRecords.push(...(await readMergedEvidenceChunks({
      runDirectory,
      chunks: journals.map((journal) => journal[partIndex].interval_chunk)
    })).filter(({record_type: type}) => type === 'visibility_interval'));
  }
  const epochUnixMs = 1_767_225_600_000;
  const exported = exportDryRunSatellitePlanFromReplay({
    timeUs: 0,
    satelliteId: 'P01-S01',
    catalogGeometry: {
      positions: [{
        positionId: 'G000001',
        latitudeDeg: 10,
        longitudeDeg: 20,
        childMask: 127
      }]
    },
    identityRegistry: registry,
    scenario: {orbit_epoch_unix_ms: epochUnixMs, satellite_capacity: 256, cell_capacity: 128},
    planningContext: {
      planningRunId: 'streaming-replay-test',
      catalog: {id: 'global-land-l1-v1', sha256: `sha256:${'a'.repeat(64)}`},
      identityRegistry: {version: registry.registry_version, sha256: `sha256:${'b'.repeat(64)}`},
      accessProfile: {id: ACCESS_PROFILE_V1.id, sha256: ACCESS_PROFILE_V1_HASH},
      catalogVersion: 1,
      scheduleVersion: 1,
      validFromUnixMs: epochUnixMs,
      validUntilUnixMs: epochUnixMs + 1000,
      activationEpochUnixMs: epochUnixMs
    },
    visibilityIntervalRecords: visibilityRecords,
    assignmentIntervalRecords: assignmentRecords
  });

  assert.equal(result.stickyStart.assignments[0].satelliteId, 'P01-S01');
  assert.equal(exported.assignment_sidecar.assigned_l1_positions.length, 1);
  assert.equal(exported.assignment_sidecar.assigned_l1_positions[0].nci, 1);
  assert.equal(validatePlanV2(exported.plan).contentHash, exported.plan.content_hash);
  assert.equal(Object.hasOwn(result.checkpoint, 'epochs'), false);
  assert.equal(Object.hasOwn(result.checkpoint, 'chunks'), false);
  assert.equal(Object.hasOwn(result.checkpoint.replay_checkpoint, 'closedIntervals'), false);
});
