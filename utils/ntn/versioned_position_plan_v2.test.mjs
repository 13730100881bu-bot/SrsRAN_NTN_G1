import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

import {
  ACCESS_PROFILE_V1_HASH,
  GOLDEN_CONTENT_HASH,
  PlanValidationError,
  canonicalPayloadV2,
  computeContentHashV2,
  formatCppDouble,
  generatePlanV2,
  makeGoldenPlanV2,
  validatePlanV2
} from './versioned_position_plan_v2.mjs';

function clone(value) {
  return structuredClone(value);
}

test('schema-v2 golden plan matches the C++ canonical hash', () => {
  assert.equal(ACCESS_PROFILE_V1_HASH,
    'sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63');
  const plan = makeGoldenPlanV2();
  assert.equal(plan.content_hash, GOLDEN_CONTENT_HASH);
  assert.equal(validatePlanV2(plan).contentHash, GOLDEN_CONTENT_HASH);
  assert.match(canonicalPayloadV2(plan), /l1=G000002,10,20\.050000000000001,127\n$/);
});

test('canonical hash is independent of cell and L1 input order', () => {
  const plan = makeGoldenPlanV2();
  const reordered = clone(plan);
  reordered.onboard_cells.reverse();
  reordered.visible_l1_positions.reverse();
  reordered.catalog.sha256 = reordered.catalog.sha256.slice('sha256:'.length).toUpperCase();
  reordered.content_hash = GOLDEN_CONTENT_HASH.toUpperCase();

  assert.equal(computeContentHashV2(reordered), GOLDEN_CONTENT_HASH);
  assert.equal(validatePlanV2(reordered).contentHash, GOLDEN_CONTENT_HASH);
});

test('generator accepts a draft without content_hash and emits an exact plan', () => {
  const draft = clone(makeGoldenPlanV2());
  delete draft.content_hash;
  draft.visible_l1_positions.reverse();

  const generated = generatePlanV2(draft);
  assert.equal(generated.content_hash, GOLDEN_CONTENT_HASH);
  assert.deepEqual(generated.visible_l1_positions.map((position) => position.position_id), ['G000001', 'G000002']);
  assert.equal(validatePlanV2(generated).contentHash, GOLDEN_CONTENT_HASH);
});

test('schema is exact at root, planning context, cell and position levels', () => {
  const mutations = [
    (plan) => { plan.unexpected = true; },
    (plan) => { plan.catalog.unexpected = true; },
    (plan) => { plan.onboard_cells[0].unexpected = true; },
    (plan) => { plan.visible_l1_positions[0].unexpected = true; },
    (plan) => { delete plan.visible_l1_positions[0].child_mask; }
  ];

  for (const mutate of mutations) {
    const plan = clone(makeGoldenPlanV2());
    mutate(plan);
    assert.throws(() => generatePlanV2(plan), PlanValidationError);
  }
});

test('canonical satellite ID, child mask and opaque cell identities fail closed', () => {
  for (const satelliteId of ['P01-S001', 'p01-S01', 'P1-S01', ' P01-S01']) {
    const plan = clone(makeGoldenPlanV2());
    plan.satellite_id = satelliteId;
    assert.throws(() => generatePlanV2(plan), /Pxx-Syy/);
  }

  for (const childMask of [0, 128, -1, 1.5]) {
    const plan = clone(makeGoldenPlanV2());
    plan.visible_l1_positions[0].child_mask = childMask;
    assert.throws(() => generatePlanV2(plan), /child_mask/);
  }

  const duplicateNci = clone(makeGoldenPlanV2());
  duplicateNci.onboard_cells[1].nci = duplicateNci.onboard_cells[0].nci;
  assert.throws(() => generatePlanV2(duplicateNci), /distinct opaque NCIs/);

  const pciReuse = clone(makeGoldenPlanV2());
  pciReuse.onboard_cells[1].pci = pciReuse.onboard_cells[0].pci;
  assert.doesNotThrow(() => generatePlanV2(pciReuse));
});

test('double formatting follows C++ defaultfloat max_digits10 behavior', () => {
  assert.equal(formatCppDouble(10), '10');
  assert.equal(formatCppDouble(20.05), '20.050000000000001');
  assert.equal(formatCppDouble(0.0001), '0.0001');
  assert.equal(formatCppDouble(0.00001), '1.0000000000000001e-05');
  assert.equal(formatCppDouble(1e17), '1e+17');
  assert.equal(formatCppDouble(-0), '-0');
});

test('headless golden CLI is deterministic and dependency-free', () => {
  const script = fileURLToPath(new URL('./versioned_position_plan_v2.mjs', import.meta.url));
  const result = spawnSync(process.execPath, [script, 'golden', '-'], {encoding: 'utf8'});
  assert.equal(result.status, 0, result.stderr);
  const plan = JSON.parse(result.stdout);
  assert.equal(plan.content_hash, GOLDEN_CONTENT_HASH);
  assert.equal(validatePlanV2(plan).contentHash, GOLDEN_CONTENT_HASH);
});
