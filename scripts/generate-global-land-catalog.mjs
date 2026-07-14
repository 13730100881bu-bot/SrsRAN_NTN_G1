import { createHash } from "node:crypto";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";
import { geoContains } from "d3-geo";
import { feature } from "topojson-client";

const require = createRequire(import.meta.url);
const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const outputPath = resolve(projectRoot, "public/data/global-land-l1-v1.json");
const publicSourcePath = resolve(projectRoot, "public/data/land-50m.json");
const sourcePath = require.resolve("world-atlas/land-50m.json");
const sourcePackagePath = resolve(dirname(sourcePath), "package.json");

const VERSION = "global-land-l1-v1";
const EARTH_RADIUS_KM = 6371.0088;
const LATITUDE_LIMIT_DEG = 57;
const L1_NOMINAL_SPACING_KM = 60;
const L2_NOMINAL_RADIUS_KM = 15;
const L2_RING_DISTANCE_KM = 30;
const TARGET_CELL_AREA_KM2 = Math.sqrt(3) / 2 * L1_NOMINAL_SPACING_KM ** 2;
const COORDINATE_PRECISION_DEG = 5;
const L2_BEARINGS_DEG = [null, 0, 60, 120, 180, 240, 300];

const radians = (degrees) => degrees * Math.PI / 180;
const degrees = (value) => value * 180 / Math.PI;
const roundCoordinate = (value) => Number(value.toFixed(COORDINATE_PRECISION_DEG));
const normalizeLongitude = (longitude) => ((longitude + 540) % 360) - 180;
const sha256 = (value) => createHash("sha256").update(value).digest("hex");
const popcount7 = (mask) => {
  let value = mask & 0x7f;
  let count = 0;
  while (value) {
    count += value & 1;
    value >>>= 1;
  }
  return count;
};

function destinationPoint(latDeg, lonDeg, bearingDeg, distanceKm) {
  const angularDistance = distanceKm / EARTH_RADIUS_KM;
  const latitude = radians(latDeg);
  const longitude = radians(lonDeg);
  const bearing = radians(bearingDeg);
  const destinationLatitude = Math.asin(
    Math.sin(latitude) * Math.cos(angularDistance)
      + Math.cos(latitude) * Math.sin(angularDistance) * Math.cos(bearing),
  );
  const destinationLongitude = longitude + Math.atan2(
    Math.sin(bearing) * Math.sin(angularDistance) * Math.cos(latitude),
    Math.cos(angularDistance) - Math.sin(latitude) * Math.sin(destinationLatitude),
  );
  return [degrees(destinationLongitude), degrees(destinationLatitude)];
}

function createLandCenterTest(land) {
  const geometries = land.type === "FeatureCollection"
    ? land.features.map((item) => item.geometry)
    : [land.type === "Feature" ? land.geometry : land];
  const polygons = geometries.flatMap((geometry) => (
    geometry.type === "Polygon" ? [geometry.coordinates] : geometry.coordinates
  ));
  const buckets = Array.from({ length: 180 }, () => []);
  for (const polygon of polygons) {
    for (const ring of polygon) {
      for (let index = 0; index < ring.length - 1; index += 1) {
        const [lon1, lat1] = ring[index];
        const [lon2, lat2] = ring[index + 1];
        if (lat1 === lat2) continue;
        const firstBucket = Math.max(0, Math.floor(Math.min(lat1, lat2) + 90));
        const lastBucket = Math.min(179, Math.floor(Math.max(lat1, lat2) + 90));
        for (let bucket = firstBucket; bucket <= lastBucket; bucket += 1) {
          buckets[bucket].push([lon1, lat1, lon2, lat2]);
        }
      }
    }
  }

  const scanlineCache = new Map();
  function scanline(lat) {
    const key = lat.toFixed(10);
    const cached = scanlineCache.get(key);
    if (cached) return cached;
    const bucket = Math.max(0, Math.min(179, Math.floor(lat + 90)));
    const intersections = [];
    for (let [lon1, lat1, lon2, lat2] of buckets[bucket]) {
      if ((lat1 > lat) === (lat2 > lat)) continue;
      if (Math.abs(lon2 - lon1) > 180) {
        if (lon1 < lon2) lon1 += 360;
        else lon2 += 360;
      }
      const longitude = lon1 + (lat - lat1) * (lon2 - lon1) / (lat2 - lat1);
      intersections.push(normalizeLongitude(longitude));
    }
    intersections.sort((left, right) => left - right);
    // One spherical query seeds parity at the antimeridian. The remaining
    // points on this latitude use the much faster Natural Earth scanline.
    const value = {
      startsOnLand: geoContains(land, [-179.999999, lat]),
      intersections,
    };
    scanlineCache.set(key, value);
    return value;
  }

  return (lon, lat) => {
    const normalizedLon = normalizeLongitude(lon);
    const { startsOnLand, intersections } = scanline(lat);
    let low = 0;
    let high = intersections.length;
    while (low < high) {
      const middle = (low + high) >>> 1;
      if (intersections[middle] <= normalizedLon) low = middle + 1;
      else high = middle;
    }
    return startsOnLand !== (low % 2 === 1);
  };
}

