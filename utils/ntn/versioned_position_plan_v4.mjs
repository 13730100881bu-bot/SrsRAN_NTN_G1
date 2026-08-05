#!/usr/bin/env node

import {
  createHash,
  createPrivateKey,
  createPublicKey,
  sign as cryptoSign,
  verify as cryptoVerify
} from 'node:crypto';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';

import {
  ACCESS_PROFILE_V1,
  ACCESS_PROFILE_V1_HASH,
  MAX_PLANNING_CONTEXT_IDENTIFIER_BYTES,
  MAX_POSITION_PLAN_ENTRIES,
  MAX_POSITION_PLAN_FILE_BYTES,
  PlanValidationError,
  canonicalPayloadV3,
  generatePlanV3,
  readPositionPlanUtf8FileBounded,
  validatePlanV3,
  writePositionPlanTextAtomic
} from './versioned_position_plan_v3.mjs';

export {
  ACCESS_PROFILE_V1,
  ACCESS_PROFILE_V1_HASH,
  MAX_PLANNING_CONTEXT_IDENTIFIER_BYTES,
  MAX_POSITION_PLAN_ENTRIES,
  MAX_POSITION_PLAN_FILE_BYTES,
  PlanValidationError
};

export const POSITION_PLAN_SIGNATURE_ALGORITHM = 'ecdsa-p256-sha256';
export const MAX_POSITION_PLAN_SIGNATURE_BYTES = 256;
export const MAX_POSITION_PLAN_CROSS_LANGUAGE_INTEGER = Number.MAX_SAFE_INTEGER;
export const MAX_POSITION_PLAN_UNIX_TIME_MS = 8_000_000_000_000;
export const POSITION_PLAN_SIGNATURE_DOMAIN = 'srsran-ntn-position-plan-v4-signature-v1';

// Updated only when the schema-v4 canonical representation is intentionally changed.
// The signature itself is intentionally not a golden constant because ECDSA signing is randomized.
export const GOLDEN_CONTENT_HASH_V4 =
  'sha256:6b0f70aff33b6e5e7cc71069a316184653155185cc76e4bf2f8173e18e1bd21a';

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
  'assigned_l1_position_ids',
  'authentication'
];

function fail(message) {
  throw new PlanValidationError(message);
}

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function assertExactKeys(value, context, required, optional = []) {
  if (!isPlainObject(value)) fail(`${context} must be an object`);
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) fail(`unknown field '${context}.${key}'`);
  }
  for (const key of required) {
    if (!Object.hasOwn(value, key)) fail(`missing field '${context}.${key}'`);
  }
}

function assertBoundedIdentifier(value, context) {
  if (typeof value !== 'string' || value.length === 0) {
    fail(`${context} must be a non-empty string`);
  }
  const size = Buffer.byteLength(value, 'utf8');
  if (size > MAX_PLANNING_CONTEXT_IDENTIFIER_BYTES) {
    fail(`input_too_large: ${context} is ${size} UTF-8 bytes; maximum is ${MAX_PLANNING_CONTEXT_IDENTIFIER_BYTES}`);
  }
}

function assertSignatureSafeIdentifier(value, context) {
  assertBoundedIdentifier(value, context);
  if (!/^[A-Za-z0-9][A-Za-z0-9._:/-]*$/.test(value)) {
    fail(`${context} contains unsupported characters for schema-v4 signing`);
  }
}

function normalizeContentHash(value) {
  if (typeof value !== 'string') fail('content_hash must be a string');
  const normalized = value.toLowerCase().startsWith('sha256:')
    ? value.toLowerCase()
    : `sha256:${value.toLowerCase()}`;
  if (!/^sha256:[0-9a-f]{64}$/.test(normalized)) fail('content_hash must be a SHA-256 digest');
  return normalized;
}

function assertCanonicalBase64(value) {
  if (typeof value !== 'string' || value.length === 0) {
    fail('authentication.signature_base64 must be a non-empty string');
  }
  if (Buffer.byteLength(value, 'utf8') > MAX_POSITION_PLAN_SIGNATURE_BYTES) {
    fail(`input_too_large: authentication.signature_base64 exceeds ${MAX_POSITION_PLAN_SIGNATURE_BYTES} bytes`);
  }
  if (!/^[A-Za-z0-9+/]+={0,2}$/.test(value) || value.length % 4 !== 0) {
    fail('authentication.signature_base64 must use canonical base64');
  }
  const decoded = Buffer.from(value, 'base64');
  if (decoded.length === 0 || decoded.toString('base64') !== value) {
    fail('authentication.signature_base64 must use canonical base64');
  }
  return decoded;
}

