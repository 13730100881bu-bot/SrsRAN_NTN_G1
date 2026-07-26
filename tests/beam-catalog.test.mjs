import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import test from "node:test";
import { geoContains } from "d3-geo";
import {
  axisForL1Cell,
  GLOBAL_CATALOG_URL,
  globalL2CatalogIds,
  l1Catalog,
  l1CellForAxis,
  l2CatalogIds,
  loadGlobalL1Catalog,
  NATIONAL_L1_TARGET,
} from "../app/beam-catalog.ts";
import { landFeatureFromTopology } from "../app/land-topology.ts";

const require = createRequire(import.meta.url);
const globalCatalogPath = new URL("../public/data/global-land-l1-v1.json", import.meta.url);
const globalCatalog = JSON.parse(readFileSync(globalCatalogPath, "utf8"));
const popcount7 = (mask) => {
  let count = 0;
  for (let index = 0; index < 7; index += 1) count += (mask >>> index) & 1;
  return count;
};

test("mainland and Hainan catalog has stable simple IDs and clipped edge groups", () => {
  assert.equal(l1Catalog.length, NATIONAL_L1_TARGET);
  assert.equal(new Set(l1Catalog.map((cell) => cell.id)).size, NATIONAL_L1_TARGET);
  assert.equal(l1Catalog[0].id, "A0001");
  assert.equal(l1Catalog.at(-1).id, "A2620");

  const full = l1Catalog.filter((cell) => cell.kind === "full");
  const edge = l1Catalog.filter((cell) => cell.kind === "edge");
  const digitalCount = l1Catalog.reduce((sum, cell) => sum + cell.childCount, 0);
  assert.equal(full.length, 2552);
  assert.equal(edge.length, 68);
  assert.equal(digitalCount, 18208);
  assert.equal(Math.min(...edge.map((cell) => cell.childCount)), 4);
  assert.equal(Math.max(...edge.map((cell) => cell.childCount)), 6);
});

test("catalog lookup keeps human IDs separate from machine axes", () => {
  const original = l1Catalog[1419];
  const axis = axisForL1Cell(original);
  const roundTrip = l1CellForAxis(axis.u, axis.v);
  assert.equal(roundTrip.id, original.id);
  assert.deepEqual(l2CatalogIds(original), Array.from({ length: original.childCount }, (_, index) => `${original.id}-${index}`));
});

test("global land catalog records its local Natural Earth source and honest generation limits", () => {
  const { metadata } = globalCatalog;
  assert.equal(metadata.version, "global-land-l1-v1");
  assert.deepEqual(metadata.scope.latitudeDeg, [-57, 57]);
  assert.deepEqual(metadata.scope.longitudeDeg, [-180, 180]);
  assert.equal(metadata.source.dataset, "Natural Earth land");
  assert.equal(metadata.source.naturalEarthVersion, "4.1.0");
  assert.equal(metadata.source.scale, "1:50m");
  assert.equal(metadata.source.package, "world-atlas");
  assert.equal(metadata.source.packageVersion, "2.0.2");
  assert.equal(metadata.source.publicPath, "/data/land-50m.json");
  assert.equal(metadata.generation.l2NominalRadiusKm, 15);
  assert.equal(metadata.generation.l1NominalSpacingKm, 60);
  assert.equal(Math.round(metadata.generation.targetCellAreaKm2 / 10) * 10, 3120);
  assert.equal(metadata.generation.exactCoastlineClipping, false);
  assert.equal(metadata.generation.equalAreaGrid, true);
  assert.equal(metadata.generation.exactRegularSphericalHexagons, false);
  assert.equal(metadata.generation.quasiHexagonalCenters, true);
  assert.ok(metadata.generation.latitudeBandCount > 200);
  assert.ok(metadata.generation.maximumRelativeAreaError < 0.0012);
  assert.ok(metadata.generation.minimumCellAreaKm2 > 3100);
  assert.ok(metadata.generation.maximumCellAreaKm2 < 3130);

  const sourceBytes = readFileSync(require.resolve("world-atlas/land-50m.json"));
  assert.deepEqual(readFileSync(new URL("../public/data/land-50m.json", import.meta.url)), sourceBytes);
  assert.equal(createHash("sha256").update(sourceBytes).digest("hex"), metadata.source.sha256);
  assert.equal(
    createHash("sha256").update(JSON.stringify(globalCatalog.cells)).digest("hex"),
    metadata.integrity.sha256,
  );
});

