#!/usr/bin/env node

import {readFile, writeFile, mkdir} from 'node:fs/promises';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {gunzipSync} from 'node:zlib';

import {
  createWalkerDelta,
  resolveOrbitModel
} from './constellation_audit_core.mjs';
import {
  auditRunDirectory,
  canonicalJson,
  createAuditSummary,
  loadCheckpoint,
  readCatalog,
  readIdentityRegistry,
  readMergedEvidenceChunks,
  readSlabJournalEntries,
  SLAB_JOURNAL_SCHEMA_VERSION,
  saveCheckpoint,
  scenarioInputHash,
  sha256Bytes,
  validateScenarioManifest,
  verifyEvidenceBundle,
  writeCanonicalJsonArtifact,
  writeEvidenceBundle,
  writeNdjsonGzipChunk,
  writeSlabJournalEntry
} from './constellation_audit_io.mjs';
import {planAssignmentEpoch} from './constellation_assignment.mjs';
import {auditEpochSnapshot, createCatalogGeometry, createEpochSnapshot} from './constellation_epoch.mjs';
import {exportDryRunSatellitePlan} from './constellation_plan_export.mjs';
import {
  readStreamingAssignmentSnapshot,
  runStreamingAssignmentLedger
} from './constellation_streaming_assignment_ledger.mjs';
import {replayAssignmentTimeline} from './constellation_timeline.mjs';
import {
  createSlabVisibilityContext,
  createSlabVisibilityState,
  processNextVisibilitySlab,
  restoreSlabVisibilityState
} from './constellation_slab_visibility.mjs';
import {auditPairVisibility} from './constellation_visibility.mjs';
import {ACCESS_PROFILE_V1, ACCESS_PROFILE_V1_HASH} from './versioned_position_plan_v3.mjs';

const moduleDirectory = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(moduleDirectory, '..', '..');
const defaultScenarioPath = resolve(moduleDirectory, 'scenarios', 'global_f1_seven_day.json');
const defaultCatalogPath = resolve(moduleDirectory, 'data', 'global-land-l1-v1.json');
const defaultRegistryPath = resolve(moduleDirectory, 'data', 'onboard-cell-identity-registry.json');
const MAX_INLINE_ASSIGNMENT_POSITIONS = 1000;

export const PLANNER_SOURCE_FILES = Object.freeze([
  'utils/ntn/constellation_assignment.mjs',
  'utils/ntn/constellation_audit.mjs',
  'utils/ntn/constellation_audit_core.mjs',
  'utils/ntn/constellation_audit_io.mjs',
  'utils/ntn/constellation_epoch.mjs',
  'utils/ntn/constellation_plan_export.mjs',
  'utils/ntn/constellation_replay_plan_export.mjs',
  'utils/ntn/constellation_slab_visibility.mjs',
  'utils/ntn/constellation_streaming_assignment.mjs',
  'utils/ntn/constellation_streaming_assignment_ledger.mjs',
  'utils/ntn/constellation_timeline.mjs',
  'utils/ntn/constellation_visibility.mjs',
  'utils/ntn/versioned_position_plan_v2.mjs',
  'utils/ntn/versioned_position_plan_v3.mjs'
]);

function fail(message) {
  throw new Error(message);
}

function compareText(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function integer(value, context, minimum = Number.MIN_SAFE_INTEGER) {
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed < minimum) fail(`${context} must be an integer >= ${minimum}`);
  return parsed;
}

async function readJson(path, context) {
  try {
    return JSON.parse(await readFile(path, 'utf8'));
  } catch (error) {
    throw new Error(`cannot read ${context} at ${path}: ${error.message}`, {cause: error});
  }
}

export async function computePlannerSourceHash(root = repositoryRoot) {
  const entries = [];
  for (const relativePath of PLANNER_SOURCE_FILES) {
    const bytes = await readFile(resolve(root, ...relativePath.split('/')));
    entries.push({path: relativePath, sha256: sha256Bytes(bytes)});
  }
  return sha256Bytes(`${entries.map(({path, sha256}) => `${path}\0${sha256}\n`).join('')}`);
}

function exactSatelliteRegistryMatch(satellites, registry) {
  const walkerIds = satellites.map(({id}) => id).sort(compareText);
  const registryIds = registry.satellites.map(({satellite_id: id}) => id).sort(compareText);
  if (walkerIds.length !== registryIds.length || walkerIds.some((id, index) => id !== registryIds[index])) {
    fail('identity registry satellite IDs do not exactly match the Walker scenario');
  }
}

function orbitModelForScenario(scenario) {
  return resolveOrbitModel({
    altitudeKm: scenario.altitude_km,
    earthRadiusKm: scenario.earth_radius_km,
    earthMuKm3S2: scenario.earth_mu_km3_s2,
    earthRotationRadPerSecond: scenario.earth_rotation_rad_per_second
  });
}

function walkerForScenario(scenario) {
  return createWalkerDelta({
    altitudeKm: scenario.altitude_km,
    inclinationDeg: scenario.inclination_deg,
    planes: scenario.planes,
    satellitesPerPlane: scenario.satellites_per_plane,
    phaseFactor: scenario.phase_factor,
    raanOffsetDeg: scenario.raan_offset_deg,
    phaseOffsetDeg: scenario.phase_offset_deg
  });
}

export async function loadPlannerInputs({scenarioPath, catalogPath, registryPath, outputRoot}) {
  const manifest = validateScenarioManifest(await readJson(resolve(scenarioPath), 'scenario manifest'));
  const actualSourceHash = await computePlannerSourceHash();
  if (manifest.engine.source_sha256 !== actualSourceHash) {
    fail(`planner source hash mismatch: scenario=${manifest.engine.source_sha256}, actual=${actualSourceHash}`);
  }
  const catalog = await readCatalog(resolve(catalogPath), {
    id: manifest.inputs.catalog.id,
    sha256: manifest.inputs.catalog.sha256
  });
  if (catalog.fileSha256 !== manifest.inputs.catalog.file_sha256) fail('catalog file hash does not match scenario');
  const registry = await readIdentityRegistry(resolve(registryPath), manifest.inputs.identity_registry);
  if (manifest.inputs.access_profile.id !== ACCESS_PROFILE_V1.id ||
      manifest.inputs.access_profile.sha256 !== ACCESS_PROFILE_V1_HASH) {
    fail('scenario access profile does not match the canonical planning profile');
  }
  if (manifest.scenario.satellite_capacity !== ACCESS_PROFILE_V1.max_l1_positions_per_satellite ||
      manifest.scenario.cell_capacity !== ACCESS_PROFILE_V1.max_l1_positions_per_cell) {
    fail('scenario capacities do not match the canonical planning profile');
  }
  const satellites = walkerForScenario(manifest.scenario);
  exactSatelliteRegistryMatch(satellites, registry.data);
  const orbitModel = orbitModelForScenario(manifest.scenario);
  if (manifest.scenario.pre_roll_ms < Math.ceil(orbitModel.orbitPeriodSeconds * 1000)) {
    fail('pre_roll_ms must cover at least one orbit period');
  }
  const auditStartTimeUs = (manifest.scenario.start_unix_ms - manifest.scenario.orbit_epoch_unix_ms) * 1000;
  const auditEndTimeUs = auditStartTimeUs + manifest.scenario.duration_ms * 1000;
  if (!Number.isSafeInteger(auditStartTimeUs) || !Number.isSafeInteger(auditEndTimeUs)) {
    fail('scenario time range cannot be represented as integer microseconds relative to orbit epoch');
  }
  const catalogGeometry = createCatalogGeometry(catalog.data);
  return {
    manifest,
    inputHash: scenarioInputHash(manifest),
    catalog: catalog.data,
    catalogFileSha256: catalog.fileSha256,
    registry: registry.data,
    satellites,
    orbitModel,
    catalogGeometry,
    auditStartTimeUs,
    auditEndTimeUs,
    simulationStartTimeUs: auditStartTimeUs - manifest.scenario.pre_roll_ms * 1000,
    runDirectory: auditRunDirectory(manifest.run_id, outputRoot)
  };
}

function planSnapshot(inputs, timeUs, previousAssignments = new Map()) {
  const scenario = inputs.manifest.scenario;
  const snapshot = createEpochSnapshot({
    timeUs,
    satellites: inputs.satellites,
    catalogGeometry: inputs.catalogGeometry,
    orbitModel: inputs.orbitModel,
    entryElevationDeg: scenario.entry_elevation_deg,
    releaseElevationDeg: scenario.exit_elevation_deg,
    marginTolerance: scenario.margin_tolerance
  });
  const snapshotAudit = auditEpochSnapshot(snapshot, {
    catalogGeometry: inputs.catalogGeometry,
    satellites: inputs.satellites
  });
  const plan = planAssignmentEpoch({
    completeCandidateSets: snapshot.completeCandidateSets,
    assignmentCandidateSets: snapshot.assignmentCandidateSets,
    registry: inputs.registry,
    previousAssignments,
    satelliteCapacity: scenario.satellite_capacity,
    cellCapacity: scenario.cell_capacity
  });
  return {snapshot, snapshotAudit, plan};
}

function visibleInventoryForSatellite(inputs, snapshot, satelliteId) {
  const positionById = new Map(inputs.catalogGeometry.positions.map((position) => [position.positionId, position]));
  return snapshot.completeCandidateSets.filter((candidateSet) => {
    return candidateSet.candidates.some((candidate) => candidate.satelliteId === satelliteId);
  }).map((candidateSet) => {
    const position = positionById.get(candidateSet.positionId);
    return {
      id: position.positionId,
      lat: position.latitudeDeg,
      lon: position.longitudeDeg,
      childMask: position.childMask
    };
  });
}

