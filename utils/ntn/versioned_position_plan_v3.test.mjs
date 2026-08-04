import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

import {
  GOLDEN_CONTENT_HASH_V3,
  MAX_PLANNING_CONTEXT_IDENTIFIER_BYTES,
  MAX_POSITION_PLAN_ENTRIES,
  MAX_POSITION_PLAN_FILE_BYTES,
  PlanValidationError,
  canonicalPayloadV3,
  computeContentHashV3,
  generatePlanV3,
  makeGoldenPlanV3,
  validatePlanV3,
  writePlanFileV3
} from './versioned_position_plan_v3.mjs';

const script = fileURLToPath(new URL('./versioned_position_plan_v3.mjs', import.meta.url));

function clone(value) {
  return structuredClone(value);
}

function position(index) {
  return {
    position_id: `G${String(index + 1).padStart(6, '0')}`,
    latitude_deg: -20 + (index % 1000) * 0.01,
    longitude_deg: 30 + Math.floor(index / 1000) * 0.01,
    child_mask: index % 2 === 0 ? 127 : 1
  };
}

function temporaryDirectory(t) {
  const directory = mkdtempSync(join(tmpdir(), 'ntn-plan-v3-'));
  t.after(() => rmSync(directory, {recursive: true, force: true}));
  return directory;
}

test('schema-v3 golden plan covers the visible inventory and assigned subset', () => {
  const plan = makeGoldenPlanV3();
  assert.equal(plan.content_hash, GOLDEN_CONTENT_HASH_V3);
  assert.equal(validatePlanV3(plan).contentHash, GOLDEN_CONTENT_HASH_V3);
  assert.match(canonicalPayloadV3(plan), /l1=G000002,10,20\.050000000000001,127\nassigned_l1=G000001\n$/);
});

test('canonical hash is independent of cell, visible and assigned input order', () => {
  const draft = clone(makeGoldenPlanV3());
  delete draft.content_hash;
  draft.assigned_l1_position_ids = ['G000002', 'G000001'];
  const expected = generatePlanV3(draft);

  draft.onboard_cells.reverse();
  draft.visible_l1_positions.reverse();
  draft.assigned_l1_position_ids.reverse();
  draft.catalog.sha256 = draft.catalog.sha256.slice('sha256:'.length).toUpperCase();

  assert.equal(computeContentHashV3(draft), expected.content_hash);
  assert.deepEqual(generatePlanV3(draft), expected);
});