function validateShapeV4(plan, {contentHashRequired, signatureRequired}) {
  assertExactKeys(
    plan,
    'root',
    contentHashRequired ? ROOT_KEYS : ROOT_KEYS.filter((key) => key !== 'content_hash'),
    contentHashRequired ? [] : ['content_hash']
  );
  if (plan.schema_version !== 4) fail('schema_version must be exactly 4');
  assertExactKeys(
    plan.authentication,
    'authentication',
    signatureRequired ? ['algorithm', 'key_id', 'signature_base64'] : ['algorithm', 'key_id'],
    signatureRequired ? [] : ['signature_base64']
  );
  if (plan.authentication.algorithm !== POSITION_PLAN_SIGNATURE_ALGORITHM) {
    fail(`authentication.algorithm must be '${POSITION_PLAN_SIGNATURE_ALGORITHM}'`);
  }
  assertSignatureSafeIdentifier(plan.authentication.key_id, 'authentication.key_id');
  if (signatureRequired || Object.hasOwn(plan.authentication, 'signature_base64')) {
    assertCanonicalBase64(plan.authentication.signature_base64);
  }
  if (Object.hasOwn(plan, 'content_hash')) normalizeContentHash(plan.content_hash);

  for (const [value, context] of [
    [plan.planning_run_id, 'planning_run_id'],
    [plan.catalog?.id, 'catalog.id'],
    [plan.identity_registry?.version, 'identity_registry.version'],
    [plan.access_profile?.id, 'access_profile.id']
  ]) {
    assertSignatureSafeIdentifier(value, context);
  }
  // Reuse all schema-v3 planning, identity, timing, inventory and size validation.
  generatePlanV3(toV3Draft(plan));
  for (const [index, position] of plan.visible_l1_positions.entries()) {
    if (Object.is(position.latitude_deg, -0) || Object.is(position.longitude_deg, -0)) {
      fail(`visible_l1_positions[${index}] contains negative zero, which is not stable in JSON`);
    }
  }
  if (plan.catalog_version > MAX_POSITION_PLAN_CROSS_LANGUAGE_INTEGER ||
      plan.schedule_version > MAX_POSITION_PLAN_CROSS_LANGUAGE_INTEGER) {
    fail('schema-v4 versions exceed the cross-language exact integer range');
  }
  if (plan.valid_from_unix_ms < -MAX_POSITION_PLAN_UNIX_TIME_MS ||
      plan.valid_from_unix_ms > MAX_POSITION_PLAN_UNIX_TIME_MS ||
      plan.valid_until_unix_ms < -MAX_POSITION_PLAN_UNIX_TIME_MS ||
      plan.valid_until_unix_ms > MAX_POSITION_PLAN_UNIX_TIME_MS ||
      plan.activation_epoch_unix_ms < 0 ||
      plan.activation_epoch_unix_ms > MAX_POSITION_PLAN_UNIX_TIME_MS) {
    fail('schema-v4 time is outside the supported millisecond range');
  }
}

function toV3Draft(plan) {
  return {
    schema_version: 3,
    planning_run_id: plan.planning_run_id,
    catalog: plan.catalog,
    identity_registry: plan.identity_registry,
    access_profile: plan.access_profile,
    satellite_id: plan.satellite_id,
    catalog_version: plan.catalog_version,
    schedule_version: plan.schedule_version,
    valid_from_unix_ms: plan.valid_from_unix_ms,
    valid_until_unix_ms: plan.valid_until_unix_ms,
    activation_epoch_unix_ms: plan.activation_epoch_unix_ms,
    onboard_cells: plan.onboard_cells,
    visible_l1_positions: plan.visible_l1_positions,
    assigned_l1_position_ids: plan.assigned_l1_position_ids
  };
}

function normalizedPlanV4(plan, contentHash, signatureBase64) {
  const base = generatePlanV3(toV3Draft(plan));
  return {
    schema_version: 4,
    planning_run_id: base.planning_run_id,
    catalog: base.catalog,
    identity_registry: base.identity_registry,
    access_profile: base.access_profile,
    satellite_id: base.satellite_id,
    catalog_version: base.catalog_version,
    schedule_version: base.schedule_version,
    content_hash: contentHash,
    valid_from_unix_ms: base.valid_from_unix_ms,
    valid_until_unix_ms: base.valid_until_unix_ms,
    activation_epoch_unix_ms: base.activation_epoch_unix_ms,
    onboard_cells: base.onboard_cells,
    visible_l1_positions: base.visible_l1_positions,
    assigned_l1_position_ids: base.assigned_l1_position_ids,
    authentication: {
      algorithm: POSITION_PLAN_SIGNATURE_ALGORITHM,
      key_id: plan.authentication.key_id,
      signature_base64: signatureBase64
    }
  };
}

