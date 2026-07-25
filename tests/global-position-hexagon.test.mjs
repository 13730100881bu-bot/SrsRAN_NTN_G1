import assert from "node:assert/strict";
import test from "node:test";
import { geoArea } from "d3-geo";
import {
  createGlobalPositionHexagon,
  DEFAULT_GLOBAL_POSITION_SPACING_KM,
  EARTH_MEAN_RADIUS_KM,
  globalPositionHexagonRadiusKm,
  wrappedLongitudeDifference,
} from "../app/global-position-hexagon.ts";

const radians = (degrees) => degrees * Math.PI / 180;

function distanceKm([leftLon, leftLat], [rightLon, rightLat]) {
  const latitudeDelta = radians(rightLat - leftLat);
  const longitudeDelta = radians(wrappedLongitudeDifference(rightLon, leftLon));
  const value = Math.sin(latitudeDelta / 2) ** 2
    + Math.cos(radians(leftLat)) * Math.cos(radians(rightLat)) * Math.sin(longitudeDelta / 2) ** 2;
  return 2 * EARTH_MEAN_RADIUS_KM * Math.asin(Math.min(1, Math.sqrt(value)));
}

test("catalog spacing creates a closed point-up spherical hexagon", () => {
  const center = [116.4, 39.9];
  const hexagon = createGlobalPositionHexagon(center[1], center[0]);
  const ring = hexagon.coordinates[0];

  assert.equal(hexagon.type, "Polygon");
  assert.equal(ring.length, 7);
  assert.deepEqual(ring[0], ring[6]);
  assert.ok(ring[0][1] > center[1], "the first vertex should point north");

  const expectedRadius = globalPositionHexagonRadiusKm(DEFAULT_GLOBAL_POSITION_SPACING_KM);
  for (const vertex of ring.slice(0, 6)) {
    assert.ok(Math.abs(distanceKm(center, vertex) - expectedRadius) < 1e-6);
  }
});

test("spherical hexagon retains the catalog target area", () => {
  const hexagon = createGlobalPositionHexagon(0, 0);
  const areaKm2 = geoArea(hexagon) * EARTH_MEAN_RADIUS_KM ** 2;
  const targetAreaKm2 = Math.sqrt(3) / 2 * DEFAULT_GLOBAL_POSITION_SPACING_KM ** 2;

  assert.ok(Math.abs(areaKm2 - targetAreaKm2) / targetAreaKm2 < 0.001);
});

test("hexagons and selection distance wrap safely across the antimeridian", () => {
  const hexagon = createGlobalPositionHexagon(12, 179.9);
  const ring = hexagon.coordinates[0];

  assert.ok(ring.some(([longitude]) => longitude < -179));
  assert.ok(ring.some(([longitude]) => longitude > 179));
  assert.ok(ring.every(([longitude, latitude]) => (
    longitude >= -180 && longitude < 180 && Number.isFinite(latitude)
  )));
  assert.ok(Math.abs(wrappedLongitudeDifference(-179.9, 179.9)) < 0.21);
});

test("invalid catalog spacing is rejected instead of drawing misleading cells", () => {
  assert.throws(() => globalPositionHexagonRadiusKm(0), /positive finite/);
  assert.throws(() => createGlobalPositionHexagon(91, 0), /latitude/);
});
