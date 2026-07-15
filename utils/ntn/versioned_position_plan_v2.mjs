#!/usr/bin/env node

import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';

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
  'visible_l1_positions'
];

const MAX_NCI = 0xfffffffff;
const MAX_PCI = 1007;

export const ACCESS_PROFILE_V1 = Object.freeze({
  id: 'ntn-access-16a-64d-v1',
  max_l1_positions_per_cell: 128,
  max_l1_positions_per_satellite: 256,
  analog_ports_per_cell: 16,
  analog_ports_per_satellite: 32,
  digital_ports_per_cell: 64,
  digital_ports_per_satellite: 128,
  access_slot_us: 10000,
  subvisit_duration_us: 2500,
  max_ssb_interval_us: 80000,
  max_prach_interval_us: 640000,
  activation_alignment_ms: 640,
  cell_access_slot_stride: 2,
  subvisits_per_access_slot: 4,
  phases: [
    {downlink_port_mask: 0x07ff, uplink_port_mask: 0xf800, digital_downlink_capacity: 43, digital_uplink_capacity: 21},
    {downlink_port_mask: 0x07ff, uplink_port_mask: 0xf800, digital_downlink_capacity: 43, digital_uplink_capacity: 21},
    {downlink_port_mask: 0x03ff, uplink_port_mask: 0xfc00, digital_downlink_capacity: 42, digital_uplink_capacity: 22}
  ]
});

export function canonicalAccessProfileV1() {
  const profile = ACCESS_PROFILE_V1;
  const lines = [
    `access_profile_id=${profile.id}`,
    `max_l1_positions_per_cell=${profile.max_l1_positions_per_cell}`,
    `max_l1_positions_per_satellite=${profile.max_l1_positions_per_satellite}`,
    `analog_ports_per_cell=${profile.analog_ports_per_cell}`,
    `analog_ports_per_satellite=${profile.analog_ports_per_satellite}`,
    `digital_ports_per_cell=${profile.digital_ports_per_cell}`,
    `digital_ports_per_satellite=${profile.digital_ports_per_satellite}`,
    `access_slot_us=${profile.access_slot_us}`,
    `subvisit_duration_us=${profile.subvisit_duration_us}`,
    `max_ssb_interval_us=${profile.max_ssb_interval_us}`,
    `max_prach_interval_us=${profile.max_prach_interval_us}`,
    `activation_alignment_ms=${profile.activation_alignment_ms}`,
    `cell_access_slot_stride=${profile.cell_access_slot_stride}`,
    `subvisits_per_access_slot=${profile.subvisits_per_access_slot}`
  ];
  for (const [index, phase] of profile.phases.entries()) {
    lines.push(
      `phase=${index},${phase.downlink_port_mask},${phase.uplink_port_mask},` +
      `${phase.digital_downlink_capacity},${phase.digital_uplink_capacity}`);
  }
  return `${lines.join('\n')}\n`;
}

export const ACCESS_PROFILE_V1_HASH =
  `sha256:${createHash('sha256').update(canonicalAccessProfileV1(), 'utf8').digest('hex')}`;

export const GOLDEN_CONTENT_HASH =
  'sha256:374593193a7a297734e583978263759e12209c491418eed55ee82af884ab599d';

export class PlanValidationError extends Error {
  constructor(message) {
    super(message);
    this.name = 'PlanValidationError';
  }
}

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

export function normalizeSha256(value, context = 'sha256') {
  assertNonEmptyString(value, context);
  const lower = value.toLowerCase();
  const normalized = lower.startsWith('sha256:') ? lower : `sha256:${lower}`;
  if (!/^sha256:[0-9a-f]{64}$/.test(normalized)) {
    fail(`${context} must be a SHA-256 digest`);
  }
  return normalized;
}

// Equivalent to C++ classic-locale defaultfloat with max_digits10 for IEEE-754 double.
// JavaScript toExponential(16) supplies 17 significant digits; the remaining code
// applies the defaultfloat/%g fixed-vs-scientific threshold and removes insignificant zeros.
export function formatCppDouble(value) {
  if (!Number.isFinite(value)) {
    fail('coordinate must be finite');
  }
  if (Object.is(value, -0)) {
    return '-0';
  }
  if (value === 0) {
    return '0';
  }

  const precision = 17;
  const raw = Math.abs(value).toExponential(precision - 1);
  const [rawMantissa, rawExponent] = raw.split('e');
  const exponent = Number.parseInt(rawExponent, 10);
  let digits = rawMantissa.replace('.', '').replace(/0+$/, '');
  if (digits.length === 0) {
    digits = '0';
  }
  const sign = value < 0 ? '-' : '';

  if (exponent >= -4 && exponent < precision) {
    const decimalPosition = exponent + 1;
    let fixed;
    if (decimalPosition <= 0) {
      fixed = `0.${'0'.repeat(-decimalPosition)}${digits}`;
    } else if (decimalPosition >= digits.length) {
      fixed = `${digits}${'0'.repeat(decimalPosition - digits.length)}`;
    } else {
      fixed = `${digits.slice(0, decimalPosition)}.${digits.slice(decimalPosition)}`;
    }
    return `${sign}${fixed}`;
  }

  const mantissa = digits.length === 1 ? digits : `${digits[0]}.${digits.slice(1)}`;
  const exponentSign = exponent < 0 ? '-' : '+';
  const exponentDigits = Math.abs(exponent).toString().padStart(2, '0');
  return `${sign}${mantissa}e${exponentSign}${exponentDigits}`;
}

