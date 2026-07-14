import { catalogGroundPoint, l1Catalog, type L1CatalogCell } from "./beam-catalog";
import {
  CANDIDATE_ELEVATION_DEG,
  CANDIDATE_LOOKAHEAD_MS,
  CELL_L1_CAPACITY,
  HOLD_ELEVATION_DEG,
  SATELLITE_L1_CAPACITY,
  createCandidateInventory,
  type PositionSatelliteCandidate,
} from "./beam-hopping-model";
import {
  EARTH_RADIUS_KM,
  ORBIT_RADIUS_KM,
  baselineWalkerDelta,
  coverageRadiusKm,
  createWalkerDelta,
  propagateSatellite,
  type ResolvedWalkerDeltaConfig,
  type SatelliteDefinition,
  type SatelliteState,
  type WalkerDeltaConfig,
} from "./orbit-model";
import baselineIdentityRegistryJson from "./onboard-cell-identity-registry.json";

export type CellBank = 0 | 1;

export type SatelliteCellPlanningContext = {
  readonly registryVersion: string;
  readonly config: Readonly<ResolvedWalkerDeltaConfig>;
  readonly satellites: readonly SatelliteDefinition[];
  readonly onboardCellRegistry: readonly OnboardCellIdentity[];
  readonly identitiesBySatellite: ReadonlyMap<string, readonly [OnboardCellIdentity, OnboardCellIdentity]>;
  readonly pciColoringAudit: PciColoringAudit;
};

export type ManagementCenterCellIdentity = {
  readonly bank: CellBank;
  readonly nci: string;
  readonly pci: number;
};

export type ManagementCenterSatelliteIdentity = {
  readonly satellite_id: string;
  readonly cells: readonly ManagementCenterCellIdentity[];
};

/** Versioned management-center input. NCI and PCI values are opaque data, never locally derived. */
export type ManagementCenterIdentityRegistry = {
  readonly registry_version: string;
  readonly satellites: readonly ManagementCenterSatelliteIdentity[];
};

export type OnboardCellIdentity = {
  readonly satelliteId: string;
  readonly nci: string;
  readonly pci: number;
  /** Resource bank only; it is not an additional radio-cell identifier. */
  readonly bank: CellBank;
};

export type OnboardNrCell = OnboardCellIdentity & {
  readonly l1Ids: readonly string[];
};

export type L1PositionAssignment = {
  readonly positionId: string;
  readonly satelliteId: string;
  readonly nci: string;
  readonly pci: number;
  readonly cellBank: CellBank;
  readonly elevationDeg: number;
};

export type SatellitePositionLoad = {
  readonly assigned: number;
  readonly cellCounts: readonly [number, number];
};

export type PciConflict = {
  readonly pci: number;
  readonly leftPositionId: string;
  readonly rightPositionId: string;
  readonly leftNci: string;
  readonly rightNci: string;
};

export type PciConflictEdge = readonly [string, string];

export type PciColoringAudit = {
  readonly nodeCount: number;
  readonly edgeCount: number;
  readonly colorsUsed: number;
  readonly maximumColors: number;
  readonly conflictCount: number;
  readonly status: "proxy_only";
  readonly model: "walker_local_covisibility_proxy" | "caller_supplied";
};

export type PciColoringResult = {
  readonly colorByNode: ReadonlyMap<string, number>;
  readonly audit: PciColoringAudit;
};

/**
 * Difference between two offline optimizer snapshots.  It is deliberately
 * free of RF state and timing because the target has not been re-evaluated at
 * a future activation epoch and no ready/applied evidence exists.
 */
export type L1ReassignmentDelta = {
  readonly positionId: string;
  readonly sourceSatelliteId: string;
  readonly sourceNci: string;
  readonly sourcePci: number;
  readonly targetSatelliteId: string | null;
  readonly targetNci: string | null;
  readonly targetPci: number | null;
};

export type L1AssignmentReport = {
  readonly timeSeconds: number;
  readonly planningContext: SatelliteCellPlanningContext;
  readonly success: boolean;
  readonly assignments: readonly L1PositionAssignment[];
  readonly assignmentByPositionId: ReadonlyMap<string, L1PositionAssignment>;
  /** Current, 120 s future-entry, and incumbent-hold inventory; never clipped by 128/256 capacity. */
  readonly candidateInventoryByPositionId: ReadonlyMap<string, readonly PositionSatelliteCandidate[]>;
  /** Count of the complete inventory above, including future-entry candidates. */
  readonly candidateCountsByPositionId: ReadonlyMap<string, number>;
  /** Only candidates eligible to serve now: current >=45 degrees plus incumbent >=42-degree hold. */
  readonly servingCandidateCountsByPositionId: ReadonlyMap<string, number>;
  readonly satelliteLoads: ReadonlyMap<string, SatellitePositionLoad>;
  /** Raw changes versus the previous planning snapshot; not transfer-state evidence. */
  readonly reassignmentDeltas: readonly L1ReassignmentDelta[];
  readonly unassignedPositionIds: readonly string[];
  readonly pciConflicts: readonly PciConflict[];
  readonly activeSatelliteCount: number;
  readonly activeCellCount: number;
  /** Satellites with at least one currently serviceable L1 candidate. */
  readonly servingSatelliteCount: number;
  /** Aggregate 256-slot capacity of serving satellites minus this catalog's demand. */
  readonly fleetCapacityHeadroom: number;
  readonly maximumSatelliteLoad: number;
  readonly minimumCandidateCount: number;
  readonly eligibleIncumbentCount: number;
  readonly retainedIncumbentCount: number;
  readonly incumbentRetentionRate: number;
};

export type L1AssignmentFailureEpoch = {
  readonly timeSeconds: number;
  readonly unassignedCount: number;
  readonly sampleUnassignedPositionIds: readonly string[];
  readonly pciConflictCount: number;
  readonly maximumSatelliteLoad: number;
  readonly reason: "no_visible_candidate" | "capacity_exhausted" | "pci_conflict";
};

export type L1AssignmentDayReport = {
  readonly sampleCount: number;
  readonly successfulSamples: number;
  readonly successRate: number;
  readonly failureEpochs: readonly L1AssignmentFailureEpoch[];
  /** Previous-owner changes/releases between snapshots; acquisitions from an unassigned state are excluded. */
  readonly rawReassignments: number;
  readonly minimumUnassigned: number;
  readonly maximumUnassigned: number;
  readonly averageUnassigned: number;
  readonly peakSatelliteLoad: number;
  readonly minimumActiveSatellites: number;
  readonly maximumActiveSatellites: number;
  readonly pciConflictSamples: number;
  readonly minimumCandidateCount: number;
  readonly minimumFleetCapacityHeadroom: number;
  readonly minimumPeakSatelliteHeadroom: number;
};

export type L1AssignmentDayOptions = {
  readonly startSeconds?: number;
  readonly durationSeconds?: number;
  readonly sampleStepSeconds?: number;
  readonly planningContext?: SatelliteCellPlanningContext;
};

export type L1AssignmentPlanOptions = {
  /** Daily capacity sweeps disable this because future candidates cannot serve the current epoch. */
  readonly includeFutureCandidates?: boolean;
  readonly planningContext?: SatelliteCellPlanningContext;
};

export type L1AssignmentFeasibilityProbe = {
  readonly plannedSampleCount: number;
  readonly evaluatedSampleCount: number;
  readonly success: boolean;
  readonly firstFailure: L1AssignmentFailureEpoch | null;
  readonly minimumCandidateCount: number;
  readonly minimumFleetCapacityHeadroom: number;
  readonly peakSatelliteLoad: number;
  readonly minimumActiveSatellites: number;
  readonly maximumActiveSatellites: number;
  readonly minimumPeakSatelliteHeadroom: number;
};

