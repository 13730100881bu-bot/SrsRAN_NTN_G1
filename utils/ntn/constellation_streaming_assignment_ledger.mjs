import {createHash} from 'node:crypto';
import {createReadStream} from 'node:fs';
import {mkdir, readFile, readdir, rename, stat, writeFile} from 'node:fs/promises';
import {dirname, isAbsolute, join, resolve} from 'node:path';
import {createInterface} from 'node:readline';
import {createGunzip} from 'node:zlib';

import {
  canonicalJson,
  canonicalJsonHash,
  loadCheckpoint,
  normalizeSha256,
  saveCheckpoint,
  sha256Bytes,
  writeNdjsonGzipChunk
} from './constellation_audit_io.mjs';
import {
  createStreamingAssignmentSession,
  restoreStreamingAssignmentSession,
} from './constellation_streaming_assignment.mjs';

export const STREAMING_ASSIGNMENT_LEDGER_SCHEMA_VERSION = 1;
export const STREAMING_ASSIGNMENT_LEDGER_PHASE = 'streaming_assignment_ledger_v1';

const OUTPUT_JOURNAL_SCHEMA_VERSION = 1;
const SHA256_PATTERN = /^sha256:[0-9a-f]{64}$/;
const EVENT_ORDER = Object.freeze({
  release_exit: 0,
  entry_exit: 1,
  release_enter: 2,
  entry_enter: 3
});

let temporaryFileSequence = 0;

export class StreamingAssignmentLedgerError extends Error {
  constructor(code, message, options = undefined) {
    super(message, options);
    this.name = 'StreamingAssignmentLedgerError';
    this.code = code;
  }
}

function fail(code, message, cause = undefined) {
  throw new StreamingAssignmentLedgerError(code, message, cause === undefined ? undefined : {cause});
}

function compareText(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function isPlainObject(value) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function assertObject(value, context) {
  if (!isPlainObject(value)) fail('invalid_shape', `${context} must be an object`);
}

function assertExactKeys(value, expected, context) {
  assertObject(value, context);
  const actual = Object.keys(value).sort(compareText);
  const wanted = [...expected].sort(compareText);
  if (canonicalJson(actual) !== canonicalJson(wanted)) {
    fail('invalid_shape', `${context} has missing or unknown fields`);
  }
}

function assertInteger(value, context, minimum = Number.MIN_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < minimum) {
    fail('invalid_value', `${context} must be a safe integer no smaller than ${minimum}`);
  }
}

function normalizeHash(value, context) {
  try {
    return normalizeSha256(value, context);
  } catch (error) {
    fail('invalid_hash', `${context} must be a SHA-256 value`, error);
  }
}

function validateRelativeDirectory(value, context) {
  if (typeof value !== 'string' || value.length === 0 || isAbsolute(value) || value.includes('\\')) {
    fail('unsafe_path', `${context} must be a portable relative directory`);
  }
  if (value.split('/').some((part) => part.length === 0 || part === '.' || part === '..')) {
    fail('unsafe_path', `${context} contains an unsafe path segment`);
  }
  return value;
}

function localPath(runDirectory, relativePath) {
  validateRelativeDirectory(relativePath, 'relative path');
  return join(resolve(runDirectory), ...relativePath.split('/'));
}

function emptyRecordCounts() {
  return {
    total_record_count: 0,
    initialization_record_count: 0,
    epoch_record_count: 0,
    assignment_interval_record_count: 0,
    transition_record_count: 0,
    failure_interval_record_count: 0
  };
}

function zeroFailureCounts() {
  return {
    no_visible_candidate: 0,
    schedule_overflow: 0,
    cell_partition_overflow: 0
  };
}

function zeroFailureDurations() {
  return {
    no_visible_candidate: '0',
    schedule_overflow: '0',
    cell_partition_overflow: '0'
  };
}

function registryIdentities(registry) {
  const entries = Array.isArray(registry) ? registry : registry?.satellites;
  return entries.map((entry) => ({
    satelliteId: entry.satellite_id ?? entry.satelliteId,
    ncis: entry.cells.map(({nci}) => BigInt(nci).toString())
  })).sort((left, right) => compareText(left.satelliteId, right.satelliteId));
}

function emptyAuditMetrics(registry) {
  const identities = registryIdentities(registry);
  return {
    raw_visible_peak_by_satellite: identities.map(({satelliteId}) => ({satellite_id: satelliteId, count: 0})),
    assigned_peak_by_satellite: identities.map(({satelliteId}) => ({satellite_id: satelliteId, count: 0})),
    assigned_peak_by_nci: identities.flatMap(({satelliteId, ncis}) => ncis.map((nci) => ({
      satellite_id: satelliteId,
      nci,
      count: 0
    }))),
    failure_peak_counts: zeroFailureCounts(),
    failure_interval_count_by_reason: zeroFailureCounts(),
    // L1-microseconds can exceed Number.MAX_SAFE_INTEGER over seven days.
    // Decimal strings keep checkpoint and report hashes exact and JSON-safe.
    failure_duration_us_by_reason: zeroFailureDurations(),
    raw_visibility_event_count: 0,
    raw_visibility_pair_mutation_count: 0,
    raw_visibility_peak_observation_count: 0,
    assignment_metric_transition_count: 0,
    assignment_peak_observation_count: 0,
    owner_change_count: 0,
    handover_count: 0,
    release_count: 0,
    acquisition_count: 0
  };
}

function addRecordCounts(counts, records) {
  const next = {...counts};
  next.total_record_count += records.length;
  next.initialization_record_count += records.filter(
    ({record_type: type}) => type === 'assignment_initialization').length;
  next.epoch_record_count += records.filter(({record_type: type}) => type === 'assignment_epoch').length;
  next.assignment_interval_record_count += records.filter(
    ({record_type: type}) => type === 'assignment_interval').length;
  next.transition_record_count += records.filter(
    ({record_type: type}) => type === 'assignment_transition').length;
  next.failure_interval_record_count += records.filter(
    ({record_type: type}) => type === 'unassigned_interval').length;
  if (Object.values(next).some((value) => !Number.isSafeInteger(value) || value < 0)) {
    fail('invalid_value', 'assignment evidence record count exceeds the safe integer range');
  }
  return next;
}

function normalizedRegistryForHash(registry) {
  const entries = Array.isArray(registry) ? registry : registry?.satellites;
  if (!Array.isArray(entries)) fail('invalid_registry', 'registry must contain a satellites array');
  return entries.map((entry) => ({
    satellite_id: entry.satellite_id ?? entry.satelliteId,
    cells: Array.isArray(entry.cells) ? entry.cells.map((cell) => ({
      bank: cell.bank,
      nci: cell.nci,
      pci: cell.pci
    })).sort((left, right) => left.bank - right.bank) : entry.cells
  })).sort((left, right) => compareText(left.satellite_id, right.satellite_id));
}

function assignmentContextHash(options) {
  return canonicalJsonHash({
    position_ids: [...options.positionIds].sort(compareText),
    registry: normalizedRegistryForHash(options.registry),
    satellite_capacity: options.satelliteCapacity,
    cell_capacity: options.cellCapacity,
    simulation_start_time_us: options.simulationStartTimeUs,
    audit_start_time_us: options.auditStartTimeUs,
    audit_end_time_us: options.auditEndTimeUs
  });
}

