import assert from "node:assert/strict";
import test from "node:test";
import { CELL_L1_CAPACITY, SATELLITE_L1_CAPACITY } from "../app/beam-hopping-model.ts";
import {
  allocateVisiblePositionsToTwoCells,
  baselineSatelliteCellPlanningContext,
  colorPciConflictGraph,
  createSatelliteCellPlanningContext,
  matchL1CandidatesToCapacity,
  onboardCellRegistry,
  visibleAccessTableForSatellite,
  visibleSatelliteInventoryForPosition,
} from "../app/satellite-cell-model.ts";
import { propagateSatellite, satellites } from "../app/orbit-model.ts";

const SMALL_CONFIG = {
  planes: 2,
  satellitesPerPlane: 2,
  phaseFactor: 1,
  altitudeKm: 600,
  inclinationDeg: 70,
  raanOffsetDeg: 7,
  phaseOffsetDeg: 11,
};

const SMALL_IDENTITY_REGISTRY = {
  registry_version: "management-center-test-v7",
  satellites: [
    { satellite_id: "P01-S01", cells: [
      { bank: 0, nci: "0x00A19C3F2", pci: 101 }, { bank: 1, nci: "0x0F37A21B4", pci: 102 },
    ] },
    { satellite_id: "P01-S02", cells: [
      { bank: 0, nci: "0x6B08D4E15", pci: 103 }, { bank: 1, nci: "0x31E7FA902", pci: 104 },
    ] },
    { satellite_id: "P02-S01", cells: [
      { bank: 0, nci: "0xD4290C7A1", pci: 105 }, { bank: 1, nci: "0x18C5B3D6E", pci: 106 },
    ] },
    { satellite_id: "P02-S02", cells: [
      { bank: 0, nci: "0x8E3A671C0", pci: 107 }, { bank: 1, nci: "0x247D09F5B", pci: 108 },
    ] },
  ],
};

test("maximum matching follows an augmenting path and keeps feasible incumbents", () => {
  const candidates = [
    { positionId: "FLEX", candidates: [
      { satelliteId: "SAT-A", elevationDeg: 60 }, { satelliteId: "SAT-B", elevationDeg: 50 },
    ] },
    { positionId: "ONLY-A", candidates: [{ satelliteId: "SAT-A", elevationDeg: 55 }] },
  ];
  const matched = matchL1CandidatesToCapacity(candidates, 1);
  assert.deepEqual([...matched].sort(), [["FLEX", "SAT-B"], ["ONLY-A", "SAT-A"]]);
  assert.deepEqual(
    [...matchL1CandidatesToCapacity(candidates, 1, new Map([["FLEX", "SAT-B"], ["ONLY-A", "SAT-A"]]))].sort(),
    [["FLEX", "SAT-B"], ["ONLY-A", "SAT-A"]],
  );
});

test("deterministic PCI graph coloring rejects adjacent reuse and audits the proxy", () => {
  const nodes = ["A", "B", "C", "D"];
  const edges = [["A", "B"], ["B", "C"], ["C", "A"], ["C", "D"]];
  const first = colorPciConflictGraph(nodes, edges);
  const second = colorPciConflictGraph(nodes, [...edges].reverse());
  assert.deepEqual([...first.colorByNode], [...second.colorByNode]);
  assert.equal(first.audit.nodeCount, 4);
  assert.equal(first.audit.edgeCount, 4);
  assert.equal(first.audit.colorsUsed, 3);
  assert.equal(first.audit.conflictCount, 0);
  for (const [left, right] of edges) assert.notEqual(first.colorByNode.get(left), first.colorByNode.get(right));
  assert.throws(() => colorPciConflictGraph(nodes, edges, 2), /requires more than 2 colors/);
});

test("versioned management-center registry owns exactly two stable cells for all 3528 satellites", () => {
  const context = baselineSatelliteCellPlanningContext;
  assert.equal(context.registryVersion, "mc-ntn-onboard-cell-registry-v1");
  assert.equal(satellites.length, 3528);
  assert.equal(onboardCellRegistry.length, 7056);
  assert.equal(new Set(onboardCellRegistry.map((cell) => cell.nci)).size, 7056);
  assert.equal(onboardCellRegistry.every((cell) => BigInt(cell.nci) >= 0n && BigInt(cell.nci) < (1n << 36n)), true);
  assert.equal(onboardCellRegistry.every((cell) => cell.pci >= 0 && cell.pci <= 1007), true);
  assert.ok(new Set(onboardCellRegistry.map((cell) => cell.pci)).size < onboardCellRegistry.length, "PCI reuse is allowed");
  assert.equal(context.pciColoringAudit.nodeCount, 7056);
  assert.equal(context.pciColoringAudit.conflictCount, 0);
  assert.equal(context.pciColoringAudit.status, "proxy_only");
  assert.equal(context.pciColoringAudit.colorsUsed <= 1008, true);
  for (const satellite of satellites) {
    const cells = context.identitiesBySatellite.get(satellite.id);
    assert.deepEqual(cells.map((cell) => cell.bank), [0, 1]);
    assert.notEqual(cells[0].nci, cells[1].nci);
  }
});

