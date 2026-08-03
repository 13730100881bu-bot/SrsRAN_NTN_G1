#!/usr/bin/env node

import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';

import {
  ACCESS_PROFILE_V1,
  ACCESS_PROFILE_V1_HASH,
  PlanValidationError,
  formatCppDouble,
  normalizeSha256
} from './versioned_position_plan_v2.mjs';

export {ACCESS_PROFILE_V1, ACCESS_PROFILE_V1_HASH, PlanValidationError};

const ROOT_KEYS = [
  'schema_version',
  'planning_run_id',
  'catalog',
  'identity_registry',
  'access_profile',
  'satellite_id',
  'catalog_version',
  'schedule_version',
  'content_hash',
  'valid_from_unix_ms',
  'valid_until_unix_ms',
  'activation_epoch_unix_ms',
  'onboard_cells',
  'visible_l1_positions',
  'assigned_l1_position_ids'
];

const MAX_NCI = 0xfffffffff;
const MAX_PCI = 1007;

// Updated after the canonical representation is intentionally changed.
export const GOLDEN_CONTENT_HASH_V3 =
  'sha256:005e172665221e8bad772cc5f372d3b03378e762fb3487c836389760ce6b0155';

function fail(message) {
  throw new PlanValidationError(message);
}

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function assertExactKeys(value, context, required, optional = []) {
  if (!isPlainObject(value)) {
    fail(`${context} must be an object`);
  }
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) {
      fail(`unknown field '${context}.${key}'`);
    }
  }
  for (const key of required) {
    if (!Object.hasOwn(value, key)) {
      fail(`missing field '${context}.${key}'`);
    }
  }
}

function assertNonEmptyString(value, context) {
  if (typeof value !== 'string' || value.length === 0) {
    fail(`${context} must be a non-empty string`);
  }
}

function assertSafeInteger(value, context, minimum, maximum = Number.MAX_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
    fail(`${context} must be a safe integer in ${minimum}..${maximum}`);
  }
}

function validateShape(plan, {contentHashRequired}) {
  assertExactKeys(
    plan,
    'root',
    contentHashRequired ? ROOT_KEYS : ROOT_KEYS.filter((key) => key !== 'content_hash'),
    contentHashRequired ? [] : ['content_hash']
  );

  if (plan.schema_version !== 3) {
    fail('schema_version must be exactly 3');
  }
  assertNonEmptyString(plan.planning_run_id, 'planning_run_id');

  assertExactKeys(plan.catalog, 'catalog', ['id', 'sha256']);
  assertNonEmptyString(plan.catalog.id, 'catalog.id');
  normalizeSha256(plan.catalog.sha256, 'catalog.sha256');

  assertExactKeys(plan.identity_registry, 'identity_registry', ['version', 'sha256']);
  assertNonEmptyString(plan.identity_registry.version, 'identity_registry.version');
  normalizeSha256(plan.identity_registry.sha256, 'identity_registry.sha256');

  assertExactKeys(plan.access_profile, 'access_profile', ['id', 'sha256']);
  assertNonEmptyString(plan.access_profile.id, 'access_profile.id');
  normalizeSha256(plan.access_profile.sha256, 'access_profile.sha256');

  if (typeof plan.satellite_id !== 'string' || !/^P\d{2}-S\d{2}$/.test(plan.satellite_id)) {
    fail('satellite_id must use canonical Pxx-Syy form');
  }
  assertSafeInteger(plan.catalog_version, 'catalog_version', 1);
  assertSafeInteger(plan.schedule_version, 'schedule_version', 1);
  assertSafeInteger(plan.valid_from_unix_ms, 'valid_from_unix_ms', Number.MIN_SAFE_INTEGER);
  assertSafeInteger(plan.valid_until_unix_ms, 'valid_until_unix_ms', Number.MIN_SAFE_INTEGER);
  assertSafeInteger(plan.activation_epoch_unix_ms, 'activation_epoch_unix_ms', 0);
  if (plan.valid_from_unix_ms >= plan.valid_until_unix_ms) {
    fail('valid_from_unix_ms must be before valid_until_unix_ms');
  }
  if (plan.activation_epoch_unix_ms < plan.valid_from_unix_ms ||
      plan.activation_epoch_unix_ms >= plan.valid_until_unix_ms) {
    fail('activation_epoch_unix_ms must be inside the validity interval');
  }

  if (!Array.isArray(plan.onboard_cells) || plan.onboard_cells.length !== 2) {
    fail('onboard_cells must contain exactly two identities');
  }
  const seenNcis = new Set();
  for (const [index, cell] of plan.onboard_cells.entries()) {
    const context = `onboard_cells[${index}]`;
    assertExactKeys(cell, context, ['nci', 'pci']);
    assertSafeInteger(cell.nci, `${context}.nci`, 0, MAX_NCI);
    assertSafeInteger(cell.pci, `${context}.pci`, 0, MAX_PCI);
    if (seenNcis.has(cell.nci)) {
      fail('onboard_cells must contain two distinct opaque NCIs');
    }
    seenNcis.add(cell.nci);
  }

  if (!Array.isArray(plan.visible_l1_positions)) {
    fail('visible_l1_positions must be an array');
  }
  const visiblePositionIds = new Set();
  for (const [index, position] of plan.visible_l1_positions.entries()) {
    const context = `visible_l1_positions[${index}]`;
    assertExactKeys(position, context, ['position_id', 'latitude_deg', 'longitude_deg', 'child_mask']);
    if (typeof position.position_id !== 'string' || !/^G\d{6}$/.test(position.position_id)) {
      fail(`${context}.position_id must use G###### form`);
    }
    if (visiblePositionIds.has(position.position_id)) {
      fail(`duplicate position_id '${position.position_id}'`);
    }
    visiblePositionIds.add(position.position_id);
    if (!Number.isFinite(position.latitude_deg) || position.latitude_deg < -90 || position.latitude_deg > 90) {
      fail(`${context}.latitude_deg must be finite and inside -90..90`);
    }
    if (!Number.isFinite(position.longitude_deg) || position.longitude_deg < -180 || position.longitude_deg > 180) {
      fail(`${context}.longitude_deg must be finite and inside -180..180`);
    }
    assertSafeInteger(position.child_mask, `${context}.child_mask`, 1, 127);
  }

  if (!Array.isArray(plan.assigned_l1_position_ids)) {
    fail('assigned_l1_position_ids must be an array');
  }
  const assignedPositionIds = new Set();
  for (const [index, positionId] of plan.assigned_l1_position_ids.entries()) {
    const context = `assigned_l1_position_ids[${index}]`;
    if (typeof positionId !== 'string' || !/^G\d{6}$/.test(positionId)) {
      fail(`${context} must use G###### form`);
    }
    if (assignedPositionIds.has(positionId)) {
      fail(`duplicate assigned position_id '${positionId}'`);
    }
    if (!visiblePositionIds.has(positionId)) {
      fail(`assigned position '${positionId}' is absent from visible_l1_positions`);
    }
    assignedPositionIds.add(positionId);
  }

  if (Object.hasOwn(plan, 'content_hash')) {
    normalizeSha256(plan.content_hash, 'content_hash');
  }
}

