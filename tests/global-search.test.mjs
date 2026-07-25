import assert from "node:assert/strict";
import test from "node:test";
import { parseGlobalSearchTarget } from "../app/global-search.ts";

test("normalizes satellite identifiers without changing their meaning", () => {
  assert.deepEqual(parseGlobalSearchTarget("P35-S34"), { kind: "satellite", id: "P35-S34" });
  assert.deepEqual(parseGlobalSearchTarget("p35 s34"), { kind: "satellite", id: "P35-S34" });
  assert.deepEqual(parseGlobalSearchTarget("P1S1"), { kind: "satellite", id: "P01-S01" });
});

test("normalizes ground-position identifiers and rejects unrelated text", () => {
  assert.deepEqual(parseGlobalSearchTarget("G21777"), { kind: "position", id: "G021777" });
  assert.deepEqual(parseGlobalSearchTarget("g 000001"), { kind: "position", id: "G000001" });
  assert.equal(parseGlobalSearchTarget("satellite 35"), null);
  assert.equal(parseGlobalSearchTarget(""), null);
});
