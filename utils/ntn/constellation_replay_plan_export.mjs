import {exportDryRunSatellitePlan} from './constellation_plan_export.mjs';

export class ConstellationReplayPlanExportError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'ConstellationReplayPlanExportError';
    this.code = code;
  }
}

function fail(code, message) {
  throw new ConstellationReplayPlanExportError(code, message);
}

function isPlainObject(value) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function compareText(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function compareInteger(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function assertSafeInteger(value, context, minimum = Number.MIN_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < minimum) {
    fail('invalid_value', `${context} must be a safe integer no smaller than ${minimum}`);
  }
  return value;
}

function normalizeNci(value, context) {
  let result;
  try {
    if (typeof value === 'number' && (!Number.isSafeInteger(value) || value < 0)) throw new TypeError();
    if (typeof value !== 'number' && (typeof value !== 'string' || value.length === 0)) throw new TypeError();
    result = BigInt(value);
  } catch {
    fail('identity_mismatch', `${context} must be a numeric 36-bit NCI`);
  }
  if (result < 0n || result >= (1n << 36n)) {
    fail('identity_mismatch', `${context} must fit in 36 bits`);
  }
  return Number(result);
}

function catalogPositions(catalogGeometry) {
  if (!isPlainObject(catalogGeometry) || !Array.isArray(catalogGeometry.positions)) {
    fail('invalid_catalog', 'catalogGeometry must contain a positions array');
  }
  const byId = new Map();
  for (const [index, position] of catalogGeometry.positions.entries()) {
    if (!isPlainObject(position)) fail('invalid_catalog', `catalogGeometry.positions[${index}] must be an object`);
    const {positionId, latitudeDeg, longitudeDeg, childMask} = position;
    if (typeof positionId !== 'string' || !/^G\d{6}$/.test(positionId)) {
      fail('invalid_catalog', `catalogGeometry.positions[${index}].positionId must use G###### form`);
    }
    if (byId.has(positionId)) fail('invalid_catalog', `duplicate catalog position '${positionId}'`);
    if (!Number.isFinite(latitudeDeg) || latitudeDeg < -90 || latitudeDeg > 90 ||
        !Number.isFinite(longitudeDeg) || longitudeDeg < -180 || longitudeDeg > 180) {
      fail('invalid_catalog', `catalog position '${positionId}' has invalid coordinates`);
    }
    if (!Number.isSafeInteger(childMask) || childMask < 1 || childMask > 127) {
      fail('invalid_catalog', `catalog position '${positionId}' childMask must be in 1..127`);
    }
    byId.set(positionId, {
      id: positionId,
      lat: Object.is(latitudeDeg, -0) ? 0 : latitudeDeg,
      lon: Object.is(longitudeDeg, -0) ? 0 : longitudeDeg,
      childMask
    });
  }
  return byId;
}

function registryBySatellite(identityRegistry) {
  const entries = Array.isArray(identityRegistry) ? identityRegistry : identityRegistry?.satellites;
  if (!Array.isArray(entries)) fail('invalid_identity_registry', 'identityRegistry must contain a satellites array');
  const result = new Map();
  for (const [index, entry] of entries.entries()) {
    if (!isPlainObject(entry)) fail('invalid_identity_registry', `identityRegistry.satellites[${index}] must be an object`);
    const satelliteId = entry.satellite_id ?? entry.satelliteId;
    if (typeof satelliteId !== 'string' || !/^P\d{2}-S\d{2}$/.test(satelliteId)) {
      fail('invalid_identity_registry', `identityRegistry.satellites[${index}] has an invalid satellite id`);
    }
    if (result.has(satelliteId)) fail('invalid_identity_registry', `duplicate registry satellite '${satelliteId}'`);
    if (!Array.isArray(entry.cells) || entry.cells.length !== 2) {
      fail('invalid_identity_registry', `registry satellite '${satelliteId}' must contain exactly two cells`);
    }
    const cells = new Map();
    for (const [cellIndex, cell] of entry.cells.entries()) {
      if (!isPlainObject(cell) || (cell.bank !== 0 && cell.bank !== 1) || cells.has(cell.bank)) {
        fail('invalid_identity_registry', `registry satellite '${satelliteId}' must contain distinct banks 0 and 1`);
      }
      if (!Number.isSafeInteger(cell.pci) || cell.pci < 0 || cell.pci > 1007) {
        fail('invalid_identity_registry', `identityRegistry.satellites[${index}].cells[${cellIndex}].pci is invalid`);
      }
      cells.set(cell.bank, {
        bank: cell.bank,
        nci: normalizeNci(cell.nci, `identityRegistry.satellites[${index}].cells[${cellIndex}].nci`),
        pci: cell.pci
      });
    }
    if (!cells.has(0) || !cells.has(1) || cells.get(0).nci === cells.get(1).nci) {
      fail('invalid_identity_registry', `registry satellite '${satelliteId}' must contain two distinct identities`);
    }
    result.set(satelliteId, cells);
  }
  return result;
}

function intervalFields(record, context) {
  if (!isPlainObject(record)) fail('invalid_interval', `${context} must be an object`);
  const startTimeUs = assertSafeInteger(record.start_time_us, `${context}.start_time_us`);
  const endTimeUs = assertSafeInteger(record.end_time_us, `${context}.end_time_us`);
  if (endTimeUs <= startTimeUs) fail('invalid_interval', `${context} must use a non-empty [start,end) interval`);
  if (typeof record.position_id !== 'string' || !/^G\d{6}$/.test(record.position_id)) {
    fail('invalid_interval', `${context}.position_id must use G###### form`);
  }
  if (typeof record.satellite_id !== 'string' || !/^P\d{2}-S\d{2}$/.test(record.satellite_id)) {
    fail('invalid_interval', `${context}.satellite_id must use Pxx-Syy form`);
  }
  return {
    startTimeUs,
    endTimeUs,
    positionId: record.position_id,
    satelliteId: record.satellite_id
  };
}

function assertKnownReferences(interval, context, positions, registry) {
  if (!positions.has(interval.positionId)) {
    fail('unknown_position', `${context} references unknown position '${interval.positionId}'`);
  }
  if (!registry.has(interval.satelliteId)) {
    fail('unknown_identity', `${context} references satellite '${interval.satelliteId}' without registry identity`);
  }
}

function visibilityKey(interval) {
  return `${interval.threshold}\0${interval.positionId}\0${interval.satelliteId}`;
}

function pairKey(interval) {
  return `${interval.positionId}\0${interval.satelliteId}`;
}

function normalizeVisibility(records, positions, registry) {
  if (!Array.isArray(records)) fail('invalid_interval', 'visibilityIntervalRecords must be an array');
  const normalized = records.map((record, index) => {
    const context = `visibilityIntervalRecords[${index}]`;
    const interval = intervalFields(record, context);
    assertKnownReferences(interval, context, positions, registry);
    if (record.record_type !== undefined && record.record_type !== 'visibility_interval') {
      fail('invalid_interval', `${context}.record_type must be 'visibility_interval'`);
    }
    if (record.threshold !== 'entry' && record.threshold !== 'release') {
      fail('invalid_interval', `${context}.threshold must be 'entry' or 'release'`);
    }
    return {...interval, threshold: record.threshold};
  });
  normalized.sort((left, right) => compareText(visibilityKey(left), visibilityKey(right))
    || compareInteger(left.startTimeUs, right.startTimeUs)
    || compareInteger(left.endTimeUs, right.endTimeUs));
  const previousByKey = new Map();
  for (const interval of normalized) {
    const key = visibilityKey(interval);
    const previous = previousByKey.get(key);
    if (previous && interval.startTimeUs < previous.endTimeUs) {
      fail('overlapping_visibility', `visibility intervals overlap for '${interval.positionId}/${interval.satelliteId}/${interval.threshold}'`);
    }
    previousByKey.set(key, interval);
  }
  return normalized;
}

function normalizeAssignments(records, positions, registry) {
  if (!Array.isArray(records)) fail('invalid_interval', 'assignmentIntervalRecords must be an array');
  const normalized = records.map((record, index) => {
    const context = `assignmentIntervalRecords[${index}]`;
    const interval = intervalFields(record, context);
    assertKnownReferences(interval, context, positions, registry);
    if (record.record_type !== undefined && record.record_type !== 'assignment_interval') {
      fail('invalid_interval', `${context}.record_type must be 'assignment_interval'`);
    }
    if (record.cell_bank !== 0 && record.cell_bank !== 1) {
      fail('identity_mismatch', `${context}.cell_bank must be 0 or 1`);
    }
    const identity = registry.get(interval.satelliteId).get(record.cell_bank);
    if (record.nci !== undefined && normalizeNci(record.nci, `${context}.nci`) !== identity.nci) {
      fail('identity_mismatch', `${context}.nci differs from the identity registry`);
    }
    if (record.pci !== undefined && record.pci !== identity.pci) {
      fail('identity_mismatch', `${context}.pci differs from the identity registry`);
    }
    return {...interval, cellBank: record.cell_bank, nci: identity.nci, pci: identity.pci};
  });
  normalized.sort((left, right) => compareText(left.positionId, right.positionId)
    || compareInteger(left.startTimeUs, right.startTimeUs)
    || compareInteger(left.endTimeUs, right.endTimeUs)
    || compareText(left.satelliteId, right.satelliteId)
    || left.cellBank - right.cellBank);
  const previousByPosition = new Map();
  for (const interval of normalized) {
    const previous = previousByPosition.get(interval.positionId);
    if (previous && interval.startTimeUs < previous.endTimeUs) {
      fail('overlapping_assignment', `assignment intervals overlap for '${interval.positionId}'`);
    }
    previousByPosition.set(interval.positionId, interval);
  }
  return normalized;
}

function groupByPair(intervals, predicate = () => true) {
  const result = new Map();
  for (const interval of intervals) {
    if (!predicate(interval)) continue;
    const key = pairKey(interval);
    const group = result.get(key) ?? [];
    group.push(interval);
    result.set(key, group);
  }
  return result;
}

function rangeCovered(intervals, startTimeUs, endTimeUs) {
  let coveredUntilUs = startTimeUs;
  for (const interval of intervals ?? []) {
    if (interval.endTimeUs <= coveredUntilUs) continue;
    if (interval.startTimeUs > coveredUntilUs) return false;
    coveredUntilUs = interval.endTimeUs;
    if (coveredUntilUs >= endTimeUs) return true;
  }
  return false;
}

function validateVisibilityRelations(visibility, assignments) {
  const releaseByPair = groupByPair(visibility, ({threshold}) => threshold === 'release');
  const entryByPair = groupByPair(visibility, ({threshold}) => threshold === 'entry');
  for (const interval of visibility) {
    if (interval.threshold === 'entry' &&
        !rangeCovered(releaseByPair.get(pairKey(interval)), interval.startTimeUs, interval.endTimeUs)) {
      fail('entry_not_release_visible', `entry interval is outside release visibility for '${interval.positionId}/${interval.satelliteId}'`);
    }
  }
  for (const interval of assignments) {
    if (!rangeCovered(releaseByPair.get(pairKey(interval)), interval.startTimeUs, interval.endTimeUs)) {
      fail('assignment_not_visible', `assignment interval is outside release visibility for '${interval.positionId}/${interval.satelliteId}'`);
    }
  }
  const previousByPosition = new Map();
  for (const interval of assignments) {
    const previous = previousByPosition.get(interval.positionId);
    const continuesSameOwner = previous !== undefined && previous.endTimeUs === interval.startTimeUs &&
      previous.satelliteId === interval.satelliteId;
    if (!continuesSameOwner && !(entryByPair.get(pairKey(interval)) ?? []).some((entry) => {
      return entry.startTimeUs <= interval.startTimeUs && interval.startTimeUs < entry.endTimeUs;
    })) {
      fail(
        'assignment_without_entry',
        `assignment acquisition lacks entry-threshold evidence for '${interval.positionId}/${interval.satelliteId}'`
      );
    }
    previousByPosition.set(interval.positionId, interval);
  }
}

function contains(interval, timeUs) {
  return interval.startTimeUs <= timeUs && timeUs < interval.endTimeUs;
}

function planningTimeUnixMs(scenario, timeUs) {
  if (!isPlainObject(scenario)) fail('invalid_scenario', 'scenario must be an object');
  const epochUnixMs = scenario.orbit_epoch_unix_ms ?? scenario.orbitEpochUnixMs;
  assertSafeInteger(epochUnixMs, 'scenario.orbit_epoch_unix_ms', 0);
  const relativeMs = Math.floor(timeUs / 1000);
  const result = epochUnixMs + relativeMs;
  assertSafeInteger(result, 'derived planningTimeUnixMs', 0);
  return result;
}

function validateActiveCapacity(assignments, satelliteId, scenario) {
  const satelliteCapacity = scenario.satellite_capacity ?? scenario.satelliteCapacity;
  const cellCapacity = scenario.cell_capacity ?? scenario.cellCapacity;
  if (satelliteCapacity === undefined && cellCapacity === undefined) return;
  assertSafeInteger(satelliteCapacity, 'scenario.satellite_capacity', 1);
  assertSafeInteger(cellCapacity, 'scenario.cell_capacity', 1);
  if (satelliteCapacity > 2 * cellCapacity) {
    fail('invalid_scenario', 'scenario satellite capacity must fit inside two cell capacities');
  }
  if (assignments.length > satelliteCapacity) {
    fail('schedule_overflow', `satellite '${satelliteId}' has ${assignments.length} assignments at the replay time`);
  }
  const byBank = [0, 0];
  for (const assignment of assignments) byBank[assignment.cellBank] += 1;
  if (byBank.some((count) => count > cellCapacity)) {
    fail('cell_partition_overflow', `satellite '${satelliteId}' exceeds a stable cell capacity at the replay time`);
  }
}

/**
 * Selects one relative instant from replayable interval evidence and exports a
 * schema-v3 dry-run candidate plan plus its compatibility assignment sidecar.
 *
 * timeUs is relative to scenario.orbit_epoch_unix_ms. Intervals are interpreted
 * strictly as [start_time_us,end_time_us); the millisecond plan time is the
 * containing Unix millisecond when timeUs has sub-millisecond precision.
 */
export function exportDryRunSatellitePlanFromReplay(options) {
  if (!isPlainObject(options)) fail('invalid_shape', 'options must be an object');
  const {
    timeUs,
    satelliteId,
    catalogGeometry,
    identityRegistry,
    scenario,
    planningContext,
    visibilityIntervalRecords,
    assignmentIntervalRecords
  } = options;
  assertSafeInteger(timeUs, 'timeUs');
  if (typeof satelliteId !== 'string' || !/^P\d{2}-S\d{2}$/.test(satelliteId)) {
    fail('invalid_satellite_id', 'satelliteId must use canonical Pxx-Syy form');
  }
  const positions = catalogPositions(catalogGeometry);
  const registry = registryBySatellite(identityRegistry);
  if (!registry.has(satelliteId)) {
    fail('unknown_identity', `satellite '${satelliteId}' has no identity registry entry`);
  }
  const visibility = normalizeVisibility(visibilityIntervalRecords, positions, registry);
  const assignments = normalizeAssignments(assignmentIntervalRecords, positions, registry);
  validateVisibilityRelations(visibility, assignments);

  const visibleInventory = visibility
    .filter((interval) => interval.threshold === 'release' && interval.satelliteId === satelliteId && contains(interval, timeUs))
    .map((interval) => positions.get(interval.positionId));
  const activeAssignments = assignments
    .filter((interval) => interval.satelliteId === satelliteId && contains(interval, timeUs));
  validateActiveCapacity(activeAssignments, satelliteId, scenario);

  const exported = exportDryRunSatellitePlan({
    satelliteId,
    planningTimeUnixMs: planningTimeUnixMs(scenario, timeUs),
    visibleInventory,
    assignments: activeAssignments.map((assignment) => ({
      positionId: assignment.positionId,
      satelliteId: assignment.satelliteId,
      cellBank: assignment.cellBank,
      nci: assignment.nci,
      pci: assignment.pci
    })),
    identityRegistry,
    planningContext
  });
  return {
    replay_time_us: timeUs,
    ...exported
  };
}
