#!/usr/bin/env node

import {createHash} from 'node:crypto';
import {mkdir, readFile, writeFile} from 'node:fs/promises';
import {dirname, resolve} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

import {
  centralAngleForElevation,
  createWalkerDelta,
  resolveOrbitModel,
  satelliteUnitEcef
} from './constellation_audit_core.mjs';
import {DEFAULT_CELL_CAPACITY, DEFAULT_SATELLITE_CAPACITY} from './constellation_assignment.mjs';
import {createCatalogGeometry} from './constellation_epoch.mjs';

const moduleDirectory = dirname(fileURLToPath(import.meta.url));
const defaultManifestPath = resolve(moduleDirectory, 'scenarios', 'global_constellation_screen_v1.json');
const defaultCatalogPath = resolve(moduleDirectory, 'data', 'global-land-l1-v1.json');
const RADIANS_TO_DEGREES = 180 / Math.PI;

function fail(message) {
  throw new TypeError(message);
}

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function assertExactKeys(value, context, required, optional = []) {
  if (!isPlainObject(value)) fail(`${context} must be an object`);
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) fail(`unknown field '${context}.${key}'`);
  }
  for (const key of required) {
    if (!Object.hasOwn(value, key)) fail(`missing field '${context}.${key}'`);
  }
}

function finiteNumber(value, context, minimum = -Infinity, maximum = Infinity) {
  if (!Number.isFinite(value) || value < minimum || value > maximum) {
    fail(`${context} must be a finite number in ${minimum}..${maximum}`);
  }
  return value;
}

function positiveInteger(value, context) {
  if (!Number.isSafeInteger(value) || value <= 0) fail(`${context} must be a positive safe integer`);
  return value;
}

