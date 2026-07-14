#!/usr/bin/env node

import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const EARTH_RADIUS_KM = 6378.137;
const EARTH_MU_KM3_S2 = 398600.4418;
const EARTH_ROTATION_RAD_S = 7.2921159e-5;
const ALTITUDE_KM = 500;
const MINIMUM_ELEVATION_DEG = 45;
const L1_CAPACITY_PER_SATELLITE = 256;
const DEFAULT_SCENARIO = Object.freeze({
  inclinationDeg: 60,
  planes: 42,
  satellitesPerPlane: 84,
  phaseFactor: 0,
  raanOffsetDeg: 0,
  phaseOffsetDeg: 0,
});
const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const defaultCatalogPath = resolve(scriptDirectory, "../public/data/global-land-l1-v1.json");

const radians = (value) => value * Math.PI / 180;
const degrees = (value) => value * 180 / Math.PI;
const normalizeLongitude = (value) => ((value + 540) % 360) - 180;

function finiteNumber(value, label) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) throw new Error(`${label} must be a finite number`);
  return parsed;
}

function positiveInteger(value, label) {
  const parsed = finiteNumber(value, label);
  if (!Number.isInteger(parsed) || parsed <= 0) throw new Error(`${label} must be a positive integer`);
  return parsed;
}

export function parseScenario(value = "60,42,84,0,0,0") {
  const fields = value.split(",").map((item) => item.trim());
  if (fields.length !== 6) {
    throw new Error("--scenario requires inclination,planes,slots,F,RAAN,phase");
  }
  const scenario = {
    inclinationDeg: finiteNumber(fields[0], "inclination"),
    planes: positiveInteger(fields[1], "planes"),
    satellitesPerPlane: positiveInteger(fields[2], "slots"),
    phaseFactor: finiteNumber(fields[3], "F"),
    raanOffsetDeg: finiteNumber(fields[4], "RAAN"),
    phaseOffsetDeg: finiteNumber(fields[5], "phase"),
  };
  if (scenario.inclinationDeg < 0 || scenario.inclinationDeg > 180) {
    throw new Error("inclination must be in the range 0..180 degrees");
  }
  if (!Number.isInteger(scenario.phaseFactor) || scenario.phaseFactor < 0 || scenario.phaseFactor >= scenario.planes) {
    throw new Error("F must be an integer in the range 0..planes-1");
  }
  return scenario;
}

export function parseArguments(argv) {
  const options = {
    scenario: { ...DEFAULT_SCENARIO },
    days: 1,
    stepSeconds: 120,
    snapshot: false,
    output: null,
    catalogPath: defaultCatalogPath,
  };
  const nextValue = (index, option) => {
    const value = argv[index + 1];
    if (value === undefined || value.startsWith("--")) throw new Error(`${option} requires a value`);
    return value;
  };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--snapshot") options.snapshot = true;
    else if (argument === "--scenario") options.scenario = parseScenario(nextValue(index++, argument));
    else if (argument === "--days") options.days = finiteNumber(nextValue(index++, argument), "days");
    else if (argument === "--step-seconds") options.stepSeconds = positiveInteger(nextValue(index++, argument), "step-seconds");
    else if (argument === "--output") options.output = resolve(nextValue(index++, argument));
    else if (argument === "--catalog") options.catalogPath = resolve(nextValue(index++, argument));
    else if (argument === "--help" || argument === "-h") options.help = true;
    else throw new Error(`Unknown argument ${argument}`);
  }
  if (!(options.days > 0)) throw new Error("days must be positive");
  return options;
}

function coverageCentralAngleRad(elevationDeg = MINIMUM_ELEVATION_DEG) {
  const elevation = radians(elevationDeg);
  const orbitRadiusKm = EARTH_RADIUS_KM + ALTITUDE_KM;
  return Math.acos(EARTH_RADIUS_KM / orbitRadiusKm * Math.cos(elevation)) - elevation;
}