function validateChunk(chunk, context) {
  assertExactKeys(chunk, ['path', 'sha256', 'size_bytes', 'record_count', 'uncompressed_bytes'], context);
  validateRelativeDirectory(chunk.path, `${context}.path`);
  if (!chunk.path.endsWith('.ndjson.gz')) fail('invalid_journal', `${context}.path must end in .ndjson.gz`);
  const normalized = {...chunk, sha256: normalizeHash(chunk.sha256, `${context}.sha256`)};
  for (const field of ['size_bytes', 'record_count', 'uncompressed_bytes']) {
    assertInteger(normalized[field], `${context}.${field}`, 0);
  }
  return normalized;
}

function normalizeInputJournals({
  journals,
  inputHash,
  sourceHash,
  simulationStartTimeUs,
  auditStartTimeUs,
  auditEndTimeUs
}) {
  if (!Array.isArray(journals) || journals.length === 0 || journals.some((journal) => !Array.isArray(journal))) {
    fail('invalid_journal', 'journals must contain one non-empty journal array per satellite shard');
  }
  const normalizedInputHash = normalizeHash(inputHash, 'inputHash');
  const normalizedSourceHash = normalizeHash(sourceHash, 'sourceHash');
  const normalized = journals.map((journal, journalIndex) => {
    if (journal.length === 0) fail('invalid_journal', `journals[${journalIndex}] must not be empty`);
    return journal.map((entry, entryIndex) => {
      const context = `journals[${journalIndex}][${entryIndex}]`;
      assertExactKeys(entry, [
        'schema_version', 'input_hash', 'part_index', 'shard_index', 'shard_count',
        'slab_start_time_us', 'slab_end_time_us', 'event_chunk', 'interval_chunk'
      ], context);
      if (entry.schema_version !== 2) fail('invalid_journal', `${context}.schema_version is unsupported`);
      for (const field of ['part_index', 'shard_index', 'shard_count']) {
        assertInteger(entry[field], `${context}.${field}`, field === 'shard_count' ? 1 : 0);
      }
      assertInteger(entry.slab_start_time_us, `${context}.slab_start_time_us`);
      assertInteger(entry.slab_end_time_us, `${context}.slab_end_time_us`);
      if (normalizeHash(entry.input_hash, `${context}.input_hash`) !== normalizedInputHash) {
        fail('input_hash_mismatch', `${context} belongs to a different planning input`);
      }
      if (entry.slab_end_time_us <= entry.slab_start_time_us) {
        fail('invalid_journal', `${context} has an empty or reversed slab`);
      }
      return {
        ...entry,
        input_hash: normalizedInputHash,
        event_chunk: validateChunk(entry.event_chunk, `${context}.event_chunk`),
        interval_chunk: validateChunk(entry.interval_chunk, `${context}.interval_chunk`)
      };
    });
  }).sort((left, right) => left[0].shard_index - right[0].shard_index);

  const shardCount = normalized.length;
  const partCount = normalized[0].length;
  for (const [shardIndex, journal] of normalized.entries()) {
    if (journal.length !== partCount) fail('invalid_journal', 'all satellite shard journals must contain the same slabs');
    for (const [partIndex, entry] of journal.entries()) {
      if (entry.shard_count !== shardCount || entry.shard_index !== shardIndex || entry.part_index !== partIndex) {
        fail('invalid_journal', 'journals contain a missing, duplicate or misnumbered shard/slab');
      }
    }
  }
  for (let partIndex = 0; partIndex < partCount; partIndex += 1) {
    const first = normalized[0][partIndex];
    if (normalized.some((journal) => {
      const entry = journal[partIndex];
      return entry.slab_start_time_us !== first.slab_start_time_us ||
        entry.slab_end_time_us !== first.slab_end_time_us;
    })) {
      fail('invalid_journal', `satellite shards disagree on slab ${partIndex} time range`);
    }
    const expectedStart = partIndex === 0
      ? simulationStartTimeUs
      : normalized[0][partIndex - 1].slab_end_time_us;
    if (first.slab_start_time_us !== expectedStart) {
      fail('invalid_journal', `slab ${partIndex} is not contiguous with its predecessor`);
    }
    if (first.slab_start_time_us < auditStartTimeUs && first.slab_end_time_us > auditStartTimeUs) {
      fail('invalid_journal', 'auditStartTimeUs must coincide with a slab boundary');
    }
  }
  if (normalized[0][0].slab_start_time_us !== simulationStartTimeUs ||
      normalized[0][partCount - 1].slab_end_time_us !== auditEndTimeUs) {
    fail('invalid_journal', 'journals do not cover the complete initialization and audit range');
  }
  const ledgerSetHash = canonicalJsonHash({
    schema_version: STREAMING_ASSIGNMENT_LEDGER_SCHEMA_VERSION,
    input_hash: normalizedInputHash,
    source_hash: normalizedSourceHash,
    shards: normalized
  });
  return {
    journals: normalized,
    inputHash: normalizedInputHash,
    sourceHash: normalizedSourceHash,
    ledgerSetHash,
    partCount
  };
}

function validateVisibilityRecord(record, context, partIndex, slabStartTimeUs, slabEndTimeUs,
                                  simulationStartTimeUs, auditStartTimeUs, auditEndTimeUs) {
  assertObject(record, context);
  if (record.record_type === 'visibility_initial_state') {
    assertExactKeys(record, [
      'record_type', 'kind', 'window', 'time_us', 'position_id', 'satellite_id',
      'above_entry', 'above_release'
    ], context);
    if (partIndex !== 0 || record.kind !== 'initial_state' || record.window !== 'initialization' ||
        record.time_us !== simulationStartTimeUs || typeof record.above_entry !== 'boolean' ||
        typeof record.above_release !== 'boolean' || (record.above_entry && !record.above_release)) {
      fail('invalid_evidence', `${context} is not a valid first-slab initial state`);
    }
    return {
      initial: {
        positionId: record.position_id,
        satelliteId: record.satellite_id,
        aboveEntry: record.above_entry,
        aboveRelease: record.above_release
      }
    };
  }
  if (record.record_type === 'visibility_event') {
    assertExactKeys(record, [
      'record_type', 'window', 'time_us', 'kind', 'threshold', 'position_id', 'satellite_id'
    ], context);
    if (!Object.hasOwn(EVENT_ORDER, record.kind) ||
        record.threshold !== (record.kind.startsWith('entry_') ? 'entry' : 'release') ||
        record.time_us < slabStartTimeUs || record.time_us >= slabEndTimeUs ||
        record.window !== (record.time_us < auditStartTimeUs ? 'initialization' : 'audit')) {
      fail('invalid_evidence', `${context} is not a valid threshold crossing`);
    }
    return {event: {
      timeUs: record.time_us,
      kind: record.kind,
      positionId: record.position_id,
      satelliteId: record.satellite_id
    }};
  }
  if (record.record_type === 'visibility_ambiguity') {
    assertExactKeys(record, [
      'record_type', 'threshold', 'start_time_us', 'end_time_us', 'reason',
      'position_id', 'satellite_id', 'window'
    ], context);
    fail('ambiguous_visibility', `${context} prevents exact assignment replay`);
  }
  fail('invalid_evidence', `${context}.record_type is not allowed in an event chunk`);
}

function evidenceRecordRank(record) {
  return record.kind === 'initial_state' ? -1 : EVENT_ORDER[record.kind] ?? 4;
}

function compareEventEnvelope(left, right) {
  const leftTime = left.record.time_us ?? left.record.start_time_us ?? 0;
  const rightTime = right.record.time_us ?? right.record.start_time_us ?? 0;
  return leftTime - rightTime
    || evidenceRecordRank(left.record) - evidenceRecordRank(right.record)
    || compareText(left.record.position_id ?? '', right.record.position_id ?? '')
    || compareText(left.record.satellite_id ?? '', right.record.satellite_id ?? '')
    || compareText(String(left.record.threshold ?? ''), String(right.record.threshold ?? ''))
    || compareText(left.line, right.line);
}

