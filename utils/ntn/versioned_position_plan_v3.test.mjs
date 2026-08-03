import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

import {
  GOLDEN_CONTENT_HASH_V3,
  PlanValidationError,
  canonicalPayloadV3,
  computeContentHashV3,
  generatePlanV3,
  makeGoldenPlanV3,
  validatePlanV3
} from './versioned_position_plan_v3.mjs';

function clone(value) {
  return structuredClone(value);
}

function position(index) {
  return {
    position_id: `G${String(index + 1).padStart(6, '0')}`,
    latitude_deg: -20 + index * 0.01,
    longitude_deg: 30 + index * 0.01,
    child_mask: index % 2 === 0 ? 127 : 1
  };
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

test('schema-v3 remains exact at the root and assigned collection is required', () => {
  const unexpected = clone(makeGoldenPlanV3());
  unexpected.unexpected = true;
  assert.throws(() => generatePlanV3(unexpected), PlanValidationError);

  const missing = clone(makeGoldenPlanV3());
  delete missing.assigned_l1_position_ids;
  assert.throws(() => generatePlanV3(missing), /missing field/);
});

test('headless golden CLI is deterministic and validates its own output', () => {
  const script = fileURLToPath(new URL('./versioned_position_plan_v3.mjs', import.meta.url));
  const result = spawnSync(process.execPath, [script, 'golden', '-'], {encoding: 'utf8'});
  assert.equal(result.status, 0, result.stderr);
  const plan = JSON.parse(result.stdout);
  assert.equal(plan.content_hash, GOLDEN_CONTENT_HASH_V3);
  assert.equal(validatePlanV3(plan).contentHash, GOLDEN_CONTENT_HASH_V3);
});