function createSatelliteDefinitions(scenario) {
  const definitions = [];
  for (let plane = 0; plane < scenario.planes; plane += 1) {
    for (let slot = 0; slot < scenario.satellitesPerPlane; slot += 1) {
      definitions.push({
        raanRad: radians(scenario.raanOffsetDeg + plane * 360 / scenario.planes),
        phaseRad: radians(
          scenario.phaseOffsetDeg
          + slot * 360 / scenario.satellitesPerPlane
          + plane * scenario.phaseFactor * 360 / (scenario.planes * scenario.satellitesPerPlane),
        ),
      });
    }
  }
  return definitions;
}

function satelliteUnit(definition, seconds, scenario) {
  const inclination = radians(scenario.inclinationDeg);
  const orbitRadiusKm = EARTH_RADIUS_KM + ALTITUDE_KM;
  const meanMotion = Math.sqrt(EARTH_MU_KM3_S2 / orbitRadiusKm ** 3);
  const argument = definition.phaseRad + seconds * meanMotion;
  const cosRaan = Math.cos(definition.raanRad);
  const sinRaan = Math.sin(definition.raanRad);
  const cosArgument = Math.cos(argument);
  const sinArgument = Math.sin(argument);
  const cosInclination = Math.cos(inclination);
  const sinInclination = Math.sin(inclination);
  const xEci = cosRaan * cosArgument - sinRaan * sinArgument * cosInclination;
  const yEci = sinRaan * cosArgument + cosRaan * sinArgument * cosInclination;
  const z = sinArgument * sinInclination;
  const earthAngle = EARTH_ROTATION_RAD_S * seconds;
  const x = Math.cos(earthAngle) * xEci + Math.sin(earthAngle) * yEci;
  const y = -Math.sin(earthAngle) * xEci + Math.cos(earthAngle) * yEci;
  return { x, y, z, lat: degrees(Math.asin(z)), lon: normalizeLongitude(degrees(Math.atan2(y, x))) };
}

export async function loadCatalog(catalogPath = defaultCatalogPath) {
  const payload = JSON.parse(await readFile(catalogPath, "utf8"));
  if (payload.metadata?.version !== "global-land-l1-v1" || !Array.isArray(payload.cells)) {
    throw new Error("global catalog is malformed or unsupported");
  }
  const latitudeBands = new Map();
  const cells = payload.cells.map((cell, index) => {
    const lat = radians(cell.lat);
    const lon = radians(cell.lon);
    const geometry = {
      index,
      id: cell.id,
      lat: cell.lat,
      x: Math.cos(lat) * Math.cos(lon),
      y: Math.cos(lat) * Math.sin(lon),
      z: Math.sin(lat),
    };
    const band = Math.floor(cell.lat);
    const members = latitudeBands.get(band) ?? [];
    members.push(geometry);
    latitudeBands.set(band, members);
    return geometry;
  });
  return { metadata: payload.metadata, cells, latitudeBands };
}

export function auditEpoch(catalog, definitions, scenario, timeSeconds) {
  const candidateCounts = new Uint16Array(catalog.cells.length);
  const visibleCounts = new Uint32Array(definitions.length);
  const centralAngle = coverageCentralAngleRad();
  const centralAngleDeg = degrees(centralAngle);
  const dotThreshold = Math.cos(centralAngle);

  definitions.forEach((definition, satelliteIndex) => {
    const satellite = satelliteUnit(definition, timeSeconds, scenario);
    const minimumBand = Math.floor(satellite.lat - centralAngleDeg);
    const maximumBand = Math.floor(satellite.lat + centralAngleDeg);
    let visible = 0;
    for (let band = minimumBand; band <= maximumBand; band += 1) {
      for (const cell of catalog.latitudeBands.get(band) ?? []) {
        if (satellite.x * cell.x + satellite.y * cell.y + satellite.z * cell.z < dotThreshold) continue;
        candidateCounts[cell.index] += 1;
        visible += 1;
      }
    }
    visibleCounts[satelliteIndex] = visible;
  });

  let uncoveredL1 = 0;
  let minimumCandidates = Number.POSITIVE_INFINITY;
  for (const count of candidateCounts) {
    if (count === 0) uncoveredL1 += 1;
    minimumCandidates = Math.min(minimumCandidates, count);
  }
  let maximumVisibleL1PerSatellite = 0;
  let satellitesOver256L1 = 0;
  for (const count of visibleCounts) {
    maximumVisibleL1PerSatellite = Math.max(maximumVisibleL1PerSatellite, count);
    if (count > L1_CAPACITY_PER_SATELLITE) satellitesOver256L1 += 1;
  }
  return {
    timeSeconds,
    uncoveredL1,
    minimumCandidates: Number.isFinite(minimumCandidates) ? minimumCandidates : 0,
    maximumVisibleL1PerSatellite,
    satellitesOver256L1,
  };
}