test("planning context preserves registry values and orders satellites and banks deterministically", () => {
  const reversedRegistry = {
    registry_version: SMALL_IDENTITY_REGISTRY.registry_version,
    satellites: [...SMALL_IDENTITY_REGISTRY.satellites].reverse().map((satellite) => ({
      ...satellite, cells: [...satellite.cells].reverse(),
    })),
  };
  const context = createSatelliteCellPlanningContext(SMALL_CONFIG, SMALL_IDENTITY_REGISTRY);
  const reversed = createSatelliteCellPlanningContext(SMALL_CONFIG, reversedRegistry);
  assert.equal(context.registryVersion, SMALL_IDENTITY_REGISTRY.registry_version);
  assert.equal(context.satellites.length, 4);
  assert.deepEqual(context.config, SMALL_CONFIG);
  assert.equal(context.onboardCellRegistry.length, 8);
  assert.deepEqual(reversed.onboardCellRegistry, context.onboardCellRegistry);
  assert.deepEqual(context.identitiesBySatellite.get("P01-S01"), [
    { satelliteId: "P01-S01", bank: 0, nci: "0x00A19C3F2", pci: 101 },
    { satelliteId: "P01-S01", bank: 1, nci: "0x0F37A21B4", pci: 102 },
  ]);
  assert.equal(context.pciColoringAudit.conflictCount, 0);
});

test("identity registry rejects missing, duplicate, malformed and mismatched management-center input", () => {
  assert.throws(() => createSatelliteCellPlanningContext(SMALL_CONFIG), /identity registry is required/);

  assert.throws(
    () => createSatelliteCellPlanningContext(SMALL_CONFIG, { ...SMALL_IDENTITY_REGISTRY, registry_version: "" }),
    /registry_version is malformed/,
  );

  const withSatellites = (satellites) => ({
    registry_version: SMALL_IDENTITY_REGISTRY.registry_version,
    satellites,
  });
  assert.throws(
    () => createSatelliteCellPlanningContext(SMALL_CONFIG, withSatellites(SMALL_IDENTITY_REGISTRY.satellites.slice(1))),
    /missing satellite P01-S01/,
  );
  assert.throws(
    () => createSatelliteCellPlanningContext(SMALL_CONFIG, withSatellites([
      ...SMALL_IDENTITY_REGISTRY.satellites, structuredClone(SMALL_IDENTITY_REGISTRY.satellites[0]),
    ])),
    /duplicate satellite P01-S01/,
  );

  const wrongSatellite = structuredClone(SMALL_IDENTITY_REGISTRY);
  wrongSatellite.satellites[0].satellite_id = "P99-S99";
  assert.throws(() => createSatelliteCellPlanningContext(SMALL_CONFIG, wrongSatellite), /unexpected satellite P99-S99/);

  const oneCell = structuredClone(SMALL_IDENTITY_REGISTRY);
  oneCell.satellites[0].cells.pop();
  assert.throws(() => createSatelliteCellPlanningContext(SMALL_CONFIG, oneCell), /must contain exactly two cells/);

  const duplicateBank = structuredClone(SMALL_IDENTITY_REGISTRY);
  duplicateBank.satellites[0].cells[1].bank = 0;
  assert.throws(() => createSatelliteCellPlanningContext(SMALL_CONFIG, duplicateBank), /duplicate bank 0/);

  const duplicateNci = structuredClone(SMALL_IDENTITY_REGISTRY);
  duplicateNci.satellites[1].cells[0].nci = duplicateNci.satellites[0].cells[0].nci;
  assert.throws(() => createSatelliteCellPlanningContext(SMALL_CONFIG, duplicateNci), /duplicate NCI/);

  const oversizedNci = structuredClone(SMALL_IDENTITY_REGISTRY);
  oversizedNci.satellites[0].cells[0].nci = "0x1000000000";
  assert.throws(() => createSatelliteCellPlanningContext(SMALL_CONFIG, oversizedNci), /36-bit NCI/);

  const invalidPci = structuredClone(SMALL_IDENTITY_REGISTRY);
  invalidPci.satellites[0].cells[0].pci = 1008;
  assert.throws(() => createSatelliteCellPlanningContext(SMALL_CONFIG, invalidPci), /range 0\.\.1007/);
});

test("global visibility inventory is complete and two-cell spatial allocation reports overflow", () => {
  const satellite = satellites[0];
  const state = propagateSatellite(satellite, 0);
  const overhead = { id: "OVERHEAD", lat: state.lat, lon: state.lon };
  const inventory = visibleSatelliteInventoryForPosition(overhead, 0);
  assert.equal(inventory[0].satelliteId, satellite.id);
  assert.ok(inventory[0].elevationDeg > 89.99);
  assert.equal(inventory.every((candidate, index) => index === 0 || inventory[index - 1].elevationDeg >= candidate.elevationDeg), true);

  const positions = Array.from({ length: SATELLITE_L1_CAPACITY + 44 }, (_, index) => ({
    id: `G${String(index).padStart(3, "0")}`,
    lat: state.lat + ((index % 20) - 10) * 0.01,
    lon: state.lon + (Math.floor(index / 20) - 7) * 0.01,
  }));
  const table = visibleAccessTableForSatellite(satellite.id, positions, 0);
  assert.equal(table.length, positions.length, "visible inventory must not be capacity-trimmed");
  const allocation = allocateVisiblePositionsToTwoCells(satellite.id, table);
  assert.deepEqual(allocation.cells.map((cell) => cell.l1Ids.length), [CELL_L1_CAPACITY, CELL_L1_CAPACITY]);
  assert.equal(allocation.unallocatedPositionIds.length, 44);
  assert.equal(new Set(allocation.cells.flatMap((cell) => cell.l1Ids)).size, SATELLITE_L1_CAPACITY);
});
