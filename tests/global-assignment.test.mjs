import assert from "node:assert/strict";
import test from "node:test";

import { matchL1CandidatesToCapacity } from "../app/capacity-matching.ts";

const candidate = (satelliteId, elevationDeg = 50) => ({ satelliteId, elevationDeg });

test("one L1 gets exactly one owner even when several satellites can see it", () => {
  const assignments = matchL1CandidatesToCapacity([
    { positionId: "G000001", candidates: [candidate("P01-S01", 60), candidate("P02-S01", 55)] },
  ], 256);

  assert.deepEqual([...assignments], [["G000001", "P01-S01"]]);
});

test("the matcher rearranges flexible positions instead of silently dropping a constrained one", () => {
  const assignments = matchL1CandidatesToCapacity([
    { positionId: "G000001", candidates: [candidate("P01-S01", 60), candidate("P02-S01", 50)] },
    { positionId: "G000002", candidates: [candidate("P01-S01", 58)] },
  ], 1);

  assert.deepEqual([...assignments].sort(), [
    ["G000001", "P02-S01"],
    ["G000002", "P01-S01"],
  ]);
});

test("257 positions remain visible but one is explicitly unassigned at a 256-position capacity", () => {
  const visibleCandidateSets = Array.from({ length: 257 }, (_, index) => ({
    positionId: `G${String(index + 1).padStart(6, "0")}`,
    candidates: [candidate("P01-S01", 60 - index / 1000)],
  }));
  const assignments = matchL1CandidatesToCapacity(visibleCandidateSets, 256);

  assert.equal(visibleCandidateSets.length, 257, "the complete visible inventory is retained");
  assert.equal(assignments.size, 256);
  assert.equal(visibleCandidateSets.length - assignments.size, 1);
});

test("valid previous owners are sticky and repeated runs are deterministic", () => {
  const candidateSets = [
    { positionId: "G000001", candidates: [candidate("P01-S01", 60), candidate("P02-S01", 59)] },
    { positionId: "G000002", candidates: [candidate("P01-S01", 58), candidate("P02-S01", 57)] },
  ];
  const previous = new Map([
    ["G000001", "P02-S01"],
    ["G000002", "P01-S01"],
  ]);
  const first = matchL1CandidatesToCapacity(candidateSets, 1, previous);
  const second = matchL1CandidatesToCapacity([...candidateSets].reverse(), 1, previous);

  assert.deepEqual([...first].sort(), [...previous].sort());
  assert.deepEqual([...second].sort(), [...previous].sort());
});