function sortedCells(plan) {
  return [...plan.onboard_cells].sort((left, right) => left.nci - right.nci || left.pci - right.pci);
}

function sortedPositions(plan) {
  return [...plan.visible_l1_positions].sort((left, right) => {
    const positionIdOrder = left.position_id < right.position_id ? -1 : left.position_id > right.position_id ? 1 : 0;
    return positionIdOrder || left.latitude_deg - right.latitude_deg || left.longitude_deg - right.longitude_deg;
  });
}

function sortedAssignedPositionIds(plan) {
  return [...plan.assigned_l1_position_ids].sort();
}

export function canonicalPayloadV3(plan) {
  validateShape(plan, {contentHashRequired: false});
  const lines = [
    'schema_version=3',
    `planning_run_id=${plan.planning_run_id}`,
    `catalog=${plan.catalog.id},${normalizeSha256(plan.catalog.sha256, 'catalog.sha256')}`,
    `identity_registry=${plan.identity_registry.version},${normalizeSha256(plan.identity_registry.sha256, 'identity_registry.sha256')}`,
    `access_profile=${plan.access_profile.id},${normalizeSha256(plan.access_profile.sha256, 'access_profile.sha256')}`,
    `satellite_id=${plan.satellite_id}`,
    `catalog_version=${plan.catalog_version}`,
    `schedule_version=${plan.schedule_version}`,
    `valid_from_unix_ms=${plan.valid_from_unix_ms}`,
    `valid_until_unix_ms=${plan.valid_until_unix_ms}`,
    `activation_epoch_unix_ms=${plan.activation_epoch_unix_ms}`
  ];

  for (const cell of sortedCells(plan)) {
    lines.push(`cell=${cell.nci},${cell.pci}`);
  }
  for (const position of sortedPositions(plan)) {
    lines.push(
      `l1=${position.position_id},${formatCppDouble(position.latitude_deg)},` +
      `${formatCppDouble(position.longitude_deg)},${position.child_mask}`
    );
  }
  for (const positionId of sortedAssignedPositionIds(plan)) {
    lines.push(`assigned_l1=${positionId}`);
  }
  return `${lines.join('\n')}\n`;
}

export function computeContentHashV3(plan) {
  return `sha256:${createHash('sha256').update(canonicalPayloadV3(plan), 'utf8').digest('hex')}`;
}