function compareText(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function normalizeBareSha256(value, context) {
  if (typeof value !== 'string') fail(`${context} must be a SHA-256 digest`);
  const normalized = value.toLowerCase().replace(/^sha256:/, '');
  if (!/^[0-9a-f]{64}$/.test(normalized)) fail(`${context} must be a SHA-256 digest`);
  return normalized;
}

function verifyCatalogContentHash(catalog) {
  const declared = catalog?.metadata?.integrity?.sha256;
  if (declared === undefined) return null;
  if (!Array.isArray(catalog?.cells)) fail('catalog.cells must be an array');
  const normalizedDeclared = normalizeBareSha256(declared, 'catalog.metadata.integrity.sha256');
  const computed = createHash('sha256').update(JSON.stringify(catalog.cells)).digest('hex');
  if (computed !== normalizedDeclared) {
    fail('catalog content hash does not match catalog.metadata.integrity.sha256');
  }
  return computed;
}

function validateScenario(value, index) {
  const context = `manifest.scenarios[${index}]`;
  assertExactKeys(
    value,
    context,
    ['id', 'inclination_deg', 'planes', 'satellites_per_plane', 'phase_factor'],
    ['raan_offset_deg', 'phase_offset_deg']
  );
  if (typeof value.id !== 'string' || value.id.length === 0) fail(`${context}.id must be a non-empty string`);
  const planes = positiveInteger(value.planes, `${context}.planes`);
  const satellitesPerPlane = positiveInteger(value.satellites_per_plane, `${context}.satellites_per_plane`);
  const phaseFactor = value.phase_factor;
  if (!Number.isSafeInteger(phaseFactor) || phaseFactor < 0 || phaseFactor >= planes) {
    fail(`${context}.phase_factor must be a safe integer in 0..planes-1`);
  }
  return Object.freeze({
    id: value.id,
    inclinationDeg: finiteNumber(value.inclination_deg, `${context}.inclination_deg`, 0, 180),
    planes,
    satellitesPerPlane,
    phaseFactor,
    raanOffsetDeg: finiteNumber(value.raan_offset_deg ?? 0, `${context}.raan_offset_deg`),
    phaseOffsetDeg: finiteNumber(value.phase_offset_deg ?? 0, `${context}.phase_offset_deg`)
  });
}

export function validateScreenManifest(value) {
  assertExactKeys(value, 'manifest', [
    'schema_version',
    'screen_id',
    'altitude_km',
    'entry_elevation_deg',
    'release_elevation_deg',
    'duration_seconds',
    'step_seconds',
    'satellite_capacity',
    'cell_capacity',
    'scenarios'
  ], ['catalog_version', 'catalog_content_hash']);
  if (value.schema_version !== 1) fail('manifest.schema_version must be 1');
  if (typeof value.screen_id !== 'string' || value.screen_id.length === 0) {
    fail('manifest.screen_id must be a non-empty string');
  }
  const entryElevationDeg = finiteNumber(value.entry_elevation_deg, 'manifest.entry_elevation_deg', 0, 90);
  const releaseElevationDeg = finiteNumber(value.release_elevation_deg, 'manifest.release_elevation_deg', 0, 90);
  if (releaseElevationDeg >= entryElevationDeg) {
    fail('manifest.release_elevation_deg must be lower than manifest.entry_elevation_deg');
  }
  if (!Array.isArray(value.scenarios) || value.scenarios.length === 0) {
    fail('manifest.scenarios must be a non-empty array');
  }
  const scenarios = value.scenarios.map(validateScenario);
  const satelliteCapacity = positiveInteger(value.satellite_capacity, 'manifest.satellite_capacity');
  const cellCapacity = positiveInteger(value.cell_capacity, 'manifest.cell_capacity');
  if (cellCapacity * 2 < satelliteCapacity) {
    fail('manifest two cell capacities must cover manifest.satellite_capacity');
  }
  const seenIds = new Set();
  for (const scenario of scenarios) {
    if (seenIds.has(scenario.id)) fail(`manifest contains duplicate scenario id '${scenario.id}'`);
    seenIds.add(scenario.id);
  }
  const catalogVersion = value.catalog_version;
  const catalogContentHash = value.catalog_content_hash;
  if ((catalogVersion === undefined) !== (catalogContentHash === undefined)) {
    fail('manifest.catalog_version and manifest.catalog_content_hash must be provided together');
  }
  if (catalogVersion !== undefined &&
      (typeof catalogVersion !== 'string' || catalogVersion.length === 0)) {
    fail('manifest.catalog_version must be a non-empty string');
  }
  return Object.freeze({
    schemaVersion: 1,
    screenId: value.screen_id,
    altitudeKm: finiteNumber(value.altitude_km, 'manifest.altitude_km', Number.MIN_VALUE),
    entryElevationDeg,
    releaseElevationDeg,
    durationSeconds: positiveInteger(value.duration_seconds, 'manifest.duration_seconds'),
    stepSeconds: positiveInteger(value.step_seconds, 'manifest.step_seconds'),
    satelliteCapacity,
    cellCapacity,
    catalogVersion: catalogVersion ?? null,
    catalogContentHash: catalogContentHash === undefined
      ? null
      : normalizeBareSha256(catalogContentHash, 'manifest.catalog_content_hash'),
    scenarios: Object.freeze(scenarios)
  });
}

function subSatelliteLatitudeDeg(unit) {
  return Math.asin(Math.max(-1, Math.min(1, unit[2]))) * RADIANS_TO_DEGREES;
}

function bucketRange(geometry, centreLatitudeDeg, angularRadiusRad) {
  const radiusDeg = angularRadiusRad * RADIANS_TO_DEGREES;
  const minimumLatitudeDeg = Math.max(-90, centreLatitudeDeg - radiusDeg);
  const maximumLatitudeDeg = Math.min(90, centreLatitudeDeg + radiusDeg);
  return {
    first: Math.max(0, Math.floor((minimumLatitudeDeg + 90) / geometry.latitudeBucketDeg)),
    last: Math.min(
      geometry.bucketCount - 1,
      Math.floor((maximumLatitudeDeg + 90) / geometry.latitudeBucketDeg)
    )
  };
}

function unitDot(left, right) {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

/**
 * Computes one sampled coverage and ownership epoch.
 *
 * The release-threshold inventory is counted in full. Ownership uses only the
 * entry-threshold candidates and therefore cannot trim or redefine visibility.
 * When every satellite's complete release inventory is within the configured
 * capacity, any one-owner-per-L1 selection is capacity-safe because the owned
 * set is necessarily a subset of that inventory.
 */
export function screenEpoch({
  timeUs,
  satellites,
  catalogGeometry,
  orbitModel,
  entryElevationDeg,
  releaseElevationDeg,
  satelliteCapacity = DEFAULT_SATELLITE_CAPACITY,
  cellCapacity = DEFAULT_CELL_CAPACITY,
  previousOwners = undefined
}) {
  if (!Number.isSafeInteger(timeUs)) fail('screenEpoch.timeUs must be a safe integer');
  if (!Array.isArray(satellites) || satellites.length === 0) fail('screenEpoch.satellites must be non-empty');
  if (!isPlainObject(catalogGeometry) || !Array.isArray(catalogGeometry.positions) ||
      !Array.isArray(catalogGeometry.buckets)) {
    fail('screenEpoch.catalogGeometry must be created by createCatalogGeometry');
  }
  positiveInteger(satelliteCapacity, 'screenEpoch.satelliteCapacity');
  positiveInteger(cellCapacity, 'screenEpoch.cellCapacity');
  if (cellCapacity * 2 < satelliteCapacity) {
    fail('screenEpoch two cell capacities must cover the satellite capacity');
  }
  if (previousOwners !== undefined &&
      (!(previousOwners instanceof Int32Array) || previousOwners.length !== catalogGeometry.positions.length)) {
    fail('screenEpoch.previousOwners must be an Int32Array matching the catalog');
  }

  const model = orbitModel ?? resolveOrbitModel({altitudeKm: satellites[0].altitudeKm ?? 500});
  const entryAngleRad = centralAngleForElevation(entryElevationDeg, model);
  const releaseAngleRad = centralAngleForElevation(releaseElevationDeg, model);
  const entryMinimumDot = Math.cos(entryAngleRad);
  const releaseMinimumDot = Math.cos(releaseAngleRad);
  const entryCandidateCounts = new Uint16Array(catalogGeometry.positions.length);
  const bestSatelliteIndices = new Int32Array(catalogGeometry.positions.length);
  bestSatelliteIndices.fill(-1);
  const bestDots = new Float64Array(catalogGeometry.positions.length);
  bestDots.fill(-Infinity);
  const incumbentStillEligible = previousOwners === undefined
    ? undefined
    : new Uint8Array(catalogGeometry.positions.length);
  const incumbentEntryEligible = previousOwners === undefined
    ? undefined
    : new Uint8Array(catalogGeometry.positions.length);
  const releaseVisibleCounts = new Uint32Array(satellites.length);
  const entryVisibleCounts = new Uint32Array(satellites.length);

  for (let satelliteIndex = 0; satelliteIndex < satellites.length; satelliteIndex += 1) {
    const satelliteUnit = satelliteUnitEcef(satellites[satelliteIndex], timeUs, model);
    const range = bucketRange(catalogGeometry, subSatelliteLatitudeDeg(satelliteUnit), releaseAngleRad);
    for (let bucketIndex = range.first; bucketIndex <= range.last; bucketIndex += 1) {
      for (const position of catalogGeometry.buckets[bucketIndex]) {
        const dot = unitDot(satelliteUnit, position.observerUnit);
        if (dot < releaseMinimumDot) continue;
        releaseVisibleCounts[satelliteIndex] += 1;
        if (incumbentStillEligible !== undefined &&
            previousOwners[position.positionIndex] === satelliteIndex) {
          incumbentStillEligible[position.positionIndex] = 1;
        }
        if (dot < entryMinimumDot) continue;
        entryVisibleCounts[satelliteIndex] += 1;
        entryCandidateCounts[position.positionIndex] += 1;
        if (incumbentEntryEligible !== undefined &&
            previousOwners[position.positionIndex] === satelliteIndex) {
          incumbentEntryEligible[position.positionIndex] = 1;
        }
        if (dot > bestDots[position.positionIndex] ||
            (dot === bestDots[position.positionIndex] &&
             compareText(satellites[satelliteIndex].id, satellites[bestSatelliteIndices[position.positionIndex]]?.id) < 0)) {
          bestDots[position.positionIndex] = dot;
          bestSatelliteIndices[position.positionIndex] = satelliteIndex;
        }
      }
    }
  }

  const owners = new Int32Array(catalogGeometry.positions.length);
  owners.fill(-1);
  const assignedCounts = new Uint32Array(satellites.length);
  let uncoveredL1 = 0;
  let minimumEntryCandidateCount = Number.POSITIVE_INFINITY;
  let minimumEligibleCandidateCount = Number.POSITIVE_INFINITY;
  let ownerChangeCount = 0;
  for (let positionIndex = 0; positionIndex < entryCandidateCounts.length; positionIndex += 1) {
    const retainedIncumbentOnly = incumbentStillEligible?.[positionIndex] === 1 &&
      incumbentEntryEligible?.[positionIndex] !== 1;
    const eligibleCandidateCount = entryCandidateCounts[positionIndex] + Number(retainedIncumbentOnly);
    minimumEntryCandidateCount = Math.min(
      minimumEntryCandidateCount,
      entryCandidateCounts[positionIndex]
    );
    minimumEligibleCandidateCount = Math.min(minimumEligibleCandidateCount, eligibleCandidateCount);
    if (eligibleCandidateCount === 0) {
      uncoveredL1 += 1;
      continue;
    }
    const previousOwner = previousOwners?.[positionIndex] ?? -1;
    const owner = previousOwner >= 0 && incumbentStillEligible?.[positionIndex] === 1
      ? previousOwner
      : bestSatelliteIndices[positionIndex];
    owners[positionIndex] = owner;
    assignedCounts[owner] += 1;
    if (previousOwner >= 0 && previousOwner !== owner) ownerChangeCount += 1;
  }

  const maximumReleaseVisibleL1PerSatellite = Math.max(0, ...releaseVisibleCounts);
  const maximumEntryVisibleL1PerSatellite = Math.max(0, ...entryVisibleCounts);
  const maximumAssignedL1PerSatellite = Math.max(0, ...assignedCounts);
  const balancedCellPartitionPeak = Math.ceil(maximumAssignedL1PerSatellite / 2);
  const capacitySafeByInventoryBound = maximumReleaseVisibleL1PerSatellite <= satelliteCapacity;
  const scheduleOverflow = maximumAssignedL1PerSatellite > satelliteCapacity ||
    balancedCellPartitionPeak > cellCapacity;

  return Object.freeze({
    timeUs,
    uncoveredL1,
    minimumEntryCandidateCount: Number.isFinite(minimumEntryCandidateCount) ? minimumEntryCandidateCount : 0,
    minimumEligibleCandidateCount: Number.isFinite(minimumEligibleCandidateCount)
      ? minimumEligibleCandidateCount
      : 0,
    maximumReleaseVisibleL1PerSatellite,
    maximumEntryVisibleL1PerSatellite,
    maximumAssignedL1PerSatellite,
    balancedCellPartitionPeak,
    ownerChangeCount,
    capacitySafeByInventoryBound,
    scheduleOverflow,
    owners
  });
}

export function screenScenario({scenario, manifest, catalogGeometry}) {
  const satellites = createWalkerDelta({
    altitudeKm: manifest.altitudeKm,
    inclinationDeg: scenario.inclinationDeg,
    planes: scenario.planes,
    satellitesPerPlane: scenario.satellitesPerPlane,
    phaseFactor: scenario.phaseFactor,
    raanOffsetDeg: scenario.raanOffsetDeg,
    phaseOffsetDeg: scenario.phaseOffsetDeg
  });
  const orbitModel = resolveOrbitModel({altitudeKm: manifest.altitudeKm});
  const sampleCount = Math.ceil(manifest.durationSeconds / manifest.stepSeconds);
  let previousOwners;
  let maximumUncoveredL1 = 0;
  let minimumEntryCandidateCount = Number.POSITIVE_INFINITY;
  let minimumEligibleCandidateCount = Number.POSITIVE_INFINITY;
  let maximumReleaseVisibleL1PerSatellite = 0;
  let maximumEntryVisibleL1PerSatellite = 0;
  let maximumAssignedL1PerSatellite = 0;
  let maximumBalancedCellPartitionPeak = 0;
  let maximumOwnerChangesPerSample = 0;
  let scheduleOverflowSamples = 0;
  let conclusiveAssignmentSamples = 0;
  let firstFailure = null;

  for (let sampleIndex = 0; sampleIndex < sampleCount; sampleIndex += 1) {
    const timeSeconds = sampleIndex * manifest.stepSeconds;
    const epoch = screenEpoch({
      timeUs: timeSeconds * 1_000_000,
      satellites,
      catalogGeometry,
      orbitModel,
      entryElevationDeg: manifest.entryElevationDeg,
      releaseElevationDeg: manifest.releaseElevationDeg,
      satelliteCapacity: manifest.satelliteCapacity,
      cellCapacity: manifest.cellCapacity,
      previousOwners
    });
    previousOwners = epoch.owners;
    maximumUncoveredL1 = Math.max(maximumUncoveredL1, epoch.uncoveredL1);
    minimumEntryCandidateCount = Math.min(minimumEntryCandidateCount, epoch.minimumEntryCandidateCount);
    minimumEligibleCandidateCount = Math.min(
      minimumEligibleCandidateCount,
      epoch.minimumEligibleCandidateCount
    );
    maximumReleaseVisibleL1PerSatellite = Math.max(
      maximumReleaseVisibleL1PerSatellite,
      epoch.maximumReleaseVisibleL1PerSatellite
    );
    maximumEntryVisibleL1PerSatellite = Math.max(
      maximumEntryVisibleL1PerSatellite,
      epoch.maximumEntryVisibleL1PerSatellite
    );
    maximumAssignedL1PerSatellite = Math.max(
      maximumAssignedL1PerSatellite,
      epoch.maximumAssignedL1PerSatellite
    );
    maximumBalancedCellPartitionPeak = Math.max(
      maximumBalancedCellPartitionPeak,
      epoch.balancedCellPartitionPeak
    );
    maximumOwnerChangesPerSample = Math.max(maximumOwnerChangesPerSample, epoch.ownerChangeCount);
    if (epoch.scheduleOverflow) scheduleOverflowSamples += 1;
    if (epoch.capacitySafeByInventoryBound) conclusiveAssignmentSamples += 1;
    if (firstFailure === null && (epoch.uncoveredL1 > 0 || epoch.scheduleOverflow)) {
      firstFailure = Object.freeze({
        sampleIndex,
        timeSeconds,
        uncoveredL1: epoch.uncoveredL1,
        scheduleOverflow: epoch.scheduleOverflow
      });
    }
  }

  const coveragePassed = maximumUncoveredL1 === 0;
  const assignmentPassed = coveragePassed &&
    conclusiveAssignmentSamples === sampleCount &&
    scheduleOverflowSamples === 0;
  return Object.freeze({
    id: scenario.id,
    inclination_deg: scenario.inclinationDeg,
    planes: scenario.planes,
    satellites_per_plane: scenario.satellitesPerPlane,
    phase_factor: scenario.phaseFactor,
    raan_offset_deg: scenario.raanOffsetDeg,
    phase_offset_deg: scenario.phaseOffsetDeg,
    satellite_count: satellites.length,
    sample_count: sampleCount,
    coverage_passed: coveragePassed,
    assignment_passed: assignmentPassed,
    maximum_uncovered_l1: maximumUncoveredL1,
    minimum_entry_candidate_count: Number.isFinite(minimumEntryCandidateCount)
      ? minimumEntryCandidateCount
      : 0,
    minimum_eligible_candidate_count: Number.isFinite(minimumEligibleCandidateCount)
      ? minimumEligibleCandidateCount
      : 0,
    maximum_release_visible_l1_per_satellite: maximumReleaseVisibleL1PerSatellite,
    maximum_entry_visible_l1_per_satellite: maximumEntryVisibleL1PerSatellite,
    maximum_assigned_l1_per_satellite: maximumAssignedL1PerSatellite,
    maximum_balanced_cell_partition_peak: maximumBalancedCellPartitionPeak,
    maximum_owner_changes_per_sample: maximumOwnerChangesPerSample,
    schedule_overflow_samples: scheduleOverflowSamples,
    conclusive_assignment_samples: conclusiveAssignmentSamples,
    assignment_method: 'sticky_best_elevation_with_complete_inventory_bound',
    first_failure: firstFailure
  });
}

export function runScreen({manifest, catalog}) {
  const validated = validateScreenManifest(manifest);
  const catalogContentHash = verifyCatalogContentHash(catalog);
  const catalogGeometry = createCatalogGeometry(catalog);
  const catalogVersion = typeof catalog?.metadata?.version === 'string'
    ? catalog.metadata.version
    : null;
  if (validated.catalogVersion !== null && catalogVersion !== validated.catalogVersion) {
    fail(`catalog version mismatch: expected '${validated.catalogVersion}'`);
  }
  if (validated.catalogContentHash !== null && catalogContentHash !== validated.catalogContentHash) {
    fail('catalog content hash does not match manifest.catalog_content_hash');
  }
  const scenarios = validated.scenarios
    .map((scenario) => screenScenario({scenario, manifest: validated, catalogGeometry}))
    .sort((left, right) =>
      left.satellite_count - right.satellite_count ||
      left.planes - right.planes ||
      left.satellites_per_plane - right.satellites_per_plane ||
      left.phase_factor - right.phase_factor ||
      compareText(left.id, right.id)
    );
  const passing = scenarios.filter((scenario) => scenario.assignment_passed);
  const smallestPassing = passing[0] ?? null;
  return Object.freeze({
    schema_version: 1,
    screen_id: validated.screenId,
    audit_level: 'coarse',
    exact: false,
    selectedScenario: null,
    selection_eligible: false,
    catalog_l1_count: catalogGeometry.positions.length,
    catalog_version: catalogVersion,
    catalog_content_hash: catalogContentHash,
    catalog_hash_verified: catalogContentHash !== null,
    sampling: Object.freeze({
      duration_seconds: validated.durationSeconds,
      step_seconds: validated.stepSeconds,
      sample_count: Math.ceil(validated.durationSeconds / validated.stepSeconds)
    }),
    planning_limits: Object.freeze({
      altitude_km: validated.altitudeKm,
      entry_elevation_deg: validated.entryElevationDeg,
      release_elevation_deg: validated.releaseElevationDeg,
      satellite_capacity: validated.satelliteCapacity,
      cell_capacity: validated.cellCapacity
    }),
    scenarios: Object.freeze(scenarios),
    smallest_passing_sampled_candidate: smallestPassing === null
      ? null
      : Object.freeze({
        id: smallestPassing.id,
        satellite_count: smallestPassing.satellite_count,
        planes: smallestPassing.planes,
        satellites_per_plane: smallestPassing.satellites_per_plane
      }),
    limitations: Object.freeze([
      'fixed_step_sampling_is_not_continuous_coverage_proof',
      'orbit_epoch_and_absolute_raan_phase_not_frozen',
      'owner_change_counts_at_120_second_steps_are_not_handover_evidence',
      'smallest_passing_candidate_is_only_smallest_in_this_manifest',
      'scenarios_exceeding_the_complete_inventory_capacity_bound_require_full_matching',
      'gateway_constraints_not_modeled',
      'n_minus_one_failure_audit_not_run',
      'pci_time_conflict_audit_not_run',
      'software_planning_evidence_only'
    ])
  });
}

export function parseArguments(argv) {
  const options = {
    manifestPath: defaultManifestPath,
    catalogPath: defaultCatalogPath,
    outputPath: null,
    help: false
  };
  const take = (index, option) => {
    const value = argv[index + 1];
    if (value === undefined || value.startsWith('--')) fail(`${option} requires a value`);
    return value;
  };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === '--manifest') options.manifestPath = resolve(take(index++, argument));
    else if (argument === '--catalog') options.catalogPath = resolve(take(index++, argument));
    else if (argument === '--output') options.outputPath = resolve(take(index++, argument));
    else if (argument === '--help' || argument === '-h') options.help = true;
    else fail(`unknown argument '${argument}'`);
  }
  return options;
}

