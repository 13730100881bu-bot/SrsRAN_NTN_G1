import assert from 'node:assert/strict';
import {mkdtemp, readFile, readdir, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {gunzipSync} from 'node:zlib';

import {
  computePlannerSourceHash,
  loadPlannerInputs,
  finalizeSlabVisibilityRun,
  main,
  mergeVisibilityShards,
  readMergedSlabPart,
  runPreflight,
  runSlabVisibilityStream,
  runStreamingAssignmentAudit,
  runVisibilityAudit
} from './constellation_audit.mjs';
import {createEpochSnapshot} from './constellation_epoch.mjs';
import {
  canonicalJson,
  readSlabJournalEntries,
  sha256Bytes,
  verifyEvidenceBundle,
  writeEvidenceBundle,
  writeNdjsonGzipChunk
} from './constellation_audit_io.mjs';
import {
  ACCESS_PROFILE_V1,
  ACCESS_PROFILE_V1_HASH,
  validatePlanV2
} from './versioned_position_plan_v2.mjs';

async function fixture(t, outputName = 'outputs', {
  satelliteShardCount = 1,
  satelliteCount = 1,
  cellCount = 2,
  fixedPosition = undefined,
  durationMs = 120000,
  startOffsetMs = 0
} = {}) {
  const root = await mkdtemp(join(tmpdir(), 'ntn-audit-cli-'));
  t.after(() => rm(root, {recursive: true, force: true}));
  const cells = Array.from({length: cellCount}, (_, index) => ({
    id: `G${String(index + 1).padStart(6, '0')}`,
    lat: fixedPosition?.lat ?? (index < 2 ? 0 : -50 + (index % 21) * 5),
    lon: fixedPosition?.lon ?? (index < 2 ? index * 2 : -180 + (index % 72) * 5),
    childMask: index % 2 === 0 ? 127 : 63
  }));
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
  const registry = {
    registry_version: 'tiny-registry-v1',
    satellites: Array.from({length: satelliteCount}, (_, index) => ({
      satellite_id: `P01-S${String(index + 1).padStart(2, '0')}`,
      cells: [
        {bank: 0, nci: `0x${String(index * 2 + 1).padStart(9, '0')}`, pci: (index * 2) % 1008},
        {bank: 1, nci: `0x${String(index * 2 + 2).padStart(9, '0')}`, pci: (index * 2 + 1) % 1008}
      ]
    }))
  };
  const catalogPath = join(root, 'catalog.json');
  const registryPath = join(root, 'registry.json');
  const catalogBytes = Buffer.from(JSON.stringify(catalog), 'utf8');
  const registryBytes = Buffer.from(JSON.stringify(registry), 'utf8');
  await writeFile(catalogPath, catalogBytes);
  await writeFile(registryPath, registryBytes);
  const orbitEpochUnixMs = Date.UTC(2026, 0, 1);
  const startUnixMs = orbitEpochUnixMs + startOffsetMs;
  const manifest = {
    schema_version: 1,
    audit_kind: 'ntn_constellation_planning_evidence',
    run_id: 'tiny-f1-replay',
    engine: {
      name: 'ntn-headless-planner',
      version: '1.0.0',
      source_revision: 'test-worktree',
      source_sha256: await computePlannerSourceHash()
    },
    scenario: {
      scenario_id: 'tiny-f1',
      altitude_km: 500,
      inclination_deg: 0,
      planes: 1,
      satellites_per_plane: satelliteCount,
      phase_factor: 0,
      raan_offset_deg: 0,
      phase_offset_deg: 0,
      orbit_epoch_unix_ms: orbitEpochUnixMs,
      start_unix_ms: startUnixMs,
      duration_ms: durationMs,
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
      satellite_shard_count: satelliteShardCount,
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
      identity_registry: {version: registry.registry_version, sha256: sha256Bytes(registryBytes)},
      access_profile: {id: ACCESS_PROFILE_V1.id, sha256: ACCESS_PROFILE_V1_HASH}
    }
  };
  const scenarioPath = join(root, 'scenario.json');
  await writeFile(scenarioPath, JSON.stringify(manifest), 'utf8');
  return {
    scenarioPath,
    catalogPath,
    registryPath,
    outputRoot: join(root, outputName)
  };
}

async function filesBelow(root, relative = '') {
  const directory = relative === '' ? root : join(root, ...relative.split('/'));
  const entries = await readdir(directory, {withFileTypes: true});
  const files = [];
  for (const entry of entries) {
    const child = relative === '' ? entry.name : `${relative}/${entry.name}`;
    if (entry.isDirectory()) files.push(...await filesBelow(root, child));
    else files.push(child);
  }
  return files.sort();
}

async function captureMain(argv) {
  const original = process.stdout.write;
  let output = '';
  process.stdout.write = (chunk) => {
    output += String(chunk);
    return true;
  };
  try {
    const result = await main(argv);
    assert.deepEqual(JSON.parse(output), result);
    return result;
  } finally {
    process.stdout.write = original;
  }
}

function plannerCliArguments(command, options, extra = []) {
  return [
    command,
    '--scenario', options.scenarioPath,
    '--catalog', options.catalogPath,
    '--registry', options.registryPath,
    '--output-root', options.outputRoot,
    ...extra
  ];
}

test('preflight binds inputs, audits a complete snapshot and never selects a scenario', async (t) => {
  const options = await fixture(t);
  const result = await runPreflight({...options, benchmarkPositionCount: 1, benchmarkDurationMs: 10000});
  assert.equal(result.metrics.catalog_l1_count, 2);
  assert.equal(result.summary.exact, false);
  assert.equal(result.summary.selection_eligible, false);
  assert.equal(result.summary.selectedScenario, null);
  const verified = await verifyEvidenceBundle(result.runDirectory);
  assert.equal(verified.summary.completion_status, 'incomplete');
});

test('small exact run is byte-identical after interruption and resume', async (t) => {
  const freshOptions = await fixture(t, 'fresh');
  const resumedOptions = {...freshOptions, outputRoot: join(freshOptions.outputRoot, '..', 'resumed')};
  const fresh = await runVisibilityAudit({
    ...freshOptions,
    positionsPerChunk: 1,
    inlineAssignmentLimit: 10
  });
  assert.equal(fresh.complete, true);
  assert.equal(fresh.summary.exact, false);
  assert.equal(fresh.summary.geometry.exact_under_model, true);
  const freshEvidence = await verifyEvidenceBundle(fresh.runDirectory);
  assert.ok(freshEvidence.manifest.chunks.some(({path}) => path.startsWith('assignment-ledger/')));
  assert.ok(fresh.summary.metrics.assignment_ledger_record_count > 0);
  assert.ok(fresh.summary.metrics.peak_assigned_l1_by_satellite.length > 0);
  const visibilityRecords = [];
  for (const chunk of freshEvidence.manifest.chunks.filter(({path}) => path.startsWith('visibility/'))) {
    const content = gunzipSync(await readFile(join(fresh.runDirectory, ...chunk.path.split('/')))).toString('utf8');
    for (const line of content.trimEnd().split('\n')) {
      if (line.length > 0) visibilityRecords.push(JSON.parse(line));
    }
  }
  const auditStartTimeUs = 0;
  assert.ok(visibilityRecords.some(({window}) => window === 'initialization'));
  assert.ok(visibilityRecords.some(({window}) => window === 'audit'));
  assert.ok(visibilityRecords.every((record) => record.window === 'initialization'
    ? record.end_time_us <= auditStartTimeUs
    : record.window === 'audit' && record.start_time_us >= auditStartTimeUs));
  const assignmentRecords = [];
  for (const chunk of freshEvidence.manifest.chunks.filter(({path}) => path.startsWith('assignments/'))) {
    const content = gunzipSync(await readFile(join(fresh.runDirectory, ...chunk.path.split('/')))).toString('utf8');
    for (const line of content.trimEnd().split('\n')) {
      if (line.length > 0) assignmentRecords.push(JSON.parse(line));
    }
  }
  assert.ok(assignmentRecords.some(({window}) => window === 'initialization'));
  assert.ok(assignmentRecords.some(({window}) => window === 'audit'));
  assert.ok(assignmentRecords.some((before) => before.window === 'initialization' &&
    before.end_time_us === auditStartTimeUs && assignmentRecords.some((after) =>
      after.window === 'audit' && after.start_time_us === auditStartTimeUs &&
      after.position_id === before.position_id && after.satellite_id === before.satellite_id)));
  const partial = await runVisibilityAudit({
    ...resumedOptions,
    positionsPerChunk: 1,
    maxPositions: 1,
    inlineAssignmentLimit: 10
  });
  assert.equal(partial.complete, false);
  const resumed = await runVisibilityAudit({
    ...resumedOptions,
    positionsPerChunk: 1,
    inlineAssignmentLimit: 10,
    resume: true
  });
  assert.equal(resumed.complete, true);
  for (const name of ['manifest.json', 'summary.json', 'sha256sums.txt']) {
    assert.deepEqual(
      await readFile(join(fresh.runDirectory, name)),
      await readFile(join(resumed.runDirectory, name))
    );
  }
});

test('source hash mismatch fails before planning', async (t) => {
  const options = await fixture(t);
  const manifest = JSON.parse(await readFile(options.scenarioPath, 'utf8'));
  manifest.engine.source_sha256 = `sha256:${'0'.repeat(64)}`;
  await writeFile(options.scenarioPath, JSON.stringify(manifest), 'utf8');
  await assert.rejects(() => runPreflight(options), /planner source hash mismatch/);
});

test('scenario capacities cannot diverge from the bound access profile', async (t) => {
  const options = await fixture(t);
  const manifest = JSON.parse(await readFile(options.scenarioPath, 'utf8'));
  manifest.scenario.satellite_capacity = 300;
  manifest.scenario.cell_capacity = 150;
  await writeFile(options.scenarioPath, JSON.stringify(manifest), 'utf8');
  await assert.rejects(
    () => runPreflight(options),
    /scenario capacities do not match the canonical planning profile/
  );
});

test('completed satellite shards merge in deterministic index order', async (t) => {
  const options = await fixture(t, 'sharded', {satelliteShardCount: 2});
  for (let shardIndex = 0; shardIndex < 2; shardIndex += 1) {
    const shard = await runVisibilityAudit({
      ...options,
      shardIndex,
      shardCount: 2,
      positionsPerChunk: 1,
      inlineAssignmentLimit: 10
    });
    assert.equal(shard.shardComplete, true);
    assert.equal(shard.complete, false);
  }
  const merged = await mergeVisibilityShards({...options, shardCount: 2, inlineAssignmentLimit: 10});
  assert.equal(merged.complete, true);
  assert.equal(merged.summary.geometry.exact_under_model, true);
  assert.equal((await verifyEvidenceBundle(merged.runDirectory)).summary.completion_status, 'complete');
});

test('time-slab CLI persists bounded state and resume is byte-identical', async (t) => {
  const freshOptions = await fixture(t, 'slab-fresh');
  const resumedOptions = {...freshOptions, outputRoot: join(freshOptions.outputRoot, '..', 'slab-resumed')};
  const fresh = await runSlabVisibilityStream({...freshOptions, shardIndex: 0, shardCount: 1});
  assert.equal(fresh.shardComplete, true);
  assert.equal(fresh.geometryStreamComplete, false);
  assert.equal(fresh.assignmentComplete, false);
  assert.equal(fresh.evidenceWritten, false);
  const freshFinalized = await finalizeSlabVisibilityRun({...freshOptions, shardCount: 1});
  assert.equal(freshFinalized.geometryStreamComplete, true);
  assert.equal(freshFinalized.assignmentComplete, false);
  assert.equal(freshFinalized.evidenceWritten, true);
  assert.equal(freshFinalized.evidenceVerified, true);
  assert.equal(freshFinalized.summary.completion_status, 'incomplete');
  assert.equal(freshFinalized.summary.exact, false);
  assert.equal(freshFinalized.summary.selectedScenario, null);
  assert.deepEqual(freshFinalized.summary.geometry, {exact_under_model: false, result: 'not_run'});
  assert.deepEqual(freshFinalized.summary.nominal_assignment, {exact_under_model: false, result: 'not_run'});
  assert.equal(freshFinalized.metrics.visibility_computation_exact_under_model, true);
  assert.ok(freshFinalized.summary.limitations.includes('geometry_coverage_not_evaluated'));
  assert.ok(freshFinalized.summary.limitations.includes('external_stream_assignment_required'));
  assert.equal(freshFinalized.metrics.catalog_l1_count, 2);
  assert.equal(freshFinalized.metrics.assignment_phase_complete, false);

  const partial = await runSlabVisibilityStream({
    ...resumedOptions,
    shardIndex: 0,
    shardCount: 1,
    maxSlabs: 7
  });
  assert.equal(partial.shardComplete, false);
  assert.equal(partial.slabsProcessed, 7);
  assert.ok(partial.summary.limitations.includes('external_stream_assignment_required'));
  const checkpointText = await readFile(partial.checkpointPath, 'utf8');
  assert.doesNotMatch(checkpointText, /"chunks"|"epochs"|"records"/);
  const resumed = await runSlabVisibilityStream({
    ...resumedOptions,
    shardIndex: 0,
    shardCount: 1,
    resume: true
  });
  assert.equal(resumed.geometryStreamComplete, false);
  assert.equal(resumed.assignmentComplete, false);
  const resumedFinalized = await finalizeSlabVisibilityRun({...resumedOptions, shardCount: 1});
  assert.equal(resumedFinalized.geometryStreamComplete, true);
  assert.equal(resumedFinalized.evidenceVerified, true);
  const verified = await verifyEvidenceBundle(resumed.runDirectory);
  assert.equal(verified.summary.completion_status, 'incomplete');
  assert.ok(verified.manifest.chunks.length > 1);
  assert.ok(verified.manifest.chunks.some(({path}) => path.startsWith('slab-events/')));
  assert.ok(verified.manifest.chunks.some(({path}) => path.startsWith('slab-intervals/')));

  const records = [];
  for (const chunk of verified.manifest.chunks) {
    const content = gunzipSync(await readFile(join(resumed.runDirectory, ...chunk.path.split('/')))).toString('utf8');
    const chunkRecords = content.trimEnd().split('\n').filter((line) => line.length > 0).map(JSON.parse);
    if (chunk.path.startsWith('slab-events/')) {
      assert.ok(chunkRecords.every(({record_type}) => [
        'visibility_initial_state', 'visibility_event', 'visibility_ambiguity'
      ].includes(record_type)));
      const firstCrossing = chunkRecords.findIndex(({record_type}) => record_type === 'visibility_event');
      const lastInitial = chunkRecords.findLastIndex(({record_type}) => record_type === 'visibility_initial_state');
      if (lastInitial >= 0 && firstCrossing >= 0) assert.ok(lastInitial < firstCrossing);
    } else if (chunk.path.startsWith('slab-intervals/')) {
      assert.ok(chunkRecords.every(({record_type}) => record_type === 'visibility_interval'));
    }
    for (const record of chunkRecords) records.push(record);
  }
  assert.ok(records.some(({record_type}) => record_type === 'visibility_event'));
  assert.ok(records.some(({record_type}) => record_type === 'visibility_interval'));
  const initialStates = records.filter(({record_type}) => record_type === 'visibility_initial_state');
  assert.ok(initialStates.length > 0);
  assert.ok(initialStates.every((record) => record.kind === 'initial_state' &&
    record.window === 'initialization' && record.above_release === true &&
    typeof record.above_entry === 'boolean'));
  assert.ok(records.some(({window}) => window === 'initialization'));
  assert.ok(records.some(({window}) => window === 'audit'));
  assert.ok(records.every(({window}) => window === 'initialization' || window === 'audit'));

  const freshFiles = await filesBelow(fresh.runDirectory);
  const resumedFiles = await filesBelow(resumed.runDirectory);
  assert.deepEqual(resumedFiles, freshFiles);
  for (const path of freshFiles) {
    assert.deepEqual(
      await readFile(join(fresh.runDirectory, ...path.split('/'))),
      await readFile(join(resumed.runDirectory, ...path.split('/'))),
      path
    );
  }
});

test('streaming assignment publishes one combined bundle and resumes byte-identically', async (t) => {
  const freshOptions = await fixture(t, 'assignment-fresh', {durationMs: 240000});
  const resumedOptions = {
    ...freshOptions,
    outputRoot: join(freshOptions.outputRoot, '..', 'assignment-resumed')
  };
  for (const options of [freshOptions, resumedOptions]) {
    const slab = await runSlabVisibilityStream({...options, shardIndex: 0, shardCount: 1});
    assert.equal(slab.shardComplete, true);
    await finalizeSlabVisibilityRun({...options, shardCount: 1});
  }

  const fresh = await captureMain(plannerCliArguments('run-assignment', freshOptions));
  assert.equal(fresh.complete, true);
  assert.equal(fresh.assignmentComplete, true);
  assert.equal(fresh.evidenceWritten, true);
  assert.equal(fresh.evidenceVerified, true);
  for (const largeField of ['finalAssignments', 'finalCandidateInventory', 'metrics', 'summary', 'chunks', 'checkpoint']) {
    assert.equal(Object.hasOwn(fresh, largeField), false, largeField);
  }

  const resumedRunDirectory = join(resumedOptions.outputRoot, 'tiny-f1-replay');
  const geometryBundle = await Promise.all(['manifest.json', 'summary.json', 'sha256sums.txt'].map(
    (name) => readFile(join(resumedRunDirectory, name))));
  const partial = await captureMain(plannerCliArguments(
    'run-assignment', resumedOptions, ['--max-parts', '51']));
  assert.equal(partial.complete, false);
  assert.equal(partial.evidenceWritten, false);
  assert.equal(partial.outputHash, null);
  assert.equal(partial.assignmentChunkCount, 1);
  for (const [index, name] of ['manifest.json', 'summary.json', 'sha256sums.txt'].entries()) {
    assert.deepEqual(await readFile(join(resumedRunDirectory, name)), geometryBundle[index], name);
  }
  const resumed = await captureMain(plannerCliArguments('resume-assignment', resumedOptions));
  assert.equal(resumed.complete, true);
  assert.equal(resumed.outputHash, fresh.outputHash);
  assert.equal(resumed.ledgerSetHash, fresh.ledgerSetHash);
  const completedBundle = await Promise.all(['manifest.json', 'summary.json', 'sha256sums.txt'].map(
    (name) => readFile(join(resumed.runDirectory, name))));
  const repeatedResume = await captureMain(plannerCliArguments('resume-assignment', resumedOptions));
  assert.equal(repeatedResume.complete, true);
  assert.equal(repeatedResume.partsProcessed, 0);
  assert.equal(repeatedResume.outputHash, resumed.outputHash);
  for (const [index, name] of ['manifest.json', 'summary.json', 'sha256sums.txt'].entries()) {
    assert.deepEqual(await readFile(join(resumed.runDirectory, name)), completedBundle[index], name);
  }

  const freshEvidence = await verifyEvidenceBundle(fresh.runDirectory);
  const resumedEvidence = await verifyEvidenceBundle(resumed.runDirectory);
  const shallowEvidence = await captureMain([
    'verify', '--run-directory', fresh.runDirectory, '--shallow'
  ]);
  assert.equal(shallowEvidence.summary.completion_status, 'complete');
  assert.equal(freshEvidence.summary.completion_status, 'complete');
  assert.deepEqual(freshEvidence.summary.geometry.exact_under_model, true);
  assert.deepEqual(freshEvidence.summary.nominal_assignment.exact_under_model, true);
  assert.equal(freshEvidence.summary.exact, false);
  assert.equal(freshEvidence.summary.selection_eligible, false);
  assert.equal(freshEvidence.summary.selectedScenario, null);
  assert.ok(!freshEvidence.summary.limitations.includes('external_stream_assignment_required'));
  assert.ok(freshEvidence.summary.limitations.includes('n_minus_one_failure_audit_not_run'));
  assert.ok(freshEvidence.summary.limitations.includes('gateway_constraints_not_modeled'));
  assert.ok(freshEvidence.summary.limitations.includes('pci_time_conflict_audit_not_run'));
  assert.ok(freshEvidence.summary.limitations.includes('rf_execution_not_audited'));
  assert.ok(freshEvidence.manifest.chunks.some(({path}) => path.startsWith('slab-events/')));
  assert.ok(freshEvidence.manifest.chunks.some(({path}) => path.startsWith('slab-intervals/')));
  assert.ok(freshEvidence.manifest.chunks.some(({path}) => path.startsWith('streaming-assignments/')));
  assert.equal(
    freshEvidence.summary.metrics.total_chunk_count,
    freshEvidence.manifest.chunks.length
  );
  assert.equal(freshEvidence.summary.metrics.assignment_output_hash, fresh.outputHash);
  assert.ok(freshEvidence.summary.metrics.geometry_record_counts.total_record_count > 0);
  assert.ok(freshEvidence.summary.metrics.assignment_record_counts.total_record_count > 0);
  assert.deepEqual(resumedEvidence.manifest, freshEvidence.manifest);
  assert.deepEqual(resumedEvidence.summary, freshEvidence.summary);
  for (const name of ['manifest.json', 'summary.json', 'sha256sums.txt']) {
    assert.deepEqual(
      await readFile(join(fresh.runDirectory, name)),
      await readFile(join(resumed.runDirectory, name)),
      name
    );
  }
});

test('export-plan writes a registry-bound sticky snapshot without changing the audit bundle', async (t) => {
  // At 69 seconds this equatorial satellite has fallen below 45 degrees but
  // remains above the 42-degree release threshold. The owner must therefore
  // come from the replayed sticky assignment, not a fresh-entry candidate.
  const options = await fixture(t, 'plan-export', {
    cellCount: 1,
    fixedPosition: {lat: 0, lon: 0},
    startOffsetMs: 68000,
    durationMs: 1000
  });
  await runSlabVisibilityStream({...options, shardIndex: 0, shardCount: 1});
  await finalizeSlabVisibilityRun({...options, shardCount: 1});
  const assignment = await captureMain(plannerCliArguments('run-assignment', options));
  assert.equal(assignment.complete, true);

  const planTimeUs = 68999999;
  const inputs = await loadPlannerInputs(options);
  const geometry = createEpochSnapshot({
    timeUs: planTimeUs,
    satellites: inputs.satellites,
    catalogGeometry: inputs.catalogGeometry,
    orbitModel: inputs.orbitModel,
    entryElevationDeg: inputs.manifest.scenario.entry_elevation_deg,
    releaseElevationDeg: inputs.manifest.scenario.exit_elevation_deg,
    marginTolerance: inputs.manifest.scenario.margin_tolerance
  });
  assert.equal(geometry.completeCandidateSets[0].candidates[0].satelliteId, 'P01-S01');
  assert.equal(geometry.assignmentCandidateSets[0].candidates.length, 0);

  const protectedNames = ['manifest.json', 'summary.json', 'sha256sums.txt'];
  const protectedBytes = await Promise.all(
    protectedNames.map((name) => readFile(join(assignment.runDirectory, name)))
  );
  const exported = await captureMain(plannerCliArguments('export-plan', options, [
    '--satellite-id', 'P01-S01', '--plan-time-us', String(planTimeUs)
  ]));
  assert.equal(exported.checkpointTimeUs, 69000000);
  assert.equal(exported.planTimeReference, 'orbit_epoch_relative');
  assert.equal(exported.timeQuantizationUs, 999);
  assert.equal(exported.visibleL1Count, 1);
  assert.equal(exported.assignedL1Count, 1);
  assert.equal(exported.unassignedL1Count, 0);
  assert.equal(exported.validator.validator, 'versioned_position_plan_v2');

  const plan = JSON.parse(await readFile(
    join(exported.runDirectory, ...exported.plan.path.split('/')), 'utf8'
  ));
  const sidecar = JSON.parse(await readFile(
    join(exported.runDirectory, ...exported.assignmentSidecar.path.split('/')), 'utf8'
  ));
  assert.equal(validatePlanV2(plan).contentHash, exported.validator.content_hash);
  assert.equal(plan.content_hash, exported.plan.content_hash);
  assert.equal(plan.satellite_id, 'P01-S01');
  assert.equal(plan.schedule_version, exported.processedPartCount);
  assert.deepEqual(plan.visible_l1_positions.map(({position_id: id}) => id), ['G000001']);
  assert.deepEqual(plan.onboard_cells.map(({nci, pci}) => ({nci, pci})), [
    {nci: 1, pci: 0},
    {nci: 2, pci: 1}
  ]);
  assert.deepEqual(sidecar.assigned_l1_positions.map((item) => ({
    position_id: item.position_id,
    nci: item.nci,
    pci: item.pci
  })), [{position_id: 'G000001', nci: 1, pci: 0}]);

  for (const [index, name] of protectedNames.entries()) {
    assert.deepEqual(await readFile(join(exported.runDirectory, name)), protectedBytes[index], name);
  }
  await assert.rejects(
    () => main(plannerCliArguments('export-plan', options, [
      '--satellite-id', 'P01-S01', '--plan-time-us', String(planTimeUs - 1)
    ])),
    /checkpoint left-limit/
  );
  await assert.rejects(
    () => main(plannerCliArguments('export-plan', options, [
      '--satellite-id', 'P01-S02', '--plan-time-us', String(planTimeUs)
    ])),
    /unknown registry satellite/
  );
  await assert.rejects(
    () => main(plannerCliArguments('export-plan', options, [
      '--satellite-id', 'P01-S01', '--plan-time-us', '67999999'
    ])),
    /must be inside/
  );

  const checkpointBytes = await readFile(assignment.checkpointPath);
  const checkpoint = JSON.parse(checkpointBytes.toString('utf8'));
  checkpoint.checkpoint_hash = `sha256:${'0'.repeat(64)}`;
  await writeFile(assignment.checkpointPath, `${canonicalJson(checkpoint)}\n`, 'utf8');
  await assert.rejects(
    () => main(plannerCliArguments('export-plan', options, [
      '--satellite-id', 'P01-S01', '--plan-time-us', String(planTimeUs)
    ])),
    /checkpoint content hash mismatch/
  );
  await writeFile(assignment.checkpointPath, checkpointBytes);

  const journalPath = join(assignment.runDirectory, 'streaming-assignment-journal', 'part-000000.json');
  const journalBytes = await readFile(journalPath);
  const journal = JSON.parse(journalBytes.toString('utf8'));
  journal.source_hash = `sha256:${'0'.repeat(64)}`;
  await writeFile(journalPath, `${canonicalJson(journal)}\n`, 'utf8');
  await assert.rejects(
    () => main(plannerCliArguments('export-plan', options, [
      '--satellite-id', 'P01-S01', '--plan-time-us', String(planTimeUs)
    ])),
    /different assignment evidence/
  );
  await writeFile(journalPath, journalBytes);
});

test('assignment requires every complete geometry shard and exact CLI option ownership', async (t) => {
  const options = await fixture(t, 'assignment-incomplete');
  await runSlabVisibilityStream({...options, shardIndex: 0, shardCount: 1, maxSlabs: 1});
  await assert.rejects(
    () => runStreamingAssignmentAudit(options),
    /completed 0\/1 satellite shards/
  );
  await runSlabVisibilityStream({...options, shardIndex: 0, shardCount: 1, resume: true});
  const finalized = await finalizeSlabVisibilityRun({...options, shardCount: 1});
  const geometryEvidence = await verifyEvidenceBundle(finalized.runDirectory);
  const extraChunk = await writeNdjsonGzipChunk({
    runDirectory: finalized.runDirectory,
    chunkIndex: 0,
    relativeDirectory: 'unexpected-extra',
    records: [{record_type: 'unexpected_test_record'}]
  });
  await writeEvidenceBundle({
    runDirectory: finalized.runDirectory,
    scenarioManifest: geometryEvidence.scenario,
    summary: geometryEvidence.summary,
    chunks: [...geometryEvidence.manifest.chunks, extraChunk],
    deepVerifyChunks: false
  });
  await assert.rejects(
    () => runStreamingAssignmentAudit(options),
    /chunks outside the complete geometry set/
  );
  await assert.rejects(() => main(['preflight', '--max-parts', '1']), /--max-parts is only valid/);
  await assert.rejects(() => main(['preflight', '--shallow']), /--shallow is only valid/);
  await assert.rejects(
    () => main(['run-assignment', '--shard-count', '1']),
    /every scenario shard is required/
  );
  await assert.rejects(
    () => main(['run-assignment', '--run-directory', 'ignored']),
    /--run-directory is only valid for verify/
  );
  await assert.rejects(
    () => main(['export-plan', '--satellite-id', 'P01-S01']),
    /requires --satellite-id and --plan-time-us/
  );
});

test('final summary keeps geometry absence separate from capacity overflow', async (t) => {
  const cases = [
    {
      name: 'no-visible',
      fixtureOptions: {cellCount: 1, fixedPosition: {lat: 90, lon: 0}},
      geometryResult: 'fail',
      reason: 'no_visible_candidate'
    },
    {
      name: 'overflow',
      fixtureOptions: {cellCount: 257, fixedPosition: {lat: 0, lon: 0}, durationMs: 10000},
      geometryResult: 'pass',
      reason: 'schedule_overflow'
    }
  ];
  for (const item of cases) {
    const options = await fixture(t, `assignment-${item.name}`, item.fixtureOptions);
    await runSlabVisibilityStream({...options, shardIndex: 0, shardCount: 1});
    await finalizeSlabVisibilityRun({...options, shardCount: 1});
    const result = await runStreamingAssignmentAudit(options);
    const evidence = await verifyEvidenceBundle(result.runDirectory);
    const assignmentMetrics = evidence.summary.metrics.assignment_metrics;
    assert.equal(evidence.summary.geometry.result, item.geometryResult, item.name);
    assert.equal(evidence.summary.geometry.exact_under_model, true, item.name);
    assert.equal(evidence.summary.nominal_assignment.result, 'fail', item.name);
    assert.equal(evidence.summary.nominal_assignment.exact_under_model, true, item.name);
    assert.ok(assignmentMetrics.failure_interval_count_by_reason[item.reason] > 0, item.name);
    assert.ok(BigInt(assignmentMetrics.failure_duration_us_by_reason[item.reason]) > 0n, item.name);
    if (item.reason === 'schedule_overflow') {
      assert.equal(assignmentMetrics.failure_peak_counts.no_visible_candidate, 0);
      assert.equal(assignmentMetrics.failure_peak_counts.schedule_overflow, 1);
    }
  }
});

test('time-slab path keeps a 257-position inventory without capacity cropping', async (t) => {
  const options = await fixture(t, 'slab-257', {cellCount: 257});
  const result = await runSlabVisibilityStream({
    ...options,
    shardIndex: 0,
    shardCount: 1,
    maxSlabs: 1
  });
  assert.equal(result.metrics.catalog_l1_count, 257);
  assert.equal(result.metrics.slab_engine_metrics.potentialPairs, 257);
  assert.equal(result.assignmentComplete, false);
  assert.ok(result.summary.limitations.includes('external_stream_assignment_required'));
  await assert.rejects(
    () => finalizeSlabVisibilityRun({...options, shardCount: 1}),
    /completed 0\/1 satellite shards/
  );
});

test('fourteen satellite shards merge one slab in deterministic crossing-event order', async (t) => {
  const shardCount = 14;
  const options = await fixture(t, 'slab-14', {satelliteShardCount: shardCount, satelliteCount: shardCount});
  let runDirectory;
  const journals = [];
  for (let shardIndex = 0; shardIndex < shardCount; shardIndex += 1) {
    const result = await runSlabVisibilityStream({...options, shardIndex, shardCount});
    assert.equal(result.evidenceWritten, false);
    runDirectory = result.runDirectory;
    journals.push(await readSlabJournalEntries({
      runDirectory,
      relativeDirectory: `slab-journal/shard-${String(shardIndex).padStart(5, '0')}-of-${String(shardCount).padStart(5, '0')}`
    }));
  }
  const finalized = await finalizeSlabVisibilityRun({...options, shardCount});
  assert.equal(finalized.geometryStreamComplete, true);
  assert.equal(finalized.evidenceWritten, true);
  assert.equal(finalized.evidenceVerified, true);
  assert.equal(finalized.metrics.completed_satellite_shard_count, shardCount);
  assert.equal(finalized.metrics.shard_index, null);
  assert.equal(finalized.metrics.shard_satellite_count, shardCount);
  const partIndex = journals[0].findIndex((_, index) => {
    return journals.some((journal) => journal[index].event_chunk.record_count > 0);
  });
  assert.ok(partIndex >= 0);
  const forward = await readMergedSlabPart({runDirectory, journals, partIndex});
  const reversed = await readMergedSlabPart({runDirectory, journals: [...journals].reverse(), partIndex});
  assert.deepEqual(reversed, forward);
  await assert.rejects(
    () => readMergedSlabPart({runDirectory, journals: journals.slice(0, -1), partIndex}),
    /complete 14-shard input set/
  );
  const duplicate = [...journals];
  duplicate[13] = journals[0];
  await assert.rejects(
    () => readMergedSlabPart({runDirectory, journals: duplicate, partIndex}),
    /duplicate or missing satellite shard/
  );
  const wrongPath = structuredClone(journals);
  wrongPath[0][partIndex].event_chunk.path = wrongPath[1][partIndex].event_chunk.path;
  await assert.rejects(
    () => readMergedSlabPart({runDirectory, journals: wrongPath, partIndex}),
    /points outside its shard part/
  );
  assert.ok(forward.length > 0);
  assert.ok(forward.every(({record_type}) => record_type !== 'visibility_interval'));
  for (let index = 1; index < forward.length; index += 1) {
    const previousTime = forward[index - 1].time_us ?? forward[index - 1].start_time_us;
    const currentTime = forward[index].time_us ?? forward[index].start_time_us;
    assert.ok(previousTime <= currentTime);
  }
});
