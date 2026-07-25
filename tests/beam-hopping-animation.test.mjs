import assert from "node:assert/strict";
import test from "node:test";
import {
  BEAM_ANIMATION_CYCLE_MS,
  BEAM_ANIMATION_FRAME_MS,
  createBeamAnimationFrames,
} from "../app/beam-hopping-animation-model.ts";

function makeCells(perBank) {
  return [0, 1].flatMap((cellBank) =>
    Array.from({ length: perBank }, (_, index) => ({
      id: `G${String(cellBank * perBank + index + 1).padStart(6, "0")}`,
      lat: 20 + index / 100,
      lon: 100 + cellBank * 4 + index / 100,
      cellBank,
    })),
  );
}

test("animation turns the existing 80 ms access schedule into 32 readable frames", () => {
  const frames = createBeamAnimationFrames(makeCells(16), "P02-S04", ["000000001", "000000002"]);

  assert.equal(BEAM_ANIMATION_CYCLE_MS, 80);
  assert.equal(BEAM_ANIMATION_FRAME_MS, 2.5);
  assert.equal(frames.length, 32);
  assert.deepEqual(
    frames.map(({ startMs, endMs }) => [startMs, endMs]),
    Array.from({ length: 32 }, (_, index) => [index * 2.5, (index + 1) * 2.5]),
  );
  for (const frame of frames) {
    assert.equal(frame.cellBank, Math.floor(frame.startMs / 10) % 2);
    assert.ok(frame.beamCount >= 10 && frame.beamCount <= 11);
    assert.ok(frame.activePositionIds.length > 0);
  }
});

test("all 256 assigned positions appear and no position outside the assignment is invented", () => {
  const cells = makeCells(128);
  const frames = createBeamAnimationFrames(cells, "P02-S04", ["000000001", "000000002"]);
  const assigned = new Set(cells.map(({ id }) => id));
  const animated = new Set(frames.flatMap(({ activePositionIds }) => activePositionIds));

  assert.equal(animated.size, 256);
  assert.deepEqual([...animated].sort(), [...assigned].sort());
});

test("animation order is deterministic and empty assignments remain visibly empty", () => {
  const cells = makeCells(24);
  const forward = createBeamAnimationFrames(cells, "P02-S04", ["000000001", "000000002"]);
  const reversed = createBeamAnimationFrames([...cells].reverse(), "P02-S04", ["000000001", "000000002"]);
  const empty = createBeamAnimationFrames([], "P02-S04", ["000000001", "000000002"]);

  assert.deepEqual(reversed, forward);
  assert.equal(empty.length, 32);
  assert.ok(empty.every(({ cellBank, beamCount, activePositionIds }) =>
    cellBank === null && beamCount === 0 && activePositionIds.length === 0,
  ));
});

test("animation does not hide a per-cell capacity overflow", () => {
  const cells = makeCells(129);
  assert.throws(
    () => createBeamAnimationFrames(cells, "P02-S04", ["000000001", "000000002"]),
    /between 1 and 128 L1 positions/,
  );
});