function createChildMask(isLandCenter, lat, lon) {
  return L2_BEARINGS_DEG.reduce((mask, bearing, index) => {
    const [childLon, childLat] = bearing === null
      ? [lon, lat]
      : destinationPoint(lat, lon, bearing, L2_RING_DISTANCE_KM);
    const inLatitudeBand = Math.abs(childLat) <= LATITUDE_LIMIT_DEG;
    return inLatitudeBand && isLandCenter(normalizeLongitude(childLon), childLat)
      ? mask | (1 << index)
      : mask;
  }, 0);
}

function createEqualAreaLatitudeBands() {
  const north = Math.sin(radians(LATITUDE_LIMIT_DEG));
  const south = -north;
  const sphereAreaFactor = 2 * Math.PI * EARTH_RADIUS_KM ** 2;
  const nominalBandHeightKm = L1_NOMINAL_SPACING_KM * Math.sqrt(3) / 2;
  const bands = [];
  let northSinLatitude = north;
  let row = 0;
  while (northSinLatitude > south + Number.EPSILON) {
    const northLatitude = Math.asin(northSinLatitude);
    const provisionalCenterLatitude = northLatitude - nominalBandHeightKm / (2 * EARTH_RADIUS_KM);
    let columns = Math.max(1, Math.round(
      2 * Math.PI * EARTH_RADIUS_KM * Math.cos(provisionalCenterLatitude) / L1_NOMINAL_SPACING_KM,
    ));
    let southSinLatitude = northSinLatitude - columns * TARGET_CELL_AREA_KM2 / sphereAreaFactor;
    if (southSinLatitude < south) {
      southSinLatitude = south;
      columns = Math.max(1, Math.round(
        sphereAreaFactor * (northSinLatitude - southSinLatitude) / TARGET_CELL_AREA_KM2,
      ));
    }
    const lat = degrees(Math.asin((northSinLatitude + southSinLatitude) / 2));
    const cellAreaKm2 = sphereAreaFactor * (northSinLatitude - southSinLatitude) / columns;
    bands.push({ row, lat, columns, cellAreaKm2 });
    northSinLatitude = southSinLatitude;
    row += 1;
  }
  return bands;
}

function generateCells(land) {
  const isLandCenter = createLandCenterTest(land);
  const latitudeBands = createEqualAreaLatitudeBands();
  const cells = [];
  for (const { row, lat, columns } of latitudeBands) {
    const longitudeStepDeg = 360 / columns;
    const rowOffsetDeg = row % 2 === 0 ? 0 : longitudeStepDeg / 2;
    const rowCells = [];
    for (let column = 0; column < columns; column += 1) {
      const lon = normalizeLongitude(-180 + rowOffsetDeg + column * longitudeStepDeg);
      if (!isLandCenter(lon, lat)) continue;
      const childMask = createChildMask(isLandCenter, lat, lon);
      // The L1 center itself is on land, so bit zero must always be present.
      if ((childMask & 1) === 0) throw new Error(`center child unexpectedly outside land at ${lat},${lon}`);
      rowCells.push({ lat: roundCoordinate(lat), lon: roundCoordinate(lon), childMask });
    }
    rowCells.sort((left, right) => left.lon - right.lon);
    cells.push(...rowCells);
  }
  return {
    cells: cells.map((cell, index) => ({
      id: `G${String(index + 1).padStart(6, "0")}`,
      ...cell,
    })),
    latitudeBands,
  };
}