function compareMergeHead(left, right) {
  return compareEventEnvelope(left.value, right.value) || left.shardIndex - right.shardIndex;
}

function pushMergeHead(heap, head) {
  heap.push(head);
  let index = heap.length - 1;
  while (index > 0) {
    const parent = Math.floor((index - 1) / 2);
    if (compareMergeHead(heap[parent], head) <= 0) break;
    heap[index] = heap[parent];
    index = parent;
  }
  heap[index] = head;
}

function popMergeHead(heap) {
  const first = heap[0];
  const last = heap.pop();
  if (heap.length === 0) return first;
  let index = 0;
  while (true) {
    const left = index * 2 + 1;
    if (left >= heap.length) break;
    const right = left + 1;
    const child = right < heap.length && compareMergeHead(heap[right], heap[left]) < 0 ? right : left;
    if (compareMergeHead(last, heap[child]) <= 0) break;
    heap[index] = heap[child];
    index = child;
  }
  heap[index] = last;
  return first;
}

async function* readVerifiedEventChunk(runDirectory, chunk, context) {
  const path = localPath(runDirectory, chunk.path);
  const source = createReadStream(path);
  const compressedHash = createHash('sha256');
  let compressedBytes = 0;
  let uncompressedBytes = 0;
  let recordCount = 0;
  let header = Buffer.alloc(0);
  let lastUncompressedByte;
  source.on('data', (bytes) => {
    compressedHash.update(bytes);
    compressedBytes += bytes.length;
    if (header.length < 10) header = Buffer.concat([header, bytes.subarray(0, 10 - header.length)]);
  });
  const gunzip = createGunzip();
  source.on('error', (error) => gunzip.destroy(error));
  gunzip.on('data', (bytes) => {
    uncompressedBytes += bytes.length;
    if (bytes.length > 0) lastUncompressedByte = bytes[bytes.length - 1];
  });
  source.pipe(gunzip);
  const lines = createInterface({input: gunzip, crlfDelay: Infinity});
  let previousEnvelope;
  try {
    for await (const line of lines) {
      if (line.length === 0) fail('invalid_evidence', `${context} contains an empty NDJSON line`);
      let record;
      try {
        record = JSON.parse(line);
      } catch (error) {
        fail('invalid_evidence', `${context} contains invalid JSON`, error);
      }
      if (canonicalJson(record) !== line) fail('invalid_evidence', `${context} is not canonical NDJSON`);
      const envelope = {record, line};
      if (previousEnvelope !== undefined && compareEventEnvelope(previousEnvelope, envelope) > 0) {
        fail('invalid_evidence', `${context} is not in canonical event order`);
      }
      previousEnvelope = envelope;
      recordCount += 1;
      yield envelope;
    }
  } catch (error) {
    if (error instanceof StreamingAssignmentLedgerError) throw error;
    fail('invalid_evidence', `cannot stream ${context}`, error);
  } finally {
    lines.close();
    source.destroy();
    gunzip.destroy();
  }
  const actualHash = `sha256:${compressedHash.digest('hex')}`;
  if (header.length !== 10 || header[0] !== 0x1f || header[1] !== 0x8b || header[2] !== 8 || header[3] !== 0 ||
      header.readUInt32LE(4) !== 0 || header[8] !== 2 || header[9] !== 255 ||
      compressedBytes !== chunk.size_bytes || uncompressedBytes !== chunk.uncompressed_bytes ||
      recordCount !== chunk.record_count || actualHash !== chunk.sha256 ||
      uncompressedBytes > 0 && lastUncompressedByte !== 0x0a) {
    fail('evidence_hash_mismatch', `${context} bytes or metadata do not match its journal`);
  }
}

async function* mergeEventChunks(runDirectory, entries) {
  const iterators = entries.map((entry, shardIndex) => readVerifiedEventChunk(
    runDirectory,
    entry.event_chunk,
    `slab ${entry.part_index} shard ${shardIndex} event chunk`
  )[Symbol.asyncIterator]());
  try {
    const first = await Promise.all(iterators.map((iterator) => iterator.next()));
    const heap = [];
    for (const [shardIndex, head] of first.entries()) {
      if (!head.done) pushMergeHead(heap, {shardIndex, value: head.value});
    }
    while (heap.length > 0) {
      const head = popMergeHead(heap);
      yield head.value.record;
      const next = await iterators[head.shardIndex].next();
      if (!next.done) pushMergeHead(heap, {shardIndex: head.shardIndex, value: next.value});
    }
  } finally {
    await Promise.allSettled(iterators.map((iterator) => iterator.return?.()));
  }
}

async function prepareSlabEventInput(runDirectory, entries, partIndex, slab, options) {
  const iterator = mergeEventChunks(runDirectory, entries)[Symbol.asyncIterator]();
  const initialVisibility = [];
  let next = await iterator.next();
  let recordIndex = 0;
  while (!next.done && next.value.record_type === 'visibility_initial_state') {
    const parsed = validateVisibilityRecord(
      next.value,
      `slab ${partIndex} record ${recordIndex++}`,
      partIndex,
      slab.slab_start_time_us,
      slab.slab_end_time_us,
      options.simulationStartTimeUs,
      options.auditStartTimeUs,
      options.auditEndTimeUs
    );
    initialVisibility.push(parsed.initial);
    next = await iterator.next();
  }
  const eventGroups = (async function* () {
    let pending = next;
    while (!pending.done) {
      const parsed = validateVisibilityRecord(
        pending.value,
        `slab ${partIndex} record ${recordIndex++}`,
        partIndex,
        slab.slab_start_time_us,
        slab.slab_end_time_us,
        options.simulationStartTimeUs,
        options.auditStartTimeUs,
        options.auditEndTimeUs
      );
      if (!parsed.event) fail('invalid_evidence', 'initial states must precede every crossing event');
      const timeUs = parsed.event.timeUs;
      const group = [parsed.event];
      pending = await iterator.next();
      while (!pending.done && pending.value.time_us === timeUs) {
        const sameTime = validateVisibilityRecord(
          pending.value,
          `slab ${partIndex} record ${recordIndex++}`,
          partIndex,
          slab.slab_start_time_us,
          slab.slab_end_time_us,
          options.simulationStartTimeUs,
          options.auditStartTimeUs,
          options.auditEndTimeUs
        );
        if (!sameTime.event) fail('invalid_evidence', 'initial states must precede every crossing event');
        group.push(sameTime.event);
        pending = await iterator.next();
      }
      yield group;
    }
  })();
  return {initialVisibility, eventGroups};
}

function visibilityFromReplayCheckpoint(checkpoint) {
  const entry = new Map(checkpoint.entryCandidates.map((record) => [
    record.positionId,
    new Set(record.candidates.map(({satelliteId}) => satelliteId))
  ]));
  return checkpoint.completeCandidates.flatMap((record) => record.candidates.map((candidate) => ({
    positionId: record.positionId,
    satelliteId: candidate.satelliteId,
    aboveEntry: entry.get(record.positionId)?.has(candidate.satelliteId) ?? false,
    aboveRelease: true,
    score: candidate.score
  }))).sort((left, right) => compareText(left.positionId, right.positionId)
    || compareText(left.satelliteId, right.satelliteId));
}

