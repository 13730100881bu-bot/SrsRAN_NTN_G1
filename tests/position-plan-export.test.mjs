import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import {
  POSITION_PLAN_ARTIFACT_KIND,
  buildVersionedPositionPlan,
  canonicalPositionPlanPayload,
  formatCppMaxDigits10,
} from "../app/position-plan-model.ts";
import { baselineSatelliteCellPlanningContext } from "../app/satellite-cell-model.ts";
import { exportCandidatePlan, parseArguments } from "../scripts/export-onboard-position-plan.mjs";

const GOLDEN_CANONICAL = [
  "satellite_id=P01-S001",
  "catalog_version=10",
  "schedule_version=20",
  "valid_from_unix_ms=1780000000000",
  "valid_until_unix_ms=1780003600000",
  "activation_epoch_unix_ms=1780000000640",
  "cell=4886691841,101",
  "cell=4886691842,202",
  "l1=G000001,10,20",
  "l1=G000002,56.7654,-158.36066",
  "",
].join("\n");
const GOLDEN_HASH = "sha256:9fc109aa6a2ec0029667603257afaf05f2dcac46e8fbb228121b5fa1bd27c720";

function candidate(overrides = {}) {
  return {
    artifact_kind: POSITION_PLAN_ARTIFACT_KIND,
    satellite_id: "P01-S001",
    catalog_version: 10,
    schedule_version: 20,
    valid_from_unix_ms: 1_780_000_000_000,
    valid_until_unix_ms: 1_780_003_600_000,
    activation_epoch_unix_ms: 1_780_000_000_640,
    onboard_cells: [
      { nci: "4886691842", pci: 202 },
      { nci: "0x123450001", pci: 101 },
    ],
    visible_l1_positions: [
      { position_id: "G000002", latitude_deg: 56.7654, longitude_deg: -158.36066 },
      { position_id: "G000001", latitude_deg: 10, longitude_deg: 20 },
    ],
    ...overrides,
  };
}

test("candidate exporter matches the C++ canonical SHA-256 golden vector", async () => {
  const plan = await buildVersionedPositionPlan(candidate());
  assert.equal(canonicalPositionPlanPayload(plan), GOLDEN_CANONICAL);
  assert.equal(plan.content_hash, GOLDEN_HASH);
  assert.deepEqual(plan.onboard_cells, [
    { nci: "0x123450001", pci: 101 },
    { nci: "0x123450002", pci: 202 },
  ]);
  assert.deepEqual(plan.visible_l1_positions.map(({ position_id }) => position_id), ["G000001", "G000002"]);
  assert.equal("artifact_kind" in plan, false);
  assert.equal("access_calendar" in plan, false);
});

test("C++ max_digits10 formatter covers fixed, rounded, exponent and signed-zero forms", () => {
  assert.deepEqual(
    [56.7654, -158.36066, 0.22225, 10, 20, 0.1, 1e-5, 1e-4, -0].map(formatCppMaxDigits10),
    ["56.7654", "-158.36066", "0.22225", "10", "20", "0.10000000000000001", "1.0000000000000001e-05", "0.0001", "-0"],
  );
});

test("negative-zero coordinates are normalized before hashing and JSON serialization", async () => {
  const plan = await buildVersionedPositionPlan(candidate({
    visible_l1_positions: [{ position_id: "G000001", latitude_deg: -0, longitude_deg: -0 }],
  }));
  assert.equal(Object.is(plan.visible_l1_positions[0].latitude_deg, -0), false);
  assert.equal(Object.is(plan.visible_l1_positions[0].longitude_deg, -0), false);
  assert.deepEqual(JSON.parse(JSON.stringify(plan)).visible_l1_positions[0], {
    position_id: "G000001", latitude_deg: 0, longitude_deg: 0,
  });
});

test("candidate inventory is sorted but never clipped at the 256-L1 calendar capacity", async () => {
  const visible = Array.from({ length: 257 }, (_, index) => ({
    position_id: `G${String(257 - index).padStart(6, "0")}`,
    latitude_deg: (index % 90) - 45,
    longitude_deg: (index % 180) - 90,
  }));
  const plan = await buildVersionedPositionPlan(candidate({ visible_l1_positions: visible }));
  assert.equal(plan.visible_l1_positions.length, 257);
  assert.equal(plan.visible_l1_positions[0].position_id, "G000001");
  assert.equal(plan.visible_l1_positions.at(-1).position_id, "G000257");
});

