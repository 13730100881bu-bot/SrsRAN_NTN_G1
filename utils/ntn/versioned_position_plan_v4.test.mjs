import assert from 'node:assert/strict';
import {generateKeyPairSync} from 'node:crypto';
import {mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';

import {generatePlanV3, makeGoldenPlanV3} from './versioned_position_plan_v3.mjs';
import {
  GOLDEN_CONTENT_HASH_V4,
  MAX_POSITION_PLAN_UNIX_TIME_MS,
  POSITION_PLAN_SIGNATURE_ALGORITHM,
  canonicalPayloadV4,
  computeContentHashV4,
  generatePlanV4,
  parsePlanV4JsonText,
  publicKeyFingerprint,
  signaturePayloadV4,
  upgradeAndSignPlanV4,
  validatePlanV4,
  writePlanFileV4
} from './versioned_position_plan_v4.mjs';

const FIXED_GOLDEN_PLAN_PATH = new URL('./testdata/versioned-position-plan-v4-golden.json', import.meta.url);
const FIXED_GOLDEN_PUBLIC_KEY_PATH = new URL(
  './testdata/versioned-position-plan-v4-public-key.pem',
  import.meta.url
);
const FIXED_GOLDEN_PUBLIC_KEY_FINGERPRINT =
  'sha256:1f30855499b0759a2341994ae997682567ba78f8de94d01bb5ac11bec75cad5d';

function clone(value) {
  return structuredClone(value);
}

function keyPair(namedCurve = 'prime256v1') {
  const pair = generateKeyPairSync('ec', {
    namedCurve,
    publicKeyEncoding: {type: 'spki', format: 'pem'},
    privateKeyEncoding: {type: 'pkcs8', format: 'pem'}
  });
  return {publicKey: pair.publicKey, privateKey: pair.privateKey};
}

function temporaryDirectory(t) {
  const directory = mkdtempSync(join(tmpdir(), 'ntn-plan-v4-'));
  t.after(() => rmSync(directory, {recursive: true, force: true}));
  return directory;
}

function signedGolden(key = keyPair()) {
  return {
    key,
    plan: upgradeAndSignPlanV4(makeGoldenPlanV3(), 'planning-key-2026-01', key.privateKey)
  };
}

test('fixed schema-v4 golden independently recomputes its hash and verifies its signature', () => {
  const plan = JSON.parse(readFileSync(FIXED_GOLDEN_PLAN_PATH, 'utf8'));
  const publicKey = readFileSync(FIXED_GOLDEN_PUBLIC_KEY_PATH, 'utf8');

  assert.equal(plan.content_hash, GOLDEN_CONTENT_HASH_V4);
  assert.equal(computeContentHashV4(plan), GOLDEN_CONTENT_HASH_V4);
  assert.equal(plan.authentication.key_id, 'ntn-test-signing-key-v1');
  assert.equal(
    plan.authentication.signature_base64,
    'MEQCICGY2vaeBkfRbmXf5obfO2NBzBOC0TeUjZOIhjrQQWrSAiAhiQ4u9dwEzNW1oWVlOd3TBWPZDUr0PRZYwSCLMTIrbw=='
  );

  const result = validatePlanV4(plan, publicKey);
  assert.equal(result.contentHash, GOLDEN_CONTENT_HASH_V4);
  assert.equal(result.publicKeyFingerprint, FIXED_GOLDEN_PUBLIC_KEY_FINGERPRINT);
  assert.equal(publicKeyFingerprint(publicKey), FIXED_GOLDEN_PUBLIC_KEY_FINGERPRINT);
});

test('schema-v4 upgrades a validated v3 candidate and verifies its P-256 signature', () => {
  const {key, plan} = signedGolden();
  const result = validatePlanV4(plan, key.publicKey);

  assert.equal(plan.schema_version, 4);
  assert.equal(plan.authentication.algorithm, POSITION_PLAN_SIGNATURE_ALGORITHM);
  assert.equal(result.contentHash, plan.content_hash);
  assert.equal(result.keyId, 'planning-key-2026-01');
  assert.equal(result.publicKeyFingerprint, publicKeyFingerprint(key.publicKey));
  assert.match(canonicalPayloadV4(plan), /^schema_version=4\n/);
  assert.match(signaturePayloadV4(plan), /^signature_context=srsran-ntn-position-plan-v4-signature-v1\n/);
  assert.match(signaturePayloadV4(plan), /authentication_key_id=planning-key-2026-01\n$/);
});

test('content tampering fails before signature verification and recomputed tampering fails the signature', () => {
  const {key, plan} = signedGolden();
  const staleHash = clone(plan);
  staleHash.schedule_version += 1;
  assert.throws(() => validatePlanV4(staleHash, key.publicKey), /content_hash mismatch/);

  const recomputedHash = clone(staleHash);
  recomputedHash.content_hash = computeContentHashV4(recomputedHash);
  assert.throws(() => validatePlanV4(recomputedHash, key.publicKey), /invalid_signature/);
});

test('authentication metadata is exact, bounded and signed', () => {
  const {key, plan} = signedGolden();
  const unknownField = clone(plan);
  unknownField.authentication.public_key = 'forbidden';
  assert.throws(() => validatePlanV4(unknownField, key.publicKey), /unknown field/);

  const wrongAlgorithm = clone(plan);
  wrongAlgorithm.authentication.algorithm = 'rsa-sha256';
  assert.throws(() => validatePlanV4(wrongAlgorithm, key.publicKey), /algorithm/);

  const unsafeKeyId = clone(makeGoldenPlanV3());
  assert.throws(
    () => upgradeAndSignPlanV4(unsafeKeyId, 'key\nsecond-line', key.privateKey),
    /unsupported characters/
  );

  const malformedSignature = clone(plan);
  malformedSignature.authentication.signature_base64 = 'AA==\n';
  assert.throws(() => validatePlanV4(malformedSignature, key.publicKey), /canonical base64/);
});

test('schema-v4 rejects context delimiters that could collide in the signed canonical text', () => {
  const key = keyPair();
  const base = makeGoldenPlanV3();
  const catalogHash = base.catalog.sha256;
  const candidates = [
    ['planning_run_id', `x\ncatalog=y,${catalogHash}`],
    ['catalog.id', `y,${catalogHash}\ncatalog=a`],
    ['identity_registry.version', 'registry,alternate'],
    ['access_profile.id', 'profile=alternate']
  ];

  for (const [field, value] of candidates) {
    const draft = clone(base);
    delete draft.content_hash;
    if (field === 'planning_run_id') draft.planning_run_id = value;
    if (field === 'catalog.id') draft.catalog.id = value;
    if (field === 'identity_registry.version') draft.identity_registry.version = value;
    if (field === 'access_profile.id') draft.access_profile.id = value;
    const validV3 = generatePlanV3(draft);
    assert.throws(
      () => upgradeAndSignPlanV4(validV3, 'planning-key-1', key.privateKey),
      /unsupported characters for schema-v4 signing/,
      field
    );
  }
});

test('schema-v4 raw JSON rejects duplicate members before last-wins parsing', () => {
  const {plan} = signedGolden();
  const text = JSON.stringify(plan);
  const duplicateRoot = text.replace('"schema_version":4', '"schema_version":4,"schema_version":4');
  assert.throws(() => parsePlanV4JsonText(duplicateRoot), /duplicate JSON member 'schema_version'/);

  const algorithm = `"algorithm":"${POSITION_PLAN_SIGNATURE_ALGORITHM}"`;
  const duplicateAuthentication = text.replace(algorithm, `${algorithm},${algorithm}`);
  assert.throws(() => parsePlanV4JsonText(duplicateAuthentication), /duplicate JSON member 'algorithm'/);
});

test('schema-v4 time values stay inside the shared C++ millisecond range', () => {
  const key = keyPair();
  const draft = makeGoldenPlanV3();
  delete draft.content_hash;
  draft.valid_from_unix_ms = MAX_POSITION_PLAN_UNIX_TIME_MS;
  draft.activation_epoch_unix_ms = MAX_POSITION_PLAN_UNIX_TIME_MS;
  draft.valid_until_unix_ms = MAX_POSITION_PLAN_UNIX_TIME_MS + 1000;
  const validV3 = generatePlanV3(draft);
  assert.throws(
    () => upgradeAndSignPlanV4(validV3, 'planning-key-1', key.privateKey),
    /outside the supported millisecond range/
  );
});

test('schema-v4 rejects negative-zero coordinates before hashing and JSON output', () => {
  const key = keyPair();
  for (const coordinate of ['latitude_deg', 'longitude_deg']) {
    const draft = makeGoldenPlanV3();
    delete draft.content_hash;
    draft.visible_l1_positions[0][coordinate] = -0;
    const validV3 = generatePlanV3(draft);
    assert.equal(Object.is(validV3.visible_l1_positions[0][coordinate], -0), true);
    assert.throws(
      () => upgradeAndSignPlanV4(validV3, 'planning-key-1', key.privateKey),
      /negative zero, which is not stable in JSON/,
      coordinate
    );
  }
});

test('wrong key and non-P-256 keys are rejected', () => {
  const {plan} = signedGolden();
  const wrong = keyPair();
  assert.throws(() => validatePlanV4(plan, wrong.publicKey), /invalid_signature/);

  const p384 = keyPair('secp384r1');
  assert.throws(() => validatePlanV4(plan, p384.publicKey), /P-256/);
  assert.throws(() => upgradeAndSignPlanV4(makeGoldenPlanV3(), 'key-1', p384.privateKey), /P-256/);
});

test('v3 to v4 upgrade preserves complete visibility and the independent assigned subset', () => {
  const key = keyPair();
  const candidate = makeGoldenPlanV3();
  candidate.visible_l1_positions = Array.from({length: 257}, (_, index) => ({
    position_id: `G${String(index + 1).padStart(6, '0')}`,
    latitude_deg: 10 + index * 0.001,
    longitude_deg: 20 + index * 0.001,
    child_mask: 127
  }));
  candidate.assigned_l1_position_ids = candidate.visible_l1_positions
    .slice(0, 256)
    .map(({position_id}) => position_id);
  // Regenerate the v3 hash before it becomes an approved signing input.
  delete candidate.content_hash;
  const validCandidate = generatePlanV3(candidate);

  const plan = upgradeAndSignPlanV4(validCandidate, 'planning-key-1', key.privateKey);
  assert.equal(plan.visible_l1_positions.length, 257);
  assert.equal(plan.assigned_l1_position_ids.length, 256);
  assert.doesNotThrow(() => validatePlanV4(plan, key.publicKey));

  const emptyDraft = makeGoldenPlanV3();
  delete emptyDraft.content_hash;
  emptyDraft.assigned_l1_position_ids = [];
  const emptyCandidate = generatePlanV3(emptyDraft);
  const emptyPlan = upgradeAndSignPlanV4(emptyCandidate, 'planning-key-1', key.privateKey);
  assert.deepEqual(emptyPlan.assigned_l1_position_ids, []);
});

test('atomic v4 output preserves the old file when replacement fails', (t) => {
  const {key, plan} = signedGolden();
  const directory = temporaryDirectory(t);
  const outputPath = join(directory, 'signed-plan.json');
  writeFileSync(outputPath, 'old complete plan\n', 'utf8');

  assert.throws(
    () => writePlanFileV4(plan, key.publicKey, outputPath, {
      renameSync() {
        throw new Error('injected rename failure');
      }
    }),
    /atomic_write_failed: .*injected rename failure/
  );
  assert.equal(readFileSync(outputPath, 'utf8'), 'old complete plan\n');
  assert.deepEqual(readdirSync(directory), ['signed-plan.json']);

  writePlanFileV4(plan, key.publicKey, outputPath);
  assert.doesNotThrow(() => validatePlanV4(JSON.parse(readFileSync(outputPath, 'utf8')), key.publicKey));
});