function transitionKind(change) {
  if (change.before && change.after) {
    return change.before.satelliteId === change.after.satelliteId ? 'cell_rebalance' : 'handover';
  }
  return change.before ? 'release' : 'acquisition';
}

function assignmentRecord(interval, window = 'audit') {
  return {
    record_type: 'assignment_interval',
    window,
    start_time_us: interval.startTimeUs,
    end_time_us: interval.endTimeUs,
    position_id: interval.positionId,
    satellite_id: interval.satelliteId,
    cell_bank: interval.cellBank,
    nci: interval.nci,
    pci: interval.pci
  };
}

function transitionRecord(epoch, change) {
  return {
    record_type: 'assignment_transition',
    time_us: epoch.timeUs,
    position_id: change.positionId,
    kind: transitionKind(change),
    before: change.before,
    after: change.after
  };
}

function epochRecord(epoch) {
  return {
    record_type: 'assignment_epoch',
    time_us: epoch.timeUs,
    success: epoch.success,
    failure_counts: epoch.failureCounts
  };
}

function normalizeOpenFailures(records, context = 'open_failures') {
  if (!Array.isArray(records)) fail('invalid_checkpoint', `${context} must be an array`);
  const result = new Map();
  for (const [index, record] of records.entries()) {
    assertExactKeys(record, ['position_id', 'reason', 'start_time_us'], `${context}[${index}]`);
    if (typeof record.position_id !== 'string' || record.position_id.length === 0 ||
        typeof record.reason !== 'string' || record.reason.length === 0 ||
        !Number.isSafeInteger(record.start_time_us) || result.has(record.position_id)) {
      fail('invalid_checkpoint', `${context}[${index}] is invalid`);
    }
    result.set(record.position_id, {reason: record.reason, startTimeUs: record.start_time_us});
  }
  return result;
}

function serializeOpenFailures(openFailures) {
  return [...openFailures].map(([positionId, value]) => ({
    position_id: positionId,
    reason: value.reason,
    start_time_us: value.startTimeUs
  })).sort((left, right) => compareText(left.position_id, right.position_id));
}

function applyFailureChanges(openFailures, epoch, completed) {
  for (const change of epoch.failureChanges ?? []) {
    const opened = openFailures.get(change.positionId);
    if (opened && epoch.timeUs > opened.startTimeUs) {
      completed.push({
        record_type: 'unassigned_interval',
        start_time_us: opened.startTimeUs,
        end_time_us: epoch.timeUs,
        position_id: change.positionId,
        reason: opened.reason
      });
    }
    openFailures.delete(change.positionId);
    if (change.after !== null && change.after !== undefined) {
      openFailures.set(change.positionId, {reason: change.after, startTimeUs: epoch.timeUs});
    }
  }
}

function initializeFailureIntervals(openFailures, epoch) {
  for (const failure of epoch.failures ?? []) {
    openFailures.set(failure.positionId, {reason: failure.reason, startTimeUs: epoch.timeUs});
  }
}

function finishFailureIntervals(openFailures, endTimeUs, completed) {
  for (const [positionId, opened] of openFailures) {
    if (endTimeUs > opened.startTimeUs) {
      completed.push({
        record_type: 'unassigned_interval',
        start_time_us: opened.startTimeUs,
        end_time_us: endTimeUs,
        position_id: positionId,
        reason: opened.reason
      });
    }
  }
  openFailures.clear();
}

function initializationProvenance(options, replayCheckpoint, preRollResult) {
  const visibility = visibilityFromReplayCheckpoint(replayCheckpoint);
  const assignments = replayCheckpoint.assignments.map((assignment) => ({...assignment}));
  const stickyStateHash = canonicalJsonHash({assignments, visibility});
  return {
    record_type: 'assignment_initialization',
    kind: 'initial_state',
    initialization_method: 'pre_roll_event_replay',
    time_us: options.auditStartTimeUs,
    simulation_start_time_us: options.simulationStartTimeUs,
    audit_start_time_us: options.auditStartTimeUs,
    pre_roll_duration_us: options.auditStartTimeUs - options.simulationStartTimeUs,
    input_hash: options.inputHash,
    source_hash: options.sourceHash,
    ledger_set_hash: options.ledgerSetHash,
    sticky_state_hash: stickyStateHash,
    sticky_assignment_count: assignments.length,
    sticky_visible_pair_count: visibility.length,
    pre_roll_failure_counts: preRollResult.finalPlan.failureCounts,
    pre_roll_owner_change_count: preRollResult.ownerChangeCount,
    pre_roll_handover_count: preRollResult.handoverCount,
    pre_roll_release_count: preRollResult.releaseCount,
    pre_roll_acquisition_count: preRollResult.acquisitionCount
  };
}

function carryInAssignmentRecords(checkpoint, auditStartTimeUs) {
  if (!Array.isArray(checkpoint?.openIntervals)) {
    fail('internal_mismatch', 'pre-roll checkpoint is missing its open assignment intervals');
  }
  return checkpoint.openIntervals.filter(({startTimeUs}) => startTimeUs < auditStartTimeUs)
    .map(({assignment, startTimeUs}) => assignmentRecord({
      ...assignment,
      startTimeUs,
      endTimeUs: auditStartTimeUs
    }, 'initialization'));
}

function evidenceRecords(result, openFailures, {
  includeInitialization,
  provenance,
  carryInAssignments = [],
  final
}) {
  const records = [...carryInAssignments, ...result.intervals.map((interval) => assignmentRecord(interval))];
  if (includeInitialization) initializeFailureIntervals(openFailures, result.epochs[0]);
  for (const epoch of result.epochs) {
    records.push(epochRecord(epoch));
    records.push(...(epoch.assignmentChanges ?? []).map((change) => transitionRecord(epoch, change)));
    applyFailureChanges(openFailures, epoch, records);
  }
  if (includeInitialization) records.push(provenance);
  if (final) finishFailureIntervals(openFailures, result.checkpoint.timeUs, records);
  return records;
}

function assignmentMap(assignments) {
  return new Map(assignments.map((assignment) => [assignment.positionId, {...assignment}]));
}

function updatePeakRecords(records, counts, keyName) {
  const peakByKey = new Map(records.map((record) => [record[keyName], record.count]));
  for (const [key, count] of counts) peakByKey.set(key, Math.max(peakByKey.get(key) ?? 0, count));
  for (const record of records) record.count = peakByKey.get(record[keyName]) ?? 0;
}

