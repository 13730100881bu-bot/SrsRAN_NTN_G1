import {createHash} from 'node:crypto';

import {generatePlanV2, validatePlanV2} from './versioned_position_plan_v2.mjs';

export const DRY_RUN_EXPORT_SCHEMA_VERSION = 1;
export const ASSIGNMENT_SIDECAR_SCHEMA_VERSION = 1;

export class ConstellationPlanExportError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'ConstellationPlanExportError';
    this.code = code;
  }
}

function fail(code, message) {
  throw new ConstellationPlanExportError(code, message);
}

function isPlainObject(value) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return false;
  }
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function assertPlainObject(value, context) {
  if (!isPlainObject(value)) {
    fail('invalid_shape', `${context} must be an object`);
  }
}

function assertSafeInteger(value, context, minimum = Number.MIN_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < minimum) {
    fail('invalid_value', `${context} must be a safe integer no smaller than ${minimum}`);
  }
}

function compareText(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function canonicalJsonValue(value) {
  if (value === null || typeof value === 'string' || typeof value === 'boolean') {
    return JSON.stringify(value);
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) {
      fail('non_canonical_value', 'sidecar contains a non-finite number');
    }
    return JSON.stringify(Object.is(value, -0) ? 0 : value);
  }
  if (Array.isArray(value)) {
    return `[${value.map(canonicalJsonValue).join(',')}]`;
  }
  if (!isPlainObject(value)) {
    fail('non_canonical_value', 'sidecar contains a non-JSON value');
  }
  return `{${Object.keys(value).sort(compareText).map((key) =>
    `${JSON.stringify(key)}:${canonicalJsonValue(value[key])}`).join(',')}}`;
}

function canonicalHash(value) {
  return `sha256:${createHash('sha256').update(canonicalJsonValue(value), 'utf8').digest('hex')}`;
}

function normalizeNci(value, context) {
  let numeric;
  try {
    if (typeof value === 'number' && (!Number.isSafeInteger(value) || value < 0)) {
      throw new TypeError();
    }
    if (typeof value !== 'number' && (typeof value !== 'string' || value.length === 0)) {
      throw new TypeError();
    }
    numeric = BigInt(value);
  } catch {
    fail('invalid_identity_registry', `${context} must be a numeric 36-bit NCI`);
  }
  if (numeric < 0n || numeric >= (1n << 36n)) {
    fail('invalid_identity_registry', `${context} must fit in 36 bits`);
  }
  return Number(numeric);
}

function registrySatelliteId(entry) {
  return entry?.satellite_id ?? entry?.satelliteId;
}

function findRegistryCells(registry, satelliteId) {
  const entries = Array.isArray(registry) ? registry : registry?.satellites;
  if (!Array.isArray(entries)) {
    fail('invalid_identity_registry', 'identityRegistry must contain a satellites array');
  }
  const matching = entries.filter((entry) => registrySatelliteId(entry) === satelliteId);
  if (matching.length !== 1) {
    fail('identity_mismatch', `identity registry must contain satellite '${satelliteId}' exactly once`);
  }
  const cells = matching[0]?.cells;
  if (!Array.isArray(cells) || cells.length !== 2) {
    fail('invalid_identity_registry', `satellite '${satelliteId}' must contain exactly two cells`);
  }

  const normalized = cells.map((cell, index) => {
    assertPlainObject(cell, `identityRegistry.cells[${index}]`);
    if (cell.bank !== 0 && cell.bank !== 1) {
      fail('invalid_identity_registry', `satellite '${satelliteId}' cell bank must be 0 or 1`);
    }
    if (!Number.isSafeInteger(cell.pci) || cell.pci < 0 || cell.pci > 1007) {
      fail('invalid_identity_registry', `satellite '${satelliteId}' PCI must be in 0..1007`);
    }
    return {bank: cell.bank, nci: normalizeNci(cell.nci, `identityRegistry.cells[${index}].nci`), pci: cell.pci};
  }).sort((left, right) => left.bank - right.bank);

  if (normalized[0].bank !== 0 || normalized[1].bank !== 1 || normalized[0].nci === normalized[1].nci) {
    fail('invalid_identity_registry', `satellite '${satelliteId}' must contain distinct bank 0 and bank 1 identities`);
  }
  return normalized;
}