function exportPreflightPlan(inputs, snapshot, assignments) {
  const satelliteId = inputs.satellites[0].id;
  const scenario = inputs.manifest.scenario;
  return exportDryRunSatellitePlan({
    satelliteId,
    planningTimeUnixMs: scenario.start_unix_ms,
    visibleInventory: visibleInventoryForSatellite(inputs, snapshot, satelliteId),
    assignments,
    identityRegistry: inputs.registry,
    planningContext: {
      planningRunId: inputs.manifest.run_id,
      catalog: {id: inputs.manifest.inputs.catalog.id, sha256: inputs.manifest.inputs.catalog.sha256},
      identityRegistry: inputs.manifest.inputs.identity_registry,
      accessProfile: inputs.manifest.inputs.access_profile,
      catalogVersion: 1,
      scheduleVersion: 1,
      validFromUnixMs: scenario.start_unix_ms,
      validUntilUnixMs: scenario.start_unix_ms + scenario.duration_ms,
      activationEpochUnixMs: scenario.start_unix_ms
    }
  });
}

async function benchmarkVisibility(inputs, samplePositionCount, benchmarkDurationMs) {
  const started = performance.now();
  const scenario = inputs.manifest.scenario;
  const samplePositions = inputs.catalogGeometry.positions.slice(0, samplePositionCount);
  const endTimeUs = inputs.auditStartTimeUs + benchmarkDurationMs * 1000;
  let intervalCount = 0;
  let ambiguityCount = 0;
  let evaluations = 0;
  for (const position of samplePositions) {
    for (const satellite of inputs.satellites) {
      const audit = auditPairVisibility({
        satellite,
        observerUnit: position.observerUnit,
        startTimeUs: inputs.auditStartTimeUs,
        endTimeUs,
        boundaryPaddingUs: scenario.scan_step_ms * 1000,
        orbitModel: inputs.orbitModel,
        entryElevationDeg: scenario.entry_elevation_deg,
        releaseElevationDeg: scenario.exit_elevation_deg,
        toleranceUs: scenario.root_tolerance_ms * 1000,
        scanStepUs: scenario.scan_step_ms * 1000,
        marginTolerance: scenario.margin_tolerance,
        maxEvaluations: scenario.max_evaluations
      });
      intervalCount += audit.entryIntervals.length + audit.releaseIntervals.length;
      ambiguityCount += audit.ambiguous.length;
      evaluations += audit.evaluations;
    }
  }
  const elapsedMs = performance.now() - started;
  const positionScale = inputs.catalogGeometry.positions.length / samplePositions.length;
  const durationScale = scenario.duration_ms / benchmarkDurationMs;
  const estimatedRecords = Math.ceil(intervalCount * positionScale * durationScale);
  return {
    sample_position_count: samplePositions.length,
    sample_duration_ms: benchmarkDurationMs,
    elapsed_ms: Math.round(elapsedMs),
    interval_count: intervalCount,
    ambiguity_count: ambiguityCount,
    margin_evaluations: evaluations,
    estimated_full_interval_records: estimatedRecords,
    estimated_single_process_hours: elapsedMs * positionScale * durationScale / 3_600_000,
    estimated_gzip_gib_at_28_bytes_per_record: estimatedRecords * 28 / 2 ** 30
  };
}

export async function runPreflight(options) {
  const inputs = await loadPlannerInputs(options);
  const started = performance.now();
  const {snapshot, snapshotAudit, plan} = planSnapshot(inputs, inputs.auditStartTimeUs);
  const planExport = exportPreflightPlan(inputs, snapshot, plan.assignments);
  const benchmark = await benchmarkVisibility(
    inputs,
    options.benchmarkPositionCount ?? 1,
    options.benchmarkDurationMs ?? Math.min(3_600_000, inputs.manifest.scenario.duration_ms)
  );
  const metrics = {
    mode: 'preflight',
    duration_days: inputs.manifest.scenario.duration_ms / 86_400_000,
    catalog_l1_count: snapshotAudit.positionCount,
    satellite_count: snapshotAudit.satelliteCount,
    initial_release_pair_count: snapshotAudit.inventoryCandidateCount,
    initial_entry_pair_count: snapshotAudit.assignmentCandidateCount,
    initial_maximum_raw_visible_l1: snapshotAudit.maximumRawVisibleCount,
    initial_assignment_success: plan.success,
    initial_failure_counts: plan.failureCounts,
    sample_plan_satellite_id: planExport.plan.satellite_id,
    sample_plan_visible_l1_count: planExport.plan.visible_l1_positions.length,
    benchmark,
    elapsed_ms: Math.round(performance.now() - started)
  };
  const chunk = await writeNdjsonGzipChunk({
    runDirectory: inputs.runDirectory,
    chunkIndex: 0,
    relativeDirectory: 'preflight',
    records: [metrics]
  });
  const summary = createAuditSummary({
    scenarioManifest: inputs.manifest,
    metrics,
    limitations: ['preflight_is_not_a_continuous_seven_day_result']
  });
  await writeEvidenceBundle({
    runDirectory: inputs.runDirectory,
    scenarioManifest: inputs.manifest,
    summary,
    chunks: [chunk]
  });
  await mkdir(join(inputs.runDirectory, 'plans'), {recursive: true});
  await writeFile(
    join(inputs.runDirectory, 'plans', `${planExport.plan.satellite_id}-preflight.json`),
    `${canonicalJson(planExport)}\n`,
    'utf8'
  );
  return {runDirectory: inputs.runDirectory, metrics, summary};
}

function shardBounds(itemCount, shardIndex, shardCount) {
  if (shardIndex < 0 || shardIndex >= shardCount) fail('shardIndex must be inside 0..shardCount-1');
  return {
    first: Math.floor(itemCount * shardIndex / shardCount),
    last: Math.floor(itemCount * (shardIndex + 1) / shardCount)
  };
}

function visibilityRecordsForPair(inputs, position, satellite) {
  const scenario = inputs.manifest.scenario;
  const audited = auditPairVisibility({
    satellite,
    observerUnit: position.observerUnit,
    startTimeUs: inputs.simulationStartTimeUs,
    endTimeUs: inputs.auditEndTimeUs,
    boundaryPaddingUs: scenario.scan_step_ms * 1000,
    orbitModel: inputs.orbitModel,
    entryElevationDeg: scenario.entry_elevation_deg,
    releaseElevationDeg: scenario.exit_elevation_deg,
    toleranceUs: scenario.root_tolerance_ms * 1000,
    scanStepUs: scenario.scan_step_ms * 1000,
    marginTolerance: scenario.margin_tolerance,
    maxEvaluations: scenario.max_evaluations
  });
  const records = [];
  const appendWindowedRecord = (record) => {
    for (const [window, startTimeUs, endTimeUs] of [
      ['initialization', inputs.simulationStartTimeUs, inputs.auditStartTimeUs],
      ['audit', inputs.auditStartTimeUs, inputs.auditEndTimeUs]
    ]) {
      const clippedStartTimeUs = Math.max(record.start_time_us, startTimeUs);
      const clippedEndTimeUs = Math.min(record.end_time_us, endTimeUs);
      if (clippedEndTimeUs > clippedStartTimeUs) {
        records.push({
          ...record,
          window,
          start_time_us: clippedStartTimeUs,
          end_time_us: clippedEndTimeUs
        });
      }
    }
  };
  for (const [threshold, intervals] of [
    ['entry', audited.entryIntervals],
    ['release', audited.releaseIntervals]
  ]) {
    for (const interval of intervals) appendWindowedRecord({
      record_type: 'visibility_interval',
      threshold,
      start_time_us: interval.startTimeUs,
      end_time_us: interval.endTimeUs,
      position_id: position.positionId,
      satellite_id: satellite.id
    });
  }
  for (const ambiguity of audited.ambiguous) appendWindowedRecord({
    record_type: 'visibility_ambiguity',
    threshold: ambiguity.threshold,
    start_time_us: ambiguity.startTimeUs,
    end_time_us: ambiguity.endTimeUs,
    reason: ambiguity.reason,
    position_id: position.positionId,
    satellite_id: satellite.id
  });
  return {records, exact: audited.exact, evaluations: audited.evaluations};
}

function validateRunnerCheckpoint(checkpoint, options, bounds) {
  const state = checkpoint.state;
  const keys = Object.keys(state).sort(compareText);
  const expected = ['audit_record_count', 'chunks', 'exact', 'initialization_record_count', 'margin_evaluations',
    'next_position_index', 'phase', 'record_count', 'shard_count', 'shard_index'].sort(compareText);
  if (canonicalJson(keys) !== canonicalJson(expected) || state.phase !== 'visibility' ||
      state.shard_index !== options.shardIndex || state.shard_count !== options.shardCount ||
      !Number.isSafeInteger(state.next_position_index) || state.next_position_index < bounds.first ||
      state.next_position_index > bounds.last || !Array.isArray(state.chunks) || typeof state.exact !== 'boolean' ||
      !Number.isSafeInteger(state.record_count) || !Number.isSafeInteger(state.initialization_record_count) ||
      !Number.isSafeInteger(state.audit_record_count) || !Number.isSafeInteger(state.margin_evaluations)) {
    fail('checkpoint state is invalid for this visibility shard');
  }
  return state;
}

async function loadVisibilityRecords(runDirectory, chunks) {
  const records = [];
  for (const chunk of chunks) {
    const bytes = await readFile(join(runDirectory, ...chunk.path.split('/')));
    const text = gunzipSync(bytes).toString('utf8');
    for (const line of text.trimEnd().split('\n')) {
      if (line.length > 0) records.push(JSON.parse(line));
    }
  }
  return records;
}