function updateAssignmentMetrics(metrics, beforeCheckpoint, result) {
  const current = assignmentMap(beforeCheckpoint.assignments);
  const satelliteCounts = new Map();
  const nciCounts = new Map();
  for (const assignment of current.values()) {
    satelliteCounts.set(assignment.satelliteId, (satelliteCounts.get(assignment.satelliteId) ?? 0) + 1);
    const nci = BigInt(assignment.nci).toString();
    nciCounts.set(nci, (nciCounts.get(nci) ?? 0) + 1);
  }
  const satellitePeaks = new Map(
    metrics.assigned_peak_by_satellite.map((record) => [record.satellite_id, record]));
  const nciPeaks = new Map(metrics.assigned_peak_by_nci.map((record) => [record.nci, record]));
  const observe = (changedSatellites, changedNcis) => {
    for (const satelliteId of changedSatellites) {
      const peak = satellitePeaks.get(satelliteId);
      if (peak === undefined) fail('internal_mismatch', `assignment references unknown satellite '${satelliteId}'`);
      peak.count = Math.max(peak.count, satelliteCounts.get(satelliteId) ?? 0);
      metrics.assignment_peak_observation_count += 1;
    }
    for (const nci of changedNcis) {
      const peak = nciPeaks.get(nci);
      if (peak === undefined) fail('internal_mismatch', `assignment references unknown NCI '${nci}'`);
      peak.count = Math.max(peak.count, nciCounts.get(nci) ?? 0);
      metrics.assignment_peak_observation_count += 1;
    }
  };
  for (const epoch of result.epochs) {
    const changedSatellites = new Set();
    const changedNcis = new Set();
    for (const change of epoch.assignmentChanges ?? []) {
      const before = current.get(change.positionId);
      if (before !== undefined) {
        const nci = BigInt(before.nci).toString();
        satelliteCounts.set(before.satelliteId, satelliteCounts.get(before.satelliteId) - 1);
        nciCounts.set(nci, nciCounts.get(nci) - 1);
        changedSatellites.add(before.satelliteId);
        changedNcis.add(nci);
      }
      if (change.after === null) {
        current.delete(change.positionId);
      } else {
        const after = {...change.after};
        const nci = BigInt(after.nci).toString();
        current.set(change.positionId, after);
        satelliteCounts.set(after.satelliteId, (satelliteCounts.get(after.satelliteId) ?? 0) + 1);
        nciCounts.set(nci, (nciCounts.get(nci) ?? 0) + 1);
        changedSatellites.add(after.satelliteId);
        changedNcis.add(nci);
      }
      metrics.assignment_metric_transition_count += 1;
    }
    if (metrics.assignment_peak_observation_count === 0) {
      observe(satellitePeaks.keys(), nciPeaks.keys());
    } else {
      observe(changedSatellites, changedNcis);
    }
    for (const [reason, count] of Object.entries(epoch.failureCounts)) {
      metrics.failure_peak_counts[reason] = Math.max(metrics.failure_peak_counts[reason], count);
    }
  }
  const expected = assignmentMap(result.finalAssignments);
  if (canonicalJson([...current].sort(([left], [right]) => compareText(left, right))) !==
      canonicalJson([...expected].sort(([left], [right]) => compareText(left, right)))) {
    fail('internal_mismatch', 'assignment metric replay differs from the streaming assignment result');
  }
  metrics.owner_change_count = result.ownerChangeCount;
  metrics.handover_count = result.handoverCount;
  metrics.release_count = result.releaseCount;
  metrics.acquisition_count = result.acquisitionCount;
}

function releasePairs(checkpoint) {
  const pairs = new Set();
  for (const record of checkpoint.completeCandidates) {
    for (const {satelliteId} of record.candidates) pairs.add(`${record.positionId}\0${satelliteId}`);
  }
  return pairs;
}

function rawCountsBySatellite(pairs) {
  const counts = new Map();
  for (const pair of pairs) {
    const satelliteId = pair.slice(pair.indexOf('\0') + 1);
    counts.set(satelliteId, (counts.get(satelliteId) ?? 0) + 1);
  }
  return counts;
}

function trackRawVisibilityMetrics(metrics, beforeCheckpoint, eventGroups, segmentStartTimeUs) {
  const pairs = releasePairs(beforeCheckpoint);
  const counts = rawCountsBySatellite(pairs);
  const peaks = new Map(metrics.raw_visible_peak_by_satellite.map((record) => [record.satellite_id, record]));
  const observe = (satelliteIds) => {
    for (const satelliteId of satelliteIds) {
      const peak = peaks.get(satelliteId);
      if (peak === undefined) fail('internal_mismatch', `raw visibility references unknown satellite '${satelliteId}'`);
      peak.count = Math.max(peak.count, counts.get(satelliteId) ?? 0);
      metrics.raw_visibility_peak_observation_count += 1;
    }
  };
  const observeAll = () => observe(peaks.keys());
  return (async function* () {
    let sawGroup = false;
    for await (const group of eventGroups) {
      if (!sawGroup && group[0].timeUs > segmentStartTimeUs) {
        observeAll();
      }
      sawGroup = true;
      const changedSatellites = new Set();
      for (const event of group) {
        const key = `${event.positionId}\0${event.satelliteId}`;
        metrics.raw_visibility_event_count += 1;
        if (event.kind === 'release_exit' && pairs.delete(key)) {
          counts.set(event.satelliteId, (counts.get(event.satelliteId) ?? 0) - 1);
          changedSatellites.add(event.satelliteId);
          metrics.raw_visibility_pair_mutation_count += 1;
        } else if ((event.kind === 'release_enter' || event.kind === 'entry_enter') && !pairs.has(key)) {
          pairs.add(key);
          counts.set(event.satelliteId, (counts.get(event.satelliteId) ?? 0) + 1);
          changedSatellites.add(event.satelliteId);
          metrics.raw_visibility_pair_mutation_count += 1;
        }
      }
      if (metrics.raw_visibility_peak_observation_count === 0) observeAll();
      else observe(changedSatellites);
      yield group;
    }
    if (!sawGroup) observeAll();
  })();
}

function updateFailureIntervalMetrics(metrics, records) {
  for (const record of records) {
    if (record.record_type !== 'unassigned_interval') continue;
    metrics.failure_interval_count_by_reason[record.reason] += 1;
    metrics.failure_duration_us_by_reason[record.reason] = (
      BigInt(metrics.failure_duration_us_by_reason[record.reason]) +
      BigInt(record.end_time_us - record.start_time_us)
    ).toString();
  }
}

function planSummary(result) {
  return {
    success: result.finalPlan.success,
    failure_reason: result.finalPlan.failureReason,
    failure_counts: {...result.finalPlan.failureCounts},
    failures: result.finalPlan.failures.map((failure) => ({
      position_id: failure.positionId,
      reason: failure.reason
    }))
  };
}

function initialState(options) {
  return {
    phase: 'initialization',
    source_hash: options.sourceHash,
    ledger_set_hash: options.ledgerSetHash,
    assignment_context_hash: options.assignmentContextHash,
    simulation_start_time_us: options.simulationStartTimeUs,
    audit_start_time_us: options.auditStartTimeUs,
    audit_end_time_us: options.auditEndTimeUs,
    next_input_part_index: 0,
    output_sequence: 0,
    output_chain_hash: canonicalJsonHash([]),
    replay_checkpoint: null,
    open_failures: [],
    initialization_provenance: null,
    record_counts: emptyRecordCounts(),
    audit_metrics: emptyAuditMetrics(options.registry),
    current_plan_summary: null
  };
}