const sourceBytes = await readFile(sourcePath);
const sourceTopology = JSON.parse(sourceBytes.toString("utf8"));
const sourcePackage = JSON.parse(await readFile(sourcePackagePath, "utf8"));
const land = feature(sourceTopology, sourceTopology.objects.land);
const { cells, latitudeBands } = generateCells(land);
const compactCellsJson = JSON.stringify(cells);
const l2Count = cells.reduce((sum, cell) => sum + popcount7(cell.childMask), 0);
const cellAreasKm2 = latitudeBands.map((band) => band.cellAreaKm2);
const minimumCellAreaKm2 = Math.min(...cellAreasKm2);
const maximumCellAreaKm2 = Math.max(...cellAreasKm2);
const maximumRelativeAreaError = Math.max(...cellAreasKm2.map(
  (area) => Math.abs(area - TARGET_CELL_AREA_KM2) / TARGET_CELL_AREA_KM2,
));

const catalog = {
  metadata: {
    version: VERSION,
    scope: {
      longitudeDeg: [-180, 180],
      latitudeDeg: [-LATITUDE_LIMIT_DEG, LATITUDE_LIMIT_DEG],
      surface: "land-center-samples",
    },
    source: {
      dataset: "Natural Earth land",
      naturalEarthVersion: "4.1.0",
      scale: "1:50m",
      package: "world-atlas",
      packageVersion: sourcePackage.version,
      file: "land-50m.json",
      publicPath: "/data/land-50m.json",
      packageLicense: sourcePackage.license,
      sha256: sha256(sourceBytes),
      url: "https://www.naturalearthdata.com/downloads/50m-physical-vectors/50m-land/",
    },
    generation: {
      algorithm: "equal-area latitude-band staggered quasi-hex center sampling",
      landTest: "center-point containment using Natural Earth longitude/latitude scanlines; antimeridian parity seeded by d3-geo geoContains",
      exactCoastlineClipping: false,
      equalAreaGrid: true,
      equalAreaConstruction: "each longitude sector in an equal-sin(latitude) band has analytically controlled spherical area",
      exactRegularSphericalHexagons: false,
      quasiHexagonalCenters: true,
      earthRadiusKm: EARTH_RADIUS_KM,
      l1NominalSpacingKm: L1_NOMINAL_SPACING_KM,
      targetCellAreaKm2: Number(TARGET_CELL_AREA_KM2.toFixed(6)),
      latitudeBandCount: latitudeBands.length,
      minimumCellAreaKm2: Number(minimumCellAreaKm2.toFixed(6)),
      maximumCellAreaKm2: Number(maximumCellAreaKm2.toFixed(6)),
      maximumRelativeAreaError: Number(maximumRelativeAreaError.toFixed(12)),
      l2NominalRadiusKm: L2_NOMINAL_RADIUS_KM,
      l2RingDistanceKm: L2_RING_DISTANCE_KM,
      l2BearingsDeg: L2_BEARINGS_DEG,
      coordinatePrecisionDeg: COORDINATE_PRECISION_DEG,
      ordering: "latitude descending, then longitude ascending",
      idFormat: "L1 G000001...; L2 <L1>-0...6 for set childMask bits",
      generatedBy: "scripts/generate-global-land-catalog.mjs",
    },
    counts: {
      l1: cells.length,
      l2: l2Count,
      fullL1: cells.filter((cell) => cell.childMask === 0x7f).length,
      edgeL1: cells.filter((cell) => cell.childMask !== 0x7f).length,
    },
    integrity: {
      algorithm: "SHA-256",
      scope: "UTF-8 JSON.stringify(cells)",
      sha256: sha256(compactCellsJson),
    },
  },
  cells,
};

const serializedCatalog = `${JSON.stringify(catalog)}\n`;
if (process.argv.includes("--check")) {
  const currentCatalog = await readFile(outputPath, "utf8");
  if (currentCatalog !== serializedCatalog) {
    throw new Error(`Generated catalog differs from ${outputPath}; run npm run catalog:generate`);
  }
  const currentPublicSource = await readFile(publicSourcePath);
  if (!currentPublicSource.equals(sourceBytes)) {
    throw new Error(`Packaged Natural Earth source differs from ${sourcePath}; run npm run catalog:generate`);
  }
} else {
  await mkdir(dirname(outputPath), { recursive: true });
  await writeFile(outputPath, serializedCatalog, "utf8");
  await writeFile(publicSourcePath, sourceBytes);
}
console.log(JSON.stringify({
  mode: process.argv.includes("--check") ? "check" : "write",
  outputPath,
  publicSourcePath,
  ...catalog.metadata.counts,
  bytes: Buffer.byteLength(serializedCatalog),
}, null, 2));