function normalizeVisibleInventory(visibleInventory) {
  if (!Array.isArray(visibleInventory)) {
    fail('invalid_inventory', 'visibleInventory must be an array');
  }
  const seen = new Set();
  const normalized = visibleInventory.map((position, index) => {
    assertPlainObject(position, `visibleInventory[${index}]`);
    if (typeof position.id !== 'string' || !/^G\d{6}$/.test(position.id)) {
      fail('invalid_inventory', `visibleInventory[${index}].id must use G###### form`);
    }
    if (seen.has(position.id)) {
      fail('invalid_inventory', `duplicate visible position '${position.id}'`);
    }
    seen.add(position.id);
    if (!Number.isFinite(position.lat) || position.lat < -90 || position.lat > 90) {
      fail('invalid_inventory', `visibleInventory[${index}].lat must be inside -90..90`);
    }
    if (!Number.isFinite(position.lon) || position.lon < -180 || position.lon > 180) {
      fail('invalid_inventory', `visibleInventory[${index}].lon must be inside -180..180`);
    }
    if (!Number.isSafeInteger(position.childMask) || position.childMask < 1 || position.childMask > 127) {
      fail('invalid_inventory', `visibleInventory[${index}].childMask must be in 1..127`);
    }
    return {
      position_id: position.id,
      latitude_deg: position.lat,
      longitude_deg: position.lon,
      child_mask: position.childMask
    };
  });
  normalized.sort((left, right) => compareText(left.position_id, right.position_id));
  return normalized;
}

function assignmentSatelliteId(assignment) {
  return assignment?.satelliteId ?? assignment?.satellite_id;
}

function assignmentPositionId(assignment) {
  return assignment?.positionId ?? assignment?.position_id;
}

function assignmentCellBank(assignment) {
  return assignment?.cellBank ?? assignment?.cell_bank;
}

function normalizeAssignments(assignments, satelliteId, visiblePositionIds, cells) {
  if (!Array.isArray(assignments)) {
    fail('invalid_assignment', 'assignments must be an array');
  }
  const seenPositions = new Set();
  const targetAssignments = [];
  for (const [index, assignment] of assignments.entries()) {
    assertPlainObject(assignment, `assignments[${index}]`);
    const positionId = assignmentPositionId(assignment);
    const ownerSatelliteId = assignmentSatelliteId(assignment);
    if (typeof positionId !== 'string' || !/^G\d{6}$/.test(positionId)) {
      fail('invalid_assignment', `assignments[${index}] position id must use G###### form`);
    }
    if (seenPositions.has(positionId)) {
      fail('invalid_assignment', `duplicate assignment for '${positionId}'`);
    }
    seenPositions.add(positionId);
    if (typeof ownerSatelliteId !== 'string' || ownerSatelliteId.length === 0) {
      fail('invalid_assignment', `assignments[${index}] satellite id must be a non-empty string`);
    }
    if (ownerSatelliteId !== satelliteId) {
      continue;
    }
    if (!visiblePositionIds.has(positionId)) {
      fail('assignment_not_visible', `assigned position '${positionId}' is absent from the complete visible inventory`);
    }
    const bank = assignmentCellBank(assignment);
    if (bank !== 0 && bank !== 1) {
      fail('invalid_assignment', `assignment '${positionId}' cell bank must be 0 or 1`);
    }
    const identity = cells[bank];
    if (Object.hasOwn(assignment, 'nci') && normalizeNci(assignment.nci, `assignments[${index}].nci`) !== identity.nci) {
      fail('identity_mismatch', `assignment '${positionId}' NCI differs from the identity registry`);
    }
    if (Object.hasOwn(assignment, 'pci') && assignment.pci !== identity.pci) {
      fail('identity_mismatch', `assignment '${positionId}' PCI differs from the identity registry`);
    }
    targetAssignments.push({
      position_id: positionId,
      cell_bank: bank,
      nci: identity.nci,
      pci: identity.pci
    });
  }
  targetAssignments.sort((left, right) => compareText(left.position_id, right.position_id));
  return targetAssignments;
}