test('assigned positions must be unique, canonical and drawn from the complete visible inventory', () => {
  const mutations = [
    {ids: ['G000001', 'G000001'], pattern: /duplicate assigned/},
    {ids: ['G000003'], pattern: /absent from visible/},
    {ids: ['G00001'], pattern: /G######/},
    {ids: [1], pattern: /G######/}
  ];

  for (const {ids, pattern} of mutations) {
    const draft = clone(makeGoldenPlanV3());
    delete draft.content_hash;
    draft.assigned_l1_position_ids = ids;
    assert.throws(() => generatePlanV3(draft), pattern);
  }
});

test('the schema accepts a complete 257-position inventory with a 256-position assigned subset', () => {
  const draft = clone(makeGoldenPlanV3());
  delete draft.content_hash;
  draft.visible_l1_positions = Array.from({length: 257}, (_, index) => position(index));
  draft.assigned_l1_position_ids = draft.visible_l1_positions
    .slice(0, 256)
    .map(({position_id}) => position_id)
    .reverse();

  const plan = generatePlanV3(draft);
  assert.equal(plan.visible_l1_positions.length, 257);
  assert.equal(plan.assigned_l1_position_ids.length, 256);
  assert.equal(validatePlanV3(plan).contentHash, plan.content_hash);
});

test('planning context identifiers use UTF-8 byte limits without changing canonical hashing', () => {
  const draft = clone(makeGoldenPlanV3());
  delete draft.content_hash;
  const maximum = 'a'.repeat(MAX_PLANNING_CONTEXT_IDENTIFIER_BYTES);
  draft.planning_run_id = maximum;
  draft.catalog.id = maximum;
  draft.identity_registry.version = maximum;
  draft.access_profile.id = maximum;

  const accepted = generatePlanV3(draft);
  assert.equal(validatePlanV3(accepted).contentHash, accepted.content_hash);

  for (const mutate of [
    (plan, value) => { plan.planning_run_id = value; },
    (plan, value) => { plan.catalog.id = value; },
    (plan, value) => { plan.identity_registry.version = value; },
    (plan, value) => { plan.access_profile.id = value; }
  ]) {
    const oversized = clone(makeGoldenPlanV3());
    delete oversized.content_hash;
    mutate(oversized, `${maximum}a`);
    assert.throws(() => generatePlanV3(oversized), /input_too_large: .*257 UTF-8 bytes/);
  }

  const multibyte = clone(makeGoldenPlanV3());
  delete multibyte.content_hash;
  multibyte.planning_run_id = `${'界'.repeat(85)}a`;
  assert.equal(Buffer.byteLength(multibyte.planning_run_id, 'utf8'), MAX_PLANNING_CONTEXT_IDENTIFIER_BYTES);
  assert.doesNotThrow(() => generatePlanV3(multibyte));
});

test('visible and assigned arrays stop at 65,536 entries before oversized arrays are traversed', (t) => {
  const draft = clone(makeGoldenPlanV3());
  delete draft.content_hash;
  draft.visible_l1_positions = Array.from({length: MAX_POSITION_PLAN_ENTRIES}, (_, index) => position(index));
  draft.assigned_l1_position_ids = draft.visible_l1_positions.map(({position_id}) => position_id);

  const accepted = generatePlanV3(draft);
  assert.equal(accepted.visible_l1_positions.length, MAX_POSITION_PLAN_ENTRIES);
  assert.equal(accepted.assigned_l1_position_ids.length, MAX_POSITION_PLAN_ENTRIES);

  const outputPath = join(temporaryDirectory(t), 'plan.json');
  writeFileSync(outputPath, 'previous complete plan\n');
  assert.throws(() => writePlanFileV3(accepted, outputPath), /input_too_large: serialized plan/);
  assert.equal(readFileSync(outputPath, 'utf8'), 'previous complete plan\n');

  const tooManyVisible = clone(makeGoldenPlanV3());
  delete tooManyVisible.content_hash;
  tooManyVisible.visible_l1_positions = new Array(MAX_POSITION_PLAN_ENTRIES + 1).fill(position(0));
  assert.throws(
    () => generatePlanV3(tooManyVisible),
    new RegExp(`input_too_large: visible_l1_positions contains ${MAX_POSITION_PLAN_ENTRIES + 1}`)
  );

  const tooManyAssigned = clone(makeGoldenPlanV3());
  delete tooManyAssigned.content_hash;
  tooManyAssigned.assigned_l1_position_ids = new Array(MAX_POSITION_PLAN_ENTRIES + 1).fill('G000001');
  assert.throws(
    () => generatePlanV3(tooManyAssigned),
    new RegExp(`input_too_large: assigned_l1_position_ids contains ${MAX_POSITION_PLAN_ENTRIES + 1}`)
  );
});

test('plan input accepts exactly 4 MiB and rejects one additional byte', (t) => {
  const directory = temporaryDirectory(t);
  const exactPath = join(directory, 'exact.json');
  const oversizedPath = join(directory, 'oversized.json');
  const compactPlan = JSON.stringify(makeGoldenPlanV3());
  const compactBytes = Buffer.byteLength(compactPlan, 'utf8');
  assert.ok(compactBytes < MAX_POSITION_PLAN_FILE_BYTES);
  const exactText = compactPlan + ' '.repeat(MAX_POSITION_PLAN_FILE_BYTES - compactBytes);
  writeFileSync(exactPath, exactText, 'utf8');
  writeFileSync(oversizedPath, `${exactText} `, 'utf8');

  const exact = spawnSync(process.execPath, [script, 'validate', exactPath], {encoding: 'utf8'});
  assert.equal(exact.status, 0, exact.stderr);
  assert.match(exact.stdout, new RegExp(`^valid ${GOLDEN_CONTENT_HASH_V3}`));

  const oversized = spawnSync(process.execPath, [script, 'validate', oversizedPath], {encoding: 'utf8'});
  assert.equal(oversized.status, 1);
  assert.match(oversized.stderr, /input_too_large: plan file is 4194305 bytes/);
});

test('atomic plan output replaces complete files and preserves the old file when rename fails', (t) => {
  const directory = temporaryDirectory(t);
  const outputPath = join(directory, 'plan.json');
  writeFileSync(outputPath, 'old complete plan\n', 'utf8');

  assert.throws(
    () => writePlanFileV3(makeGoldenPlanV3(), outputPath, {
      renameSync() {
        throw new Error('injected rename failure');
      }
    }),
    /atomic_write_failed: .*injected rename failure/
  );
  assert.equal(readFileSync(outputPath, 'utf8'), 'old complete plan\n');
  assert.deepEqual(readdirSync(directory), ['plan.json']);

  writePlanFileV3(makeGoldenPlanV3(), outputPath);
  const written = JSON.parse(readFileSync(outputPath, 'utf8'));
  assert.equal(validatePlanV3(written).contentHash, GOLDEN_CONTENT_HASH_V3);
  assert.deepEqual(readdirSync(directory), ['plan.json']);
});

test('schema-v3 remains exact at the root and assigned collection is required', () => {
  const unexpected = clone(makeGoldenPlanV3());
  unexpected.unexpected = true;
  assert.throws(() => generatePlanV3(unexpected), PlanValidationError);

  const missing = clone(makeGoldenPlanV3());
  delete missing.assigned_l1_position_ids;
  assert.throws(() => generatePlanV3(missing), /missing field/);
});

test('headless golden CLI is deterministic and validates its own output', () => {
  const result = spawnSync(process.execPath, [script, 'golden', '-'], {encoding: 'utf8'});
  assert.equal(result.status, 0, result.stderr);
  const plan = JSON.parse(result.stdout);
  assert.equal(plan.content_hash, GOLDEN_CONTENT_HASH_V3);
  assert.equal(validatePlanV3(plan).contentHash, GOLDEN_CONTENT_HASH_V3);
});