test("registry identities are looked up and serialized rather than reconstructed by the exporter", async () => {
  const identities = baselineSatelliteCellPlanningContext.identitiesBySatellite.get("P01-S01");
  assert.ok(identities);
  const plan = await buildVersionedPositionPlan(candidate({
    satellite_id: "P01-S01",
    onboard_cells: [...identities].reverse().map(({ nci, pci }) => ({ nci, pci })),
  }));
  assert.deepEqual(
    plan.onboard_cells.map(({ nci, pci }) => ({ nci, pci })),
    [...identities].sort((left, right) => BigInt(left.nci) < BigInt(right.nci) ? -1 : 1)
      .map(({ nci, pci }) => ({ nci, pci })),
  );

  const samePciPlan = await buildVersionedPositionPlan(candidate({
    onboard_cells: [
      { nci: identities[0].nci, pci: 77 },
      { nci: identities[1].nci, pci: 77 },
    ],
  }));
  assert.deepEqual(samePciPlan.onboard_cells.map(({ pci }) => pci), [77, 77]);
});

test("input order does not affect the emitted document or hash", async () => {
  const first = await buildVersionedPositionPlan(candidate());
  const second = await buildVersionedPositionPlan(candidate({
    onboard_cells: [...candidate().onboard_cells].reverse(),
    visible_l1_positions: [...candidate().visible_l1_positions].reverse(),
  }));
  assert.deepEqual(second, first);
});

test("strict validation rejects malformed identity, version, timing and L1 inventory inputs", async () => {
  const invalidCases = [
    [candidate({ artifact_kind: "operational" }), /artifact_kind/],
    [{ ...candidate(), unexpected: true }, /unknown field unexpected/],
    [candidate({ catalog_version: 0 }), /catalog_version/],
    [candidate({ schedule_version: Number.MAX_SAFE_INTEGER + 1 }), /schedule_version/],
    [candidate({ valid_until_unix_ms: candidate().valid_from_unix_ms }), /validity window/],
    [candidate({ activation_epoch_unix_ms: candidate().valid_from_unix_ms - 640 }), /validity window/],
    [candidate({ activation_epoch_unix_ms: candidate().activation_epoch_unix_ms + 1 }), /aligned to 640/],
    [candidate({ onboard_cells: [{ nci: 1, pci: 1 }] }), /exactly two identities/],
    [candidate({ onboard_cells: [{ nci: 1, pci: 1 }, { nci: 1, pci: 2 }] }), /distinct NCIs/],
    [candidate({ onboard_cells: [{ nci: 1, pci: 1008 }, { nci: 2, pci: 2 }] }), /0\.\.1007/],
    [candidate({ onboard_cells: [{ nci: "0x1000000000", pci: 1 }, { nci: 2, pci: 2 }] }), /36-bit/],
    [candidate({ visible_l1_positions: [
      { position_id: "G000001", latitude_deg: 1, longitude_deg: 2 },
      { position_id: "G000001", latitude_deg: 3, longitude_deg: 4 },
    ] }), /duplicate G000001/],
    [candidate({ visible_l1_positions: [{ position_id: "A0001", latitude_deg: 1, longitude_deg: 2 }] }), /G######/],
    [candidate({ visible_l1_positions: [{ position_id: "G000001", latitude_deg: 91, longitude_deg: 2 }] }), /-90\.\.90/],
    [candidate({ visible_l1_positions: [{ position_id: "G000001", latitude_deg: 1, longitude_deg: 181 }] }), /-180\.\.180/],
  ];
  for (const [input, expected] of invalidCases) {
    await assert.rejects(() => buildVersionedPositionPlan(input), expected);
  }
});

test("CLI parser and file exporter emit the exact CU-CP document", async () => {
  assert.deepEqual(parseArguments(["--input", "candidate.json", "--output", "plan.json"]), {
    input: "candidate.json", output: "plan.json",
  });
  assert.throws(() => parseArguments(["--output", "plan.json"]), /--input is required/);
  assert.throws(() => parseArguments(["--calendar", "calendar.json"]), /unknown argument --calendar/);

  const directory = await mkdtemp(join(tmpdir(), "ntn-position-plan-"));
  try {
    const inputPath = join(directory, "candidate.json");
    const outputPath = join(directory, "plan.json");
    await writeFile(inputPath, JSON.stringify(candidate()), "utf8");
    const result = await exportCandidatePlan({ input: inputPath, output: outputPath });
    assert.equal(result.plan.content_hash, GOLDEN_HASH);
    assert.deepEqual(JSON.parse(await readFile(outputPath, "utf8")), result.plan);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
