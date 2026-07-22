import {createHash} from 'node:crypto';
import {mkdir, open, readFile, readdir, rename, rm, stat} from 'node:fs/promises';
import {basename, dirname, isAbsolute, join, resolve} from 'node:path';
import {gunzipSync, gzipSync} from 'node:zlib';

export const SCENARIO_MANIFEST_SCHEMA_VERSION = 1;
export const EVIDENCE_SCHEMA_VERSION = 1;
export const CHECKPOINT_SCHEMA_VERSION = 1;
export const SLAB_JOURNAL_SCHEMA_VERSION = 2;
export const DEFAULT_AUDIT_OUTPUT_ROOT = resolve('run_artifacts', 'ntn_planning');

export const REQUIRED_PLANNING_LIMITATIONS = Object.freeze([
  'gateway_constraints_not_modeled',
  'n_minus_one_failure_audit_not_run',
  'pci_time_conflict_audit_not_run',
  'software_planning_evidence_only'
]);

const SHA256_PATTERN = /^sha256:[0-9a-f]{64}$/;
const RUN_ID_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/;
const SATELLITE_ID_PATTERN = /^P\d{2}-S\d{2}$/;
let temporaryFileSequence = 0;

export class AuditIoError extends Error {
  constructor(code, message, options = undefined) {
    super(message, options);
    this.name = 'AuditIoError';
    this.code = code;
  }
}

function fail(code, message, cause = undefined) {
  throw new AuditIoError(code, message, cause === undefined ? undefined : {cause});
}

function isPlainObject(value) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function assertPlainObject(value, context) {
  if (!isPlainObject(value)) fail('invalid_shape', `${context} must be an object`);
}

function assertExactKeys(value, context, required) {
  assertPlainObject(value, context);
  const allowed = new Set(required);
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) fail('unknown_field', `unknown field '${context}.${key}'`);
  }
  for (const key of required) {
    if (!Object.hasOwn(value, key)) fail('missing_field', `missing field '${context}.${key}'`);
  }
}

function assertString(value, context) {
  if (typeof value !== 'string' || value.length === 0) {
    fail('invalid_value', `${context} must be a non-empty string`);
  }
}

function assertFinite(value, context, minimum = -Infinity, maximum = Infinity) {
  if (!Number.isFinite(value) || value < minimum || value > maximum) {
    fail('invalid_value', `${context} must be finite and inside ${minimum}..${maximum}`);
  }
}

function assertInteger(value, context, minimum = Number.MIN_SAFE_INTEGER, maximum = Number.MAX_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
    fail('invalid_value', `${context} must be a safe integer inside ${minimum}..${maximum}`);
  }
}

function canonicalJsonValue(value, stack, context) {
  if (value === null) return 'null';
  if (typeof value === 'string' || typeof value === 'boolean') return JSON.stringify(value);
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) fail('non_canonical_value', `${context} contains a non-finite number`);
    return JSON.stringify(Object.is(value, -0) ? 0 : value);
  }
  if (typeof value !== 'object') {
    fail('non_canonical_value', `${context} contains unsupported ${typeof value}`);
  }
  if (stack.has(value)) fail('non_canonical_value', `${context} contains a cycle`);
  stack.add(value);
  try {
    if (Array.isArray(value)) {
      if (Object.keys(value).length !== value.length) {
        fail('non_canonical_value', `${context} contains a sparse array or extra array properties`);
      }
      return `[${value.map((item, index) => canonicalJsonValue(item, stack, `${context}[${index}]`)).join(',')}]`;
    }
    if (!isPlainObject(value)) fail('non_canonical_value', `${context} contains a non-JSON object`);
    const fields = Object.keys(value).sort().map((key) => {
      const encoded = canonicalJsonValue(value[key], stack, `${context}.${key}`);
      return `${JSON.stringify(key)}:${encoded}`;
    });
    return `{${fields.join(',')}}`;
  } finally {
    stack.delete(value);
  }
}

export function canonicalJson(value) {
  return canonicalJsonValue(value, new Set(), 'root');
}

export function normalizeSha256(value, context = 'sha256') {
  assertString(value, context);
  const lower = value.toLowerCase();
  const normalized = lower.startsWith('sha256:') ? lower : `sha256:${lower}`;
  if (!SHA256_PATTERN.test(normalized)) fail('invalid_hash', `${context} must be a SHA-256 digest`);
  return normalized;
}

export function sha256Bytes(value) {
  if (typeof value !== 'string' && !Buffer.isBuffer(value) && !(value instanceof Uint8Array)) {
    fail('invalid_value', 'sha256Bytes requires a string, Buffer or Uint8Array');
  }
  return `sha256:${createHash('sha256').update(value).digest('hex')}`;
}

export function canonicalJsonHash(value) {
  return sha256Bytes(canonicalJson(value));
}

function canonicalClone(value) {
  return JSON.parse(canonicalJson(value));
}