function validateCheckpointState(state, options) {
  assertExactKeys(state, [
    'phase', 'source_hash', 'ledger_set_hash', 'assignment_context_hash',
    'simulation_start_time_us', 'audit_start_time_us', 'audit_end_time_us',
    'next_input_part_index', 'output_sequence', 'output_chain_hash', 'replay_checkpoint',
    'open_failures', 'initialization_provenance', 'record_counts', 'audit_metrics',
    'current_plan_summary'
  ], 'assignment checkpoint state');
  if (!['initialization', 'audit_pending', 'audit', 'complete'].includes(state.phase) ||
      state.source_hash !== options.sourceHash || state.ledger_set_hash !== options.ledgerSetHash ||
      state.assignment_context_hash !== options.assignmentContextHash ||
      state.simulation_start_time_us !== options.simulationStartTimeUs ||
      state.audit_start_time_us !== options.auditStartTimeUs ||
      state.audit_end_time_us !== options.auditEndTimeUs) {
    fail('checkpoint_mismatch', 'assignment checkpoint belongs to a different input, source or replay context');
  }
  assertInteger(state.next_input_part_index, 'assignment checkpoint next_input_part_index', 0);
  assertInteger(state.output_sequence, 'assignment checkpoint output_sequence', 0);
  if (!SHA256_PATTERN.test(state.output_chain_hash)) {
    fail('invalid_checkpoint', 'assignment checkpoint output_chain_hash is invalid');
  }
  normalizeOpenFailures(state.open_failures);
  assertExactKeys(state.record_counts, Object.keys(emptyRecordCounts()), 'assignment checkpoint record_counts');
  for (const [name, value] of Object.entries(state.record_counts)) {
    assertInteger(value, `assignment checkpoint record_counts.${name}`, 0);
  }
  if (canonicalJson(Object.keys(state.audit_metrics).sort(compareText)) !==
      canonicalJson(Object.keys(emptyAuditMetrics(options.registry)).sort(compareText))) {
    fail('invalid_checkpoint', 'assignment checkpoint audit_metrics has missing or unknown fields');
  }
  if (state.next_input_part_index > options.partCount || state.output_sequence > state.next_input_part_index ||
      (state.phase === 'complete') !== (state.next_input_part_index === options.partCount)) {
    fail('invalid_checkpoint', 'assignment checkpoint progress is inconsistent');
  }
  if (state.next_input_part_index > 0 && !isPlainObject(state.replay_checkpoint)) {
    fail('invalid_checkpoint', 'assignment checkpoint is missing live replay state');
  }
  return state;
}

function outputJournalPath(runDirectory, journalDirectory, partIndex) {
  return localPath(runDirectory, `${journalDirectory}/part-${String(partIndex).padStart(6, '0')}.json`);
}

async function atomicWriteCanonical(path, value) {
  await mkdir(dirname(path), {recursive: true});
  const temporary = `${path}.tmp-${process.pid}-${temporaryFileSequence++}`;
  try {
    await writeFile(temporary, `${canonicalJson(value)}\n`, {encoding: 'utf8', flag: 'wx'});
    await rename(temporary, path);
  } catch (error) {
    fail('write_failed', `cannot atomically write ${path}`, error);
  }
}

function validateOutputJournalEntry(entry, context, options) {
  assertExactKeys(entry, [
    'schema_version', 'input_hash', 'source_hash', 'ledger_set_hash', 'part_index',
    'source_part_index', 'slab_start_time_us', 'slab_end_time_us', 'chunk'
  ], context);
  if (entry.schema_version !== OUTPUT_JOURNAL_SCHEMA_VERSION || entry.input_hash !== options.inputHash ||
      entry.source_hash !== options.sourceHash || entry.ledger_set_hash !== options.ledgerSetHash) {
    fail('output_mismatch', `${context} belongs to different assignment evidence`);
  }
  for (const field of ['part_index', 'source_part_index']) assertInteger(entry[field], `${context}.${field}`, 0);
  assertInteger(entry.slab_start_time_us, `${context}.slab_start_time_us`);
  assertInteger(entry.slab_end_time_us, `${context}.slab_end_time_us`);
  if (entry.slab_end_time_us <= entry.slab_start_time_us) {
    fail('invalid_journal', `${context} has an invalid audit slab range`);
  }
  return {...entry, chunk: validateChunk(entry.chunk, `${context}.chunk`)};
}

async function readOutputJournal(runDirectory, journalDirectory, options) {
  const directory = localPath(runDirectory, journalDirectory);
  let names;
  try {
    names = await readdir(directory);
  } catch (error) {
    if (error?.code === 'ENOENT') return [];
    fail('read_failed', `cannot read assignment output journal ${directory}`, error);
  }
  const entries = [];
  for (const name of names.sort(compareText)) {
    if (!/^part-\d{6}\.json$/.test(name)) fail('invalid_journal', `unexpected assignment journal file '${name}'`);
    let text;
    try {
      text = await readFile(join(directory, name), 'utf8');
    } catch (error) {
      fail('read_failed', `cannot read assignment journal file '${name}'`, error);
    }
    let parsed;
    try {
      parsed = JSON.parse(text);
    } catch (error) {
      fail('invalid_journal', `assignment journal file '${name}' is invalid JSON`, error);
    }
    if (text !== `${canonicalJson(parsed)}\n`) fail('invalid_journal', `assignment journal file '${name}' is not canonical`);
    const entry = validateOutputJournalEntry(parsed, `assignment journal ${name}`, options);
    if (entry.part_index !== entries.length || name !== `part-${String(entry.part_index).padStart(6, '0')}.json`) {
      fail('invalid_journal', 'assignment output journal contains a missing or duplicate part');
    }
    entries.push(entry);
  }
  return entries;
}

function extendOutputChain(previousHash, entry) {
  return canonicalJsonHash({previous_hash: previousHash, entry});
}

function outputChain(entries) {
  return entries.reduce((hash, entry) => extendOutputChain(hash, entry), canonicalJsonHash([]));
}

async function verifyOutputChunks(runDirectory, entries) {
  for (const entry of entries) {
    let bytes;
    try {
      bytes = await readFile(localPath(runDirectory, entry.chunk.path));
    } catch (error) {
      fail('read_failed', `cannot read assignment evidence '${entry.chunk.path}'`, error);
    }
    if (bytes.length !== entry.chunk.size_bytes || sha256Bytes(bytes) !== entry.chunk.sha256) {
      fail('output_hash_mismatch', `assignment evidence '${entry.chunk.path}' differs from its journal`);
    }
  }
}

async function checkpointExists(path) {
  try {
    await stat(path);
    return true;
  } catch (error) {
    if (error?.code === 'ENOENT') return false;
    fail('read_failed', `cannot inspect assignment checkpoint ${path}`, error);
  }
}

function replayCommon(options) {
  return {
    positionIds: options.positionIds,
    registry: options.registry,
    satelliteCapacity: options.satelliteCapacity,
    cellCapacity: options.cellCapacity
  };
}

async function splitStartEventGroup(eventGroups, startTimeUs) {
  const iterator = eventGroups[Symbol.asyncIterator]();
  const first = await iterator.next();
  const startEvents = !first.done && first.value[0].timeUs === startTimeUs ? first.value : [];
  const remaining = (async function* () {
    if (!first.done && first.value[0].timeUs !== startTimeUs) yield first.value;
    while (true) {
      const next = await iterator.next();
      if (next.done) break;
      yield next.value;
    }
  })();
  return {startEvents, remaining};
}

async function replayInitializationSegment(session, partInput, startTimeUs, endTimeUs, options) {
  if (session === null) {
    const split = await splitStartEventGroup(partInput.eventGroups, startTimeUs);
    session = createStreamingAssignmentSession({
      startTimeUs,
      initialVisibility: partInput.initialVisibility,
      startEvents: split.startEvents,
      ...replayCommon(options)
    });
    return {
      session,
      result: await session.advanceEventGroups({endTimeUs, eventGroups: split.remaining, finalize: false})
    };
  }
  return {
    session,
    result: await session.advanceEventGroups({
      endTimeUs,
      eventGroups: partInput.eventGroups,
      finalize: false
    })
  };
}

async function startAuditSegment(state, partInput, endTimeUs, options) {
  const preRollCheckpoint = state.replay_checkpoint;
  const initialVisibility = visibilityFromReplayCheckpoint(preRollCheckpoint);
  const initialAssignments = preRollCheckpoint.assignments.map((assignment) => ({...assignment}));
  const split = await splitStartEventGroup(partInput.eventGroups, options.auditStartTimeUs);
  const session = createStreamingAssignmentSession({
    startTimeUs: options.auditStartTimeUs,
    initialVisibility,
    initialAssignments,
    startEvents: split.startEvents,
    ...replayCommon(options)
  });
  return {
    session,
    result: await session.advanceEventGroups({
      endTimeUs,
      eventGroups: split.remaining,
      finalize: endTimeUs === options.auditEndTimeUs
    })
  };
}