export const PCI_COUNT = 1008;
const DEG = Math.PI / 180;
const HOLD_CENTRAL_ANGLE_RAD = coverageRadiusKm(HOLD_ELEVATION_DEG) / EARTH_RADIUS_KM;
const BOUNDING_LAT_MARGIN_DEG = HOLD_CENTRAL_ANGLE_RAD / DEG + 0.5;
const BOUNDING_LON_MARGIN_DEG = 17;

type GroundGeometry = {
  readonly cell: L1CatalogCell;
  readonly lat: number;
  readonly lon: number;
  readonly unitX: number;
  readonly unitY: number;
  readonly unitZ: number;
};

const groundGeometry: readonly GroundGeometry[] = l1Catalog.map((cell) => {
  const point = catalogGroundPoint(cell);
  const lat = point.lat * DEG;
  const lon = point.lon * DEG;
  return {
    cell,
    lat: point.lat,
    lon: point.lon,
    unitX: Math.cos(lat) * Math.cos(lon),
    unitY: Math.cos(lat) * Math.sin(lon),
    unitZ: Math.sin(lat),
  };
});
const l1IndexById = new Map(l1Catalog.map((cell, index) => [cell.id, index]));

const minGroundLat = Math.min(...groundGeometry.map((point) => point.lat));
const maxGroundLat = Math.max(...groundGeometry.map((point) => point.lat));
const minGroundLon = Math.min(...groundGeometry.map((point) => point.lon));
const maxGroundLon = Math.max(...groundGeometry.map((point) => point.lon));

/** Deterministic greedy coloring for a caller-supplied PCI conflict graph. */
export function colorPciConflictGraph(
  nodeIds: readonly string[],
  edges: readonly PciConflictEdge[],
  maximumColors = PCI_COUNT,
  model: PciColoringAudit["model"] = "caller_supplied",
): PciColoringResult {
  if (!Number.isInteger(maximumColors) || maximumColors <= 0 || maximumColors > PCI_COUNT) {
    throw new Error(`maximumColors must be in the range 1..${PCI_COUNT}`);
  }
  const uniqueNodes = [...new Set(nodeIds)].sort();
  if (uniqueNodes.length !== nodeIds.length) throw new Error("PCI graph node IDs must be unique");
  const adjacency = new Map(uniqueNodes.map((id) => [id, new Set<string>()]));
  for (const [left, right] of edges) {
    if (left === right) throw new Error(`PCI graph contains self-edge ${left}`);
    if (!adjacency.has(left) || !adjacency.has(right)) throw new Error("PCI graph edge references an unknown node");
    adjacency.get(left)!.add(right);
    adjacency.get(right)!.add(left);
  }
  const order = [...uniqueNodes].sort((left, right) =>
    adjacency.get(right)!.size - adjacency.get(left)!.size || left.localeCompare(right),
  );
  const colorByNode = new Map<string, number>();
  for (const node of order) {
    const unavailable = new Set([...adjacency.get(node)!]
      .map((neighbor) => colorByNode.get(neighbor))
      .filter((color): color is number => color !== undefined));
    let color = 0;
    while (color < maximumColors && unavailable.has(color)) color += 1;
    if (color === maximumColors) throw new Error(`PCI graph requires more than ${maximumColors} colors at ${node}`);
    colorByNode.set(node, color);
  }
  const normalizedEdges = new Set(edges.map(([left, right]) => left < right ? `${left}\0${right}` : `${right}\0${left}`));
  const conflictCount = [...normalizedEdges].filter((key) => {
    const [left, right] = key.split("\0");
    return colorByNode.get(left) === colorByNode.get(right);
  }).length;
  return {
    colorByNode,
    audit: Object.freeze({
      nodeCount: uniqueNodes.length,
      edgeCount: normalizedEdges.size,
      colorsUsed: colorByNode.size === 0 ? 0 : Math.max(...colorByNode.values()) + 1,
      maximumColors,
      conflictCount,
      status: "proxy_only",
      model,
    }),
  };
}