function normalizedOutputPlan(plan, contentHash) {
  return {
    schema_version: 3,
    planning_run_id: plan.planning_run_id,
    catalog: {id: plan.catalog.id, sha256: normalizeSha256(plan.catalog.sha256, 'catalog.sha256')},
    identity_registry: {
      version: plan.identity_registry.version,
      sha256: normalizeSha256(plan.identity_registry.sha256, 'identity_registry.sha256')
    },
    access_profile: {
      id: plan.access_profile.id,
      sha256: normalizeSha256(plan.access_profile.sha256, 'access_profile.sha256')
    },
    satellite_id: plan.satellite_id,
    catalog_version: plan.catalog_version,
    schedule_version: plan.schedule_version,
    content_hash: contentHash,
    valid_from_unix_ms: plan.valid_from_unix_ms,
    valid_until_unix_ms: plan.valid_until_unix_ms,
    activation_epoch_unix_ms: plan.activation_epoch_unix_ms,
    onboard_cells: sortedCells(plan).map((cell) => ({nci: cell.nci, pci: cell.pci})),
    visible_l1_positions: sortedPositions(plan).map((position) => ({
      position_id: position.position_id,
      latitude_deg: position.latitude_deg,
      longitude_deg: position.longitude_deg,
      child_mask: position.child_mask
    })),
    assigned_l1_position_ids: sortedAssignedPositionIds(plan)
  };
}

export function generatePlanV3(draft) {
  validateShape(draft, {contentHashRequired: false});
  return normalizedOutputPlan(draft, computeContentHashV3(draft));
}

export function validatePlanV3(plan) {
  validateShape(plan, {contentHashRequired: true});
  const computed = computeContentHashV3(plan);
  if (normalizeSha256(plan.content_hash, 'content_hash') !== computed) {
    fail(`content_hash mismatch: expected ${computed}`);
  }
  return {contentHash: computed, canonicalPayload: canonicalPayloadV3(plan)};
}

export function makeGoldenPlanV3() {
  return generatePlanV3({
    schema_version: 3,
    planning_run_id: 'planning-run-2026-07-15',
    catalog: {
      id: 'global-land-l1-v1',
      sha256: 'sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a'
    },
    identity_registry: {
      version: 'mc-ntn-onboard-cell-registry-v1',
      sha256: 'sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a'
    },
    access_profile: {id: ACCESS_PROFILE_V1.id, sha256: ACCESS_PROFILE_V1_HASH},
    satellite_id: 'P01-S01',
    catalog_version: 1,
    schedule_version: 1,
    valid_from_unix_ms: 640,
    valid_until_unix_ms: 64000,
    activation_epoch_unix_ms: 1920,
    onboard_cells: [
      {nci: 0x123450001, pci: 101},
      {nci: 0x123450002, pci: 202}
    ],
    visible_l1_positions: [
      {position_id: 'G000001', latitude_deg: 10, longitude_deg: 20, child_mask: 127},
      {position_id: 'G000002', latitude_deg: 10, longitude_deg: 20.05, child_mask: 127}
    ],
    assigned_l1_position_ids: ['G000001']
  });
}

function readJson(path) {
  return JSON.parse(readFileSync(path, 'utf8'));
}

function emitJson(plan, outputPath) {
  const text = `${JSON.stringify(plan, null, 2)}\n`;
  if (outputPath === undefined || outputPath === '-') {
    process.stdout.write(text);
  } else {
    writeFileSync(outputPath, text, 'utf8');
  }
}

function usage() {
  return [
    'Usage:',
    '  node versioned_position_plan_v3.mjs generate <draft.json> [output.json|-]',
    '  node versioned_position_plan_v3.mjs validate <plan.json>',
    '  node versioned_position_plan_v3.mjs golden [output.json|-]',
    '',
    'The draft may omit content_hash. Output is a deterministic single-satellite schema-v3 plan.'
  ].join('\n');
}

export function runCli(argv) {
  const [command, inputPath, outputPath, ...extra] = argv;
  if (extra.length !== 0 || command === undefined || command === '--help' || command === '-h') {
    if (command === undefined || command === '--help' || command === '-h') {
      process.stdout.write(`${usage()}\n`);
      return 0;
    }
    fail('too many command-line arguments');
  }

  if (command === 'generate') {
    if (inputPath === undefined) fail('generate requires <draft.json>');
    emitJson(generatePlanV3(readJson(inputPath)), outputPath);
    return 0;
  }
  if (command === 'validate') {
    if (inputPath === undefined || outputPath !== undefined) {
      fail('validate requires exactly one <plan.json>');
    }
    const result = validatePlanV3(readJson(inputPath));
    process.stdout.write(`valid ${result.contentHash}\n`);
    return 0;
  }
  if (command === 'golden') {
    if (outputPath !== undefined) fail('golden accepts at most one [output.json|-] argument');
    const golden = makeGoldenPlanV3();
    if (golden.content_hash !== GOLDEN_CONTENT_HASH_V3) {
      fail(`golden hash drift: expected ${GOLDEN_CONTENT_HASH_V3}, got ${golden.content_hash}`);
    }
    emitJson(golden, inputPath);
    return 0;
  }
  fail(`unknown command '${command}'`);
}

const invokedPath = process.argv[1] === undefined ? undefined : pathToFileURL(process.argv[1]).href;
if (invokedPath === import.meta.url) {
  try {
    process.exitCode = runCli(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`${error.name ?? 'Error'}: ${error.message}\n`);
    process.exitCode = 1;
  }
}
