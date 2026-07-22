import assert from 'node:assert/strict';
import {readFile, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {gunzipSync} from 'node:zlib';
import {mkdtemp} from 'node:fs/promises';
import test from 'node:test';

import {
  AuditIoError,
  auditRunDirectory,
  canonicalJson,
  canonicalJsonHash,
  checkpointFileSize,
  createAuditSummary,
  loadCheckpoint,
  readCatalog,
  readIdentityRegistry,
  readSlabJournalEntries,
  saveCheckpoint,
  SLAB_JOURNAL_SCHEMA_VERSION,
  scenarioInputHash,
  sha256Bytes,
  validateAuditSummary,
  validateScenarioManifest,
  verifyEvidenceBundle,
  writeEvidenceBundle,
  writeNdjsonGzipChunk,
  writeSlabJournalEntry
} from './constellation_audit_io.mjs';

const HASH_A = `sha256:${'a'.repeat(64)}`;
const HASH_B = `sha256:${'b'.repeat(64)}`;
const HASH_C = `sha256:${'c'.repeat(64)}`;

function makeScenarioManifest() {
  return {
    schema_version: 1,
    audit_kind: 'ntn_constellation_planning_evidence',
    run_id: 'global-f1-seven-day-v1',
    engine: {
      name: 'ntn-headless-planner',
      version: '1.0.0',
      source_revision: '0123456789abcdef',
      source_sha256: HASH_A
    },
    scenario: {
      scenario_id: 'walker-60-42-84-f1',
      altitude_km: 500,
      inclination_deg: 60,
      planes: 42,
      satellites_per_plane: 84,
      phase_factor: 1,
      raan_offset_deg: 0,
      phase_offset_deg: 0,
      orbit_epoch_unix_ms: 1767225600000,
      start_unix_ms: 1767225600000,
      duration_ms: 7 * 24 * 60 * 60 * 1000,
      pre_roll_ms: 6000000,
      entry_elevation_deg: 45,
      exit_elevation_deg: 42,
      root_tolerance_ms: 1,
      scan_step_ms: 60000,
      slab_duration_ms: 120000,
      slab_boundary_padding_ms: 120000,
      slab_scan_step_ms: 30000,
      earth_radius_km: 6378.137,
      earth_mu_km3_s2: 398600.4418,
      earth_rotation_rad_per_second: 7.2921159e-5,
      margin_tolerance: 1e-12,
      max_evaluations: 1000000,
      satellite_shard_count: 14,
      positions_per_chunk: 8,
      satellite_capacity: 256,
      cell_capacity: 128
    },
    inputs: {
      catalog: {id: 'global-land-l1-v1', sha256: HASH_A, file_sha256: HASH_B},
      identity_registry: {version: 'mc-ntn-onboard-cell-registry-v1', sha256: HASH_B},
      access_profile: {id: 'ntn-access-16a-64d-v1', sha256: HASH_C}
    }
  };
}

async function makeTemporaryDirectory(t) {
  const directory = await mkdtemp(join(tmpdir(), 'ntn-constellation-audit-'));
  t.after(async () => rm(directory, {recursive: true, force: true}));
  return directory;
}

function hasCode(code) {
  return (error) => error instanceof AuditIoError && error.code === code;
}

test('canonical JSON and hashes ignore object key insertion order and reject non-JSON values', () => {
  const left = {z: [3, {b: true, a: 'x'}], a: -0};
  const right = {a: 0, z: [3, {a: 'x', b: true}]};
  assert.equal(canonicalJson(left), '{"a":0,"z":[3,{"a":"x","b":true}]}');
  assert.equal(canonicalJson(left), canonicalJson(right));
  assert.equal(canonicalJsonHash(left), canonicalJsonHash(right));
  assert.throws(() => canonicalJson({value: Number.NaN}), hasCode('non_canonical_value'));
  assert.throws(() => canonicalJson({value: undefined}), hasCode('non_canonical_value'));
  const cycle = {};
  cycle.self = cycle;
  assert.throws(() => canonicalJson(cycle), hasCode('non_canonical_value'));
});

test('scenario manifest is exact-keyed, normalized and fully bound into its input hash', () => {
  const manifest = makeScenarioManifest();
  manifest.inputs.catalog.sha256 = 'A'.repeat(64);
  const normalized = validateScenarioManifest(manifest);
  assert.equal(normalized.inputs.catalog.sha256, HASH_A);
  assert.equal(normalized.scenario.duration_ms, 604800000);

  const reordered = {
    inputs: manifest.inputs,
    scenario: manifest.scenario,
    engine: manifest.engine,
    run_id: manifest.run_id,
    audit_kind: manifest.audit_kind,
    schema_version: manifest.schema_version
  };
  assert.equal(scenarioInputHash(manifest), scenarioInputHash(reordered));
  for (const field of ['slab_duration_ms', 'slab_boundary_padding_ms', 'slab_scan_step_ms']) {
    const changed = structuredClone(manifest);
    changed.scenario[field] += field === 'slab_scan_step_ms' ? 30000 : 120000;
    if (field === 'slab_scan_step_ms') changed.scenario.slab_duration_ms = 240000;
    assert.notEqual(scenarioInputHash(changed), scenarioInputHash(manifest), field);
  }

  const unknown = structuredClone(manifest);
  unknown.scenario.step_seconds = 120;
  assert.throws(() => validateScenarioManifest(unknown), hasCode('unknown_field'));
  const missing = structuredClone(manifest);
  delete missing.inputs.access_profile;
  assert.throws(() => validateScenarioManifest(missing), hasCode('missing_field'));
  const missingSlabField = structuredClone(manifest);
  delete missingSlabField.scenario.slab_duration_ms;
  assert.throws(() => validateScenarioManifest(missingSlabField), hasCode('missing_field'));
  const unalignedSlab = structuredClone(manifest);
  unalignedSlab.scenario.slab_duration_ms = 100000;
  assert.throws(() => validateScenarioManifest(unalignedSlab), hasCode('invalid_value'));
  const unalignedPreRoll = structuredClone(manifest);
  unalignedPreRoll.scenario.pre_roll_ms += 1;
  assert.throws(() => validateScenarioManifest(unalignedPreRoll), hasCode('invalid_value'));
  const invalidHysteresis = structuredClone(manifest);
  invalidHysteresis.scenario.exit_elevation_deg = 45;
  assert.throws(() => validateScenarioManifest(invalidHysteresis), hasCode('invalid_value'));
  assert.match(auditRunDirectory(manifest.run_id, 'outputs/ntn_exact'), /outputs[\\/]ntn_exact[\\/]global-f1-seven-day-v1$/);
});

test('catalog reader verifies identity, embedded content hash and every L1 record', async (t) => {
  const directory = await makeTemporaryDirectory(t);
  const path = join(directory, 'catalog.json');
  const cells = [
    {id: 'G000001', lat: 10, lon: 20, childMask: 127},
    {id: 'G000002', lat: -10, lon: -20, childMask: 1}
  ];
  const cellsHash = sha256Bytes(JSON.stringify(cells));
  const catalog = {
    metadata: {
      version: 'test-catalog-v1',
      counts: {l1: cells.length},
      integrity: {algorithm: 'SHA-256', scope: 'UTF-8 JSON.stringify(cells)', sha256: cellsHash}
    },
    cells
  };
  await writeFile(path, JSON.stringify(catalog), 'utf8');
  const loaded = await readCatalog(path, {id: 'test-catalog-v1', sha256: cellsHash});
  assert.equal(loaded.contentSha256, cellsHash);
  assert.equal(loaded.data.cells.length, 2);
  assert.match(loaded.fileSha256, /^sha256:[0-9a-f]{64}$/);

  await assert.rejects(() => readCatalog(path, {id: 'test-catalog-v1', sha256: HASH_A}), hasCode('input_hash_mismatch'));
  const tampered = structuredClone(catalog);
  tampered.cells[0].lat = 11;
  await writeFile(path, JSON.stringify(tampered), 'utf8');
  await assert.rejects(() => readCatalog(path, {id: 'test-catalog-v1', sha256: cellsHash}), hasCode('input_hash_mismatch'));
});

test('identity registry reader verifies raw file hash and stable two-cell identities', async (t) => {
  const directory = await makeTemporaryDirectory(t);
  const path = join(directory, 'registry.json');
  const registry = {
    registry_version: 'test-registry-v1',
    satellites: [{
      satellite_id: 'P01-S01',
      cells: [
        {bank: 0, nci: '0x012345678', pci: 101},
        {bank: 1, nci: '0x0ABCDEF01', pci: 202}
      ]
    }]
  };
  const bytes = Buffer.from(`${JSON.stringify(registry, null, 2)}\n`, 'utf8');
  await writeFile(path, bytes);
  const loaded = await readIdentityRegistry(path, {version: registry.registry_version, sha256: sha256Bytes(bytes)});
  assert.equal(loaded.data.satellites[0].cells.length, 2);
  await assert.rejects(
    () => readIdentityRegistry(path, {version: registry.registry_version, sha256: HASH_B}),
    hasCode('input_hash_mismatch'));

  const duplicate = structuredClone(registry);
  duplicate.satellites[0].cells[1].nci = duplicate.satellites[0].cells[0].nci;
  const duplicateBytes = Buffer.from(JSON.stringify(duplicate), 'utf8');
  await writeFile(path, duplicateBytes);
  await assert.rejects(
    () => readIdentityRegistry(path, {version: registry.registry_version, sha256: sha256Bytes(duplicateBytes)}),
    hasCode('invalid_registry'));
});

test('NDJSON gzip chunks are canonical and byte-identical with fixed metadata', async (t) => {
  const directory = await makeTemporaryDirectory(t);
  const firstDirectory = join(directory, 'first');
  const secondDirectory = join(directory, 'second');
  const first = await writeNdjsonGzipChunk({
    runDirectory: firstDirectory,
    chunkIndex: 7,
    records: [{time_unix_ms: 20, kind: 'exit'}, {z: 2, a: 1}]
  });
  const second = await writeNdjsonGzipChunk({
    runDirectory: secondDirectory,
    chunkIndex: 7,
    records: [{a: 1, z: 2}, {kind: 'exit', time_unix_ms: 20}]
  });
  assert.deepEqual(first, second);
  const firstBytes = await readFile(join(firstDirectory, ...first.path.split('/')));
  const secondBytes = await readFile(join(secondDirectory, ...second.path.split('/')));
  assert.deepEqual(firstBytes, secondBytes);
  assert.deepEqual([...firstBytes.subarray(3, 10)], [0, 0, 0, 0, 0, 2, 255]);
  assert.equal(
    gunzipSync(firstBytes).toString('utf8'),
    '{"a":1,"z":2}\n{"kind":"exit","time_unix_ms":20}\n');
});

test('slab journal v2 binds separate event and interval chunks with exact keys', async (t) => {
  const directory = await makeTemporaryDirectory(t);
  const eventChunk = await writeNdjsonGzipChunk({
    runDirectory: directory,
    chunkIndex: 0,
    relativeDirectory: 'slab-events/shard-00000-of-00001',
    records: [{record_type: 'visibility_event', time_us: 10, kind: 'entry_enter'}]
  });
  const intervalChunk = await writeNdjsonGzipChunk({
    runDirectory: directory,
    chunkIndex: 0,
    relativeDirectory: 'slab-intervals/shard-00000-of-00001',
    records: [{record_type: 'visibility_interval', start_time_us: 10, end_time_us: 20}]
  });
  const entry = {
    schema_version: SLAB_JOURNAL_SCHEMA_VERSION,
    input_hash: HASH_A,
    part_index: 0,
    shard_index: 0,
    shard_count: 1,
    slab_start_time_us: 0,
    slab_end_time_us: 120000000,
    event_chunk: eventChunk,
    interval_chunk: intervalChunk
  };
  await writeSlabJournalEntry({
    runDirectory: directory,
    relativeDirectory: 'slab-journal/shard-00000-of-00001',
    entry
  });
  assert.deepEqual(await readSlabJournalEntries({
    runDirectory: directory,
    relativeDirectory: 'slab-journal/shard-00000-of-00001',
    expectedInputHash: HASH_A
  }), [entry]);
  await assert.rejects(() => writeSlabJournalEntry({
    runDirectory: directory,
    relativeDirectory: 'slab-journal/invalid',
    entry: {...entry, chunk: eventChunk}
  }), hasCode('unknown_field'));
});

test('evidence bundle produces deterministic manifest, summary and checksum index', async (t) => {
  const directory = await makeTemporaryDirectory(t);
  const scenario = makeScenarioManifest();
  const firstDirectory = join(directory, 'bundle-a');
  const secondDirectory = join(directory, 'bundle-b');
  const records = [{kind: 'entry', satellite_id: 'P01-S01', time_unix_ms: 1000}];
  const firstChunk = await writeNdjsonGzipChunk({runDirectory: firstDirectory, chunkIndex: 0, records});
  const secondChunk = await writeNdjsonGzipChunk({runDirectory: secondDirectory, chunkIndex: 0, records});
  const summary = createAuditSummary({
    scenarioManifest: scenario,
    completionStatus: 'complete',
    geometryResult: 'pass',
    geometryExactUnderModel: true,
    nominalAssignmentResult: 'pass',
    nominalAssignmentExactUnderModel: true,
    metrics: {zero_visible_interval_count: 0, event_count: 1},
    limitations: ['software planning evidence only']
  });
  assert.equal(summary.exact, false);
  assert.equal(summary.selection_eligible, false);
  assert.equal(summary.selectedScenario, null);
  assert.deepEqual(validateAuditSummary(summary), summary);
  await writeEvidenceBundle({runDirectory: firstDirectory, scenarioManifest: scenario, summary, chunks: [firstChunk]});
  await writeEvidenceBundle({runDirectory: secondDirectory, scenarioManifest: scenario, summary, chunks: [secondChunk]});
  const verified = await verifyEvidenceBundle(firstDirectory, {expectedInputHash: scenarioInputHash(scenario)});
  assert.equal(verified.manifest.chunks[0].record_count, 1);
  assert.equal(verified.summary.geometry.result, 'pass');
  const metadataVerified = await verifyEvidenceBundle(firstDirectory, {
    expectedInputHash: scenarioInputHash(scenario),
    verifyChunkFiles: false
  });
  assert.equal(metadataVerified.manifest.chunks[0].sha256, firstChunk.sha256);
  assert.deepEqual(await readFile(join(firstDirectory, 'manifest.json')), await readFile(join(secondDirectory, 'manifest.json')));
  assert.deepEqual(await readFile(join(firstDirectory, 'summary.json')), await readFile(join(secondDirectory, 'summary.json')));
  assert.deepEqual(await readFile(join(firstDirectory, 'sha256sums.txt')), await readFile(join(secondDirectory, 'sha256sums.txt')));

  const chunkPath = join(firstDirectory, ...firstChunk.path.split('/'));
  await writeFile(chunkPath, Buffer.from('tampered', 'utf8'));
  await assert.rejects(() => verifyEvidenceBundle(firstDirectory), hasCode('evidence_size_mismatch'));
});

test('overall incomplete summary can independently certify completed geometry', () => {
  const summary = createAuditSummary({
    scenarioManifest: makeScenarioManifest(),
    completionStatus: 'incomplete',
    geometryResult: 'pass',
    geometryExactUnderModel: true
  });
  assert.equal(summary.exact, false);
  assert.equal(summary.selectedScenario, null);
  assert.deepEqual(summary.geometry, {exact_under_model: true, result: 'pass'});
  assert.deepEqual(summary.nominal_assignment, {exact_under_model: false, result: 'not_run'});
  assert.deepEqual(validateAuditSummary(summary), summary);
});

test('checkpoint replacement is atomic and load fails closed on mismatch or truncation', async (t) => {
  const directory = await makeTemporaryDirectory(t);
  const path = join(directory, 'checkpoint.json');
  const inputHash = scenarioInputHash(makeScenarioManifest());
  const first = await saveCheckpoint(path, {
    inputHash,
    sequence: 4,
    state: {next_event_key: [1000, 'entry', 'G000001'], assignments: {G000001: 'P01-S01'}}
  });
  assert.ok(await checkpointFileSize(path) > 0);
  assert.deepEqual(await loadCheckpoint(path, {expectedInputHash: inputHash}), first);
  await assert.rejects(() => loadCheckpoint(path, {expectedInputHash: HASH_B}), hasCode('checkpoint_input_mismatch'));

  const second = await saveCheckpoint(path, {
    inputHash,
    sequence: 5,
    state: {next_event_key: [2000, 'exit', 'G000001'], assignments: {}}
  });
  assert.equal((await loadCheckpoint(path, {expectedInputHash: inputHash})).sequence, 5);
  const fullBytes = await readFile(path);
  await writeFile(path, fullBytes.subarray(0, Math.floor(fullBytes.length / 2)));
  await assert.rejects(() => loadCheckpoint(path, {expectedInputHash: inputHash}), hasCode('checkpoint_corrupt'));
  assert.notEqual(first.checkpoint_hash, second.checkpoint_hash);
});