function asRegistryObject(value: unknown, context: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${context} must be an object`);
  }
  return value as Record<string, unknown>;
}

function assertRegistryKeys(value: Record<string, unknown>, expected: readonly string[], context: string) {
  const expectedKeys = new Set(expected);
  const unknown = Object.keys(value).filter((key) => !expectedKeys.has(key)).sort();
  const missing = expected.filter((key) => !(key in value));
  if (unknown.length > 0) throw new Error(`${context} contains unknown field ${unknown[0]}`);
  if (missing.length > 0) throw new Error(`${context} is missing field ${missing[0]}`);
}

function normalizeOpaqueNci(value: unknown, context: string) {
  if (typeof value !== "string" || !/^(?:0[xX][0-9a-fA-F]{1,9}|[0-9]{1,11})$/.test(value)) {
    throw new Error(`${context} must be a hexadecimal or decimal 36-bit NCI string`);
  }
  // 36 bits are exactly representable by JavaScript numbers (well below 2^53).
  const numeric = Number(value);
  if (!Number.isSafeInteger(numeric) || numeric < 0 || numeric >= 2 ** 36) {
    throw new Error(`${context} is outside the 36-bit NCI range`);
  }
  return `0x${numeric.toString(16).toUpperCase().padStart(9, "0")}`;
}

function parseManagementCenterIdentityRegistry(
  value: unknown,
  definitions: readonly SatelliteDefinition[],
) {
  if (value === undefined) {
    throw new Error("A versioned management-center identity registry is required; onboard NCI/PCI cannot be derived locally");
  }
  const root = asRegistryObject(value, "identity registry");
  assertRegistryKeys(root, ["registry_version", "satellites"], "identity registry");
  if (typeof root.registry_version !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$/.test(root.registry_version)) {
    throw new Error("identity registry registry_version is malformed");
  }
  if (!Array.isArray(root.satellites)) throw new Error("identity registry satellites must be an array");

  const expectedIds = new Set(definitions.map((satellite) => satellite.id));
  const identitiesBySatellite = new Map<string, readonly [OnboardCellIdentity, OnboardCellIdentity]>();
  const seenNcis = new Set<string>();
  for (let satelliteIndex = 0; satelliteIndex < root.satellites.length; satelliteIndex += 1) {
    const rawSatellite = asRegistryObject(root.satellites[satelliteIndex], `identity registry satellites[${satelliteIndex}]`);
    assertRegistryKeys(rawSatellite, ["satellite_id", "cells"], `identity registry satellites[${satelliteIndex}]`);
    const satelliteId = rawSatellite.satellite_id;
    if (typeof satelliteId !== "string" || satelliteId.length === 0) {
      throw new Error(`identity registry satellites[${satelliteIndex}].satellite_id must be a string`);
    }
    if (!expectedIds.has(satelliteId)) throw new Error(`identity registry contains unexpected satellite ${satelliteId}`);
    if (identitiesBySatellite.has(satelliteId)) throw new Error(`identity registry contains duplicate satellite ${satelliteId}`);
    if (!Array.isArray(rawSatellite.cells) || rawSatellite.cells.length !== 2) {
      throw new Error(`identity registry satellite ${satelliteId} must contain exactly two cells`);
    }

    const cellsByBank = new Map<CellBank, OnboardCellIdentity>();
    for (let cellIndex = 0; cellIndex < rawSatellite.cells.length; cellIndex += 1) {
      const rawCell = asRegistryObject(rawSatellite.cells[cellIndex], `identity registry ${satelliteId}.cells[${cellIndex}]`);
      assertRegistryKeys(rawCell, ["bank", "nci", "pci"], `identity registry ${satelliteId}.cells[${cellIndex}]`);
      if (rawCell.bank !== 0 && rawCell.bank !== 1) {
        throw new Error(`identity registry ${satelliteId}.cells[${cellIndex}].bank must be 0 or 1`);
      }
      if (cellsByBank.has(rawCell.bank)) throw new Error(`identity registry satellite ${satelliteId} contains duplicate bank ${rawCell.bank}`);
      const nci = normalizeOpaqueNci(rawCell.nci, `identity registry ${satelliteId}.cells[${cellIndex}].nci`);
      if (seenNcis.has(nci)) throw new Error(`identity registry contains duplicate NCI ${nci}`);
      seenNcis.add(nci);
      if (typeof rawCell.pci !== "number" || !Number.isInteger(rawCell.pci) || rawCell.pci < 0 || rawCell.pci >= PCI_COUNT) {
        throw new Error(`identity registry ${satelliteId}.cells[${cellIndex}].pci must be an integer in the range 0..1007`);
      }
      cellsByBank.set(rawCell.bank, Object.freeze({ satelliteId, bank: rawCell.bank, nci, pci: rawCell.pci }));
    }
    identitiesBySatellite.set(satelliteId, Object.freeze([cellsByBank.get(0)!, cellsByBank.get(1)!]));
  }

  const missingSatellite = definitions.find((satellite) => !identitiesBySatellite.has(satellite.id));
  if (missingSatellite) throw new Error(`identity registry is missing satellite ${missingSatellite.id}`);
  const registry = Object.freeze(definitions.flatMap((satellite) => [...identitiesBySatellite.get(satellite.id)!]));
  return {
    registryVersion: root.registry_version,
    onboardCellRegistry: registry,
    identitiesBySatellite,
  };
}

function auditConfiguredPciRegistry(
  registry: readonly OnboardCellIdentity[],
  edges: readonly PciConflictEdge[],
): PciColoringAudit {
  const pciByNci = new Map(registry.map((cell) => [cell.nci, cell.pci]));
  const normalizedEdges = new Set(edges.map(([left, right]) => left < right ? `${left}\0${right}` : `${right}\0${left}`));
  const conflictCount = [...normalizedEdges].filter((key) => {
    const [left, right] = key.split("\0");
    if (!pciByNci.has(left) || !pciByNci.has(right)) throw new Error("PCI audit references an unknown registry NCI");
    return pciByNci.get(left) === pciByNci.get(right);
  }).length;
  return Object.freeze({
    nodeCount: registry.length,
    edgeCount: normalizedEdges.size,
    colorsUsed: new Set(registry.map((cell) => cell.pci)).size,
    maximumColors: PCI_COUNT,
    conflictCount,
    status: "proxy_only",
    model: "walker_local_covisibility_proxy",
  });
}

function buildWalkerPciConflictEdges(
  definitions: readonly SatelliteDefinition[],
  nciBySatelliteAndBank: ReadonlyMap<string, string>,
  config: Readonly<ResolvedWalkerDeltaConfig>,
) {
  const edges = new Set<string>();
  const add = (left: string, right: string) => edges.add(left < right ? `${left}\0${right}` : `${right}\0${left}`);
  const nci = (plane: number, slot: number, bank: CellBank) => nciBySatelliteAndBank.get(`${plane}:${slot}:${bank}`)!;
  for (const satellite of definitions) {
    add(nci(satellite.plane, satellite.slot, 0), nci(satellite.plane, satellite.slot, 1));
    for (let planeOffset = -1; planeOffset <= 1; planeOffset += 1) {
      for (let slotOffset = -1; slotOffset <= 1; slotOffset += 1) {
        if (planeOffset === 0 && slotOffset === 0) continue;
        const plane = ((satellite.plane - 1 + planeOffset + config.planes) % config.planes) + 1;
        const slot = ((satellite.slot - 1 + slotOffset + config.satellitesPerPlane) % config.satellitesPerPlane) + 1;
        for (const leftBank of [0, 1] as const) {
          for (const rightBank of [0, 1] as const) add(
            nci(satellite.plane, satellite.slot, leftBank),
            nci(plane, slot, rightBank),
          );
        }
      }
    }
  }
  return [...edges].map((key) => key.split("\0") as [string, string]);
}

export function createSatelliteCellPlanningContext(
  config: WalkerDeltaConfig,
  identityRegistry?: unknown,
): SatelliteCellPlanningContext {
  const normalizedConfig = Object.freeze({
    planes: config.planes,
    satellitesPerPlane: config.satellitesPerPlane,
    phaseFactor: config.phaseFactor ?? baselineWalkerDelta.phaseFactor,
    altitudeKm: config.altitudeKm ?? baselineWalkerDelta.altitudeKm,
    inclinationDeg: config.inclinationDeg ?? baselineWalkerDelta.inclinationDeg,
    raanOffsetDeg: config.raanOffsetDeg ?? baselineWalkerDelta.raanOffsetDeg,
    phaseOffsetDeg: config.phaseOffsetDeg ?? baselineWalkerDelta.phaseOffsetDeg,
  });
  const definitions = Object.freeze(createWalkerDelta(normalizedConfig));
  const parsedRegistry = parseManagementCenterIdentityRegistry(identityRegistry, definitions);
  const nciBySatelliteAndBank = new Map(definitions.flatMap((satellite) =>
    parsedRegistry.identitiesBySatellite.get(satellite.id)!.map((cell) =>
      [`${satellite.plane}:${satellite.slot}:${cell.bank}`, cell.nci] as const,
    ),
  ));
  const pciEdges = buildWalkerPciConflictEdges(definitions, nciBySatelliteAndBank, normalizedConfig);
  const pciColoringAudit = auditConfiguredPciRegistry(parsedRegistry.onboardCellRegistry, pciEdges);
  return Object.freeze({
    registryVersion: parsedRegistry.registryVersion,
    config: normalizedConfig,
    satellites: definitions,
    onboardCellRegistry: parsedRegistry.onboardCellRegistry,
    identitiesBySatellite: parsedRegistry.identitiesBySatellite,
    pciColoringAudit,
  });
}

export const baselineSatelliteCellPlanningContext = createSatelliteCellPlanningContext(
  baselineWalkerDelta,
  baselineIdentityRegistryJson,
);
export const onboardCellRegistry = baselineSatelliteCellPlanningContext.onboardCellRegistry;

function coordinateKey(row: number, column: number) {
  return `${row}:${column}`;
}

/** Returns the six odd-row-offset neighbours present in the canonical catalog. */
export function buildL1Adjacency(catalog: readonly L1CatalogCell[] = l1Catalog) {
  const byCoordinate = new Map(catalog.map((cell) => [coordinateKey(cell.row, cell.column), cell]));
  return new Map(
    catalog.map((cell) => {
      const coordinates: Array<readonly [number, number]> = [
        [cell.row, cell.column - 1],
        [cell.row, cell.column + 1],
      ];
      if (cell.row % 2 === 1) {
        coordinates.push(
          [cell.row - 1, cell.column], [cell.row - 1, cell.column + 1],
          [cell.row + 1, cell.column], [cell.row + 1, cell.column + 1],
        );
      } else {
        coordinates.push(
          [cell.row - 1, cell.column - 1], [cell.row - 1, cell.column],
          [cell.row + 1, cell.column - 1], [cell.row + 1, cell.column],
        );
      }
      const neighbours = coordinates
        .map(([row, column]) => byCoordinate.get(coordinateKey(row, column))?.id)
        .filter((id): id is string => id !== undefined)
        .sort();
      return [cell.id, neighbours] as const;
    }),
  );
}

export const l1Adjacency = buildL1Adjacency();

export function isConnectedL1Set(l1Ids: Iterable<string>, adjacency = l1Adjacency) {
  const members = new Set(l1Ids);
  if (members.size <= 1) return true;
  const first = members.values().next().value as string;
  const visited = new Set([first]);
  const queue = [first];
  for (let cursor = 0; cursor < queue.length; cursor += 1) {
    for (const neighbour of adjacency.get(queue[cursor]) ?? []) {
      if (members.has(neighbour) && !visited.has(neighbour)) {
        visited.add(neighbour);
        queue.push(neighbour);
      }
    }
  }
  return visited.size === members.size;
}

function satelliteUnit(state: SatelliteState) {
  const radius = Math.hypot(...state.ecefKm);
  return {
    x: state.ecefKm[0] / radius,
    y: state.ecefKm[1] / radius,
    z: state.ecefKm[2] / radius,
  };
}

function dotFor(state: SatelliteState, point: GroundGeometry) {
  const unit = satelliteUnit(state);
  return unit.x * point.unitX + unit.y * point.unitY + unit.z * point.unitZ;
}

function elevationForDot(dot: number, orbitRadiusKm = ORBIT_RADIUS_KM) {
  const safeDot = Math.max(-1, Math.min(1, dot));
  const distance = Math.sqrt(
    orbitRadiusKm ** 2 + EARTH_RADIUS_KM ** 2 - 2 * orbitRadiusKm * EARTH_RADIUS_KM * safeDot,
  );
  const sine = (orbitRadiusKm * safeDot - EARTH_RADIUS_KM) / distance;
  return Math.asin(Math.max(-1, Math.min(1, sine))) / DEG;
}

export type GroundAccessPosition = {
  readonly id: string;
  readonly lat: number;
  readonly lon: number;
};

export type SatelliteVisiblePosition = GroundAccessPosition & {
  readonly satelliteId: string;
  readonly elevationDeg: number;
};

export type SpatialCellAllocation = {
  readonly satelliteId: string;
  readonly cells: readonly [OnboardNrCell, OnboardNrCell];
  readonly unallocatedPositionIds: readonly string[];
};

function unitForGroundPosition(point: Pick<GroundAccessPosition, "lat" | "lon">) {
  if (!Number.isFinite(point.lat) || point.lat < -90 || point.lat > 90 || !Number.isFinite(point.lon)) {
    throw new Error("ground position latitude/longitude is invalid");
  }
  const lat = point.lat * DEG;
  const lon = point.lon * DEG;
  return { x: Math.cos(lat) * Math.cos(lon), y: Math.cos(lat) * Math.sin(lon), z: Math.sin(lat) };
}

/** Complete per-position visibility inventory.  It is never clipped by the 128/256 service budget. */
export function visibleSatelliteInventoryForPosition(
  point: GroundAccessPosition,
  timeSeconds: number,
  planningContext: SatelliteCellPlanningContext = baselineSatelliteCellPlanningContext,
  minimumElevationDeg = CANDIDATE_ELEVATION_DEG,
) {
  const ground = unitForGroundPosition(point);
  const orbitRadiusKm = EARTH_RADIUS_KM + planningContext.config.altitudeKm;
  return planningContext.satellites
    .map((definition) => {
      const state = propagateSatellite(definition, timeSeconds);
      const unit = satelliteUnit(state);
      return {
        satelliteId: state.id,
        elevationDeg: elevationForDot(unit.x * ground.x + unit.y * ground.y + unit.z * ground.z, orbitRadiusKm),
      };
    })
    .filter((candidate) => candidate.elevationDeg >= minimumElevationDeg)
    .sort((left, right) => right.elevationDeg - left.elevationDeg || left.satelliteId.localeCompare(right.satelliteId));
}

/** UI-ready table of all supplied access positions visible to one satellite. */
export function visibleAccessTableForSatellite(
  satelliteId: string,
  positions: readonly GroundAccessPosition[],
  timeSeconds: number,
  planningContext: SatelliteCellPlanningContext = baselineSatelliteCellPlanningContext,
  minimumElevationDeg = CANDIDATE_ELEVATION_DEG,
): readonly SatelliteVisiblePosition[] {
  const definition = planningContext.satellites.find((candidate) => candidate.id === satelliteId);
  if (!definition) throw new Error(`Unknown satellite ${satelliteId}`);
  const state = propagateSatellite(definition, timeSeconds);
  const satellite = satelliteUnit(state);
  const orbitRadiusKm = EARTH_RADIUS_KM + planningContext.config.altitudeKm;
  return positions.map((position) => {
    const ground = unitForGroundPosition(position);
    return {
      ...position,
      satelliteId,
      elevationDeg: elevationForDot(satellite.x * ground.x + satellite.y * ground.y + satellite.z * ground.z, orbitRadiusKm),
    };
  }).filter((position) => position.elevationDeg >= minimumElevationDeg)
    .sort((left, right) => right.elevationDeg - left.elevationDeg || left.id.localeCompare(right.id));
}

/**
 * Splits a satellite's visible table into its two long-lived cells along the
 * dominant local latitude/longitude axis.  Overflow is reported explicitly;
 * the complete visible table remains available to the caller.
 */
export function allocateVisiblePositionsToTwoCells(
  satelliteId: string,
  visiblePositions: readonly SatelliteVisiblePosition[],
  planningContext: SatelliteCellPlanningContext = baselineSatelliteCellPlanningContext,
): SpatialCellAllocation {
  const identities = planningContext.identitiesBySatellite.get(satelliteId);
  if (!identities) throw new Error(`Unknown satellite ${satelliteId}`);
  if (visiblePositions.some((position) => position.satelliteId !== satelliteId)) {
    throw new Error("visible position table contains another satellite");
  }
  const unique = new Map(visiblePositions.map((position) => [position.id, position]));
  const positions = [...unique.values()];
  const meanLat = positions.reduce((sum, point) => sum + point.lat, 0) / Math.max(1, positions.length);
  const meanLon = positions.reduce((sum, point) => sum + point.lon, 0) / Math.max(1, positions.length);
  const latVariance = positions.reduce((sum, point) => sum + (point.lat - meanLat) ** 2, 0);
  const lonVariance = positions.reduce((sum, point) => sum + (point.lon - meanLon) ** 2, 0);
  const ordered = positions.sort((left, right) => latVariance >= lonVariance
    ? left.lat - right.lat || left.lon - right.lon || left.id.localeCompare(right.id)
    : left.lon - right.lon || left.lat - right.lat || left.id.localeCompare(right.id));
  const serviceable = ordered.slice(0, SATELLITE_L1_CAPACITY);
  const firstSize = Math.min(CELL_L1_CAPACITY, Math.ceil(serviceable.length / 2));
  const firstIds = serviceable.slice(0, firstSize).map((point) => point.id);
  const secondIds = serviceable.slice(firstSize, firstSize + CELL_L1_CAPACITY).map((point) => point.id);
  return {
    satelliteId,
    cells: [
      { ...identities[0], l1Ids: firstIds },
      { ...identities[1], l1Ids: secondIds },
    ],
    unallocatedPositionIds: ordered.slice(SATELLITE_L1_CAPACITY).map((point) => point.id),
  };
}

function satelliteCanReachServiceBox(state: SatelliteState) {
  return state.lat >= minGroundLat - BOUNDING_LAT_MARGIN_DEG
    && state.lat <= maxGroundLat + BOUNDING_LAT_MARGIN_DEG
    && state.lon >= minGroundLon - BOUNDING_LON_MARGIN_DEG
    && state.lon <= maxGroundLon + BOUNDING_LON_MARGIN_DEG;
}

type CandidateBuild = {
  readonly statesBySatellite: ReadonlyMap<string, SatelliteState>;
  readonly servingCandidatesByPosition: readonly PositionSatelliteCandidate[][];
  readonly inventoryByPosition: readonly PositionSatelliteCandidate[][];
};

function buildCandidates(
  timeSeconds: number,
  previous: L1AssignmentReport | undefined,
  includeFutureCandidates: boolean,
  planningContext: SatelliteCellPlanningContext,
): CandidateBuild {
  const orbitRadiusKm = EARTH_RADIUS_KM + planningContext.config.altitudeKm;
  const hardDotThreshold = Math.cos(coverageRadiusKm(CANDIDATE_ELEVATION_DEG, planningContext.config.altitudeKm) / EARTH_RADIUS_KM);
  const holdDotThreshold = Math.cos(coverageRadiusKm(HOLD_ELEVATION_DEG, planningContext.config.altitudeKm) / EARTH_RADIUS_KM);
  const states = planningContext.satellites.map((definition) => propagateSatellite(definition, timeSeconds));
  const statesBySatellite = new Map(states.map((state) => [state.id, state]));
  const servingCandidatesByPosition: PositionSatelliteCandidate[][] = Array.from({ length: l1Catalog.length }, () => []);

  for (const state of states) {
    if (!satelliteCanReachServiceBox(state)) continue;
    const unit = satelliteUnit(state);
    for (let index = 0; index < groundGeometry.length; index += 1) {
      const point = groundGeometry[index];
      const dot = unit.x * point.unitX + unit.y * point.unitY + unit.z * point.unitZ;
      if (dot >= hardDotThreshold) {
        servingCandidatesByPosition[index].push({ satelliteId: state.id, elevationDeg: elevationForDot(dot, orbitRadiusKm) });
      }
    }
  }

  // The 42-degree release hysteresis applies only to the incumbent.  It is
  // carried separately and never inflates the >=45-degree candidate count.
  if (previous) {
    for (let index = 0; index < groundGeometry.length; index += 1) {
      const old = previous.assignmentByPositionId.get(groundGeometry[index].cell.id);
      if (!old || servingCandidatesByPosition[index].some((candidate) => candidate.satelliteId === old.satelliteId)) continue;
      const state = statesBySatellite.get(old.satelliteId);
      if (!state) continue;
      const dot = dotFor(state, groundGeometry[index]);
      if (dot >= holdDotThreshold) {
        servingCandidatesByPosition[index].push({
          satelliteId: old.satelliteId,
          elevationDeg: elevationForDot(dot, orbitRadiusKm),
          incumbentHoldOnly: true,
        });
      }
    }
  }

  const nowMs = timeSeconds * 1_000;
  for (let index = 0; index < servingCandidatesByPosition.length; index += 1) {
    servingCandidatesByPosition[index] = createCandidateInventory(servingCandidatesByPosition[index], nowMs, 0);
  }
  const inventoryByPosition = servingCandidatesByPosition.map((candidates) => [...candidates]);
  if (includeFutureCandidates) {
    for (const offsetSeconds of [60, 120] as const) {
      const futureStates = planningContext.satellites.map((definition) => propagateSatellite(definition, timeSeconds + offsetSeconds));
      for (const futureState of futureStates) {
        if (!satelliteCanReachServiceBox(futureState)) continue;
        const futureUnit = satelliteUnit(futureState);
        const currentState = statesBySatellite.get(futureState.id)!;
        const currentUnit = satelliteUnit(currentState);
        for (let index = 0; index < groundGeometry.length; index += 1) {
          if (inventoryByPosition[index].some((candidate) => candidate.satelliteId === futureState.id)) continue;
          const point = groundGeometry[index];
          const futureDot = futureUnit.x * point.unitX + futureUnit.y * point.unitY + futureUnit.z * point.unitZ;
          if (futureDot < hardDotThreshold) continue;
          const currentDot = currentUnit.x * point.unitX + currentUnit.y * point.unitY + currentUnit.z * point.unitZ;
          inventoryByPosition[index].push({
            satelliteId: futureState.id,
            elevationDeg: elevationForDot(currentDot, orbitRadiusKm),
            // A 60 s grid is an ephemeris preview, not a protocol timer.  The
            // candidate is not eligible for current serving assignment.
            reachesCandidateElevationAtMs: nowMs + offsetSeconds * 1_000,
          });
        }
      }
    }
    for (let index = 0; index < inventoryByPosition.length; index += 1) {
      inventoryByPosition[index] = createCandidateInventory(
        inventoryByPosition[index],
        nowMs,
        CANDIDATE_LOOKAHEAD_MS,
      );
    }
  }
  return { statesBySatellite, servingCandidatesByPosition, inventoryByPosition };
}

function splitIntoCellBanks(
  satelliteId: string,
  positionIndices: readonly number[],
  previous: L1AssignmentReport | undefined,
) {
  const byBank: [number[], number[]] = [[], []];
  const fresh: number[] = [];
  for (const positionIndex of positionIndices) {
    const old = previous?.assignmentByPositionId.get(l1Catalog[positionIndex].id);
    if (old?.satelliteId === satelliteId && byBank[old.cellBank].length < CELL_L1_CAPACITY) {
      byBank[old.cellBank].push(positionIndex);
    }
    else fresh.push(positionIndex);
  }

  if (!previous && fresh.length > 0) {
    const meanX = fresh.reduce((sum, index) => sum + l1Catalog[index].x, 0) / fresh.length;
    const meanY = fresh.reduce((sum, index) => sum + l1Catalog[index].y, 0) / fresh.length;
    const varianceX = fresh.reduce((sum, index) => sum + (l1Catalog[index].x - meanX) ** 2, 0);
    const varianceY = fresh.reduce((sum, index) => sum + (l1Catalog[index].y - meanY) ** 2, 0);
    const sorted = [...fresh].sort((left, right) => {
      const a = l1Catalog[left];
      const b = l1Catalog[right];
      return varianceX >= varianceY
        ? a.x - b.x || a.y - b.y || a.id.localeCompare(b.id)
        : a.y - b.y || a.x - b.x || a.id.localeCompare(b.id);
    });
    const firstSize = Math.min(CELL_L1_CAPACITY, Math.ceil(sorted.length / 2));
    byBank[0].push(...sorted.slice(0, firstSize));
    byBank[1].push(...sorted.slice(firstSize));
    return byBank;
  }

  const members = byBank.map((indices) => new Set(indices.map((index) => l1Catalog[index].id))) as [Set<string>, Set<string>];
  const centroid = (bank: CellBank) => {
    if (byBank[bank].length === 0) return null;
    return byBank[bank].reduce((sum, index) => ({
      x: sum.x + l1Catalog[index].x / byBank[bank].length,
      y: sum.y + l1Catalog[index].y / byBank[bank].length,
    }), { x: 0, y: 0 });
  };
  // When one retained bank is empty but both banks are required, seed it with
  // the farthest fresh point so later nearest-centroid growth has two anchors.
  const totalPositions = positionIndices.length;
  for (const emptyBank of [0, 1] as const) {
    const occupiedBank = (1 - emptyBank) as CellBank;
    if (byBank[emptyBank].length > 0 || byBank[occupiedBank].length === 0 || totalPositions <= CELL_L1_CAPACITY) continue;
    const occupiedCentroid = centroid(occupiedBank)!;
    const seedOffset = fresh.reduce((bestOffset, index, offset) => {
      const cell = l1Catalog[index];
      const distance = (cell.x - occupiedCentroid.x) ** 2 + (cell.y - occupiedCentroid.y) ** 2;
      const bestCell = l1Catalog[fresh[bestOffset]];
      const bestDistance = (bestCell.x - occupiedCentroid.x) ** 2 + (bestCell.y - occupiedCentroid.y) ** 2;
      return distance > bestDistance ? offset : bestOffset;
    }, 0);
    const [seed] = fresh.splice(seedOffset, 1);
    byBank[emptyBank].push(seed);
    members[emptyBank].add(l1Catalog[seed].id);
  }

  fresh.sort((left, right) => l1Catalog[left].id.localeCompare(l1Catalog[right].id));
  for (const positionIndex of fresh) {
    const cell = l1Catalog[positionIndex];
    const score = (bank: CellBank) => {
      if (byBank[bank].length >= CELL_L1_CAPACITY) return Number.NEGATIVE_INFINITY;
      const adjacent = (l1Adjacency.get(cell.id) ?? []).filter((id) => members[bank].has(id)).length;
      const center = centroid(bank);
      const distance = center ? Math.hypot(cell.x - center.x, cell.y - center.y) : 2;
      return adjacent * 10 - distance - byBank[bank].length * 0.001;
    };
    const score0 = score(0);
    const score1 = score(1);
    const bank: CellBank = score0 > score1 ? 0 : score1 > score0 ? 1 : byBank[0].length <= byBank[1].length ? 0 : 1;
    if (!Number.isFinite(bank === 0 ? score0 : score1)) throw new Error("Satellite L1 capacity overflow while forming two cell banks");
    byBank[bank].push(positionIndex);
    members[bank].add(cell.id);
  }
  return byBank;
}

function findPciConflicts(assignments: ReadonlyMap<string, L1PositionAssignment>) {
  const conflicts: PciConflict[] = [];
  for (const [positionId, neighbours] of l1Adjacency) {
    const left = assignments.get(positionId);
    if (!left) continue;
    for (const neighbourId of neighbours) {
      if (positionId >= neighbourId) continue;
      const right = assignments.get(neighbourId);
      if (right && left.nci !== right.nci && left.pci === right.pci) {
        conflicts.push({
          pci: left.pci,
          leftPositionId: positionId,
          rightPositionId: neighbourId,
          leftNci: left.nci,
          rightNci: right.nci,
        });
      }
    }
  }
  return conflicts;
}

type FlowEdge = {
  to: number;
  reverse: number;
  capacity: number;
  satelliteId?: string;
};

export type L1CandidateSet = {
  readonly positionId: string;
  readonly candidates: readonly PositionSatelliteCandidate[];
};

/**
 * Generic deterministic maximum-cardinality b-matching.  Unlike a one-pass
 * greedy allocator, Dinic's residual graph can move an earlier flexible L1 to
 * another satellite so that a later single-candidate L1 is not stranded.
 */
export function matchL1CandidatesToCapacity(
  candidateSets: readonly L1CandidateSet[],
  satelliteCapacity = SATELLITE_L1_CAPACITY,
  previousAssignments: ReadonlyMap<string, string> = new Map(),
) {
  if (!Number.isInteger(satelliteCapacity) || satelliteCapacity <= 0) {
    throw new Error("satelliteCapacity must be a positive integer");
  }
  const satelliteIds = [...new Set(candidateSets.flatMap((set) => set.candidates.map((candidate) => candidate.satelliteId)))].sort();
  const source = 0;
  const firstPositionNode = 1;
  const firstSatelliteNode = firstPositionNode + candidateSets.length;
  const sink = firstSatelliteNode + satelliteIds.length;
  const graph: FlowEdge[][] = Array.from({ length: sink + 1 }, () => []);
  const addEdge = (from: number, to: number, capacity: number, satelliteId?: string) => {
    const forward: FlowEdge = { to, reverse: graph[to].length, capacity, satelliteId };
    const reverse: FlowEdge = { to: from, reverse: graph[from].length, capacity: 0 };
    graph[from].push(forward);
    graph[to].push(reverse);
    return forward;
  };
  const satelliteNodeById = new Map(satelliteIds.map((satelliteId, index) => [satelliteId, firstSatelliteNode + index]));
  const sourceEdgeByPositionId = new Map<string, FlowEdge>();
  const candidateEdgeByKey = new Map<string, FlowEdge>();
  const sinkEdgeBySatelliteId = new Map<string, FlowEdge>();

  candidateSets.forEach((candidateSet, setIndex) => {
    const positionNode = firstPositionNode + setIndex;
    sourceEdgeByPositionId.set(candidateSet.positionId, addEdge(source, positionNode, 1));
    const previousSatelliteId = previousAssignments.get(candidateSet.positionId);
    const orderedCandidates = [...candidateSet.candidates].sort((left, right) => {
      const leftRetained = Number(left.satelliteId === previousSatelliteId);
      const rightRetained = Number(right.satelliteId === previousSatelliteId);
      return rightRetained - leftRetained
        || right.elevationDeg - left.elevationDeg
        || left.satelliteId.localeCompare(right.satelliteId);
    });
    for (const candidate of orderedCandidates) {
      candidateEdgeByKey.set(
        `${candidateSet.positionId}\0${candidate.satelliteId}`,
        addEdge(positionNode, satelliteNodeById.get(candidate.satelliteId)!, 1, candidate.satelliteId),
      );
    }
  });
  for (const satelliteId of satelliteIds) {
    sinkEdgeBySatelliteId.set(
      satelliteId,
      addEdge(satelliteNodeById.get(satelliteId)!, sink, satelliteCapacity),
    );
  }

  // Seed valid incumbent flows first.  Subsequent residual augmenting paths
  // move them only when that is necessary to increase matching cardinality.
  const seededLoad = new Map<string, number>();
  const consume = (edge: FlowEdge) => {
    edge.capacity -= 1;
    graph[edge.to][edge.reverse].capacity += 1;
  };
  candidateSets.forEach((candidateSet) => {
    const satelliteId = previousAssignments.get(candidateSet.positionId);
    if (!satelliteId || (seededLoad.get(satelliteId) ?? 0) >= satelliteCapacity) return;
    const candidateEdge = candidateEdgeByKey.get(`${candidateSet.positionId}\0${satelliteId}`);
    const sourceEdge = sourceEdgeByPositionId.get(candidateSet.positionId);
    const sinkEdge = sinkEdgeBySatelliteId.get(satelliteId);
    if (!candidateEdge || !sourceEdge || !sinkEdge || sinkEdge.capacity <= 0) return;
    consume(sourceEdge);
    consume(candidateEdge);
    consume(sinkEdge);
    seededLoad.set(satelliteId, (seededLoad.get(satelliteId) ?? 0) + 1);
  });
  const levels = Array<number>(graph.length).fill(-1);
  const cursor = Array<number>(graph.length).fill(0);
  const buildLevels = () => {
    levels.fill(-1);
    const queue = [source];
    levels[source] = 0;
    for (let index = 0; index < queue.length; index += 1) {
      const node = queue[index];
      for (const edge of graph[node]) {
        if (edge.capacity > 0 && levels[edge.to] < 0) {
          levels[edge.to] = levels[node] + 1;
          queue.push(edge.to);
        }
      }
    }
    return levels[sink] >= 0;
  };
  const send = (node: number, flow: number): number => {
    if (node === sink) return flow;
    for (; cursor[node] < graph[node].length; cursor[node] += 1) {
      const edge = graph[node][cursor[node]];
      if (edge.capacity <= 0 || levels[edge.to] !== levels[node] + 1) continue;
      const pushed = send(edge.to, Math.min(flow, edge.capacity));
      if (pushed > 0) {
        edge.capacity -= pushed;
        graph[edge.to][edge.reverse].capacity += pushed;
        return pushed;
      }
    }
    return 0;
  };
  while (buildLevels()) {
    cursor.fill(0);
    while (send(source, Number.MAX_SAFE_INTEGER) > 0) {
      // Keep augmenting this level graph.
    }
  }

  const satelliteByPosition = new Map<string, string>();
  candidateSets.forEach((candidateSet, setIndex) => {
    const positionNode = firstPositionNode + setIndex;
    const matched = graph[positionNode].find((edge) => edge.satelliteId !== undefined && edge.capacity === 0);
    if (matched?.satelliteId) satelliteByPosition.set(candidateSet.positionId, matched.satelliteId);
  });
  return satelliteByPosition;
}

function matchPositionsToSatellites(
  candidatesByPosition: readonly (readonly PositionSatelliteCandidate[])[],
  positionOrder: readonly number[],
  previous: L1AssignmentReport | undefined,
) {
  const previousAssignments = new Map(
    previous?.assignments.map((assignment) => [assignment.positionId, assignment.satelliteId]) ?? [],
  );
  const matchedById = matchL1CandidatesToCapacity(
    positionOrder.map((positionIndex) => ({
      positionId: l1Catalog[positionIndex].id,
      candidates: candidatesByPosition[positionIndex],
    })),
    SATELLITE_L1_CAPACITY,
    previousAssignments,
  );
  return new Map(
    [...matchedById].map(([positionId, satelliteId]) => [l1IndexById.get(positionId)!, satelliteId]),
  );
}

/**
 * Plans individual ground-fixed L1 leases.  Capacity is 128 per onboard cell,
 * hence 256 per satellite; the visible candidate inventory remains untouched.
 */
export function planL1AssignmentsAtEpoch(
  timeSeconds: number,
  previous?: L1AssignmentReport,
  options: L1AssignmentPlanOptions = {},
): L1AssignmentReport {
  if (!Number.isFinite(timeSeconds) || timeSeconds < 0) throw new Error("timeSeconds must be non-negative");
  const planningContext = options.planningContext ?? baselineSatelliteCellPlanningContext;
  if (previous && previous.planningContext !== planningContext) {
    throw new Error("previous assignment report belongs to a different constellation planning context");
  }
  const { servingCandidatesByPosition, inventoryByPosition } = buildCandidates(
    timeSeconds,
    previous,
    options.includeFutureCandidates ?? true,
    planningContext,
  );
  const positionIndices = l1Catalog.map((_, index) => index).sort((left, right) => {
    const leftHard = servingCandidatesByPosition[left].filter((candidate) => !candidate.incumbentHoldOnly).length;
    const rightHard = servingCandidatesByPosition[right].filter((candidate) => !candidate.incumbentHoldOnly).length;
    return leftHard - rightHard || servingCandidatesByPosition[left].length - servingCandidatesByPosition[right].length
      || l1Catalog[left].id.localeCompare(l1Catalog[right].id);
  });

  const selectedSatelliteByPosition = matchPositionsToSatellites(servingCandidatesByPosition, positionIndices, previous);
  const positionsBySatellite = new Map<string, number[]>();
  const selectedElevationByPosition = new Map<number, number>();
  const unassignedIndices: number[] = [];
  for (let positionIndex = 0; positionIndex < l1Catalog.length; positionIndex += 1) {
    const satelliteId = selectedSatelliteByPosition.get(positionIndex);
    if (!satelliteId) {
      unassignedIndices.push(positionIndex);
      continue;
    }
    const satellitePositions = positionsBySatellite.get(satelliteId) ?? [];
    satellitePositions.push(positionIndex);
    positionsBySatellite.set(satelliteId, satellitePositions);
    selectedElevationByPosition.set(
      positionIndex,
      servingCandidatesByPosition[positionIndex].find((candidate) => candidate.satelliteId === satelliteId)!.elevationDeg,
    );
  }

  const assignments: L1PositionAssignment[] = [];
  const satelliteLoads = new Map<string, SatellitePositionLoad>();
  for (const satellite of planningContext.satellites) {
    const indices = positionsBySatellite.get(satellite.id) ?? [];
    const banks = splitIntoCellBanks(satellite.id, indices, previous);
    const identities = planningContext.identitiesBySatellite.get(satellite.id)!;
    banks.forEach((bankPositions, bankNumber) => {
      const identity = identities[bankNumber];
      for (const positionIndex of bankPositions) {
        assignments.push({
          positionId: l1Catalog[positionIndex].id,
          satelliteId: satellite.id,
          nci: identity.nci,
          pci: identity.pci,
          cellBank: identity.bank,
          elevationDeg: selectedElevationByPosition.get(positionIndex)!,
        });
      }
    });
    satelliteLoads.set(satellite.id, {
      assigned: indices.length,
      cellCounts: [banks[0].length, banks[1].length],
    });
  }
  assignments.sort((left, right) => left.positionId.localeCompare(right.positionId));
  const assignmentByPositionId = new Map(assignments.map((assignment) => [assignment.positionId, assignment]));

  const candidateInventoryByPositionId = new Map<string, readonly PositionSatelliteCandidate[]>();
  const candidateCountsByPositionId = new Map<string, number>();
  const servingCandidateCountsByPositionId = new Map<string, number>();
  inventoryByPosition.forEach((candidates, index) => {
    const id = l1Catalog[index].id;
    candidateInventoryByPositionId.set(id, candidates);
    candidateCountsByPositionId.set(id, candidates.length);
    servingCandidateCountsByPositionId.set(id, servingCandidatesByPosition[index].length);
  });

  const reassignmentDeltas: L1ReassignmentDelta[] = [];
  if (previous) {
    for (const old of previous.assignments) {
      const target = assignmentByPositionId.get(old.positionId);
      if (!target) {
        reassignmentDeltas.push({
          positionId: old.positionId,
          sourceSatelliteId: old.satelliteId,
          sourceNci: old.nci,
          sourcePci: old.pci,
          targetSatelliteId: null,
          targetNci: null,
          targetPci: null,
        });
      } else if (target.nci !== old.nci) {
        reassignmentDeltas.push({
          positionId: old.positionId,
          sourceSatelliteId: old.satelliteId,
          sourceNci: old.nci,
          sourcePci: old.pci,
          targetSatelliteId: target.satelliteId,
          targetNci: target.nci,
          targetPci: target.pci,
        });
      }
    }
  }

  const pciConflicts = findPciConflicts(assignmentByPositionId);
  const activeSatelliteCount = [...satelliteLoads.values()].filter((load) => load.assigned > 0).length;
  const activeCellCount = [...satelliteLoads.values()].reduce(
    (sum, load) => sum + Number(load.cellCounts[0] > 0) + Number(load.cellCounts[1] > 0),
    0,
  );
  const servingSatelliteIds = new Set(
    servingCandidatesByPosition.flatMap((candidates) => candidates.map((candidate) => candidate.satelliteId)),
  );
  const servingSatelliteCount = servingSatelliteIds.size;
  const fleetCapacityHeadroom = servingSatelliteCount * SATELLITE_L1_CAPACITY - l1Catalog.length;
  const maximumSatelliteLoad = Math.max(0, ...[...satelliteLoads.values()].map((load) => load.assigned));
  const minimumCandidateCount = Math.min(...candidateCountsByPositionId.values());
  const unassignedPositionIds = unassignedIndices.map((index) => l1Catalog[index].id).sort();
  let eligibleIncumbentCount = 0;
  let retainedIncumbentCount = 0;
  if (previous) {
    for (const old of previous.assignments) {
      const inventory = servingCandidatesByPosition[l1IndexById.get(old.positionId)!] ?? [];
      if (!inventory.some((candidate) => candidate.satelliteId === old.satelliteId)) continue;
      eligibleIncumbentCount += 1;
      if (assignmentByPositionId.get(old.positionId)?.nci === old.nci) retainedIncumbentCount += 1;
    }
  }

  return {
    timeSeconds,
    planningContext,
    success: unassignedPositionIds.length === 0 && pciConflicts.length === 0,
    assignments,
    assignmentByPositionId,
    candidateInventoryByPositionId,
    candidateCountsByPositionId,
    servingCandidateCountsByPositionId,
    satelliteLoads,
    reassignmentDeltas,
    unassignedPositionIds,
    pciConflicts,
    activeSatelliteCount,
    activeCellCount,
    servingSatelliteCount,
    fleetCapacityHeadroom,
    maximumSatelliteLoad,
    minimumCandidateCount,
    eligibleIncumbentCount,
    retainedIncumbentCount,
    incumbentRetentionRate: eligibleIncumbentCount === 0 ? 1 : retainedIncumbentCount / eligibleIncumbentCount,
  };
}

/**
 * Returns two stable NCI/PCI cells.  Their fresh L1 split is best-effort
 * compact and sticky; multi-beam cells are not required to be graph-connected.
 */
export function allocationForSatellite(report: L1AssignmentReport, satelliteId: string): readonly [OnboardNrCell, OnboardNrCell] {
  const identities = report.planningContext.identitiesBySatellite.get(satelliteId);
  if (!identities) throw new Error(`Unknown satellite ${satelliteId}`);
  const byBank: [string[], string[]] = [[], []];
  for (const assignment of report.assignments) {
    if (assignment.satelliteId === satelliteId) byBank[assignment.cellBank].push(assignment.positionId);
  }
  const lower = identities[0];
  const upper = identities[1];
  return [
    { ...lower, l1Ids: byBank[lower.bank].sort() },
    { ...upper, l1Ids: byBank[upper.bank].sort() },
  ];
}

export function summarizeL1AssignmentDay(options: L1AssignmentDayOptions = {}): L1AssignmentDayReport {
  const startSeconds = options.startSeconds ?? 0;
  const durationSeconds = options.durationSeconds ?? 24 * 60 * 60;
  const sampleStepSeconds = options.sampleStepSeconds ?? 120;
  const planningContext = options.planningContext ?? baselineSatelliteCellPlanningContext;
  if (sampleStepSeconds <= 0 || durationSeconds <= 0) throw new Error("duration and sample step must be positive");
  const sampleCount = Math.ceil(durationSeconds / sampleStepSeconds);
  let previous: L1AssignmentReport | undefined;
  let successfulSamples = 0;
  let rawReassignments = 0;
  let minimumUnassigned = Number.POSITIVE_INFINITY;
  let maximumUnassigned = 0;
  let totalUnassigned = 0;
  let peakSatelliteLoad = 0;
  let minimumActiveSatellites = Number.POSITIVE_INFINITY;
  let maximumActiveSatellites = 0;
  let pciConflictSamples = 0;
  let minimumCandidateCount = Number.POSITIVE_INFINITY;
  let minimumFleetCapacityHeadroom = Number.POSITIVE_INFINITY;
  const failureEpochs: L1AssignmentFailureEpoch[] = [];

  for (let sample = 0; sample < sampleCount; sample += 1) {
    const timeSeconds = startSeconds + sample * sampleStepSeconds;
    const report = planL1AssignmentsAtEpoch(timeSeconds, previous, {
      includeFutureCandidates: false,
      planningContext,
    });
    if (report.success) successfulSamples += 1;
    rawReassignments += report.reassignmentDeltas.length;
    minimumUnassigned = Math.min(minimumUnassigned, report.unassignedPositionIds.length);
    maximumUnassigned = Math.max(maximumUnassigned, report.unassignedPositionIds.length);
    totalUnassigned += report.unassignedPositionIds.length;
    peakSatelliteLoad = Math.max(peakSatelliteLoad, report.maximumSatelliteLoad);
    minimumActiveSatellites = Math.min(minimumActiveSatellites, report.activeSatelliteCount);
    maximumActiveSatellites = Math.max(maximumActiveSatellites, report.activeSatelliteCount);
    minimumCandidateCount = Math.min(minimumCandidateCount, report.minimumCandidateCount);
    minimumFleetCapacityHeadroom = Math.min(minimumFleetCapacityHeadroom, report.fleetCapacityHeadroom);
    if (report.pciConflicts.length > 0) pciConflictSamples += 1;
    if (!report.success) {
      const noCandidate = report.unassignedPositionIds.some((id) => report.servingCandidateCountsByPositionId.get(id) === 0);
      failureEpochs.push({
        timeSeconds,
        unassignedCount: report.unassignedPositionIds.length,
        sampleUnassignedPositionIds: report.unassignedPositionIds.slice(0, 8),
        pciConflictCount: report.pciConflicts.length,
        maximumSatelliteLoad: report.maximumSatelliteLoad,
        reason: noCandidate ? "no_visible_candidate"
          : report.unassignedPositionIds.length > 0 ? "capacity_exhausted" : "pci_conflict",
      });
    }
    previous = report;
  }

  return {
    sampleCount,
    successfulSamples,
    successRate: successfulSamples / sampleCount,
    failureEpochs,
    rawReassignments,
    minimumUnassigned: Number.isFinite(minimumUnassigned) ? minimumUnassigned : 0,
    maximumUnassigned,
    averageUnassigned: totalUnassigned / sampleCount,
    peakSatelliteLoad,
    minimumActiveSatellites: Number.isFinite(minimumActiveSatellites) ? minimumActiveSatellites : 0,
    maximumActiveSatellites,
    pciConflictSamples,
    minimumCandidateCount: Number.isFinite(minimumCandidateCount) ? minimumCandidateCount : 0,
    minimumFleetCapacityHeadroom: Number.isFinite(minimumFleetCapacityHeadroom) ? minimumFleetCapacityHeadroom : 0,
    minimumPeakSatelliteHeadroom: SATELLITE_L1_CAPACITY - peakSatelliteLoad,
  };
}

/**
 * Fast pass/fail probe for constellation sweeps.  It exits at the first
 * failing 120-second epoch, while a passing result necessarily evaluated the
 * complete requested horizon.  Use summarizeL1AssignmentDay when churn and
 * the complete failure list are needed.
 */
export function probeL1AssignmentFeasibility(options: L1AssignmentDayOptions = {}): L1AssignmentFeasibilityProbe {
  const startSeconds = options.startSeconds ?? 0;
  const durationSeconds = options.durationSeconds ?? 24 * 60 * 60;
  const sampleStepSeconds = options.sampleStepSeconds ?? 120;
  const planningContext = options.planningContext ?? baselineSatelliteCellPlanningContext;
  if (sampleStepSeconds <= 0 || durationSeconds <= 0) throw new Error("duration and sample step must be positive");
  const plannedSampleCount = Math.ceil(durationSeconds / sampleStepSeconds);
  let previous: L1AssignmentReport | undefined;
  let minimumCandidateCount = Number.POSITIVE_INFINITY;
  let minimumFleetCapacityHeadroom = Number.POSITIVE_INFINITY;
  let peakSatelliteLoad = 0;
  let minimumActiveSatellites = Number.POSITIVE_INFINITY;
  let maximumActiveSatellites = 0;

  for (let sample = 0; sample < plannedSampleCount; sample += 1) {
    const timeSeconds = startSeconds + sample * sampleStepSeconds;
    const report = planL1AssignmentsAtEpoch(timeSeconds, previous, {
      includeFutureCandidates: false,
      planningContext,
    });
    minimumCandidateCount = Math.min(minimumCandidateCount, report.minimumCandidateCount);
    minimumFleetCapacityHeadroom = Math.min(minimumFleetCapacityHeadroom, report.fleetCapacityHeadroom);
    peakSatelliteLoad = Math.max(peakSatelliteLoad, report.maximumSatelliteLoad);
    minimumActiveSatellites = Math.min(minimumActiveSatellites, report.activeSatelliteCount);
    maximumActiveSatellites = Math.max(maximumActiveSatellites, report.activeSatelliteCount);
    if (!report.success) {
      const noCandidate = report.unassignedPositionIds.some((id) => report.servingCandidateCountsByPositionId.get(id) === 0);
      return {
        plannedSampleCount,
        evaluatedSampleCount: sample + 1,
        success: false,
        firstFailure: {
          timeSeconds,
          unassignedCount: report.unassignedPositionIds.length,
          sampleUnassignedPositionIds: report.unassignedPositionIds.slice(0, 8),
          pciConflictCount: report.pciConflicts.length,
          maximumSatelliteLoad: report.maximumSatelliteLoad,
          reason: noCandidate ? "no_visible_candidate"
            : report.unassignedPositionIds.length > 0 ? "capacity_exhausted" : "pci_conflict",
        },
        minimumCandidateCount,
        minimumFleetCapacityHeadroom,
        peakSatelliteLoad,
        minimumActiveSatellites,
        maximumActiveSatellites,
        minimumPeakSatelliteHeadroom: SATELLITE_L1_CAPACITY - peakSatelliteLoad,
      };
    }
    previous = report;
  }

  return {
    plannedSampleCount,
    evaluatedSampleCount: plannedSampleCount,
    success: true,
    firstFailure: null,
    minimumCandidateCount: Number.isFinite(minimumCandidateCount) ? minimumCandidateCount : 0,
    minimumFleetCapacityHeadroom: Number.isFinite(minimumFleetCapacityHeadroom) ? minimumFleetCapacityHeadroom : 0,
    peakSatelliteLoad,
    minimumActiveSatellites: Number.isFinite(minimumActiveSatellites) ? minimumActiveSatellites : 0,
    maximumActiveSatellites,
    minimumPeakSatelliteHeadroom: SATELLITE_L1_CAPACITY - peakSatelliteLoad,
  };
}