function timelineInputFromVisibilityRecords(inputs, records) {
  const initialVisibility = [];
  const events = [];
  for (const record of records) {
    if (record.record_type !== 'visibility_interval') continue;
    const isEntry = record.threshold === 'entry';
    if (record.start_time_us === inputs.simulationStartTimeUs) {
      initialVisibility.push({
        positionId: record.position_id,
        satelliteId: record.satellite_id,
        aboveEntry: isEntry,
        aboveRelease: true
      });
    } else events.push({
      timeUs: record.start_time_us,
      kind: isEntry ? 'entry_enter' : 'release_enter',
      positionId: record.position_id,
      satelliteId: record.satellite_id
    });
    if (record.end_time_us < inputs.auditEndTimeUs) events.push({
      timeUs: record.end_time_us,
      kind: isEntry ? 'entry_exit' : 'release_exit',
      positionId: record.position_id,
      satelliteId: record.satellite_id
    });
  }
  const initialByPair = new Map();
  for (const item of initialVisibility) {
    const key = `${item.positionId}\0${item.satelliteId}`;
    const existing = initialByPair.get(key) ?? {...item, aboveEntry: false, aboveRelease: false};
    existing.aboveEntry ||= item.aboveEntry;
    existing.aboveRelease ||= item.aboveRelease;
    initialByPair.set(key, existing);
  }
  return {initialVisibility: [...initialByPair.values()], events};
}

async function finishInlineAssignment(inputs, chunks) {
  const records = await loadVisibilityRecords(inputs.runDirectory, chunks);
  const timelineInput = timelineInputFromVisibilityRecords(inputs, records);
  const common = {
    positionIds: inputs.catalogGeometry.positions.map(({positionId}) => positionId),
    registry: inputs.registry,
    satelliteCapacity: inputs.manifest.scenario.satellite_capacity,
    cellCapacity: inputs.manifest.scenario.cell_capacity
  };
  const preRoll = replayAssignmentTimeline({
    startTimeUs: inputs.simulationStartTimeUs,
    endTimeUs: inputs.auditStartTimeUs,
    initialVisibility: timelineInput.initialVisibility,
    events: timelineInput.events.filter(({timeUs}) => timeUs < inputs.auditStartTimeUs),
    ...common
  });
  const timeline = replayAssignmentTimeline({
    startTimeUs: inputs.auditStartTimeUs,
    endTimeUs: inputs.auditEndTimeUs,
    initialVisibility: preRoll.finalVisibility,
    initialAssignments: preRoll.finalAssignments,
    events: timelineInput.events.filter(({timeUs}) => timeUs >= inputs.auditStartTimeUs),
    ...common
  });
  const assignmentIntervals = [
    ...preRoll.intervals.map((item) => ({...item, window: 'initialization'})),
    ...timeline.intervals.map((item) => ({...item, window: 'audit'}))
  ].map((item) => ({
    record_type: 'assignment_interval',
    window: item.window,
    start_time_us: item.startTimeUs,
    end_time_us: item.endTimeUs,
    position_id: item.positionId,
    satellite_id: item.satelliteId,
    cell_bank: item.cellBank,
    nci: item.nci,
    pci: item.pci
  }));
  const assignmentChunk = await writeNdjsonGzipChunk({
    runDirectory: inputs.runDirectory,
    chunkIndex: 0,
    relativeDirectory: 'assignments',
    records: assignmentIntervals
  });
  const timelineLedger = (source, window) => [
    ...source.epochs.map((epoch) => ({
      record_type: 'assignment_epoch',
      window,
      time_us: epoch.timeUs,
      success: epoch.success,
      failure_counts: epoch.failureCounts
    })),
    ...source.failureIntervals.map((item) => ({
      record_type: 'unassigned_interval',
      window,
      start_time_us: item.startTimeUs,
      end_time_us: item.endTimeUs,
      position_id: item.positionId,
      reason: item.reason
    })),
    ...source.transitions.map((item) => ({
      record_type: 'assignment_transition',
      window,
      time_us: item.timeUs,
      position_id: item.positionId,
      kind: item.kind,
      before: item.before,
      after: item.after
    }))
  ];
  const auditLedger = [
    ...timelineLedger(preRoll, 'initialization'),
    ...timelineLedger(timeline, 'audit')
  ];
  const ledgerChunk = await writeNdjsonGzipChunk({
    runDirectory: inputs.runDirectory,
    chunkIndex: 0,
    relativeDirectory: 'assignment-ledger',
    records: auditLedger
  });
  const geometryPass = timeline.epochs.every(({failureCounts}) => failureCounts.no_visible_candidate === 0);
  const assignmentPass = timeline.epochs.every(({success}) => success);
  return {
    timeline,
    evidenceChunks: [assignmentChunk, ledgerChunk],
    geometryPass,
    assignmentPass,
    assignmentIntervals,
    auditLedger
  };
}

export async function runVisibilityAudit(options) {
  const inputs = await loadPlannerInputs(options);
  const shardIndex = options.shardIndex ?? 0;
  const shardCount = options.shardCount ?? inputs.manifest.scenario.satellite_shard_count;
  if (shardCount !== inputs.manifest.scenario.satellite_shard_count) {
    fail('shardCount must match scenario.satellite_shard_count');
  }
  const satelliteBounds = shardBounds(inputs.satellites.length, shardIndex, shardCount);
  const positionBounds = {first: 0, last: inputs.catalogGeometry.positions.length};
  const shardSatellites = inputs.satellites.slice(satelliteBounds.first, satelliteBounds.last);
  const checkpointPath = join(
    inputs.runDirectory,
    'checkpoints',
    `visibility-${String(shardIndex).padStart(5, '0')}-of-${String(shardCount).padStart(5, '0')}.json`
  );
  let sequence = 0;
  let state = {
    phase: 'visibility',
    shard_index: shardIndex,
    shard_count: shardCount,
    next_position_index: positionBounds.first,
    chunks: [],
    record_count: 0,
    initialization_record_count: 0,
    audit_record_count: 0,
    margin_evaluations: 0,
    exact: true
  };
  if (options.resume) {
    const checkpoint = await loadCheckpoint(checkpointPath, {expectedInputHash: inputs.inputHash});
    sequence = checkpoint.sequence;
    state = validateRunnerCheckpoint(checkpoint, {shardIndex, shardCount}, positionBounds);
  }
  const positionsPerChunk = options.positionsPerChunk ?? inputs.manifest.scenario.positions_per_chunk;
  if (positionsPerChunk !== inputs.manifest.scenario.positions_per_chunk) {
    fail('positionsPerChunk must match scenario.positions_per_chunk');
  }
  const stopAt = options.maxPositions === undefined
    ? positionBounds.last
    : Math.min(
      positionBounds.last,
      state.next_position_index + Math.ceil(options.maxPositions / positionsPerChunk) * positionsPerChunk
    );
  while (state.next_position_index < stopAt) {
    const chunkFirst = state.next_position_index;
    const chunkLast = Math.min(positionBounds.last, chunkFirst + positionsPerChunk);
    const records = [];
    let exact = true;
    let evaluations = 0;
    for (let positionIndex = chunkFirst; positionIndex < chunkLast; positionIndex += 1) {
      const position = inputs.catalogGeometry.positions[positionIndex];
      for (const satellite of shardSatellites) {
        const pair = visibilityRecordsForPair(inputs, position, satellite);
        records.push(...pair.records);
        exact &&= pair.exact;
        evaluations += pair.evaluations;
      }
    }
    const chunkIndex = Math.floor((chunkFirst - positionBounds.first) / positionsPerChunk);
    const chunk = await writeNdjsonGzipChunk({
      runDirectory: inputs.runDirectory,
      chunkIndex,
      relativeDirectory: `visibility/shard-${String(shardIndex).padStart(5, '0')}-of-${String(shardCount).padStart(5, '0')}`,
      records
    });
    state = {
      ...state,
      next_position_index: chunkLast,
      chunks: [...state.chunks.filter(({path}) => path !== chunk.path), chunk].sort((left, right) => compareText(left.path, right.path)),
      record_count: state.record_count + records.length,
      initialization_record_count: state.initialization_record_count +
        records.filter(({window}) => window === 'initialization').length,
      audit_record_count: state.audit_record_count + records.filter(({window}) => window === 'audit').length,
      margin_evaluations: state.margin_evaluations + evaluations,
      exact: state.exact && exact
    };
    sequence += 1;
    await saveCheckpoint(checkpointPath, {inputHash: inputs.inputHash, sequence, state});
  }
  const shardComplete = state.next_position_index === positionBounds.last;
  const globalVisibilityComplete = shardComplete && shardCount === 1;
  let assignment;
  if (globalVisibilityComplete && inputs.catalogGeometry.positions.length <= MAX_INLINE_ASSIGNMENT_POSITIONS) {
    assignment = await finishInlineAssignment(inputs, state.chunks);
  }
  const complete = globalVisibilityComplete && state.exact && assignment !== undefined;
  const metrics = {
    mode: 'event_visibility_and_nominal_assignment',
    shard_index: shardIndex,
    shard_count: shardCount,
    shard_first_satellite_index: satelliteBounds.first,
    shard_last_satellite_index_exclusive: satelliteBounds.last,
    shard_satellite_count: shardSatellites.length,
    next_position_index: state.next_position_index,
    visibility_record_count: state.audit_record_count,
    initialization_record_count: state.initialization_record_count,
    total_visibility_record_count: state.record_count,
    margin_evaluations: state.margin_evaluations,
    visibility_exact_under_model: globalVisibilityComplete && state.exact,
    assignment_interval_count: assignment?.assignmentIntervals.length ?? 0,
    owner_change_count: assignment?.timeline.ownerChangeCount ?? 0,
    handover_count: assignment?.timeline.handoverCount ?? 0,
    release_count: assignment?.timeline.releaseCount ?? 0,
    acquisition_count: assignment?.timeline.acquisitionCount ?? 0,
    unassigned_interval_count: assignment?.timeline.failureIntervals.length ?? 0,
    assignment_ledger_record_count: assignment?.auditLedger.length ?? 0,
    peak_assigned_l1_by_satellite: assignment?.timeline.peakAssignedBySatellite ?? [],
    peak_assigned_l1_by_nci: assignment?.timeline.peakAssignedByNci ?? [],
    assignment_strategy: assignment !== undefined ? 'inline' : 'external_stream_required',
    assignment_phase_complete: assignment !== undefined
  };
  const summary = createAuditSummary({
    scenarioManifest: inputs.manifest,
    completionStatus: complete ? 'complete' : 'incomplete',
    geometryResult: complete ? (assignment.geometryPass ? 'pass' : 'fail') : 'not_run',
    geometryExactUnderModel: complete,
    nominalAssignmentResult: complete ? (assignment.assignmentPass ? 'pass' : 'fail') : 'not_run',
    nominalAssignmentExactUnderModel: complete,
    metrics,
    limitations: complete ? [] : [
      'seven_day_global_assignment_not_complete',
      ...(globalVisibilityComplete && assignment === undefined ? ['external_stream_assignment_required'] : [])
    ]
  });
  const chunks = [...state.chunks, ...(assignment?.evidenceChunks ?? [])];
  await writeEvidenceBundle({runDirectory: inputs.runDirectory, scenarioManifest: inputs.manifest, summary, chunks});
  return {runDirectory: inputs.runDirectory, checkpointPath, shardComplete, complete, metrics, summary};
}