function usage() {
  return [
    'Usage: node utils/ntn/constellation_screen.mjs [options]',
    '  --manifest FILE   sampled comparison manifest',
    '  --catalog FILE    global G###### catalog',
    '  --output FILE     write report; stdout is used when omitted'
  ].join('\n');
}

export async function main(argv = process.argv.slice(2)) {
  const options = parseArguments(argv);
  if (options.help) {
    process.stdout.write(`${usage()}\n`);
    return null;
  }
  const [manifest, catalog] = await Promise.all([
    readFile(options.manifestPath, 'utf8').then(JSON.parse),
    readFile(options.catalogPath, 'utf8').then(JSON.parse)
  ]);
  const report = runScreen({manifest, catalog});
  if (!report.catalog_hash_verified) {
    fail('CLI catalog must declare a valid content hash');
  }
  const serialized = `${JSON.stringify(report, null, 2)}\n`;
  if (options.outputPath === null) {
    process.stdout.write(serialized);
  } else {
    await mkdir(dirname(options.outputPath), {recursive: true});
    await writeFile(options.outputPath, serialized, 'utf8');
    process.stderr.write(`Wrote sampled constellation screen to ${options.outputPath}\n`);
  }
  return report;
}

const isMain = process.argv[1] && pathToFileURL(resolve(process.argv[1])).href === import.meta.url;
if (isMain) {
  main().catch((error) => {
    process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    process.exitCode = 1;
  });
}