export function canonicalPayloadV4(plan) {
  validateShapeV4(plan, {contentHashRequired: false, signatureRequired: false});
  return canonicalPayloadV3(toV3Draft(plan)).replace('schema_version=3\n', 'schema_version=4\n');
}

export function computeContentHashV4(plan) {
  return `sha256:${createHash('sha256').update(canonicalPayloadV4(plan), 'utf8').digest('hex')}`;
}

export function signaturePayloadV4(plan, contentHash = computeContentHashV4(plan)) {
  return `signature_context=${POSITION_PLAN_SIGNATURE_DOMAIN}\n` +
    canonicalPayloadV4(plan) +
    `content_hash=${normalizeContentHash(contentHash)}\n` +
    `authentication_algorithm=${POSITION_PLAN_SIGNATURE_ALGORITHM}\n` +
    `authentication_key_id=${plan.authentication.key_id}\n`;
}

function assertP256Key(key, context) {
  if (key.asymmetricKeyType !== 'ec' || key.asymmetricKeyDetails?.namedCurve !== 'prime256v1') {
    fail(`${context} must be an ECDSA P-256 key`);
  }
}

export function publicKeyFingerprint(publicKeyPem) {
  let key;
  try {
    key = publicKeyPem?.type === 'public' ? publicKeyPem : createPublicKey(publicKeyPem);
  } catch (error) {
    fail(`invalid public key: ${error.message}`);
  }
  assertP256Key(key, 'public key');
  const der = key.export({type: 'spki', format: 'der'});
  return `sha256:${createHash('sha256').update(der).digest('hex')}`;
}

export function generatePlanV4(draft, privateKeyPem) {
  validateShapeV4(draft, {contentHashRequired: false, signatureRequired: false});
  let privateKey;
  try {
    privateKey = createPrivateKey(privateKeyPem);
  } catch (error) {
    fail(`invalid private key: ${error.message}`);
  }
  assertP256Key(privateKey, 'private key');
  const contentHash = computeContentHashV4(draft);
  const signature = cryptoSign('sha256', Buffer.from(signaturePayloadV4(draft, contentHash), 'utf8'), privateKey);
  return normalizedPlanV4(draft, contentHash, signature.toString('base64'));
}

export function validatePlanV4(plan, publicKeyPem) {
  validateShapeV4(plan, {contentHashRequired: true, signatureRequired: true});
  const computed = computeContentHashV4(plan);
  if (normalizeContentHash(plan.content_hash) !== computed) {
    fail(`content_hash mismatch: expected ${computed}`);
  }
  let publicKey;
  try {
    publicKey = createPublicKey(publicKeyPem);
  } catch (error) {
    fail(`invalid public key: ${error.message}`);
  }
  assertP256Key(publicKey, 'public key');
  const signature = assertCanonicalBase64(plan.authentication.signature_base64);
  const valid = cryptoVerify(
    'sha256',
    Buffer.from(signaturePayloadV4(plan, computed), 'utf8'),
    publicKey,
    signature
  );
  if (!valid) fail('invalid_signature: signature verification failed');
  return {
    contentHash: computed,
    canonicalPayload: canonicalPayloadV4(plan),
    keyId: plan.authentication.key_id,
    publicKeyFingerprint: publicKeyFingerprint(publicKey)
  };
}

export function upgradeAndSignPlanV4(planV3, keyId, privateKeyPem) {
  assertBoundedIdentifier(keyId, 'authentication.key_id');
  validatePlanV3(planV3);
  return generatePlanV4({
    ...toV3Draft(planV3),
    schema_version: 4,
    authentication: {algorithm: POSITION_PLAN_SIGNATURE_ALGORITHM, key_id: keyId}
  }, privateKeyPem);
}