export async function mergeVisibilityShards(options) {
  const inputs = await loadPlannerInputs(options);
  const shardCount = options.shardCount ?? inputs.manifest.scenario.satellite_shard_count;
  if (shardCount !== inputs.manifest.scenario.satellite_shard_count) {
    fail('shardCount must match scenario.satellite_shard_count');
  }
  const positionBounds = {first: 0, last: inputs.catalogGeometry.positions.length};
  const chunks = [];
  let recordCount = 0;
  let initializationRecordCount = 0;
  let auditRecordCount = 0;
  let marginEvaluations = 0;
  let visibilityExact = true;
  for (let shardIndex = 0; shardIndex < shardCount; shardIndex += 1) {
    const checkpointPath = join(
      inputs.runDirectory,
      'checkpoints',
      `visibility-${String(shardIndex).padStart(5, '0')}-of-${String(shardCount).padStart(5, '0')}.json`
    );
    const checkpoint = await loadCheckpoint(checkpointPath, {expectedInputHash: inputs.inputHash});
    const state = validateRunnerCheckpoint(checkpoint, {shardIndex, shardCount}, positionBounds);
    if (state.next_position_index !== positionBounds.last) fail(`visibility shard ${shardIndex} is incomplete`);
    chunks.push(...state.chunks);
    recordCount += state.record_count;
    initializationRecordCount += state.initialization_record_count;
    auditRecordCount += state.audit_record_count;
    marginEvaluations += state.margin_evaluations;
    visibilityExact &&= state.exact;
  }
  chunks.sort((left, right) => compareText(left.path, right.path));
  if (new Set(chunks.map(({path}) => path)).size !== chunks.length) fail('visibility shard chunks overlap');
  let assignment;
  if (visibilityExact && inputs.catalogGeometry.positions.length <= MAX_INLINE_ASSIGNMENT_POSITIONS) {
    assignment = await finishInlineAssignment(inputs, chunks);
  }
  const complete = visibilityExact && assignment !== undefined;
  const metrics = {
    mode: 'merged_event_visibility_and_nominal_assignment',
    shard_count: shardCount,
    visibility_record_count: auditRecordCount,
    initialization_record_count: initializationRecordCount,
    total_visibility_record_count: recordCount,
    margin_evaluations: marginEvaluations,
    visibility_exact_under_model: visibilityExact,
    assignment_interval_count: assignment?.assignmentIntervals.length ?? 0,
    owner_change_count: assignment?.timeline.ownerChangeCount ?? 0,
    handover_count: assignment?.timeline.handoverCount ?? 0,
    release_count: assignment?.timeline.releaseCount ?? 0,
    acquisition_count: assignment?.timeline.acquisitionCount ?? 0,
    unassigned_interval_count: assignment?.timeline.failureIntervals.length ?? 0,
    assignment_ledger_record_count: assignment?.auditLedger.length ?? 0,
    peak_assigned_l1_by_satellite: assignment?.timeline.peakAssignedBySatellite ?? [],
    peak_assigned_l1_by_nci: assignment?.timeline.peakAssignedByNci ?? [],
    assignment_strategy: assignment !== undefined ? 'inline' : 'external_stream_required',
    assignment_phase_complete: assignment !== undefined
  };
  const summary = createAuditSummary({
    scenarioManifest: inputs.manifest,
    completionStatus: complete ? 'complete' : 'incomplete',
    geometryResult: complete ? (assignment.geometryPass ? 'pass' : 'fail') : 'not_run',
    geometryExactUnderModel: complete,
    nominalAssignmentResult: complete ? (assignment.assignmentPass ? 'pass' : 'fail') : 'not_run',
    nominalAssignmentExactUnderModel: complete,
    metrics,
    limitations: complete ? [] : [
      'seven_day_global_assignment_not_complete',
      ...(visibilityExact && assignment === undefined ? ['external_stream_assignment_required'] : [])
    ]
  });
  const evidenceChunks = [...chunks, ...(assignment?.evidenceChunks ?? [])];
  await writeEvidenceBundle({
    runDirectory: inputs.runDirectory,
    scenarioManifest: inputs.manifest,
    summary,
    chunks: evidenceChunks
  });
  return {runDirectory: inputs.runDirectory, complete, metrics, summary};
}

const SLAB_RUNNER_CHECKPOINT_PHASE = 'slab_visibility_stream_v1';

function assertExactObjectKeys(value, expected, context) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) fail(`${context} must be an object`);
  const actual = Object.keys(value).sort(compareText);
  const wanted = [...expected].sort(compareText);
  if (canonicalJson(actual) !== canonicalJson(wanted)) fail(`${context} has missing or unknown fields`);
}

function slabShardLabel(shardIndex, shardCount) {
  return `shard-${String(shardIndex).padStart(5, '0')}-of-${String(shardCount).padStart(5, '0')}`;
}

function slabCheckpointPath(inputs, shardIndex, shardCount) {
  return join(inputs.runDirectory, 'checkpoints', `slab-${slabShardLabel(shardIndex, shardCount)}.json`);
}

function slabJournalDirectory(shardIndex, shardCount) {
  return `slab-journal/${slabShardLabel(shardIndex, shardCount)}`;
}

function slabEventChunkDirectory(shardIndex, shardCount) {
  return `slab-events/${slabShardLabel(shardIndex, shardCount)}`;
}

function slabIntervalChunkDirectory(shardIndex, shardCount) {
  return `slab-intervals/${slabShardLabel(shardIndex, shardCount)}`;
}

function validateSlabJournalIdentity(entries, inputs, shardIndex, shardCount) {
  const slabDurationUs = inputs.manifest.scenario.slab_duration_ms * 1000;
  for (const entry of entries) {
    const expectedStartTimeUs = inputs.simulationStartTimeUs + entry.part_index * slabDurationUs;
    const expectedEndTimeUs = Math.min(inputs.auditEndTimeUs, expectedStartTimeUs + slabDurationUs);
    const partName = `part-${String(entry.part_index).padStart(6, '0')}.ndjson.gz`;
    const expectedEventPath = `${slabEventChunkDirectory(shardIndex, shardCount)}/${partName}`;
    const expectedIntervalPath = `${slabIntervalChunkDirectory(shardIndex, shardCount)}/${partName}`;
    if (entry.input_hash !== inputs.inputHash || entry.shard_index !== shardIndex ||
        entry.shard_count !== shardCount || entry.slab_start_time_us !== expectedStartTimeUs ||
        entry.slab_end_time_us !== expectedEndTimeUs || entry.event_chunk.path !== expectedEventPath ||
        entry.interval_chunk.path !== expectedIntervalPath) {
      fail(`slab journal identity mismatch for shard ${shardIndex} part ${entry.part_index}`);
    }
  }
  return entries;
}

function slabContextForInputs(inputs, shardIndex, shardCount) {
  const scenario = inputs.manifest.scenario;
  return createSlabVisibilityContext({
    satellites: inputs.satellites,
    catalogGeometry: inputs.catalogGeometry,
    orbitModel: inputs.orbitModel,
    entryElevationDeg: scenario.entry_elevation_deg,
    releaseElevationDeg: scenario.exit_elevation_deg,
    boundaryPaddingUs: scenario.slab_boundary_padding_ms * 1000,
    toleranceUs: scenario.root_tolerance_ms * 1000,
    scanStepUs: scenario.slab_scan_step_ms * 1000,
    screeningStepUs: scenario.slab_scan_step_ms * 1000,
    marginTolerance: scenario.margin_tolerance,
    maxEvaluations: scenario.max_evaluations,
    satelliteShard: {index: shardIndex, count: shardCount}
  });
}

function compactSlabState(context, state) {
  const positionIndex = new Map(
    context.catalogGeometry.positions.map(({positionId}, index) => [positionId, index]));
  const satelliteIndex = new Map(context.satelliteIds.map((satelliteId, index) => [satelliteId, index]));
  return {
    schema_version: state.schemaVersion,
    context_hash: state.contextHash,
    audit_start_time_us: state.auditStartTimeUs,
    audit_end_time_us: state.auditEndTimeUs,
    slab_duration_us: state.slabDurationUs,
    next_slab_start_time_us: state.nextSlabStartTimeUs,
    open_pairs: state.openPairs.map((pair) => [
      positionIndex.get(pair.positionId),
      satelliteIndex.get(pair.satelliteId),
      pair.entryStartTimeUs,
      pair.releaseStartTimeUs
    ]),
    metrics: state.metrics
  };
}

