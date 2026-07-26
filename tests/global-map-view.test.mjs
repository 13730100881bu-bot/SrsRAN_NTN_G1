import assert from "node:assert/strict";
import test from "node:test";
import {
  constrainGlobalMapView,
  createGlobalMapProjection,
  DEFAULT_GLOBAL_MAP_VIEW,
  GLOBAL_MAP_MAX_ZOOM,
  GLOBAL_MAP_MIN_ZOOM,
  globalMapViewBounds,
  zoomGlobalMapViewAt,
} from "../app/global-map-view.ts";

const closeTo = (actual, expected, tolerance = 1e-7) => {
  assert.ok(Math.abs(actual - expected) <= tolerance, `${actual} should be close to ${expected}`);
};

test("map zoom keeps the geographic point beneath the pointer", () => {
  const width = 1200;
  const height = 600;
  const pointer = [760, 340];
  const initial = { zoom: 2, centerLon: 15, centerLat: 5 };
  const anchor = createGlobalMapProjection(width, height, initial).invert(pointer);
  assert.ok(anchor);

  const zoomed = zoomGlobalMapViewAt(initial, 3, pointer, width, height);
  const projected = createGlobalMapProjection(width, height, zoomed)(anchor);
  assert.ok(projected);
  closeTo(projected[0], pointer[0]);
  closeTo(projected[1], pointer[1]);
});

test("map zoom is clamped between the full-earth view and eight times", () => {
  const width = 1000;
  const height = 500;
  const center = [width / 2, height / 2];
  const maximum = zoomGlobalMapViewAt(DEFAULT_GLOBAL_MAP_VIEW, 100, center, width, height);
  assert.equal(maximum.zoom, GLOBAL_MAP_MAX_ZOOM);

  const minimum = zoomGlobalMapViewAt(maximum, 0.01, center, width, height);
  assert.deepEqual(minimum, {
    zoom: GLOBAL_MAP_MIN_ZOOM,
    centerLon: 0,
    centerLat: 0,
  });
});

test("map center stays inside the earth after zoom and resize", () => {
  const constrained = constrainGlobalMapView(
    { zoom: 2, centerLon: 500, centerLat: -500 },
    1200,
    600,
  );
  assert.ok(Math.abs(constrained.centerLon) < 90);
  assert.ok(Math.abs(constrained.centerLat) < 45);

  for (const [width, height] of [[1200, 600], [720, 540]]) {
    const projection = createGlobalMapProjection(width, height, { zoom: 3, centerLon: 20, centerLat: 10 });
    const center = projection([20, 10]);
    assert.ok(center);
    closeTo(center[0], width / 2);
    closeTo(center[1], height / 2);
  }
});

test("full-earth bounds include catalog positions at both date-line edges", () => {
  assert.deepEqual(
    globalMapViewBounds(1200, 600, DEFAULT_GLOBAL_MAP_VIEW),
    { minLon: -180, maxLon: 180, minLat: -90, maxLat: 90 },
  );

  const closeView = globalMapViewBounds(
    1200,
    600,
    { zoom: 4, centerLon: 120, centerLat: 20 },
  );
  assert.ok(closeView.minLon > -180);
  assert.ok(closeView.maxLon <= 180);
  assert.ok(closeView.minLat > -90);
  assert.ok(closeView.maxLat < 90);
});
