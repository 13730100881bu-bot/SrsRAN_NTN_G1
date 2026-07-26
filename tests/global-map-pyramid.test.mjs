import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import {
  createGlobalMapPyramid,
  GLOBAL_MAP_MAX_DETAIL_SHAPES,
  selectGlobalMapRenderPlan,
} from "../app/global-map-pyramid.ts";
import {
  DEFAULT_GLOBAL_MAP_VIEW,
  globalMapViewBounds,
} from "../app/global-map-view.ts";

function indexesAtLevel(level) {
  return level.clusters.flatMap((cluster) => cluster.cellIndexes);
}

function assertIndexesAppearExactlyOnce(indexes, expectedCount) {
  assert.equal(indexes.length, expectedCount);
  assert.deepEqual(
    [...indexes].sort((left, right) => left - right),
    Array.from({ length: expectedCount }, (_, index) => index),
  );
}

test("each pyramid level retains every catalog position exactly once", async () => {
  const payload = JSON.parse(
    await readFile(new URL("../public/data/global-land-l1-v1.json", import.meta.url), "utf8"),
  );
  assert.equal(payload.cells.length, 36_411);

  const pyramid = createGlobalMapPyramid(payload.cells);
  assertIndexesAppearExactlyOnce(indexesAtLevel(pyramid.overview), payload.cells.length);
  assertIndexesAppearExactlyOnce(indexesAtLevel(pyramid.regional), payload.cells.length);
  assert.ok(pyramid.overview.clusters.length <= 1_000);
  assert.ok(pyramid.regional.clusters.length <= 3_500);

  const fullEarth = selectGlobalMapRenderPlan(
    pyramid,
    DEFAULT_GLOBAL_MAP_VIEW.zoom,
    globalMapViewBounds(1200, 600, DEFAULT_GLOBAL_MAP_VIEW),
  );
  assert.equal(fullEarth.mode, "overview");
  assert.equal(fullEarth.representedCellCount, payload.cells.length);
  assert.ok(fullEarth.drawItemCount <= 1_000);

  for (const [width, height] of [[360, 300], [720, 540], [1200, 600], [1600, 500]]) {
    for (const [centerLon, centerLat] of [[0, 0], [-150, 0], [120, 25], [15, -35], [75, 45]]) {
      for (const zoom of [1, 1.5, 1.75, 2, 3.25, 4, 6, 8]) {
        const view = { zoom, centerLon, centerLat };
        const plan = selectGlobalMapRenderPlan(
          pyramid,
          zoom,
          globalMapViewBounds(width, height, view),
        );
        assert.ok(
          plan.drawItemCount <= GLOBAL_MAP_MAX_DETAIL_SHAPES,
          `${width}x${height} zoom=${zoom} center=${centerLon},${centerLat} drew ${plan.drawItemCount}`,
        );
      }
    }
  }
});

test("the renderer uses progressively finer levels without exceeding the detail limit", () => {
  const cells = Array.from({ length: 10_000 }, (_, index) => ({
    lat: 20 + (index % 100) * 0.01,
    lon: 100 + Math.floor(index / 100) * 0.01,
  }));
  const pyramid = createGlobalMapPyramid(cells);
  const bounds = { minLon: 99, maxLon: 102, minLat: 19, maxLat: 22 };

  assert.equal(selectGlobalMapRenderPlan(pyramid, 1, bounds).mode, "overview");
  assert.equal(selectGlobalMapRenderPlan(pyramid, 2, bounds).mode, "regional");

  const denseCloseView = selectGlobalMapRenderPlan(pyramid, 8, bounds, 0);
  assert.equal(denseCloseView.mode, "regional");
  assert.ok(denseCloseView.drawItemCount < GLOBAL_MAP_MAX_DETAIL_SHAPES);

  const sparseCloseView = selectGlobalMapRenderPlan(
    createGlobalMapPyramid(cells.slice(0, 100)),
    8,
    bounds,
    0,
  );
  assert.equal(sparseCloseView.mode, "detail");
  assert.equal(sparseCloseView.drawItemCount, 100);
  assert.ok(sparseCloseView.drawItemCount <= GLOBAL_MAP_MAX_DETAIL_SHAPES);
});

test("cluster bounds retain positions at a viewport edge", () => {
  const cells = [
    { lat: 10, lon: -106.0274 },
    { lat: 10.2, lon: -107.9 },
    { lat: 20, lon: 20 },
  ];
  const pyramid = createGlobalMapPyramid(cells);
  const plan = selectGlobalMapRenderPlan(
    pyramid,
    2,
    { minLon: -106.1, maxLon: -105.9, minLat: 9.9, maxLat: 10.1 },
  );
  assert.equal(plan.mode, "regional");
  assert.ok(plan.clusters.some((cluster) => cluster.cellIndexes.includes(0)));
});

test("detail view is clipped to the viewport and handles the date line", () => {
  const cells = [
    { lat: 5, lon: 179.9 },
    { lat: 5, lon: -179.9 },
    { lat: 5, lon: 0 },
  ];
  const pyramid = createGlobalMapPyramid(cells);

  const east = selectGlobalMapRenderPlan(
    pyramid,
    8,
    { minLon: 179, maxLon: 180, minLat: 4, maxLat: 6 },
    0,
  );
  assert.equal(east.mode, "detail");
  assert.deepEqual(east.cellIndexes, [0]);

  const west = selectGlobalMapRenderPlan(
    pyramid,
    8,
    { minLon: -180, maxLon: -179, minLat: 4, maxLat: 6 },
    0,
  );
  assert.equal(west.mode, "detail");
  assert.deepEqual(west.cellIndexes, [1]);
});