function expandSlabState(context, compact) {
  assertExactObjectKeys(compact, [
    'schema_version', 'context_hash', 'audit_start_time_us', 'audit_end_time_us',
    'slab_duration_us', 'next_slab_start_time_us', 'open_pairs', 'metrics'
  ], 'slab runner state');
  if (!Array.isArray(compact.open_pairs)) fail('slab runner open_pairs must be an array');
  const positionIds = context.catalogGeometry.positions.map(({positionId}) => positionId);
  const openPairs = compact.open_pairs.map((pair, index) => {
    if (!Array.isArray(pair) || pair.length !== 4 ||
        !Number.isSafeInteger(pair[0]) || pair[0] < 0 || pair[0] >= positionIds.length ||
        !Number.isSafeInteger(pair[1]) || pair[1] < 0 || pair[1] >= context.satelliteIds.length) {
      fail(`slab runner open_pairs[${index}] is invalid`);
    }
    return {
      positionId: positionIds[pair[0]],
      satelliteId: context.satelliteIds[pair[1]],
      entryStartTimeUs: pair[2],
      releaseStartTimeUs: pair[3]
    };
  });
  return restoreSlabVisibilityState(context, {
    schemaVersion: compact.schema_version,
    contextHash: compact.context_hash,
    auditStartTimeUs: compact.audit_start_time_us,
    auditEndTimeUs: compact.audit_end_time_us,
    slabDurationUs: compact.slab_duration_us,
    nextSlabStartTimeUs: compact.next_slab_start_time_us,
    satelliteIds: context.satelliteIds,
    positionIds,
    openPairs,
    metrics: compact.metrics
  });
}

function emptySlabRecordCounts() {
  return {
    total_record_count: 0,
    initialization_record_count: 0,
    audit_record_count: 0,
    crossing_event_count: 0,
    initial_state_record_count: 0,
    visibility_interval_record_count: 0,
    ambiguity_record_count: 0
  };
}

function createSlabRunnerCheckpointState(context, slabState, shardIndex, shardCount, recordCounts) {
  return {
    phase: SLAB_RUNNER_CHECKPOINT_PHASE,
    shard_index: shardIndex,
    shard_count: shardCount,
    slab_state: compactSlabState(context, slabState),
    ...recordCounts
  };
}

function validateSlabRunnerCheckpoint(checkpoint, context, shardIndex, shardCount) {
  const state = checkpoint.state;
  assertExactObjectKeys(state, [
    'phase', 'shard_index', 'shard_count', 'slab_state',
    ...Object.keys(emptySlabRecordCounts())
  ], 'slab runner checkpoint');
  if (state.phase !== SLAB_RUNNER_CHECKPOINT_PHASE || state.shard_index !== shardIndex ||
      state.shard_count !== shardCount) {
    fail('slab runner checkpoint belongs to a different shard');
  }
  const slabState = expandSlabState(context, state.slab_state);
  const expectedSequence = Math.ceil(
    (slabState.nextSlabStartTimeUs - slabState.auditStartTimeUs) / slabState.slabDurationUs);
  if (checkpoint.sequence !== expectedSequence) fail('slab runner checkpoint sequence is inconsistent');
  const recordCounts = {};
  for (const key of Object.keys(emptySlabRecordCounts())) {
    if (!Number.isSafeInteger(state[key]) || state[key] < 0) fail(`slab runner ${key} is invalid`);
    recordCounts[key] = state[key];
  }
  if (recordCounts.total_record_count !==
      recordCounts.initialization_record_count + recordCounts.audit_record_count) {
    fail('slab runner record window counts are inconsistent');
  }
  return {slabState, recordCounts};
}

function appendWindowedSlabInterval(records, inputs, record) {
  for (const [window, startTimeUs, endTimeUs] of [
    ['initialization', inputs.simulationStartTimeUs, inputs.auditStartTimeUs],
    ['audit', inputs.auditStartTimeUs, inputs.auditEndTimeUs]
  ]) {
    const clippedStartTimeUs = Math.max(record.start_time_us, startTimeUs);
    const clippedEndTimeUs = Math.min(record.end_time_us, endTimeUs);
    if (clippedEndTimeUs > clippedStartTimeUs) {
      records.push({...record, window, start_time_us: clippedStartTimeUs, end_time_us: clippedEndTimeUs});
    }
  }
}

function recordsForVisibilitySlab(inputs, result) {
  const eventRecords = result.events.map((event) => ({
    record_type: 'visibility_event',
    window: event.timeUs < inputs.auditStartTimeUs ? 'initialization' : 'audit',
    time_us: event.timeUs,
    kind: event.kind,
    threshold: event.kind.startsWith('entry_') ? 'entry' : 'release',
    position_id: event.positionId,
    satellite_id: event.satelliteId
  }));
  const intervalRecords = [];
  for (const [threshold, intervals] of [
    ['entry', result.entryIntervals],
    ['release', result.releaseIntervals]
  ]) {
    for (const interval of intervals) appendWindowedSlabInterval(intervalRecords, inputs, {
      record_type: 'visibility_interval',
      threshold,
      start_time_us: interval.startTimeUs,
      end_time_us: interval.endTimeUs,
      position_id: interval.positionId,
      satellite_id: interval.satelliteId
    });
  }
  for (const ambiguity of result.ambiguous) appendWindowedSlabInterval(eventRecords, inputs, {
    record_type: 'visibility_ambiguity',
    threshold: ambiguity.threshold,
    start_time_us: ambiguity.startTimeUs,
    end_time_us: ambiguity.endTimeUs,
    reason: ambiguity.reason,
    position_id: ambiguity.positionId,
    satellite_id: ambiguity.satelliteId
  });
  return {eventRecords, intervalRecords};
}

function initialStateRecordsForSlab(inputs, result) {
  return result.initialStates.map((initial) => ({
    record_type: 'visibility_initial_state',
    kind: 'initial_state',
    window: 'initialization',
    time_us: inputs.simulationStartTimeUs,
    position_id: initial.positionId,
    satellite_id: initial.satelliteId,
    above_entry: initial.aboveEntry,
    above_release: initial.aboveRelease
  }));
}

function addSlabRecordCounts(counts, recordGroups) {
  const next = {...counts};
  for (const records of recordGroups) {
    for (const record of records) {
      next.total_record_count += 1;
      if (record.window === 'initialization') next.initialization_record_count += 1;
      else if (record.window === 'audit') next.audit_record_count += 1;
      if (record.record_type === 'visibility_event') next.crossing_event_count += 1;
      else if (record.record_type === 'visibility_initial_state') next.initial_state_record_count += 1;
      else if (record.record_type === 'visibility_interval') next.visibility_interval_record_count += 1;
      else if (record.record_type === 'visibility_ambiguity') next.ambiguity_record_count += 1;
    }
  }
  if (Object.values(next).some((value) => !Number.isSafeInteger(value))) {
    fail('slab runner record count exceeds safe integer range');
  }
  return next;
}

function addSlabMetrics(target, source) {
  if (target === undefined) return {...source};
  const result = {...target, exact: target.exact && source.exact};
  for (const [key, value] of Object.entries(source)) {
    if (key !== 'exact') result[key] += value;
  }
  return result;
}

async function collectSlabRun(inputs, shardCount) {
  const chunks = [];
  const journals = Array(shardCount);
  let metrics;
  let recordCounts = emptySlabRecordCounts();
  let completedShardCount = 0;
  for (let shardIndex = 0; shardIndex < shardCount; shardIndex += 1) {
    const context = slabContextForInputs(inputs, shardIndex, shardCount);
    let checkpoint;
    try {
      checkpoint = await loadCheckpoint(
        slabCheckpointPath(inputs, shardIndex, shardCount),
        {expectedInputHash: inputs.inputHash}
      );
    } catch (error) {
      if (error?.code === 'read_failed') continue;
      throw error;
    }
    const validated = validateSlabRunnerCheckpoint(checkpoint, context, shardIndex, shardCount);
    const journal = await readSlabJournalEntries({
      runDirectory: inputs.runDirectory,
      relativeDirectory: slabJournalDirectory(shardIndex, shardCount),
      expectedInputHash: inputs.inputHash
    });
    validateSlabJournalIdentity(journal, inputs, shardIndex, shardCount);
    if (journal.length !== checkpoint.sequence) fail(`slab journal ${shardIndex} does not match its checkpoint`);
    if (validated.slabState.nextSlabStartTimeUs !== validated.slabState.auditEndTimeUs) continue;
    journals[shardIndex] = journal;
    completedShardCount += 1;
    for (const entry of journal) {
      chunks.push(entry.event_chunk, entry.interval_chunk);
    }
    metrics = addSlabMetrics(metrics, validated.slabState.metrics);
    for (const key of Object.keys(recordCounts)) recordCounts[key] += validated.recordCounts[key];
  }
  return {
    complete: completedShardCount === shardCount,
    completedShardCount,
    chunks: chunks.sort((left, right) => compareText(left.path, right.path)),
    journals,
    metrics,
    recordCounts
  };
}

function slabSummary(inputs, shardIndex, shardCount, slabState, recordCounts, collected) {
  const geometryComplete = collected?.complete ?? false;
  const geometryMetrics = geometryComplete ? collected.metrics : slabState.metrics;
  const computationExact = geometryComplete && geometryMetrics.exact;
  const counts = geometryComplete ? collected.recordCounts : recordCounts;
  const metrics = {
    mode: 'time_slab_visibility_stream',
    catalog_l1_count: inputs.catalogGeometry.positions.length,
    satellite_count: inputs.satellites.length,
    shard_index: geometryComplete ? null : shardIndex,
    shard_count: shardCount,
    shard_satellite_count: geometryComplete ? inputs.satellites.length : slabState.satelliteIds.length,
    completed_satellite_shard_count: collected?.completedShardCount ?? 0,
    next_slab_start_time_us: slabState.nextSlabStartTimeUs,
    simulation_start_time_us: inputs.simulationStartTimeUs,
    audit_start_time_us: inputs.auditStartTimeUs,
    audit_end_time_us: inputs.auditEndTimeUs,
    slab_duration_ms: inputs.manifest.scenario.slab_duration_ms,
    slab_boundary_padding_ms: inputs.manifest.scenario.slab_boundary_padding_ms,
    slab_scan_step_ms: inputs.manifest.scenario.slab_scan_step_ms,
    geometry_stream_complete: geometryComplete,
    visibility_computation_exact_under_model: computationExact,
    assignment_strategy: 'external_stream_assignment_required',
    assignment_phase_complete: false,
    ...counts,
    slab_engine_metrics: geometryMetrics
  };
  const summary = createAuditSummary({
    scenarioManifest: inputs.manifest,
    completionStatus: 'incomplete',
    metrics,
    limitations: [
      'external_stream_assignment_required',
      ...(geometryComplete ? ['nominal_assignment_not_run'] : ['seven_day_global_visibility_not_complete']),
      ...(geometryComplete && !computationExact ? ['geometry_ambiguity_requires_resolution'] : []),
      ...(geometryComplete ? ['geometry_coverage_not_evaluated'] : [])
    ]
  });
  return {metrics, summary};
}