test("global L1 and derived L2 IDs are stable, ordered and consistent with childMask", () => {
  const { cells, metadata } = globalCatalog;
  assert.equal(cells.length, metadata.counts.l1);
  assert.ok(cells.length > NATIONAL_L1_TARGET);
  assert.equal(cells[0].id, "G000001");
  assert.equal(cells.at(-1).id, `G${String(cells.length).padStart(6, "0")}`);
  assert.equal(new Set(cells.map((cell) => cell.id)).size, cells.length);

  let l2Count = 0;
  let fullCount = 0;
  for (let index = 0; index < cells.length; index += 1) {
    const cell = cells[index];
    assert.equal(cell.id, `G${String(index + 1).padStart(6, "0")}`);
    assert.ok(cell.lat >= -57 && cell.lat <= 57);
    assert.ok(cell.lon >= -180 && cell.lon < 180);
    assert.ok(Number.isInteger(cell.childMask) && cell.childMask >= 1 && cell.childMask <= 0x7f);
    assert.equal(cell.childMask & 1, 1);
    if (index > 0) {
      const previous = cells[index - 1];
      assert.ok(previous.lat > cell.lat || (previous.lat === cell.lat && previous.lon < cell.lon));
    }
    l2Count += popcount7(cell.childMask);
    if (cell.childMask === 0x7f) fullCount += 1;
  }
  assert.equal(l2Count, metadata.counts.l2);
  assert.equal(fullCount, metadata.counts.fullL1);
  assert.equal(cells.length - fullCount, metadata.counts.edgeL1);

  const edge = cells.find((cell) => cell.childMask !== 0x7f);
  const childIds = globalL2CatalogIds(edge);
  assert.equal(childIds.length, popcount7(edge.childMask));
  assert.ok(childIds.every((id) => /^G\d{6}-[0-6]$/.test(id)));
  assert.ok(childIds.includes(`${edge.id}-0`));
});

test("sampled global L1 centers are actually inside the packaged 1:50m land geometry", () => {
  const topology = JSON.parse(readFileSync(require.resolve("world-atlas/land-50m.json"), "utf8"));
  const land = landFeatureFromTopology(topology);
  assert.equal(land.type, "FeatureCollection");
  assert.throws(
    () => landFeatureFromTopology({ type: "Topology", objects: {} }),
    /does not contain an object/,
  );
  const sampleStride = Math.max(1, Math.floor(globalCatalog.cells.length / 64));
  const sampled = globalCatalog.cells.filter((_, index) => index % sampleStride === 0).slice(0, 64);
  assert.equal(sampled.length, 64);
  sampled.forEach((cell) => assert.equal(geoContains(land, [cell.lon, cell.lat]), true, cell.id));
});

test("runtime loader uses the compact local asset without expanding every L2 ID", async () => {
  let requestedUrl = "";
  const loaded = await loadGlobalL1Catalog(undefined, async (url) => {
    requestedUrl = url;
    return { ok: true, status: 200, json: async () => globalCatalog };
  });
  assert.equal(requestedUrl, GLOBAL_CATALOG_URL);
  assert.equal(loaded.metadata.counts.l1, loaded.cells.length);
  assert.equal(Object.hasOwn(loaded.cells[0], "childIds"), false);

  await assert.rejects(
    () => loadGlobalL1Catalog("/missing.json", async () => ({ ok: false, status: 404 })),
    /request failed: 404/,
  );
});