function validateShape(plan, {contentHashRequired}) {
  assertExactKeys(plan, 'root', contentHashRequired ? ROOT_KEYS : ROOT_KEYS.filter((key) => key !== 'content_hash'),
    contentHashRequired ? [] : ['content_hash']);

  if (plan.schema_version !== 2) {
    fail('schema_version must be exactly 2');
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
  const seenPositions = new Set();
  for (const [index, position] of plan.visible_l1_positions.entries()) {
    const context = `visible_l1_positions[${index}]`;
    assertExactKeys(position, context, ['position_id', 'latitude_deg', 'longitude_deg', 'child_mask']);
    if (typeof position.position_id !== 'string' || !/^G\d{6}$/.test(position.position_id)) {
      fail(`${context}.position_id must use G###### form`);
    }
    if (seenPositions.has(position.position_id)) {
      fail(`duplicate position_id '${position.position_id}'`);
    }
    seenPositions.add(position.position_id);
    if (!Number.isFinite(position.latitude_deg) || position.latitude_deg < -90 || position.latitude_deg > 90) {
      fail(`${context}.latitude_deg must be finite and inside -90..90`);
    }
    if (!Number.isFinite(position.longitude_deg) || position.longitude_deg < -180 || position.longitude_deg > 180) {
      fail(`${context}.longitude_deg must be finite and inside -180..180`);
    }
    assertSafeInteger(position.child_mask, `${context}.child_mask`, 1, 127);
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

export function canonicalPayloadV2(plan) {
  validateShape(plan, {contentHashRequired: false});
  const lines = [
    'schema_version=2',
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
      `l1=${position.position_id},${formatCppDouble(position.latitude_deg)},${formatCppDouble(position.longitude_deg)},${position.child_mask}`
    );
  }
  return `${lines.join('\n')}\n`;
}

export function computeContentHashV2(plan) {
  const payload = canonicalPayloadV2(plan);
  return `sha256:${createHash('sha256').update(payload, 'utf8').digest('hex')}`;
}

function normalizedOutputPlan(plan, contentHash) {
  return {
    schema_version: 2,
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
    }))
  };
}

export function generatePlanV2(draft) {
  validateShape(draft, {contentHashRequired: false});
  return normalizedOutputPlan(draft, computeContentHashV2(draft));
}

export function validatePlanV2(plan) {
  validateShape(plan, {contentHashRequired: true});
  const computed = computeContentHashV2(plan);
  if (normalizeSha256(plan.content_hash, 'content_hash') !== computed) {
    fail(`content_hash mismatch: expected ${computed}`);
  }
  return {contentHash: computed, canonicalPayload: canonicalPayloadV2(plan)};
}

export function makeGoldenPlanV2() {
  return generatePlanV2({
    schema_version: 2,
    planning_run_id: 'planning-run-2026-07-15',
    catalog: {
      id: 'global-land-l1-v1',
      sha256: 'sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a'
    },
    identity_registry: {
      version: 'mc-ntn-onboard-cell-registry-v1',
      sha256: 'sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a'
    },
    access_profile: {
      id: ACCESS_PROFILE_V1.id,
      sha256: ACCESS_PROFILE_V1_HASH
    },
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
    ]
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
    '  node versioned_position_plan_v2.mjs generate <draft.json> [output.json|-]',
    '  node versioned_position_plan_v2.mjs validate <plan.json>',
    '  node versioned_position_plan_v2.mjs golden [output.json|-]',
    '',
    'The draft may omit content_hash. Output is a deterministic single-satellite schema-v2 plan.'
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
    if (inputPath === undefined) {
      fail('generate requires <draft.json>');
    }
    emitJson(generatePlanV2(readJson(inputPath)), outputPath);
    return 0;
  }
  if (command === 'validate') {
    if (inputPath === undefined || outputPath !== undefined) {
      fail('validate requires exactly one <plan.json>');
    }
    const result = validatePlanV2(readJson(inputPath));
    process.stdout.write(`valid ${result.contentHash}\n`);
    return 0;
  }
  if (command === 'golden') {
    if (outputPath !== undefined) {
      fail('golden accepts at most one [output.json|-] argument');
    }
    const golden = makeGoldenPlanV2();
    if (golden.content_hash !== GOLDEN_CONTENT_HASH) {
      fail(`golden hash drift: expected ${GOLDEN_CONTENT_HASH}, got ${golden.content_hash}`);
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