/**
 * Runs the large-catalog path. It is intentionally separate from the pair
 * runner above and stops after geometry; a later streaming assignment consumes
 * the time-ordered crossing events one slab at a time.
 */
export async function runSlabVisibilityStream(options) {
  const inputs = await loadPlannerInputs(options);
  const scenario = inputs.manifest.scenario;
  const shardIndex = options.shardIndex ?? 0;
  const shardCount = options.shardCount ?? scenario.satellite_shard_count;
  if (shardCount !== scenario.satellite_shard_count) fail('shardCount must match scenario.satellite_shard_count');
  if (!Number.isSafeInteger(shardIndex) || shardIndex < 0 || shardIndex >= shardCount) {
    fail('shardIndex must be inside 0..shardCount-1');
  }
  const context = slabContextForInputs(inputs, shardIndex, shardCount);
  const checkpointPath = slabCheckpointPath(inputs, shardIndex, shardCount);
  const journalDirectory = slabJournalDirectory(shardIndex, shardCount);
  let journal = await readSlabJournalEntries({
    runDirectory: inputs.runDirectory,
    relativeDirectory: journalDirectory,
    expectedInputHash: inputs.inputHash
  });
  validateSlabJournalIdentity(journal, inputs, shardIndex, shardCount);
  let sequence = 0;
  let slabState = createSlabVisibilityState({
    context,
    auditStartTimeUs: inputs.simulationStartTimeUs,
    auditEndTimeUs: inputs.auditEndTimeUs,
    slabDurationUs: scenario.slab_duration_ms * 1000
  });
  let recordCounts = emptySlabRecordCounts();
  if (options.resume) {
    const checkpoint = await loadCheckpoint(checkpointPath, {expectedInputHash: inputs.inputHash});
    sequence = checkpoint.sequence;
    ({slabState, recordCounts} = validateSlabRunnerCheckpoint(
      checkpoint, context, shardIndex, shardCount));
    if (journal.length < sequence || journal.length > sequence + 1) {
      fail('slab journal does not match the resumable checkpoint');
    }
  } else if (journal.length !== 0) {
    fail('slab output already exists; use resume-slab to continue it');
  }
  const maxSlabs = options.maxSlabs ?? Number.MAX_SAFE_INTEGER;
  let processed = 0;
  while (slabState.nextSlabStartTimeUs < slabState.auditEndTimeUs && processed < maxSlabs) {
    const result = processNextVisibilitySlab({context, state: slabState});
    const {eventRecords, intervalRecords} = recordsForVisibilitySlab(inputs, result);
    if (sequence === 0) {
      for (const record of initialStateRecordsForSlab(inputs, result)) eventRecords.push(record);
    }
    const eventChunk = await writeNdjsonGzipChunk({
      runDirectory: inputs.runDirectory,
      chunkIndex: sequence,
      relativeDirectory: slabEventChunkDirectory(shardIndex, shardCount),
      records: eventRecords
    });
    const intervalChunk = await writeNdjsonGzipChunk({
      runDirectory: inputs.runDirectory,
      chunkIndex: sequence,
      relativeDirectory: slabIntervalChunkDirectory(shardIndex, shardCount),
      records: intervalRecords
    });
    const journalEntry = {
      schema_version: SLAB_JOURNAL_SCHEMA_VERSION,
      input_hash: inputs.inputHash,
      part_index: sequence,
      shard_index: shardIndex,
      shard_count: shardCount,
      slab_start_time_us: result.slabStartTimeUs,
      slab_end_time_us: result.slabEndTimeUs,
      event_chunk: eventChunk,
      interval_chunk: intervalChunk
    };
    if (journal[sequence] !== undefined) {
      if (canonicalJson(journal[sequence]) !== canonicalJson(journalEntry)) {
        fail(`orphan slab journal part ${sequence} is not reproducible`);
      }
    } else {
      await writeSlabJournalEntry({
        runDirectory: inputs.runDirectory,
        relativeDirectory: journalDirectory,
        entry: journalEntry
      });
      journal = [...journal, journalEntry];
    }
    slabState = result.state;
    recordCounts = addSlabRecordCounts(recordCounts, [eventRecords, intervalRecords]);
    sequence += 1;
    await saveCheckpoint(checkpointPath, {
      inputHash: inputs.inputHash,
      sequence,
      state: createSlabRunnerCheckpointState(context, slabState, shardIndex, shardCount, recordCounts)
    });
    processed += 1;
  }
  if (journal.length !== sequence) fail('slab journal has uncommitted trailing parts');
  const shardComplete = slabState.nextSlabStartTimeUs === slabState.auditEndTimeUs;
  const {metrics, summary} = slabSummary(inputs, shardIndex, shardCount, slabState, recordCounts, undefined);
  return {
    runDirectory: inputs.runDirectory,
    checkpointPath,
    shardComplete,
    geometryStreamComplete: false,
    assignmentComplete: false,
    evidenceWritten: false,
    slabsProcessed: processed,
    metrics,
    summary
  };
}

/**
 * Finalizes geometry evidence after every satellite shard has completed. This
 * is deliberately separate from the shard runner so parallel workers never
 * compete to replace the same summary, manifest and checksum files.
 */
export async function finalizeSlabVisibilityRun(options) {
  const inputs = await loadPlannerInputs(options);
  const shardCount = options.shardCount ?? inputs.manifest.scenario.satellite_shard_count;
  if (shardCount !== inputs.manifest.scenario.satellite_shard_count) {
    fail('shardCount must match scenario.satellite_shard_count');
  }
  const collected = await collectSlabRun(inputs, shardCount);
  if (!collected.complete) {
    fail(`cannot finalize slab evidence: completed ${collected.completedShardCount}/${shardCount} satellite shards`);
  }
  const summaryState = {
    satelliteIds: slabContextForInputs(inputs, 0, shardCount).satelliteIds,
    nextSlabStartTimeUs: inputs.auditEndTimeUs,
    metrics: collected.metrics
  };
  const {metrics, summary} = slabSummary(
    inputs, 0, shardCount, summaryState, collected.recordCounts, collected);
  const written = await writeEvidenceBundle({
    runDirectory: inputs.runDirectory,
    scenarioManifest: inputs.manifest,
    summary,
    chunks: collected.chunks,
    deepVerifyChunks: false
  });
  await verifyEvidenceBundle(inputs.runDirectory, {
    expectedInputHash: inputs.inputHash,
    deepVerifyChunks: false,
    verifyChunkFiles: false
  });
  return {
    runDirectory: inputs.runDirectory,
    geometryStreamComplete: true,
    assignmentComplete: false,
    evidenceWritten: true,
    evidenceVerified: true,
    chunkCount: written.manifest.chunks.length,
    metrics,
    summary
  };
}

const ASSIGNMENT_FAILURE_REASONS = Object.freeze([
  'no_visible_candidate',
  'schedule_overflow',
  'cell_partition_overflow'
]);

function chunkSetHash(chunks) {
  return sha256Bytes(canonicalJson(chunks));
}

function failureIntervalsAreZero(metrics, reason) {
  return metrics.failure_interval_count_by_reason[reason] === 0 &&
    metrics.failure_duration_us_by_reason[reason] === '0';
}

function assignmentFailureIsZero(metrics, reason) {
  return metrics.failure_peak_counts[reason] === 0 && failureIntervalsAreZero(metrics, reason);
}

async function requireFinalizedSlabEvidence(inputs, collected) {
  let verified;
  try {
    verified = await verifyEvidenceBundle(inputs.runDirectory, {
      expectedInputHash: inputs.inputHash,
      deepVerifyChunks: false,
      verifyChunkFiles: false
    });
  } catch (error) {
    throw new Error(`finalized slab evidence is required before assignment: ${error.message}`, {cause: error});
  }
  const chunksByPath = new Map(verified.manifest.chunks.map((chunk) => [chunk.path, chunk]));
  for (const chunk of collected.chunks) {
    if (canonicalJson(chunksByPath.get(chunk.path)) !== canonicalJson(chunk)) {
      fail(`finalized slab evidence does not match geometry chunk '${chunk.path}'`);
    }
  }
  if (verified.summary.metrics.geometry_stream_complete !== true) {
    fail('finalized slab evidence does not certify a complete geometry stream');
  }
  const geometryPaths = new Set(collected.chunks.map(({path}) => path));
  const additionalChunks = verified.manifest.chunks.filter(({path}) => !geometryPaths.has(path));
  if (additionalChunks.length === 0) {
    if (verified.manifest.chunks.length !== collected.chunks.length) {
      fail('finalized slab evidence does not contain the exact geometry chunk set');
    }
    return {phase: 'geometry_only', verified};
  }
  if (verified.summary.completion_status !== 'complete' ||
      verified.summary.metrics.assignment_phase_complete !== true ||
      additionalChunks.some(({path}) => !path.startsWith('streaming-assignments/')) ||
      verified.summary.metrics.assignment_chunk_count !== additionalChunks.length ||
      verified.summary.metrics.total_chunk_count !== verified.manifest.chunks.length) {
    fail('finalized slab evidence contains chunks outside the complete geometry set');
  }
  return {phase: 'combined', verified};
}

