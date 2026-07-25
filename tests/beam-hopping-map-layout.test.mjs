import assert from "node:assert/strict";
import test from "node:test";
import {
  createBeamAnimationMapViewport,
  isInsideBeamAnimationViewport,
} from "../app/beam-hopping-map-layout.ts";

function cell(id, lat, lon, cellBank = 0) {
  return { id, lat, lon, cellBank };
}

test("animation viewport follows assigned positions instead of the satellite subpoint", () => {
  const cells = [
    cell("G000001", 31, 103),
    cell("G000002", 32, 104, 1),
    cell("G000003", 30, 105),
  ];
  const viewport = createBeamAnimationMapViewport(cells, { lat: -20, lon: -70 });

  assert.ok(viewport.centerLat > 29 && viewport.centerLat < 33);
  assert.ok(viewport.centerLon > 102 && viewport.centerLon < 106);
  assert.ok(cells.every((entry) => isInsideBeamAnimationViewport(viewport, entry)));
});

test("animation viewport keeps assignments together across the date line", () => {
  const cells = [
    cell("G000001", 10, 179.4),
    cell("G000002", 10.5, -179.6, 1),
    cell("G000003", 9.7, 178.9),
  ];
  const viewport = createBeamAnimationMapViewport(cells, { lat: 0, lon: 0 });

  assert.ok(Math.abs(viewport.centerLon) > 175);
  assert.ok(cells.every((entry) => isInsideBeamAnimationViewport(viewport, entry)));
});

test("empty and single-position assignments still receive a readable local view", () => {
  const empty = createBeamAnimationMapViewport([], { lat: 23, lon: 121 });
  const singleCell = cell("G000001", 40, 116);
  const single = createBeamAnimationMapViewport([singleCell], { lat: -20, lon: -70 });

  assert.deepEqual(empty, { centerLat: 23, centerLon: 121, radiusDeg: 4.5 });
  assert.equal(single.radiusDeg, 4.5);
  assert.ok(isInsideBeamAnimationViewport(single, singleCell));
});

test("animation viewport is deterministic regardless of input order", () => {
  const cells = [
    cell("G000001", 18, 104),
    cell("G000002", 19, 105, 1),
    cell("G000003", 20, 106),
  ];
  const forward = createBeamAnimationMapViewport(cells, { lat: 0, lon: 0 });
  const reversed = createBeamAnimationMapViewport([...cells].reverse(), { lat: 0, lon: 0 });

  assert.deepEqual(reversed, forward);
});

test("full 256-position assignment remains a local view with every position inside", () => {
  const cells = Array.from({ length: 256 }, (_, index) => {
    const row = Math.floor(index / 16);
    const column = index % 16;
    return cell(
      `G${String(index + 1).padStart(6, "0")}`,
      26 + row * 0.42,
      104 + column * 0.46,
      index % 2,
    );
  });
  const viewport = createBeamAnimationMapViewport(cells, { lat: -35, lon: -120 });

  assert.ok(viewport.radiusDeg < 8);
  assert.ok(cells.every((entry) => isInsideBeamAnimationViewport(viewport, entry)));
});