async function resumeAuditSegment(session, state, partInput, endTimeUs, options) {
  return {
    session,
    result: await session.advanceEventGroups({
      endTimeUs,
      eventGroups: partInput.eventGroups,
      finalize: endTimeUs === options.auditEndTimeUs
    })
  };
}

function finalOutputHash(options, journal) {
  return canonicalJsonHash({
    schema_version: STREAMING_ASSIGNMENT_LEDGER_SCHEMA_VERSION,
    input_hash: options.inputHash,
    source_hash: options.sourceHash,
    ledger_set_hash: options.ledgerSetHash,
    chunks: journal.map(({chunk}) => chunk)
  });
}

function normalizeLedgerOptions(options) {
  assertObject(options, 'options');
  const runDirectory = resolve(options.runDirectory);
  if (!Array.isArray(options.positionIds)) fail('invalid_value', 'positionIds must be an array');
  for (const field of ['simulationStartTimeUs', 'auditStartTimeUs', 'auditEndTimeUs']) {
    assertInteger(options[field], field);
  }
  if (!(options.simulationStartTimeUs < options.auditStartTimeUs &&
        options.auditStartTimeUs < options.auditEndTimeUs)) {
    fail('invalid_value', 'assignment replay requires a non-empty pre-roll followed by a non-empty audit range');
  }
  const satelliteCapacity = options.satelliteCapacity ?? 256;
  const cellCapacity = options.cellCapacity ?? 128;
  assertInteger(satelliteCapacity, 'satelliteCapacity', 1);
  assertInteger(cellCapacity, 'cellCapacity', 1);
  const normalizedInput = normalizeInputJournals({
    journals: options.journals,
    inputHash: options.inputHash,
    sourceHash: options.sourceHash,
    simulationStartTimeUs: options.simulationStartTimeUs,
    auditStartTimeUs: options.auditStartTimeUs,
    auditEndTimeUs: options.auditEndTimeUs
  });
  const evidenceDirectory = validateRelativeDirectory(
    options.evidenceDirectory ?? 'streaming-assignments',
    'evidenceDirectory'
  );
  const journalDirectory = validateRelativeDirectory(
    options.journalDirectory ?? 'streaming-assignment-journal',
    'journalDirectory'
  );
  const checkpointPath = resolve(
    options.checkpointPath ?? join(runDirectory, 'checkpoints', 'streaming-assignment.json')
  );
  return {
    ...options,
    runDirectory,
    satelliteCapacity,
    cellCapacity,
    ...normalizedInput,
    assignmentContextHash: assignmentContextHash({
      ...options,
      satelliteCapacity,
      cellCapacity
    }),
    evidenceDirectory,
    journalDirectory,
    checkpointPath
  };
}

async function loadValidatedAssignmentState(options, {validateReplay = false} = {}) {
  let checkpoint;
  try {
    checkpoint = await loadCheckpoint(options.checkpointPath, {expectedInputHash: options.inputHash});
  } catch (error) {
    fail(error?.code ?? 'checkpoint_read_failed', `cannot read assignment checkpoint: ${error.message}`, error);
  }
  if (checkpoint.sequence !== checkpoint.state?.next_input_part_index) {
    fail('invalid_checkpoint', 'assignment checkpoint sequence is inconsistent');
  }
  const state = validateCheckpointState(checkpoint.state, options);
  if (validateReplay && state.replay_checkpoint !== null) {
    restoreStreamingAssignmentSession(state.replay_checkpoint);
  }
  return {checkpoint, state};
}

/**
 * Reads one fully bound live assignment snapshot without replaying historical
 * visibility intervals or advancing the checkpoint.
 */
export async function readStreamingAssignmentSnapshot(options) {
  const normalizedOptions = normalizeLedgerOptions(options);
  const {checkpoint, state} = await loadValidatedAssignmentState(
    normalizedOptions,
    {validateReplay: true}
  );
  const outputJournal = await readOutputJournal(
    normalizedOptions.runDirectory,
    normalizedOptions.journalDirectory,
    normalizedOptions
  );
  if (outputJournal.length !== state.output_sequence ||
      outputChain(outputJournal) !== state.output_chain_hash) {
    fail('output_mismatch', 'assignment output journal does not exactly match the checkpoint');
  }
  await verifyOutputChunks(normalizedOptions.runDirectory, outputJournal);
  if (state.replay_checkpoint === null) {
    fail('snapshot_unavailable', 'assignment checkpoint does not yet contain replay state');
  }
  const replay = state.replay_checkpoint;
  const processedBoundary = normalizedOptions.journals[0][state.next_input_part_index - 1]?.slab_end_time_us;
  if (replay.timeUs !== processedBoundary) {
    fail('checkpoint_mismatch', 'assignment checkpoint time does not match its processed slab boundary');
  }
  return {
    phase: state.phase,
    processedPartCount: state.next_input_part_index,
    checkpointTimeUs: replay.timeUs,
    simulationStartTimeUs: normalizedOptions.simulationStartTimeUs,
    auditStartTimeUs: normalizedOptions.auditStartTimeUs,
    auditEndTimeUs: normalizedOptions.auditEndTimeUs,
    inputHash: normalizedOptions.inputHash,
    sourceHash: normalizedOptions.sourceHash,
    ledgerSetHash: normalizedOptions.ledgerSetHash,
    assignmentContextHash: normalizedOptions.assignmentContextHash,
    checkpointHash: checkpoint.checkpoint_hash,
    outputPrefixHash: state.output_chain_hash,
    completeCandidates: replay.completeCandidates.map((record) => ({
      positionId: record.positionId,
      candidates: record.candidates.map((candidate) => ({...candidate}))
    })),
    entryCandidates: replay.entryCandidates.map((record) => ({
      positionId: record.positionId,
      candidates: record.candidates.map((candidate) => ({...candidate}))
    })),
    assignments: replay.assignments.map((assignment) => ({...assignment}))
  };
}

/**
 * Consumes one completed, equal-time slab journal from every satellite shard.
 * Only one merged slab and the live matching are retained in memory. Every
 * audit slab produces one deterministic gzip member plus a constant-size
 * sidecar; the checkpoint contains live state, never historical events.
 */