function assertNoDuplicateJsonMembers(text) {
  let offset = 0;

  function skipWhitespace() {
    while (offset < text.length && /[\x20\t\r\n]/.test(text[offset])) offset += 1;
  }

  function parseStringToken() {
    if (text[offset] !== '"') fail('invalid JSON string');
    const start = offset;
    offset += 1;
    while (offset < text.length) {
      const character = text[offset];
      if (character === '"') {
        offset += 1;
        return JSON.parse(text.slice(start, offset));
      }
      if (character === '\\') {
        offset += 2;
      } else {
        offset += 1;
      }
    }
    fail('unterminated JSON string');
  }

  function parseValue(depth) {
    if (depth > 64) fail('JSON nesting exceeds 64 levels');
    skipWhitespace();
    if (text[offset] === '{') {
      parseObject(depth);
      return;
    }
    if (text[offset] === '[') {
      parseArray(depth);
      return;
    }
    if (text[offset] === '"') {
      parseStringToken();
      return;
    }
    const start = offset;
    while (offset < text.length && !/[\x20\t\r\n,\]}:]/.test(text[offset])) offset += 1;
    if (offset === start) fail('invalid JSON value');
  }

  function parseObject(depth) {
    offset += 1;
    const members = new Set();
    skipWhitespace();
    if (text[offset] === '}') {
      offset += 1;
      return;
    }
    while (offset < text.length) {
      skipWhitespace();
      const member = parseStringToken();
      if (members.has(member)) fail(`duplicate JSON member '${member}' is not allowed`);
      members.add(member);
      skipWhitespace();
      if (text[offset] !== ':') fail('JSON object member is missing a colon');
      offset += 1;
      parseValue(depth + 1);
      skipWhitespace();
      if (text[offset] === '}') {
        offset += 1;
        return;
      }
      if (text[offset] !== ',') fail('JSON object members must be comma-separated');
      offset += 1;
    }
    fail('unterminated JSON object');
  }

  function parseArray(depth) {
    offset += 1;
    skipWhitespace();
    if (text[offset] === ']') {
      offset += 1;
      return;
    }
    while (offset < text.length) {
      parseValue(depth + 1);
      skipWhitespace();
      if (text[offset] === ']') {
        offset += 1;
        return;
      }
      if (text[offset] !== ',') fail('JSON array entries must be comma-separated');
      offset += 1;
    }
    fail('unterminated JSON array');
  }

  parseValue(0);
  skipWhitespace();
  if (offset !== text.length) fail('unexpected trailing JSON input');
}

export function parsePlanV4JsonText(text) {
  if (typeof text !== 'string') fail('plan document must be UTF-8 text');
  if (Buffer.byteLength(text, 'utf8') > MAX_POSITION_PLAN_FILE_BYTES) {
    fail(`input_too_large: plan document exceeds ${MAX_POSITION_PLAN_FILE_BYTES} bytes`);
  }
  assertNoDuplicateJsonMembers(text);
  return JSON.parse(text);
}

function readJson(path) {
  return parsePlanV4JsonText(readPositionPlanUtf8FileBounded(path));
}

function serializedPlanJson(plan, publicKeyPem) {
  validatePlanV4(plan, publicKeyPem);
  const text = `${JSON.stringify(plan, null, 2)}\n`;
  if (Buffer.byteLength(text, 'utf8') > MAX_POSITION_PLAN_FILE_BYTES) {
    fail(`input_too_large: serialized plan exceeds ${MAX_POSITION_PLAN_FILE_BYTES} bytes`);
  }
  return text;
}

export function writePlanFileV4(plan, publicKeyPem, outputPath, operationOverrides = {}) {
  writePositionPlanTextAtomic(serializedPlanJson(plan, publicKeyPem), outputPath, operationOverrides);
}

function usage() {
  return [
    'Usage:',
    '  node versioned_position_plan_v4.mjs sign <draft-or-v3.json> <key-id> <private-key.pem> [output.json|-]',
    '  node versioned_position_plan_v4.mjs validate <plan.json> <public-key.pem>',
    '',
    'The signing private key remains outside the output plan. Output is atomically replaced when a file path is used.'
  ].join('\n');
}

export function runCli(argv) {
  const [command, ...args] = argv;
  if (command === undefined || command === '--help' || command === '-h') {
    process.stdout.write(`${usage()}\n`);
    return 0;
  }
  if (command === 'sign') {
    if (args.length < 3 || args.length > 4) fail('sign requires <draft-or-v3.json> <key-id> <private-key.pem> [output]');
    const [inputPath, keyId, privateKeyPath, outputPath] = args;
    const source = readJson(inputPath);
    const privateKeyPem = readFileSync(privateKeyPath, 'utf8');
    const plan = source.schema_version === 3
      ? upgradeAndSignPlanV4(source, keyId, privateKeyPem)
      : generatePlanV4({...source, authentication: {
          algorithm: POSITION_PLAN_SIGNATURE_ALGORITHM,
          key_id: keyId
        }}, privateKeyPem);
    const publicKeyPem = createPublicKey(privateKeyPem).export({type: 'spki', format: 'pem'});
    const text = serializedPlanJson(plan, publicKeyPem);
    if (outputPath === undefined || outputPath === '-') process.stdout.write(text);
    else writePositionPlanTextAtomic(text, outputPath);
    return 0;
  }
  if (command === 'validate') {
    if (args.length !== 2) fail('validate requires <plan.json> <public-key.pem>');
    const result = validatePlanV4(readJson(args[0]), readFileSync(args[1], 'utf8'));
    process.stdout.write(`valid ${result.contentHash} key_id=${result.keyId} fingerprint=${result.publicKeyFingerprint}\n`);
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