function normalizePlanningContext(planningContext, planningTimeUnixMs) {
  assertPlainObject(planningContext, 'planningContext');
  const integerFields = [
    ['catalogVersion', 1],
    ['scheduleVersion', 1],
    ['validFromUnixMs', Number.MIN_SAFE_INTEGER],
    ['validUntilUnixMs', Number.MIN_SAFE_INTEGER],
    ['activationEpochUnixMs', 0]
  ];
  for (const [field, minimum] of integerFields) {
    assertSafeInteger(planningContext[field], `planningContext.${field}`, minimum);
  }
  if (planningTimeUnixMs < planningContext.validFromUnixMs || planningTimeUnixMs >= planningContext.validUntilUnixMs) {
    fail('invalid_planning_time', 'planningTimeUnixMs must be inside the plan validity interval');
  }
  return planningContext;
}

/**
 * Exports one satellite's complete visible L1 inventory as a schema-v2 dry-run plan.
 *
 * Capacity assignment is deliberately kept in a separate sidecar. It never
 * trims visible_l1_positions and it never changes the two registry-owned cells.
 */
export function exportDryRunSatellitePlan({
  satelliteId,
  planningTimeUnixMs,
  visibleInventory,
  assignments,
  identityRegistry,
  planningContext
}) {
  if (typeof satelliteId !== 'string' || !/^P\d{2}-S\d{2}$/.test(satelliteId)) {
    fail('invalid_satellite_id', 'satelliteId must use canonical Pxx-Syy form');
  }
  assertSafeInteger(planningTimeUnixMs, 'planningTimeUnixMs', 0);
  const context = normalizePlanningContext(planningContext, planningTimeUnixMs);
  const cells = findRegistryCells(identityRegistry, satelliteId);
  const positions = normalizeVisibleInventory(visibleInventory);
  const visiblePositionIds = new Set(positions.map((position) => position.position_id));
  const assigned = normalizeAssignments(assignments, satelliteId, visiblePositionIds, cells);
  const assignedPositionIds = new Set(assigned.map((assignment) => assignment.position_id));

  const plan = generatePlanV2({
    schema_version: 2,
    planning_run_id: context.planningRunId,
    catalog: context.catalog,
    identity_registry: context.identityRegistry,
    access_profile: context.accessProfile,
    satellite_id: satelliteId,
    catalog_version: context.catalogVersion,
    schedule_version: context.scheduleVersion,
    valid_from_unix_ms: context.validFromUnixMs,
    valid_until_unix_ms: context.validUntilUnixMs,
    activation_epoch_unix_ms: context.activationEpochUnixMs,
    onboard_cells: cells.map(({nci, pci}) => ({nci, pci})),
    visible_l1_positions: positions
  });
  const validation = validatePlanV2(plan);

  const sidecarWithoutHash = {
    schema_version: ASSIGNMENT_SIDECAR_SCHEMA_VERSION,
    evidence_kind: 'nominal_assignment',
    mode: 'dry_run',
    runtime_activation_claimed: false,
    satellite_id: satelliteId,
    planning_time_unix_ms: planningTimeUnixMs,
    schedule_version: context.scheduleVersion,
    onboard_cells: cells.map((cell) => ({...cell})),
    assigned_l1_positions: assigned,
    visible_but_not_assigned_position_ids: positions
      .map((position) => position.position_id)
      .filter((positionId) => !assignedPositionIds.has(positionId))
  };
  const assignmentSidecar = {
    ...sidecarWithoutHash,
    content_hash: canonicalHash(sidecarWithoutHash)
  };

  return {
    export_schema_version: DRY_RUN_EXPORT_SCHEMA_VERSION,
    mode: 'dry_run',
    plan_role: 'candidate_only',
    runtime_activation_claimed: false,
    plan,
    plan_validation: {
      validator: 'versioned_position_plan_v2',
      content_hash: validation.contentHash
    },
    assignment_sidecar: assignmentSidecar
  };
}