function compactAssignmentStatus(inputs, assignment, extra = {}) {
  return {
    runDirectory: inputs.runDirectory,
    complete: assignment.complete,
    geometryStreamComplete: true,
    assignmentComplete: assignment.complete,
    evidenceWritten: false,
    evidenceVerified: false,
    partsProcessed: assignment.partsProcessed,
    nextInputPartIndex: assignment.nextInputPartIndex,
    checkpointPath: assignment.checkpointPath,
    assignmentChunkCount: assignment.chunks.length,
    assignmentRecordCount: assignment.recordCounts.total_record_count,
    outputPrefixHash: assignment.outputPrefixHash,
    outputHash: assignment.outputHash,
    ledgerSetHash: assignment.ledgerSetHash,
    ...extra
  };
}

function streamingAssignmentLedgerOptions(inputs, collected, extra = {}) {
  const scenario = inputs.manifest.scenario;
  return {
    runDirectory: inputs.runDirectory,
    journals: collected.journals,
    inputHash: inputs.inputHash,
    sourceHash: inputs.manifest.engine.source_sha256,
    positionIds: inputs.catalogGeometry.positions.map(({positionId}) => positionId),
    registry: inputs.registry,
    simulationStartTimeUs: inputs.simulationStartTimeUs,
    auditStartTimeUs: inputs.auditStartTimeUs,
    auditEndTimeUs: inputs.auditEndTimeUs,
    satelliteCapacity: scenario.satellite_capacity,
    cellCapacity: scenario.cell_capacity,
    ...extra
  };
}

/**
 * Consumes every completed geometry shard and publishes the final report only
 * after the complete assignment timeline has been written successfully.
 */
export async function runStreamingAssignmentAudit(options) {
  const inputs = await loadPlannerInputs(options);
  const scenario = inputs.manifest.scenario;
  const shardCount = scenario.satellite_shard_count;
  const collected = await collectSlabRun(inputs, shardCount);
  if (!collected.complete) {
    fail(`cannot run assignment: completed ${collected.completedShardCount}/${shardCount} satellite shards`);
  }
  const publishedEvidence = await requireFinalizedSlabEvidence(inputs, collected);
  const assignment = await runStreamingAssignmentLedger(streamingAssignmentLedgerOptions(inputs, collected, {
    resume: options.resume === true,
    maxParts: options.maxParts
  }));
  const compact = compactAssignmentStatus(inputs, assignment);
  if (!assignment.complete) return compact;

  const geometryExact = collected.metrics.exact === true &&
    collected.recordCounts.ambiguity_record_count === 0;
  if (!geometryExact) {
    fail('assignment completed without exact geometry evidence');
  }
  const geometryPass = assignmentFailureIsZero(assignment.metrics, 'no_visible_candidate');
  const nominalAssignmentPass = ASSIGNMENT_FAILURE_REASONS.every(
    (reason) => assignmentFailureIsZero(assignment.metrics, reason));
  const metrics = {
    mode: 'time_slab_visibility_and_streaming_assignment',
    input_hash: inputs.inputHash,
    source_hash: inputs.manifest.engine.source_sha256,
    catalog_l1_count: inputs.catalogGeometry.positions.length,
    satellite_count: inputs.satellites.length,
    shard_count: shardCount,
    geometry_stream_complete: true,
    visibility_computation_exact_under_model: true,
    assignment_strategy: 'streaming_capacity_timeline',
    assignment_phase_complete: true,
    geometry_chunk_count: collected.chunks.length,
    assignment_chunk_count: assignment.chunks.length,
    total_chunk_count: collected.chunks.length + assignment.chunks.length,
    geometry_chunk_set_hash: chunkSetHash(collected.chunks),
    assignment_ledger_set_hash: assignment.ledgerSetHash,
    assignment_output_prefix_hash: assignment.outputPrefixHash,
    assignment_output_hash: assignment.outputHash,
    geometry_record_counts: collected.recordCounts,
    geometry_engine_metrics: collected.metrics,
    assignment_record_counts: assignment.recordCounts,
    assignment_metrics: assignment.metrics
  };
  const summary = createAuditSummary({
    scenarioManifest: inputs.manifest,
    completionStatus: 'complete',
    geometryResult: geometryPass ? 'pass' : 'fail',
    geometryExactUnderModel: true,
    nominalAssignmentResult: nominalAssignmentPass ? 'pass' : 'fail',
    nominalAssignmentExactUnderModel: true,
    metrics,
    limitations: ['rf_execution_not_audited']
  });
  const evidenceChunks = [...collected.chunks, ...assignment.chunks];
  if (publishedEvidence.phase === 'combined') {
    const expectedChunks = [...evidenceChunks].sort((left, right) => compareText(left.path, right.path));
    if (canonicalJson(publishedEvidence.verified.manifest.chunks) !== canonicalJson(expectedChunks) ||
        canonicalJson(publishedEvidence.verified.summary) !== canonicalJson(summary)) {
      fail('published combined evidence does not match the recovered assignment output');
    }
  }
  const written = await writeEvidenceBundle({
    runDirectory: inputs.runDirectory,
    scenarioManifest: inputs.manifest,
    summary,
    chunks: evidenceChunks,
    deepVerifyChunks: false
  });
  await verifyEvidenceBundle(inputs.runDirectory, {
    expectedInputHash: inputs.inputHash,
    deepVerifyChunks: false,
    verifyChunkFiles: false
  });
  return compactAssignmentStatus(inputs, assignment, {
    evidenceWritten: true,
    evidenceVerified: true,
    geometryResult: summary.geometry.result,
    nominalAssignmentResult: summary.nominal_assignment.result,
    geometryChunkCount: collected.chunks.length,
    totalChunkCount: written.manifest.chunks.length
  });
}

/**
 * Exports the left-limit assignment state immediately before one processed
 * slab boundary. The export is deliberately outside the global evidence
 * manifest: it is a read-only, replay-bound planning view, not new audit proof.
 */
export async function exportStreamingAssignmentPlan(options) {
  if (!Number.isSafeInteger(options.planTimeUs)) fail('planTimeUs must be a safe integer');
  if (typeof options.satelliteId !== 'string' || !/^P\d{2}-S\d{2}$/.test(options.satelliteId)) {
    fail('satelliteId must use canonical Pxx-Syy form');
  }
  const inputs = await loadPlannerInputs(options);
  if (options.planTimeUs < inputs.auditStartTimeUs || options.planTimeUs >= inputs.auditEndTimeUs) {
    fail(`planTimeUs must be inside [${inputs.auditStartTimeUs},${inputs.auditEndTimeUs})`);
  }
  const registrySatellite = inputs.registry.satellites.find(
    ({satellite_id: satelliteId}) => satelliteId === options.satelliteId
  );
  if (registrySatellite === undefined) {
    fail(`unknown registry satellite '${options.satelliteId}'`);
  }

  const collected = await collectSlabRun(inputs, inputs.manifest.scenario.satellite_shard_count);
  if (!collected.complete) {
    fail(`cannot export plan: completed ${collected.completedShardCount}/${inputs.manifest.scenario.satellite_shard_count} satellite shards`);
  }
  await requireFinalizedSlabEvidence(inputs, collected);
  const snapshot = await readStreamingAssignmentSnapshot(
    streamingAssignmentLedgerOptions(inputs, collected)
  );
  if (snapshot.processedPartCount < 1) {
    fail('assignment checkpoint has no processed time part');
  }
  if (options.planTimeUs !== snapshot.checkpointTimeUs - 1) {
    fail(
      `planTimeUs must equal the checkpoint left-limit ${snapshot.checkpointTimeUs - 1}; ` +
      'use run-assignment or resume-assignment --max-parts to stop at the desired slab boundary'
    );
  }

  const positionById = new Map(inputs.catalogGeometry.positions.map((position) => [position.positionId, position]));
  const visibleInventory = snapshot.completeCandidates.filter(({candidates}) => {
    return candidates.some(({satelliteId}) => satelliteId === options.satelliteId);
  }).map(({positionId}) => {
    const position = positionById.get(positionId);
    if (position === undefined) fail(`assignment checkpoint contains unknown position '${positionId}'`);
    return {
      id: position.positionId,
      lat: position.latitudeDeg,
      lon: position.longitudeDeg,
      childMask: position.childMask
    };
  });
  const planTimeUnixUs = inputs.manifest.scenario.orbit_epoch_unix_ms * 1000 + options.planTimeUs;
  if (!Number.isSafeInteger(planTimeUnixUs)) fail('planning Unix time exceeds the safe integer range');
  const planningTimeUnixMs = Math.floor(planTimeUnixUs / 1000);
  const timeQuantizationUs = planTimeUnixUs - planningTimeUnixMs * 1000;
  if (!Number.isSafeInteger(planningTimeUnixMs) || planningTimeUnixMs < 0) {
    fail('planning Unix time must be a non-negative safe integer millisecond');
  }
  const exported = exportDryRunSatellitePlan({
    satelliteId: options.satelliteId,
    planningTimeUnixMs,
    visibleInventory,
    assignments: snapshot.assignments,
    identityRegistry: inputs.registry,
    planningContext: {
      planningRunId: inputs.manifest.run_id,
      catalog: {
        id: inputs.manifest.inputs.catalog.id,
        sha256: inputs.manifest.inputs.catalog.sha256
      },
      identityRegistry: {...inputs.manifest.inputs.identity_registry},
      accessProfile: {...inputs.manifest.inputs.access_profile},
      catalogVersion: 1,
      scheduleVersion: snapshot.processedPartCount,
      validFromUnixMs: planningTimeUnixMs,
      validUntilUnixMs: planningTimeUnixMs + 1,
      activationEpochUnixMs: planningTimeUnixMs
    }
  });
  const basename = `${options.satelliteId}-${options.planTimeUs}`;
  const planFile = await writeCanonicalJsonArtifact({
    runDirectory: inputs.runDirectory,
    relativePath: `plans/${basename}.plan.json`,
    value: exported.plan
  });
  const sidecarFile = await writeCanonicalJsonArtifact({
    runDirectory: inputs.runDirectory,
    relativePath: `plans/${basename}.assignment.json`,
    value: exported.assignment_sidecar
  });
  const assignedL1Count = exported.assignment_sidecar.assigned_l1_positions.length;
  return {
    runDirectory: inputs.runDirectory,
    satelliteId: options.satelliteId,
    planTimeUs: options.planTimeUs,
    planTimeReference: 'orbit_epoch_relative',
    planningTimeUnixMs,
    timeQuantizationUs,
    checkpointTimeUs: snapshot.checkpointTimeUs,
    processedPartCount: snapshot.processedPartCount,
    visibleL1Count: visibleInventory.length,
    assignedL1Count,
    unassignedL1Count: visibleInventory.length - assignedL1Count,
    plan: {...planFile, content_hash: exported.plan.content_hash},
    assignmentSidecar: {
      ...sidecarFile,
      content_hash: exported.assignment_sidecar.content_hash
    },
    validator: {...exported.plan_validation}
  };
}