function compareText(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function validateReference(value, context, identityKey) {
  assertExactKeys(value, context, [identityKey, 'sha256']);
  assertString(value[identityKey], `${context}.${identityKey}`);
  return {[identityKey]: value[identityKey], sha256: normalizeSha256(value.sha256, `${context}.sha256`)};
}

function validateCatalogReference(value, context) {
  assertExactKeys(value, context, ['id', 'sha256', 'file_sha256']);
  assertString(value.id, `${context}.id`);
  return {
    id: value.id,
    sha256: normalizeSha256(value.sha256, `${context}.sha256`),
    file_sha256: normalizeSha256(value.file_sha256, `${context}.file_sha256`)
  };
}

function validateRunId(runId, context = 'run_id') {
  if (typeof runId !== 'string' || !RUN_ID_PATTERN.test(runId)) {
    fail('invalid_value', `${context} must be a portable identifier of at most 128 characters`);
  }
  return runId;
}

export function validateScenarioManifest(value) {
  assertExactKeys(value, 'scenario_manifest', [
    'schema_version', 'audit_kind', 'run_id', 'engine', 'scenario', 'inputs'
  ]);
  if (value.schema_version !== SCENARIO_MANIFEST_SCHEMA_VERSION) {
    fail('unsupported_schema', `scenario_manifest.schema_version must be ${SCENARIO_MANIFEST_SCHEMA_VERSION}`);
  }
  if (value.audit_kind !== 'ntn_constellation_planning_evidence') {
    fail('invalid_value', "scenario_manifest.audit_kind must be 'ntn_constellation_planning_evidence'");
  }
  validateRunId(value.run_id);

  assertExactKeys(value.engine, 'scenario_manifest.engine', ['name', 'version', 'source_revision', 'source_sha256']);
  assertString(value.engine.name, 'scenario_manifest.engine.name');
  assertString(value.engine.version, 'scenario_manifest.engine.version');
  assertString(value.engine.source_revision, 'scenario_manifest.engine.source_revision');
  const sourceSha256 = normalizeSha256(value.engine.source_sha256, 'scenario_manifest.engine.source_sha256');

  const scenario = value.scenario;
  assertExactKeys(scenario, 'scenario_manifest.scenario', [
    'scenario_id', 'altitude_km', 'inclination_deg', 'planes', 'satellites_per_plane', 'phase_factor',
    'raan_offset_deg', 'phase_offset_deg', 'orbit_epoch_unix_ms', 'start_unix_ms', 'duration_ms', 'pre_roll_ms',
    'entry_elevation_deg',
    'exit_elevation_deg', 'root_tolerance_ms', 'scan_step_ms', 'slab_duration_ms',
    'slab_boundary_padding_ms', 'slab_scan_step_ms', 'earth_radius_km', 'earth_mu_km3_s2',
    'earth_rotation_rad_per_second', 'margin_tolerance', 'max_evaluations', 'satellite_shard_count',
    'positions_per_chunk', 'satellite_capacity', 'cell_capacity'
  ]);
  assertString(scenario.scenario_id, 'scenario_manifest.scenario.scenario_id');
  assertFinite(scenario.altitude_km, 'scenario_manifest.scenario.altitude_km', 0, 100000);
  assertFinite(scenario.inclination_deg, 'scenario_manifest.scenario.inclination_deg', 0, 180);
  assertInteger(scenario.planes, 'scenario_manifest.scenario.planes', 1);
  assertInteger(scenario.satellites_per_plane, 'scenario_manifest.scenario.satellites_per_plane', 1);
  assertInteger(scenario.phase_factor, 'scenario_manifest.scenario.phase_factor', 0, scenario.planes - 1);
  assertFinite(scenario.raan_offset_deg, 'scenario_manifest.scenario.raan_offset_deg');
  assertFinite(scenario.phase_offset_deg, 'scenario_manifest.scenario.phase_offset_deg');
  assertInteger(scenario.orbit_epoch_unix_ms, 'scenario_manifest.scenario.orbit_epoch_unix_ms', 0);
  assertInteger(scenario.start_unix_ms, 'scenario_manifest.scenario.start_unix_ms', 0);
  assertInteger(scenario.duration_ms, 'scenario_manifest.scenario.duration_ms', 1);
  assertInteger(scenario.pre_roll_ms, 'scenario_manifest.scenario.pre_roll_ms', 1);
  assertFinite(scenario.entry_elevation_deg, 'scenario_manifest.scenario.entry_elevation_deg', 0, 90);
  assertFinite(scenario.exit_elevation_deg, 'scenario_manifest.scenario.exit_elevation_deg', 0, 90);
  if (scenario.exit_elevation_deg >= scenario.entry_elevation_deg) {
    fail('invalid_value', 'exit_elevation_deg must be lower than entry_elevation_deg');
  }
  assertInteger(scenario.root_tolerance_ms, 'scenario_manifest.scenario.root_tolerance_ms', 1);
  if (scenario.root_tolerance_ms > 10) fail('invalid_value', 'root_tolerance_ms must not exceed 10');
  assertInteger(scenario.scan_step_ms, 'scenario_manifest.scenario.scan_step_ms', scenario.root_tolerance_ms);
  assertInteger(scenario.slab_duration_ms, 'scenario_manifest.scenario.slab_duration_ms', 1);
  assertInteger(
    scenario.slab_boundary_padding_ms,
    'scenario_manifest.scenario.slab_boundary_padding_ms',
    scenario.root_tolerance_ms
  );
  assertInteger(
    scenario.slab_scan_step_ms,
    'scenario_manifest.scenario.slab_scan_step_ms',
    scenario.root_tolerance_ms
  );
  if (scenario.slab_duration_ms % scenario.slab_scan_step_ms !== 0) {
    fail('invalid_value', 'slab_duration_ms must be an exact multiple of slab_scan_step_ms');
  }
  if (scenario.pre_roll_ms % scenario.slab_duration_ms !== 0) {
    fail('invalid_value', 'pre_roll_ms must be an exact multiple of slab_duration_ms');
  }
  assertFinite(scenario.earth_radius_km, 'scenario_manifest.scenario.earth_radius_km', 1);
  assertFinite(scenario.earth_mu_km3_s2, 'scenario_manifest.scenario.earth_mu_km3_s2', Number.MIN_VALUE);
  assertFinite(
    scenario.earth_rotation_rad_per_second,
    'scenario_manifest.scenario.earth_rotation_rad_per_second',
    0
  );
  assertFinite(scenario.margin_tolerance, 'scenario_manifest.scenario.margin_tolerance', 0);
  assertInteger(scenario.max_evaluations, 'scenario_manifest.scenario.max_evaluations', 1);
  assertInteger(scenario.satellite_shard_count, 'scenario_manifest.scenario.satellite_shard_count', 1);
  assertInteger(scenario.positions_per_chunk, 'scenario_manifest.scenario.positions_per_chunk', 1);
  assertInteger(scenario.satellite_capacity, 'scenario_manifest.scenario.satellite_capacity', 1);
  assertInteger(scenario.cell_capacity, 'scenario_manifest.scenario.cell_capacity', 1);
  if (scenario.satellite_capacity > 2 * scenario.cell_capacity) {
    fail('invalid_value', 'satellite_capacity must fit inside the two cell capacities');
  }

  assertExactKeys(value.inputs, 'scenario_manifest.inputs', ['catalog', 'identity_registry', 'access_profile']);
  const catalog = validateCatalogReference(value.inputs.catalog, 'scenario_manifest.inputs.catalog');
  const registry = validateReference(
    value.inputs.identity_registry, 'scenario_manifest.inputs.identity_registry', 'version');
  const profile = validateReference(value.inputs.access_profile, 'scenario_manifest.inputs.access_profile', 'id');

  return {
    schema_version: SCENARIO_MANIFEST_SCHEMA_VERSION,
    audit_kind: 'ntn_constellation_planning_evidence',
    run_id: value.run_id,
    engine: {
      name: value.engine.name,
      version: value.engine.version,
      source_revision: value.engine.source_revision,
      source_sha256: sourceSha256
    },
    scenario: {
      scenario_id: scenario.scenario_id,
      altitude_km: scenario.altitude_km,
      inclination_deg: scenario.inclination_deg,
      planes: scenario.planes,
      satellites_per_plane: scenario.satellites_per_plane,
      phase_factor: scenario.phase_factor,
      raan_offset_deg: scenario.raan_offset_deg,
      phase_offset_deg: scenario.phase_offset_deg,
      orbit_epoch_unix_ms: scenario.orbit_epoch_unix_ms,
      start_unix_ms: scenario.start_unix_ms,
      duration_ms: scenario.duration_ms,
      pre_roll_ms: scenario.pre_roll_ms,
      entry_elevation_deg: scenario.entry_elevation_deg,
      exit_elevation_deg: scenario.exit_elevation_deg,
      root_tolerance_ms: scenario.root_tolerance_ms,
      scan_step_ms: scenario.scan_step_ms,
      slab_duration_ms: scenario.slab_duration_ms,
      slab_boundary_padding_ms: scenario.slab_boundary_padding_ms,
      slab_scan_step_ms: scenario.slab_scan_step_ms,
      earth_radius_km: scenario.earth_radius_km,
      earth_mu_km3_s2: scenario.earth_mu_km3_s2,
      earth_rotation_rad_per_second: scenario.earth_rotation_rad_per_second,
      margin_tolerance: scenario.margin_tolerance,
      max_evaluations: scenario.max_evaluations,
      satellite_shard_count: scenario.satellite_shard_count,
      positions_per_chunk: scenario.positions_per_chunk,
      satellite_capacity: scenario.satellite_capacity,
      cell_capacity: scenario.cell_capacity
    },
    inputs: {catalog, identity_registry: registry, access_profile: profile}
  };
}

export function scenarioInputHash(manifest) {
  return canonicalJsonHash(validateScenarioManifest(manifest));
}

export function auditRunDirectory(runId, outputRoot = DEFAULT_AUDIT_OUTPUT_ROOT) {
  return join(resolve(outputRoot), validateRunId(runId));
}

function parseJsonBytes(bytes, context) {
  let text;
  try {
    text = new TextDecoder('utf-8', {fatal: true}).decode(bytes);
  } catch (error) {
    fail('invalid_json', `${context} is not valid UTF-8`, error);
  }
  try {
    return {value: JSON.parse(text), text};
  } catch (error) {
    fail('invalid_json', `${context} is not valid JSON`, error);
  }
}

async function readBytes(path, context) {
  try {
    return await readFile(path);
  } catch (error) {
    fail('read_failed', `cannot read ${context} at ${path}`, error);
  }
}

function validateCatalogCell(cell, index, seen) {
  const context = `catalog.cells[${index}]`;
  assertExactKeys(cell, context, ['id', 'lat', 'lon', 'childMask']);
  if (typeof cell.id !== 'string' || !/^G\d{6}$/.test(cell.id)) {
    fail('invalid_catalog', `${context}.id must use G###### form`);
  }
  if (seen.has(cell.id)) fail('invalid_catalog', `duplicate catalog cell '${cell.id}'`);
  seen.add(cell.id);
  assertFinite(cell.lat, `${context}.lat`, -90, 90);
  assertFinite(cell.lon, `${context}.lon`, -180, 180);
  assertInteger(cell.childMask, `${context}.childMask`, 1, 127);
}

export async function readCatalog(path, reference) {
  const expected = validateReference(reference, 'catalog_reference', 'id');
  const bytes = await readBytes(path, 'catalog');
  const {value: catalog} = parseJsonBytes(bytes, 'catalog');
  assertExactKeys(catalog, 'catalog', ['metadata', 'cells']);
  assertPlainObject(catalog.metadata, 'catalog.metadata');
  if (catalog.metadata.version !== expected.id) {
    fail('input_identity_mismatch', `catalog id mismatch: expected ${expected.id}`);
  }
  if (!Array.isArray(catalog.cells)) fail('invalid_catalog', 'catalog.cells must be an array');
  const seen = new Set();
  catalog.cells.forEach((cell, index) => validateCatalogCell(cell, index, seen));
  assertPlainObject(catalog.metadata.integrity, 'catalog.metadata.integrity');
  const declared = normalizeSha256(catalog.metadata.integrity.sha256, 'catalog.metadata.integrity.sha256');
  const contentSha256 = sha256Bytes(JSON.stringify(catalog.cells));
  if (declared !== contentSha256) fail('input_hash_mismatch', 'catalog embedded cells hash does not match its content');
  if (expected.sha256 !== contentSha256) fail('input_hash_mismatch', 'catalog hash does not match scenario manifest');
  if (catalog.metadata.counts?.l1 !== undefined && catalog.metadata.counts.l1 !== catalog.cells.length) {
    fail('invalid_catalog', 'catalog metadata L1 count does not match cells');
  }
  return {data: catalog, fileSha256: sha256Bytes(bytes), contentSha256};
}

function validateRegistryCell(cell, context, seenNcis) {
  assertExactKeys(cell, context, ['bank', 'nci', 'pci']);
  assertInteger(cell.bank, `${context}.bank`, 0, 1);
  if (typeof cell.nci !== 'string' || !/^0x[0-9a-fA-F]{9}$/.test(cell.nci)) {
    fail('invalid_registry', `${context}.nci must be a 36-bit hexadecimal NCI`);
  }
  const nci = cell.nci.toUpperCase();
  if (seenNcis.has(nci)) fail('invalid_registry', `duplicate registry NCI ${cell.nci}`);
  seenNcis.add(nci);
  assertInteger(cell.pci, `${context}.pci`, 0, 1007);
}

export async function readIdentityRegistry(path, reference) {
  const expected = validateReference(reference, 'identity_registry_reference', 'version');
  const bytes = await readBytes(path, 'identity registry');
  const fileSha256 = sha256Bytes(bytes);
  if (fileSha256 !== expected.sha256) {
    fail('input_hash_mismatch', 'identity registry file hash does not match scenario manifest');
  }
  const {value: registry} = parseJsonBytes(bytes, 'identity registry');
  assertExactKeys(registry, 'identity_registry', ['registry_version', 'satellites']);
  if (registry.registry_version !== expected.version) {
    fail('input_identity_mismatch', `identity registry version mismatch: expected ${expected.version}`);
  }
  if (!Array.isArray(registry.satellites)) fail('invalid_registry', 'identity_registry.satellites must be an array');
  const seenSatellites = new Set();
  const seenNcis = new Set();
  for (const [index, satellite] of registry.satellites.entries()) {
    const context = `identity_registry.satellites[${index}]`;
    assertExactKeys(satellite, context, ['satellite_id', 'cells']);
    if (typeof satellite.satellite_id !== 'string' || !SATELLITE_ID_PATTERN.test(satellite.satellite_id)) {
      fail('invalid_registry', `${context}.satellite_id must use Pxx-Syy form`);
    }
    if (seenSatellites.has(satellite.satellite_id)) {
      fail('invalid_registry', `duplicate satellite '${satellite.satellite_id}'`);
    }
    seenSatellites.add(satellite.satellite_id);
    if (!Array.isArray(satellite.cells) || satellite.cells.length !== 2) {
      fail('invalid_registry', `${context}.cells must contain exactly two cells`);
    }
    satellite.cells.forEach((cell, cellIndex) => validateRegistryCell(cell, `${context}.cells[${cellIndex}]`, seenNcis));
    if (new Set(satellite.cells.map((cell) => cell.bank)).size !== 2) {
      fail('invalid_registry', `${context}.cells must contain banks 0 and 1`);
    }
  }
  return {data: registry, fileSha256};
}

async function atomicWriteFile(path, value) {
  await mkdir(dirname(path), {recursive: true});
  const temporaryPath = join(
    dirname(path), `.${basename(path)}.tmp-${process.pid}-${temporaryFileSequence++}`);
  let handle;
  try {
    handle = await open(temporaryPath, 'wx', 0o600);
    await handle.writeFile(value);
    await handle.sync();
    await handle.close();
    handle = undefined;
    await rename(temporaryPath, path);
  } catch (error) {
    if (handle !== undefined) await handle.close().catch(() => {});
    await rm(temporaryPath, {force: true}).catch(() => {});
    fail('atomic_write_failed', `cannot atomically write ${path}`, error);
  }
}

function validateRelativePath(path, context) {
  if (typeof path !== 'string' || path.length === 0 || isAbsolute(path) || path.includes('\\')) {
    fail('unsafe_path', `${context} must be a portable relative path`);
  }
  const segments = path.split('/');
  if (segments.some((segment) => segment.length === 0 || segment === '.' || segment === '..')) {
    fail('unsafe_path', `${context} must not contain empty, dot or parent segments`);
  }
  return path;
}

function localEvidencePath(runDirectory, relativePath) {
  validateRelativePath(relativePath, 'evidence path');
  return join(resolve(runDirectory), ...relativePath.split('/'));
}

function fixedGzip(bytes) {
  const compressed = gzipSync(bytes, {level: 9, mtime: 0});
  if (compressed.length < 10 || compressed[0] !== 0x1f || compressed[1] !== 0x8b || compressed[2] !== 8 || compressed[3] !== 0) {
    fail('gzip_failed', 'Node produced an unsupported gzip header');
  }
  compressed.writeUInt32LE(0, 4);
  compressed[8] = 2;
  compressed[9] = 255;
  return compressed;
}

function evidenceEventRank(kind) {
  return ({
    initial_state: -1,
    release_exit: 0,
    entry_exit: 1,
    exit: 1,
    release_enter: 2,
    entry_enter: 3,
    entry: 3
  })[kind] ?? 4;
}

function compareEvidenceRecord(left, right) {
  const leftTime = left.record.time_us ?? left.record.start_time_us ?? left.record.time_unix_ms ?? 0;
  const rightTime = right.record.time_us ?? right.record.start_time_us ?? right.record.time_unix_ms ?? 0;
  return leftTime - rightTime
    || evidenceEventRank(left.record.kind) - evidenceEventRank(right.record.kind)
    || compareText(left.record.position_id ?? '', right.record.position_id ?? '')
    || compareText(left.record.satellite_id ?? '', right.record.satellite_id ?? '')
    || compareText(String(left.record.threshold ?? left.record.threshold_deg ?? ''),
      String(right.record.threshold ?? right.record.threshold_deg ?? ''))
    || compareText(left.line, right.line);
}

function normalizeEvidenceRecords(records) {
  if (records === null || records === undefined || typeof records[Symbol.iterator] !== 'function') {
    fail('invalid_value', 'records must be iterable');
  }
  const normalized = [];
  for (const record of records) normalized.push({record, line: canonicalJson(record)});
  normalized.sort(compareEvidenceRecord);
  return normalized;
}

/** Canonical ordering used both within a shard part and when merging a time slab across shards. */
export function sortEvidenceRecords(records) {
  return normalizeEvidenceRecords(records).map(({record}) => record);
}

export async function writeNdjsonGzipChunk({runDirectory, chunkIndex, records, relativeDirectory = 'events'}) {
  assertInteger(chunkIndex, 'chunkIndex', 0);
  validateRelativePath(relativeDirectory, 'relativeDirectory');
  const normalizedRecords = normalizeEvidenceRecords(records);
  const lines = normalizedRecords.map(({line}) => line);
  const uncompressed = Buffer.from(lines.length === 0 ? '' : `${lines.join('\n')}\n`, 'utf8');
  const compressed = fixedGzip(uncompressed);
  const relativePath = `${relativeDirectory}/part-${String(chunkIndex).padStart(6, '0')}.ndjson.gz`;
  await atomicWriteFile(localEvidencePath(runDirectory, relativePath), compressed);
  return {
    path: relativePath,
    sha256: sha256Bytes(compressed),
    size_bytes: compressed.length,
    record_count: lines.length,
    uncompressed_bytes: uncompressed.length
  };
}

function validateFileRecord(value, context) {
  assertExactKeys(value, context, ['path', 'sha256', 'size_bytes']);
  validateRelativePath(value.path, `${context}.path`);
  assertInteger(value.size_bytes, `${context}.size_bytes`, 0);
  return {path: value.path, sha256: normalizeSha256(value.sha256, `${context}.sha256`), size_bytes: value.size_bytes};
}

function validateChunkRecord(value, context) {
  assertExactKeys(value, context, ['path', 'sha256', 'size_bytes', 'record_count', 'uncompressed_bytes']);
  const file = validateFileRecord(
    {path: value.path, sha256: value.sha256, size_bytes: value.size_bytes}, context);
  if (!file.path.endsWith('.ndjson.gz')) fail('invalid_evidence', `${context}.path must end in .ndjson.gz`);
  assertInteger(value.record_count, `${context}.record_count`, 0);
  assertInteger(value.uncompressed_bytes, `${context}.uncompressed_bytes`, 0);
  return {...file, record_count: value.record_count, uncompressed_bytes: value.uncompressed_bytes};
}

function validateSlabJournalEntry(value, context = 'slab_journal_entry') {
  assertExactKeys(value, context, [
    'schema_version', 'input_hash', 'part_index', 'shard_index', 'shard_count',
    'slab_start_time_us', 'slab_end_time_us', 'event_chunk', 'interval_chunk'
  ]);
  if (value.schema_version !== SLAB_JOURNAL_SCHEMA_VERSION) {
    fail('unsupported_schema', `${context}.schema_version must be ${SLAB_JOURNAL_SCHEMA_VERSION}`);
  }
  const inputHash = normalizeSha256(value.input_hash, `${context}.input_hash`);
  assertInteger(value.part_index, `${context}.part_index`, 0, 999999);
  assertInteger(value.shard_count, `${context}.shard_count`, 1);
  assertInteger(value.shard_index, `${context}.shard_index`, 0, value.shard_count - 1);
  assertInteger(value.slab_start_time_us, `${context}.slab_start_time_us`);
  assertInteger(value.slab_end_time_us, `${context}.slab_end_time_us`);
  if (value.slab_end_time_us <= value.slab_start_time_us) {
    fail('invalid_value', `${context}.slab_end_time_us must be after slab_start_time_us`);
  }
  return {
    schema_version: SLAB_JOURNAL_SCHEMA_VERSION,
    input_hash: inputHash,
    part_index: value.part_index,
    shard_index: value.shard_index,
    shard_count: value.shard_count,
    slab_start_time_us: value.slab_start_time_us,
    slab_end_time_us: value.slab_end_time_us,
    event_chunk: validateChunkRecord(value.event_chunk, `${context}.event_chunk`),
    interval_chunk: validateChunkRecord(value.interval_chunk, `${context}.interval_chunk`)
  };
}

/**
 * Writes one constant-size metadata sidecar for an already persisted time slab.
 * The checkpoint therefore never needs to copy the growing historical chunk list.
 */
export async function writeSlabJournalEntry({runDirectory, relativeDirectory, entry}) {
  validateRelativePath(relativeDirectory, 'relativeDirectory');
  const normalized = validateSlabJournalEntry(entry);
  const path = `${relativeDirectory}/part-${String(normalized.part_index).padStart(6, '0')}.json`;
  await atomicWriteFile(localEvidencePath(runDirectory, path), canonicalFileBytes(normalized));
  return normalized;
}

/** Reads the ordered sidecar journal without decompressing historical chunks. */
export async function readSlabJournalEntries({runDirectory, relativeDirectory, expectedInputHash}) {
  validateRelativePath(relativeDirectory, 'relativeDirectory');
  const directory = localEvidencePath(runDirectory, relativeDirectory);
  let names;
  try {
    names = await readdir(directory);
  } catch (error) {
    if (error?.code === 'ENOENT') return [];
    fail('read_failed', `cannot read slab journal at ${directory}`, error);
  }
  const expectedHash = expectedInputHash === undefined
    ? undefined
    : normalizeSha256(expectedInputHash, 'expectedInputHash');
  const entries = [];
  for (const name of names.sort(compareText)) {
    const match = /^part-(\d{6})\.json$/.exec(name);
    if (match === null) fail('invalid_evidence', `unexpected slab journal file '${name}'`);
    const bytes = await readBytes(join(directory, name), 'slab journal entry');
    const parsed = parseJsonBytes(bytes, 'slab journal entry');
    const entry = validateSlabJournalEntry(parsed.value, `slab_journal_entry[${entries.length}]`);
    if (parsed.text !== `${canonicalJson(entry)}\n`) {
      fail('invalid_evidence', `${relativeDirectory}/${name} is not canonical JSON`);
    }
    if (entry.part_index !== Number(match[1])) {
      fail('invalid_evidence', `${relativeDirectory}/${name} part index mismatch`);
    }
    if (expectedHash !== undefined && entry.input_hash !== expectedHash) {
      fail('input_hash_mismatch', `${relativeDirectory}/${name} belongs to a different scenario input`);
    }
    entries.push(entry);
  }
  for (const [index, entry] of entries.entries()) {
    if (entry.part_index !== index) {
      fail('invalid_evidence', `${relativeDirectory} has a missing or duplicate part before ${entry.part_index}`);
    }
  }
  return entries;
}

async function verifyFileRecord(runDirectory, record, context) {
  const bytes = await readBytes(localEvidencePath(runDirectory, record.path), context);
  if (bytes.length !== record.size_bytes) fail('evidence_size_mismatch', `${record.path} size mismatch`);
  if (sha256Bytes(bytes) !== record.sha256) fail('evidence_hash_mismatch', `${record.path} hash mismatch`);
  return bytes;
}

async function verifyChunkEnvelope(runDirectory, record) {
  const bytes = await verifyFileRecord(runDirectory, record, 'event chunk');
  if (bytes.length < 10 || bytes[0] !== 0x1f || bytes[1] !== 0x8b || bytes[2] !== 8 || bytes[3] !== 0 ||
      bytes.readUInt32LE(4) !== 0 || bytes[8] !== 2 || bytes[9] !== 255) {
    fail('invalid_evidence', `${record.path} does not use the fixed gzip header`);
  }
  return bytes;
}

async function verifyChunk(runDirectory, record) {
  const bytes = await verifyChunkEnvelope(runDirectory, record);
  let uncompressed;
  try {
    uncompressed = gunzipSync(bytes);
  } catch (error) {
    fail('invalid_evidence', `${record.path} cannot be decompressed`, error);
  }
  if (uncompressed.length !== record.uncompressed_bytes) {
    fail('evidence_size_mismatch', `${record.path} uncompressed size mismatch`);
  }
  let text;
  try {
    text = new TextDecoder('utf-8', {fatal: true}).decode(uncompressed);
  } catch (error) {
    fail('invalid_evidence', `${record.path} is not valid UTF-8`, error);
  }
  if (text.length !== 0 && !text.endsWith('\n')) fail('invalid_evidence', `${record.path} lacks its final newline`);
  const lines = text.length === 0 ? [] : text.slice(0, -1).split('\n');
  if (lines.length !== record.record_count) fail('invalid_evidence', `${record.path} record count mismatch`);
  const records = [];
  for (const [index, line] of lines.entries()) {
    let parsed;
    try {
      parsed = JSON.parse(line);
    } catch (error) {
      fail('invalid_evidence', `${record.path} line ${index + 1} is invalid JSON`, error);
    }
    if (canonicalJson(parsed) !== line) fail('invalid_evidence', `${record.path} line ${index + 1} is not canonical JSON`);
    records.push(parsed);
  }
  return records;
}

/**
 * Loads and canonically merges a bounded set of parts, typically the same
 * time slab from all satellite shards. It deliberately does not scan any
 * other slab, so a later streaming assignment remains memory bounded.
 */
export async function readMergedEvidenceChunks({runDirectory, chunks}) {
  if (!Array.isArray(chunks)) fail('invalid_value', 'chunks must be an array');
  const records = [];
  for (const [index, chunk] of chunks.entries()) {
    const normalized = validateChunkRecord(chunk, `chunks[${index}]`);
    for (const record of await verifyChunk(runDirectory, normalized)) records.push(record);
  }
  return sortEvidenceRecords(records);
}

export function createAuditSummary({
  scenarioManifest,
  completionStatus = 'incomplete',
  geometryResult = 'not_run',
  geometryExactUnderModel = false,
  nominalAssignmentResult = 'not_run',
  nominalAssignmentExactUnderModel = false,
  metrics = {},
  limitations = []
}) {
  const scenario = validateScenarioManifest(scenarioManifest);
  if (!['incomplete', 'complete'].includes(completionStatus)) {
    fail('invalid_summary', 'completionStatus must be incomplete or complete');
  }
  if (!['not_run', 'pass', 'fail'].includes(geometryResult)) {
    fail('invalid_summary', 'geometryResult must be not_run, pass or fail');
  }
  if (!['not_run', 'pass', 'fail'].includes(nominalAssignmentResult)) {
    fail('invalid_summary', 'nominalAssignmentResult must be not_run, pass or fail');
  }
  if (typeof geometryExactUnderModel !== 'boolean' || typeof nominalAssignmentExactUnderModel !== 'boolean') {
    fail('invalid_summary', 'exact_under_model flags must be boolean');
  }
  if ((geometryResult === 'not_run') !== !geometryExactUnderModel ||
      (nominalAssignmentResult === 'not_run') !== !nominalAssignmentExactUnderModel) {
    fail('invalid_summary', 'a model result and exact_under_model flag must be set together');
  }
  assertPlainObject(metrics, 'metrics');
  if (!Array.isArray(limitations) || limitations.some((item) => typeof item !== 'string')) {
    fail('invalid_summary', 'limitations must be an array of strings');
  }
  return {
    schema_version: EVIDENCE_SCHEMA_VERSION,
    run_id: scenario.run_id,
    input_hash: scenarioInputHash(scenario),
    completion_status: completionStatus,
    exact: false,
    selection_eligible: false,
    selectedScenario: null,
    geometry: {exact_under_model: geometryExactUnderModel, result: geometryResult},
    nominal_assignment: {
      exact_under_model: nominalAssignmentExactUnderModel,
      result: nominalAssignmentResult
    },
    metrics: canonicalClone(metrics),
    limitations: [...new Set([...limitations, ...REQUIRED_PLANNING_LIMITATIONS])].sort(compareText)
  };
}

export function validateAuditSummary(value) {
  assertExactKeys(value, 'summary', [
    'schema_version', 'run_id', 'input_hash', 'completion_status', 'exact', 'selection_eligible',
    'selectedScenario', 'geometry', 'nominal_assignment', 'metrics', 'limitations'
  ]);
  if (value.schema_version !== EVIDENCE_SCHEMA_VERSION) fail('unsupported_schema', 'unsupported summary schema');
  validateRunId(value.run_id, 'summary.run_id');
  const inputHash = normalizeSha256(value.input_hash, 'summary.input_hash');
  if (!['incomplete', 'complete'].includes(value.completion_status)) fail('invalid_summary', 'invalid completion_status');
  if (value.exact !== false || value.selection_eligible !== false || value.selectedScenario !== null) {
    fail('invalid_summary', 'planning evidence cannot select or certify a constellation scenario');
  }
  const validateModelResult = (model, context) => {
    assertExactKeys(model, context, ['exact_under_model', 'result']);
    if (typeof model.exact_under_model !== 'boolean' || !['not_run', 'pass', 'fail'].includes(model.result)) {
      fail('invalid_summary', `${context} is invalid`);
    }
    if ((model.result === 'not_run') !== !model.exact_under_model) {
      fail('invalid_summary', `${context} result and exact_under_model are inconsistent`);
    }
    return {...model};
  };
  const geometry = validateModelResult(value.geometry, 'summary.geometry');
  const nominalAssignment = validateModelResult(value.nominal_assignment, 'summary.nominal_assignment');
  assertPlainObject(value.metrics, 'summary.metrics');
  if (!Array.isArray(value.limitations) || value.limitations.some((item) => typeof item !== 'string')) {
    fail('invalid_summary', 'summary.limitations must be an array of strings');
  }
  const normalizedLimitations = [...new Set(value.limitations)].sort(compareText);
  if (canonicalJson(normalizedLimitations) !== canonicalJson(value.limitations)) {
    fail('invalid_summary', 'summary.limitations must be unique and sorted');
  }
  for (const required of REQUIRED_PLANNING_LIMITATIONS) {
    if (!normalizedLimitations.includes(required)) {
      fail('invalid_summary', `summary.limitations must include '${required}'`);
    }
  }
  return {
    ...value,
    input_hash: inputHash,
    geometry,
    nominal_assignment: nominalAssignment,
    metrics: canonicalClone(value.metrics),
    limitations: normalizedLimitations
  };
}

function canonicalFileBytes(value) {
  return Buffer.from(`${canonicalJson(value)}\n`, 'utf8');
}

function fileRecord(path, bytes) {
  return {path, sha256: sha256Bytes(bytes), size_bytes: bytes.length};
}

/** Writes one deterministic canonical JSON artifact without adding it to the global manifest. */
export async function writeCanonicalJsonArtifact({runDirectory, relativePath, value}) {
  validateRelativePath(relativePath, 'relativePath');
  const bytes = canonicalFileBytes(value);
  await atomicWriteFile(localEvidencePath(runDirectory, relativePath), bytes);
  return fileRecord(relativePath, bytes);
}

export function validateEvidenceManifest(value) {
  assertExactKeys(value, 'manifest', [
    'schema_version', 'evidence_kind', 'run_id', 'input_hash', 'scenario', 'summary', 'chunks'
  ]);
  if (value.schema_version !== EVIDENCE_SCHEMA_VERSION) fail('unsupported_schema', 'unsupported evidence manifest schema');
  if (value.evidence_kind !== 'ntn_constellation_audit') fail('invalid_evidence', 'invalid evidence_kind');
  validateRunId(value.run_id, 'manifest.run_id');
  const inputHash = normalizeSha256(value.input_hash, 'manifest.input_hash');
  const scenario = validateFileRecord(value.scenario, 'manifest.scenario');
  const summary = validateFileRecord(value.summary, 'manifest.summary');
  if (!Array.isArray(value.chunks)) fail('invalid_evidence', 'manifest.chunks must be an array');
  const chunks = value.chunks.map((chunk, index) => validateChunkRecord(chunk, `manifest.chunks[${index}]`));
  const sortedPaths = chunks.map((chunk) => chunk.path);
  if (new Set(sortedPaths).size !== sortedPaths.length || sortedPaths.some((path, index) => index > 0 && path <= sortedPaths[index - 1])) {
    fail('invalid_evidence', 'manifest.chunks must be unique and sorted by path');
  }
  return {...value, input_hash: inputHash, scenario, summary, chunks};
}

function sha256Sums(records) {
  return [...records].sort((left, right) => compareText(left.path, right.path)).map((record) => {
    return `${normalizeSha256(record.sha256).slice('sha256:'.length)}  ${record.path}\n`;
  }).join('');
}

export async function writeEvidenceBundle({
  runDirectory,
  scenarioManifest,
  summary,
  chunks = [],
  deepVerifyChunks = true
}) {
  const scenario = validateScenarioManifest(scenarioManifest);
  const normalizedSummary = validateAuditSummary(summary);
  const inputHash = scenarioInputHash(scenario);
  if (normalizedSummary.run_id !== scenario.run_id || normalizedSummary.input_hash !== inputHash) {
    fail('input_hash_mismatch', 'summary does not belong to the scenario manifest');
  }
  const normalizedChunks = chunks.map((chunk, index) => validateChunkRecord(chunk, `chunks[${index}]`))
    .sort((left, right) => compareText(left.path, right.path));
  if (new Set(normalizedChunks.map((chunk) => chunk.path)).size !== normalizedChunks.length) {
    fail('invalid_evidence', 'chunk paths must be unique');
  }
  for (const chunk of normalizedChunks) {
    if (deepVerifyChunks) await verifyChunk(runDirectory, chunk);
    else await verifyChunkEnvelope(runDirectory, chunk);
  }

  const scenarioBytes = canonicalFileBytes(scenario);
  const summaryBytes = canonicalFileBytes(normalizedSummary);
  await atomicWriteFile(join(resolve(runDirectory), 'scenario.json'), scenarioBytes);
  await atomicWriteFile(join(resolve(runDirectory), 'summary.json'), summaryBytes);
  const scenarioRecord = fileRecord('scenario.json', scenarioBytes);
  const summaryRecord = fileRecord('summary.json', summaryBytes);
  const manifest = validateEvidenceManifest({
    schema_version: EVIDENCE_SCHEMA_VERSION,
    evidence_kind: 'ntn_constellation_audit',
    run_id: scenario.run_id,
    input_hash: inputHash,
    scenario: scenarioRecord,
    summary: summaryRecord,
    chunks: normalizedChunks
  });
  const manifestBytes = canonicalFileBytes(manifest);
  await atomicWriteFile(join(resolve(runDirectory), 'manifest.json'), manifestBytes);
  const sums = sha256Sums([...normalizedChunks, scenarioRecord, summaryRecord, fileRecord('manifest.json', manifestBytes)]);
  await atomicWriteFile(join(resolve(runDirectory), 'sha256sums.txt'), Buffer.from(sums, 'utf8'));
  return {manifest, sha256sums: sums};
}

async function readCanonicalJsonRecord(runDirectory, record, context) {
  const bytes = await verifyFileRecord(runDirectory, record, context);
  const {value, text} = parseJsonBytes(bytes, context);
  if (text !== `${canonicalJson(value)}\n`) fail('invalid_evidence', `${record.path} is not canonical JSON`);
  return value;
}

export async function verifyEvidenceBundle(runDirectory, {
  expectedInputHash = undefined,
  deepVerifyChunks = true,
  verifyChunkFiles = true
} = {}) {
  const manifestBytes = await readBytes(join(resolve(runDirectory), 'manifest.json'), 'evidence manifest');
  const parsedManifest = parseJsonBytes(manifestBytes, 'evidence manifest');
  if (parsedManifest.text !== `${canonicalJson(parsedManifest.value)}\n`) {
    fail('invalid_evidence', 'manifest.json is not canonical JSON');
  }
  const manifest = validateEvidenceManifest(parsedManifest.value);
  if (expectedInputHash !== undefined && manifest.input_hash !== normalizeSha256(expectedInputHash, 'expectedInputHash')) {
    fail('input_hash_mismatch', 'evidence manifest input hash does not match the expected input');
  }
  const scenario = validateScenarioManifest(
    await readCanonicalJsonRecord(runDirectory, manifest.scenario, 'scenario manifest'));
  const summary = validateAuditSummary(await readCanonicalJsonRecord(runDirectory, manifest.summary, 'summary'));
  const computedInputHash = scenarioInputHash(scenario);
  if (manifest.run_id !== scenario.run_id || summary.run_id !== scenario.run_id ||
      manifest.input_hash !== computedInputHash || summary.input_hash !== computedInputHash) {
    fail('input_hash_mismatch', 'evidence files do not describe the same run input');
  }
  if (verifyChunkFiles) {
    for (const chunk of manifest.chunks) {
      if (deepVerifyChunks) await verifyChunk(runDirectory, chunk);
      else await verifyChunkEnvelope(runDirectory, chunk);
    }
  }
  const manifestRecord = fileRecord('manifest.json', manifestBytes);
  const expectedSums = sha256Sums([...manifest.chunks, manifest.scenario, manifest.summary, manifestRecord]);
  const actualSums = await readBytes(join(resolve(runDirectory), 'sha256sums.txt'), 'sha256sums');
  if (actualSums.toString('utf8') !== expectedSums) fail('evidence_hash_mismatch', 'sha256sums.txt does not match evidence');
  return {manifest, scenario, summary, sha256sums: expectedSums};
}

function checkpointPayload({inputHash, sequence, state}) {
  const normalizedHash = normalizeSha256(inputHash, 'checkpoint.input_hash');
  assertInteger(sequence, 'checkpoint.sequence', 0);
  assertPlainObject(state, 'checkpoint.state');
  return {
    schema_version: CHECKPOINT_SCHEMA_VERSION,
    input_hash: normalizedHash,
    sequence,
    state: canonicalClone(state)
  };
}

export async function saveCheckpoint(path, {inputHash, sequence, state}) {
  const payload = checkpointPayload({inputHash, sequence, state});
  const checkpoint = {...payload, checkpoint_hash: canonicalJsonHash(payload)};
  await atomicWriteFile(path, canonicalFileBytes(checkpoint));
  return checkpoint;
}

export async function loadCheckpoint(path, {expectedInputHash} = {}) {
  const bytes = await readBytes(path, 'checkpoint');
  let value;
  try {
    value = parseJsonBytes(bytes, 'checkpoint').value;
  } catch (error) {
    if (error instanceof AuditIoError) fail('checkpoint_corrupt', 'checkpoint is truncated or invalid', error);
    throw error;
  }
  try {
    assertExactKeys(value, 'checkpoint', [
      'schema_version', 'input_hash', 'sequence', 'state', 'checkpoint_hash'
    ]);
    if (value.schema_version !== CHECKPOINT_SCHEMA_VERSION) fail('unsupported_schema', 'unsupported checkpoint schema');
    const payload = checkpointPayload({inputHash: value.input_hash, sequence: value.sequence, state: value.state});
    const checkpointHash = normalizeSha256(value.checkpoint_hash, 'checkpoint.checkpoint_hash');
    if (checkpointHash !== canonicalJsonHash(payload)) fail('checkpoint_corrupt', 'checkpoint content hash mismatch');
    if (expectedInputHash !== undefined && payload.input_hash !== normalizeSha256(expectedInputHash, 'expectedInputHash')) {
      fail('checkpoint_input_mismatch', 'checkpoint belongs to a different scenario input');
    }
    return {...payload, checkpoint_hash: checkpointHash};
  } catch (error) {
    if (error instanceof AuditIoError && ['checkpoint_corrupt', 'checkpoint_input_mismatch'].includes(error.code)) throw error;
    fail('checkpoint_corrupt', 'checkpoint failed validation', error);
  }
}

export async function checkpointFileSize(path) {
  try {
    return (await stat(path)).size;
  } catch (error) {
    fail('read_failed', `cannot stat checkpoint at ${path}`, error);
  }
}