export async function runAudit(options) {
  const catalog = await loadCatalog(options.catalogPath);
  const definitions = createSatelliteDefinitions(options.scenario);
  const epochCount = options.snapshot ? 1 : Math.ceil(options.days * 86400 / options.stepSeconds);
  const epochs = [];
  for (let epoch = 0; epoch < epochCount; epoch += 1) {
    epochs.push(auditEpoch(catalog, definitions, options.scenario, epoch * options.stepSeconds));
  }
  const report = {
    schemaVersion: 1,
    auditLevel: "coarse",
    exact: false,
    mode: options.snapshot ? "snapshot" : "fixed_step_sampling",
    scenario: {
      altitudeKm: ALTITUDE_KM,
      ...options.scenario,
      satelliteCount: definitions.length,
      minimumElevationDeg: MINIMUM_ELEVATION_DEG,
      l1CapacityPerSatellite: L1_CAPACITY_PER_SATELLITE,
    },
    catalog: {
      version: catalog.metadata.version,
      l1Count: catalog.cells.length,
      scope: catalog.metadata.scope,
    },
    sampling: {
      requestedDays: options.days,
      stepSeconds: options.stepSeconds,
      epochCount,
    },
    epochs,
    summary: {
      maximumUncoveredL1: Math.max(...epochs.map((epoch) => epoch.uncoveredL1)),
      minimumCandidateCount: Math.min(...epochs.map((epoch) => epoch.minimumCandidates)),
      maximumVisibleL1PerSatellite: Math.max(...epochs.map((epoch) => epoch.maximumVisibleL1PerSatellite)),
      maximumSatellitesOver256L1: Math.max(...epochs.map((epoch) => epoch.satellitesOver256L1)),
    },
    limitations: [
      "This is fixed-step discrete sampling and must not be described as exact or continuous coverage.",
      "A coarse or snapshot report cannot set selectedScenario; only a separate exact_pass audit may do so.",
      "The 256-L1 comparison is a scheduling-capacity check, not RF, power, interference, or gateway validation.",
    ],
  };
  if (Object.hasOwn(report, "selectedScenario")) throw new Error("coarse audit must not write selectedScenario");
  return report;
}

function usage() {
  return [
    "Usage: node scripts/audit-global-constellation.mjs [options]",
    "  --scenario inclination,planes,slots,F,RAAN,phase",
    "  --days N --step-seconds N",
    "  --snapshot                 audit only t=0 (fast mode)",
    "  --output report.json       explicitly write JSON; otherwise stdout only",
  ].join("\n");
}

export async function main(argv = process.argv.slice(2)) {
  const options = parseArguments(argv);
  if (options.help) {
    process.stdout.write(`${usage()}\n`);
    return null;
  }
  const report = await runAudit(options);
  const serialized = `${JSON.stringify(report, null, 2)}\n`;
  if (options.output) {
    await mkdir(dirname(options.output), { recursive: true });
    await writeFile(options.output, serialized, "utf8");
    process.stderr.write(`Wrote coarse audit report to ${options.output}\n`);
  } else process.stdout.write(serialized);
  return report;
}

const isMain = process.argv[1] && pathToFileURL(resolve(process.argv[1])).href === import.meta.url;
if (isMain) {
  main().catch((error) => {
    process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    process.exitCode = 1;
  });
}