export async function runStreamingAssignmentLedger(options) {
  const normalizedOptions = normalizeLedgerOptions(options);
  const {runDirectory, checkpointPath, journalDirectory, evidenceDirectory} = normalizedOptions;
  const normalizedInput = normalizedOptions;
  const maxParts = options.maxParts ?? Number.MAX_SAFE_INTEGER;
  assertInteger(maxParts, 'maxParts', 1);
  let outputJournal = await readOutputJournal(runDirectory, journalDirectory, normalizedOptions);
  let state;
  if (options.resume === true) {
    ({state} = await loadValidatedAssignmentState(normalizedOptions));
    if (outputJournal.length < state.output_sequence || outputJournal.length > state.output_sequence + 1 ||
        outputChain(outputJournal.slice(0, state.output_sequence)) !== state.output_chain_hash) {
      fail('output_mismatch', 'assignment output journal does not match the resumable checkpoint');
    }
    await verifyOutputChunks(runDirectory, outputJournal);
  } else {
    if (outputJournal.length !== 0 || await checkpointExists(checkpointPath)) {
      fail('output_exists', 'assignment output already exists; use resume to continue it');
    }
    state = initialState(normalizedOptions);
  }

  let processed = 0;
  let stickyStart = state.phase === 'audit_pending' ? {
    timeUs: normalizedOptions.auditStartTimeUs,
    assignments: state.replay_checkpoint.assignments.map((assignment) => ({...assignment})),
    visibility: visibilityFromReplayCheckpoint(state.replay_checkpoint),
    provenance: {...state.initialization_provenance}
  } : undefined;
  let replaySession = state.replay_checkpoint !== null &&
    state.phase !== 'audit_pending' && state.phase !== 'complete'
    ? restoreStreamingAssignmentSession(state.replay_checkpoint)
    : null;
  while (state.next_input_part_index < normalizedInput.partCount && processed < maxParts) {
    const partIndex = state.next_input_part_index;
    const entries = normalizedInput.journals.map((journal) => journal[partIndex]);
    const slab = entries[0];
    const partInput = await prepareSlabEventInput(runDirectory, entries, partIndex, slab, normalizedOptions);
    let auditResult;
    let includeInitialization = false;

    if (state.phase === 'initialization') {
      const initializationEnd = Math.min(slab.slab_end_time_us, normalizedOptions.auditStartTimeUs);
      if (initializationEnd > slab.slab_start_time_us) {
        const started = await replayInitializationSegment(
          replaySession,
          partInput,
          slab.slab_start_time_us,
          initializationEnd,
          normalizedOptions
        );
        replaySession = started.session;
        const preRoll = started.result;
        state.replay_checkpoint = preRoll.checkpoint;
        if (initializationEnd === normalizedOptions.auditStartTimeUs) {
          state.phase = 'audit_pending';
          state.initialization_provenance = initializationProvenance(
            normalizedOptions,
            preRoll.checkpoint,
            preRoll
          );
          stickyStart = {
            timeUs: normalizedOptions.auditStartTimeUs,
            assignments: preRoll.finalAssignments.map((assignment) => ({...assignment})),
            visibility: preRoll.finalVisibility.map((item) => ({...item})),
            provenance: {...state.initialization_provenance}
          };
        }
      }
    }

    if (state.phase === 'audit_pending' && slab.slab_end_time_us > normalizedOptions.auditStartTimeUs) {
      partInput.eventGroups = trackRawVisibilityMetrics(
        state.audit_metrics,
        state.replay_checkpoint,
        partInput.eventGroups,
        normalizedOptions.auditStartTimeUs
      );
      const started = await startAuditSegment(state, partInput, slab.slab_end_time_us, normalizedOptions);
      replaySession = started.session;
      auditResult = started.result;
      includeInitialization = true;
      state.phase = slab.slab_end_time_us === normalizedOptions.auditEndTimeUs ? 'complete' : 'audit';
    } else if (state.phase === 'audit') {
      partInput.eventGroups = trackRawVisibilityMetrics(
        state.audit_metrics,
        state.replay_checkpoint,
        partInput.eventGroups,
        state.replay_checkpoint.timeUs
      );
      const resumed = await resumeAuditSegment(
        replaySession,
        state,
        partInput,
        slab.slab_end_time_us,
        normalizedOptions
      );
      replaySession = resumed.session;
      auditResult = resumed.result;
      state.phase = slab.slab_end_time_us === normalizedOptions.auditEndTimeUs ? 'complete' : 'audit';
    }

    if (auditResult !== undefined) {
      const replayBeforePart = state.replay_checkpoint;
      updateAssignmentMetrics(state.audit_metrics, replayBeforePart, auditResult);
      const openFailures = normalizeOpenFailures(state.open_failures);
      const evidenceRecordsForPart = evidenceRecords(auditResult, openFailures, {
        includeInitialization,
        provenance: state.initialization_provenance,
        carryInAssignments: includeInitialization
          ? carryInAssignmentRecords(replayBeforePart, normalizedOptions.auditStartTimeUs)
          : [],
        final: state.phase === 'complete'
      });
      updateFailureIntervalMetrics(state.audit_metrics, evidenceRecordsForPart);
      const chunk = await writeNdjsonGzipChunk({
        runDirectory,
        chunkIndex: state.output_sequence,
        relativeDirectory: evidenceDirectory,
        records: evidenceRecordsForPart
      });
      const entry = {
        schema_version: OUTPUT_JOURNAL_SCHEMA_VERSION,
        input_hash: normalizedOptions.inputHash,
        source_hash: normalizedOptions.sourceHash,
        ledger_set_hash: normalizedOptions.ledgerSetHash,
        part_index: state.output_sequence,
        source_part_index: partIndex,
        // This is the audit slab owning the chunk, not a containment promise:
        // carry-in proof and long intervals may start before this boundary.
        slab_start_time_us: Math.max(slab.slab_start_time_us, normalizedOptions.auditStartTimeUs),
        slab_end_time_us: slab.slab_end_time_us,
        chunk
      };
      const existing = outputJournal[state.output_sequence];
      if (existing !== undefined) {
        if (canonicalJson(existing) !== canonicalJson(entry)) {
          fail('output_mismatch', `orphan assignment output part ${state.output_sequence} is not reproducible`);
        }
      } else {
        await atomicWriteCanonical(
          outputJournalPath(runDirectory, journalDirectory, state.output_sequence),
          entry
        );
        outputJournal = [...outputJournal, entry];
      }
      state.output_chain_hash = extendOutputChain(state.output_chain_hash, entry);
      state.output_sequence += 1;
      state.replay_checkpoint = auditResult.checkpoint;
      state.open_failures = serializeOpenFailures(openFailures);
      state.record_counts = addRecordCounts(state.record_counts, evidenceRecordsForPart);
      state.current_plan_summary = planSummary(auditResult);
    }

    state.next_input_part_index = partIndex + 1;
    if (state.next_input_part_index === normalizedInput.partCount) state.phase = 'complete';
    await saveCheckpoint(checkpointPath, {
      inputHash: normalizedOptions.inputHash,
      sequence: state.next_input_part_index,
      state
    });
    processed += 1;
  }
  if (outputJournal.length !== state.output_sequence) {
    fail('output_mismatch', 'assignment output journal contains an uncommitted trailing part');
  }
  const complete = state.phase === 'complete';
  return {
    complete,
    partsProcessed: processed,
    nextInputPartIndex: state.next_input_part_index,
    checkpointPath,
    checkpoint: JSON.parse(canonicalJson(state)),
    chunks: outputJournal.map(({chunk}) => ({...chunk})),
    outputPrefixHash: state.output_chain_hash,
    outputHash: complete ? finalOutputHash(normalizedOptions, outputJournal) : null,
    ledgerSetHash: normalizedOptions.ledgerSetHash,
    recordCounts: {...state.record_counts},
    metrics: JSON.parse(canonicalJson(state.audit_metrics)),
    stickyStart,
    finalPlan: complete ? JSON.parse(canonicalJson(state.current_plan_summary)) : undefined,
    finalAssignments: complete
      ? state.replay_checkpoint.assignments.map((assignment) => ({...assignment}))
      : undefined,
    finalVisibility: complete ? visibilityFromReplayCheckpoint(state.replay_checkpoint) : undefined,
    finalCandidateInventory: complete
      ? state.replay_checkpoint.completeCandidates.map((record) => ({
        positionId: record.positionId,
        candidates: record.candidates.map((candidate) => ({...candidate}))
      }))
      : undefined
  };
}