/** Reads one time part across shards in canonical event order without a global sort. */
export async function readMergedSlabPart({runDirectory, journals, partIndex}) {
  if (!Array.isArray(journals) || !Number.isSafeInteger(partIndex) || partIndex < 0) {
    fail('readMergedSlabPart requires journals and a non-negative partIndex');
  }
  const entries = journals.map((journal, shardIndex) => {
    if (!Array.isArray(journal) || journal[partIndex] === undefined) {
      fail(`slab journal ${shardIndex} does not contain part ${partIndex}`);
    }
    return journal[partIndex];
  });
  const inputHash = entries[0].input_hash;
  const shardCount = entries[0].shard_count;
  if (journals.length !== shardCount || entries.some((entry) => entry.input_hash !== inputHash ||
      entry.shard_count !== shardCount)) {
    fail(`slab journals do not form one complete ${shardCount}-shard input set`);
  }
  const shardIndexes = entries.map(({shard_index: shardIndex}) => shardIndex).sort((left, right) => left - right);
  if (shardIndexes.some((shardIndex, index) => shardIndex !== index)) {
    fail('slab journals contain a duplicate or missing satellite shard');
  }
  for (const entry of entries) {
    const partName = `part-${String(partIndex).padStart(6, '0')}.ndjson.gz`;
    const expectedEventPath = `${slabEventChunkDirectory(entry.shard_index, shardCount)}/${partName}`;
    const expectedIntervalPath = `${slabIntervalChunkDirectory(entry.shard_index, shardCount)}/${partName}`;
    if (entry.event_chunk.path !== expectedEventPath || entry.interval_chunk.path !== expectedIntervalPath) {
      fail(`slab journal ${entry.shard_index} points outside its shard part`);
    }
  }
  const startTimeUs = entries[0].slab_start_time_us;
  const endTimeUs = entries[0].slab_end_time_us;
  if (entries.some((entry) => entry.part_index !== partIndex ||
      entry.slab_start_time_us !== startTimeUs || entry.slab_end_time_us !== endTimeUs)) {
    fail(`slab journals disagree on time range for part ${partIndex}`);
  }
  entries.sort((left, right) => left.shard_index - right.shard_index);
  return readMergedEvidenceChunks({runDirectory, chunks: entries.map(({event_chunk: chunk}) => chunk)});
}

function parseArguments(argv) {
  const command = argv[0];
  if (![
    'preflight', 'run', 'resume', 'run-slab', 'resume-slab', 'finalize-slab',
    'run-assignment', 'resume-assignment', 'export-plan', 'verify'
  ].includes(command)) {
    fail('first argument must be preflight, run, resume, run-slab, resume-slab, finalize-slab, run-assignment, resume-assignment, export-plan or verify');
  }
  const options = {
    command,
    scenarioPath: defaultScenarioPath,
    catalogPath: defaultCatalogPath,
    registryPath: defaultRegistryPath,
    outputRoot: resolve(repositoryRoot, 'run_artifacts', 'ntn_planning')
  };
  const take = (index, name) => {
    const value = argv[index + 1];
    if (value === undefined || value.startsWith('--')) fail(`${name} requires a value`);
    return value;
  };
  for (let index = 1; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === '--scenario') options.scenarioPath = resolve(take(index++, argument));
    else if (argument === '--catalog') options.catalogPath = resolve(take(index++, argument));
    else if (argument === '--registry') options.registryPath = resolve(take(index++, argument));
    else if (argument === '--output-root') options.outputRoot = resolve(take(index++, argument));
    else if (argument === '--run-directory') options.runDirectory = resolve(take(index++, argument));
    else if (argument === '--benchmark-positions') options.benchmarkPositionCount = integer(take(index++, argument), argument, 1);
    else if (argument === '--benchmark-duration-ms') options.benchmarkDurationMs = integer(take(index++, argument), argument, 1);
    else if (argument === '--shard-index') options.shardIndex = integer(take(index++, argument), argument, 0);
    else if (argument === '--shard-count') options.shardCount = integer(take(index++, argument), argument, 1);
    else if (argument === '--positions-per-chunk') options.positionsPerChunk = integer(take(index++, argument), argument, 1);
    else if (argument === '--max-positions') options.maxPositions = integer(take(index++, argument), argument, 1);
    else if (argument === '--max-slabs') options.maxSlabs = integer(take(index++, argument), argument, 1);
    else if (argument === '--max-parts') options.maxParts = integer(take(index++, argument), argument, 1);
    else if (argument === '--satellite-id') options.satelliteId = take(index++, argument);
    else if (argument === '--plan-time-us') options.planTimeUs = integer(take(index++, argument), argument);
    else if (argument === '--shallow') options.shallow = true;
    else if (argument === '--merge-shards') options.mergeShards = true;
    else if (argument === '--help' || argument === '-h') options.help = true;
    else fail(`unknown option '${argument}'`);
  }
  if (options.shardIndex !== undefined && options.shardCount === undefined) fail('--shard-index requires --shard-count');
  if (command === 'finalize-slab' && options.shardIndex !== undefined) {
    fail('--shard-index is not valid for finalize-slab');
  }
  if (['run-assignment', 'resume-assignment', 'export-plan'].includes(command) &&
      (options.shardIndex !== undefined || options.shardCount !== undefined)) {
    fail('--shard-index and --shard-count are not valid here; every scenario shard is required');
  }
  if (options.maxSlabs !== undefined && !['run-slab', 'resume-slab'].includes(command)) {
    fail('--max-slabs is only valid for run-slab or resume-slab');
  }
  if (options.maxParts !== undefined && !['run-assignment', 'resume-assignment'].includes(command)) {
    fail('--max-parts is only valid for run-assignment or resume-assignment');
  }
  if (options.shallow === true && command !== 'verify') {
    fail('--shallow is only valid for verify');
  }
  if (options.runDirectory !== undefined && command !== 'verify') {
    fail('--run-directory is only valid for verify');
  }
  if ((options.satelliteId !== undefined || options.planTimeUs !== undefined) && command !== 'export-plan') {
    fail('--satellite-id and --plan-time-us are only valid for export-plan');
  }
  if (command === 'export-plan' &&
      (options.satelliteId === undefined || options.planTimeUs === undefined)) {
    fail('export-plan requires --satellite-id and --plan-time-us');
  }
  if (command === 'export-plan' && options.mergeShards === true) {
    fail('--merge-shards is not used by export-plan');
  }
  return options;
}

function usage() {
  return [
    'Usage: node utils/ntn/constellation_audit.mjs <preflight|run|resume|run-slab|resume-slab|finalize-slab|run-assignment|resume-assignment|export-plan|verify> [options]',
    '  --scenario FILE --catalog FILE --registry FILE --output-root DIR',
    '  preflight: [--benchmark-positions N --benchmark-duration-ms N]',
    '  run/resume: [--shard-index N --shard-count N --positions-per-chunk N --max-positions N]',
    '  run-slab/resume-slab: [--shard-index N --shard-count N --max-slabs N]',
    '  finalize-slab: [--shard-count N] (requires every satellite shard to be complete)',
    '  run-assignment/resume-assignment: [--max-parts N] (always consumes every scenario shard)',
    '  export-plan: --satellite-id Pxx-Syy --plan-time-us N (N is relative to orbit_epoch_unix_ms; reads one checkpoint left-limit)',
    '  run --merge-shards --shard-count N: merge completed satellite shards',
    '  verify: --run-directory DIR [--shallow]'
  ].join('\n');
}

export async function main(argv = process.argv.slice(2)) {
  const options = parseArguments(argv);
  if (options.help) {
    process.stdout.write(`${usage()}\n`);
    return null;
  }
  let result;
  if (options.command === 'preflight') result = await runPreflight(options);
  else if (options.command === 'run' || options.command === 'resume') {
    result = options.mergeShards
      ? await mergeVisibilityShards(options)
      : await runVisibilityAudit({...options, resume: options.command === 'resume'});
  } else if (options.command === 'run-slab' || options.command === 'resume-slab') {
    if (options.mergeShards) fail('--merge-shards is not used by the time-slab path');
    result = await runSlabVisibilityStream({...options, resume: options.command === 'resume-slab'});
  } else if (options.command === 'finalize-slab') {
    if (options.mergeShards) fail('--merge-shards is not used by finalize-slab');
    result = await finalizeSlabVisibilityRun(options);
  } else if (options.command === 'run-assignment' || options.command === 'resume-assignment') {
    if (options.mergeShards) fail('--merge-shards is not used by assignment');
    result = await runStreamingAssignmentAudit({
      ...options,
      resume: options.command === 'resume-assignment'
    });
  } else if (options.command === 'export-plan') {
    result = await exportStreamingAssignmentPlan(options);
  } else {
    if (!options.runDirectory) fail('verify requires --run-directory');
    result = await verifyEvidenceBundle(options.runDirectory, {deepVerifyChunks: options.shallow !== true});
  }
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  return result;
}

const isMain = process.argv[1] && pathToFileURL(resolve(process.argv[1])).href === import.meta.url;
if (isMain) {
  main().catch((error) => {
    process.stderr.write(`${error.stack ?? error.message ?? String(error)}\n`);
    process.exitCode = 1;
  });
}
