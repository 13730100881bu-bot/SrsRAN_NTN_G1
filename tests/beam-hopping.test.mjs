import assert from "node:assert/strict";
import test from "node:test";
import { l1Catalog } from "../app/beam-catalog.ts";
import {
  BEAM_SLOT_MS,
  CELL_ANALOG_BEAM_PORTS,
  CELL_DIGITAL_BEAM_PORTS,
  CELL_L1_CAPACITY,
  L1_SSB_REVISIT_MS,
  PRACH_REVISIT_MS,
  SATELLITE_ANALOG_BEAM_PORTS,
  SATELLITE_DIGITAL_BEAM_PORTS,
  SATELLITE_L1_CAPACITY,
  SLOT_PHASE_LIMITS,
  SSB_PERIOD_MS,
  SSB_SUBVISIT_MS,
  SSB_VISITS_PER_DL_PORT_PER_OCCASION,
  alignToActivationEpoch,
  createCandidateInventory,
  createCellBeamSchedule,
  deriveL1Capacity,
  mergeSatelliteCellSchedules,
  planPositionTransfer,
  slotPhase,
} from "../app/beam-hopping-model.ts";

const cell = (nci, count = CELL_L1_CAPACITY) => ({ nci, l1Ids: l1Catalog.slice(0, count).map(({ id }) => id) });

function bySlot(entries) {
  return Map.groupBy(entries, (entry) => Math.floor(entry.startTimeMs / BEAM_SLOT_MS) * BEAM_SLOT_MS);
}

function circularMaxGap(times, periodMs) {
  const sorted = [...new Set(times)].sort((left, right) => left - right);
  let max = 0;
  for (let index = 0; index < sorted.length; index += 1) {
    const next = index + 1 < sorted.length ? sorted[index + 1] : sorted[0] + periodMs;
    max = Math.max(max, next - sorted[index]);
  }
  return max;
}

test("128/256 L1 budget fits inside the worst 80 ms analog-DL calendar", () => {
  assert.equal(BEAM_SLOT_MS, 10);
  assert.equal(SSB_PERIOD_MS, 20);
  assert.equal(L1_SSB_REVISIT_MS, 80);
  assert.equal(PRACH_REVISIT_MS, 640);
  assert.deepEqual(deriveL1Capacity(40), {
    revisitMs: 40, slotsPerWindow: 4, ssbOccasionsPerWindow: 2,
    guaranteedDownlinkPortOccasions: 21, guaranteedDownlinkVisits: 84,
    configuredPerCell: 128, perCell: 84, perSatellite: 168,
  });
  assert.deepEqual(deriveL1Capacity(80), {
    revisitMs: 80, slotsPerWindow: 8, ssbOccasionsPerWindow: 4,
    guaranteedDownlinkPortOccasions: 42, guaranteedDownlinkVisits: 168,
    configuredPerCell: 128, perCell: 128, perSatellite: 256,
  });
  assert.deepEqual(deriveL1Capacity(160), {
    revisitMs: 160, slotsPerWindow: 16, ssbOccasionsPerWindow: 8,
    guaranteedDownlinkPortOccasions: 85, guaranteedDownlinkVisits: 340,
    configuredPerCell: 128, perCell: 128, perSatellite: 256,
  });
  assert.equal(SSB_VISITS_PER_DL_PORT_PER_OCCASION, 4);
  assert.equal(SSB_SUBVISIT_MS, 2.5);
  assert.equal(CELL_L1_CAPACITY, 128);
  assert.equal(SATELLITE_L1_CAPACITY, 256);
});

test("10 ms phases reserve per-cell 16 analog and 64 digital ports", () => {
  assert.equal(CELL_ANALOG_BEAM_PORTS, 16);
  assert.equal(CELL_DIGITAL_BEAM_PORTS, 64);
  assert.equal(SATELLITE_ANALOG_BEAM_PORTS, 32);
  assert.equal(SATELLITE_DIGITAL_BEAM_PORTS, 128);
  assert.deepEqual(SLOT_PHASE_LIMITS, [
    { analog: { downlink: 11, uplink: 5 }, digital: { downlink: 43, uplink: 21 } },
    { analog: { downlink: 11, uplink: 5 }, digital: { downlink: 43, uplink: 21 } },
    { analog: { downlink: 10, uplink: 6 }, digital: { downlink: 42, uplink: 22 } },
  ]);
  assert.equal(SLOT_PHASE_LIMITS.reduce((sum, phase) => sum + phase.analog.downlink, 0), 32);
  assert.equal(SLOT_PHASE_LIMITS.reduce((sum, phase) => sum + phase.analog.uplink, 0), 16);
  assert.equal(SLOT_PHASE_LIMITS.reduce((sum, phase) => sum + phase.digital.downlink, 0), 128);
  assert.equal(SLOT_PHASE_LIMITS.reduce((sum, phase) => sum + phase.digital.uplink, 0), 64);
});

test("idle L1 positions receive SSB in 80 ms and a mapped PRACH beam in 640 ms", () => {
  const onboardCell = cell("0x200000001");
  const entries = createCellBeamSchedule({
    satelliteId: "P01-S01", cell: onboardCell, cellBank: 0, durationMs: PRACH_REVISIT_MS * 2,
  });
  const ssb = entries.filter((entry) => entry.purpose === "ssb");
  const prach = entries.filter((entry) => entry.purpose === "prach");

  for (const l1Id of onboardCell.l1Ids) {
    const ssbTimes = ssb.filter((entry) => entry.positionId === l1Id).map((entry) => entry.startTimeMs);
    const prachTimes = prach.filter((entry) => entry.positionId === l1Id).map((entry) => entry.startTimeMs);
    assert.ok(ssbTimes.length > 0, `${l1Id} is missing SSB`);
    assert.ok(prachTimes.length > 0, `${l1Id} is missing PRACH`);
    assert.ok(ssbTimes.slice(1).every((timeMs, index) => timeMs - ssbTimes[index] <= L1_SSB_REVISIT_MS), `${l1Id} SSB gap`);
    assert.ok(circularMaxGap(prachTimes, PRACH_REVISIT_MS * 2) <= PRACH_REVISIT_MS, `${l1Id} PRACH gap`);
  }
  const ssbOccasions = [...new Set(ssb.map((entry) => Math.floor(entry.startTimeMs / BEAM_SLOT_MS) * BEAM_SLOT_MS))].sort((a, b) => a - b);
  assert.equal(ssbOccasions.every((timeMs) => timeMs % SSB_PERIOD_MS === 0), true);
  assert.equal(ssbOccasions.every((timeMs, index) => index === 0 || timeMs - ssbOccasions[index - 1] === SSB_PERIOD_MS), true);

  for (const [timeMs, slotEntries] of bySlot(entries)) {
    const phase = slotPhase(timeMs / BEAM_SLOT_MS);
    const analog = slotEntries.filter((entry) => entry.beamClass === "analog");
    const dl = analog.filter((entry) => entry.direction === "downlink");
    const ul = analog.filter((entry) => entry.direction === "uplink");
    assert.ok(dl.length <= phase.analog.downlink * SSB_VISITS_PER_DL_PORT_PER_OCCASION);
    assert.ok(ul.length <= phase.analog.uplink);
    assert.ok(new Set(dl.map((entry) => entry.portId)).size <= phase.analog.downlink);
    assert.equal(new Set(ul.map((entry) => entry.portId)).size, ul.length);
    assert.equal(Math.max(...analog.map((entry) => entry.portId)) < CELL_ANALOG_BEAM_PORTS, true);
  }
});

test("digital calendar excludes control-only and unapplied demand, then defaults to 4 DL + 2 UL", () => {
  const onboardCell = cell("0x200000001", 1);
  const demands = [
    ...Array.from({ length: 8 }, (_, index) => ({
      nci: onboardCell.nci, positionId: `DL-${index}`, direction: "downlink",
      hasPduOrDrb: index !== 6, applied: index !== 7, priority: 100 - index,
    })),
    ...Array.from({ length: 5 }, (_, index) => ({
      nci: onboardCell.nci, positionId: `UL-${index}`, direction: "uplink",
      hasPduOrDrb: true, applied: index !== 4, priority: 100 - index,
    })),
  ];
  const entries = createCellBeamSchedule({
    satelliteId: "P01-S01", cell: onboardCell, cellBank: 0, durationMs: BEAM_SLOT_MS, digitalDemands: demands,
  }).filter((entry) => entry.beamClass === "digital");

  assert.equal(entries.filter((entry) => entry.direction === "downlink").length, 4);
  assert.equal(entries.filter((entry) => entry.direction === "uplink").length, 2);
  assert.equal(entries.some((entry) => ["DL-6", "DL-7", "UL-4"].includes(entry.positionId)), false);
  assert.equal(entries.every((entry) => entry.state === "ready"), true);
});

test("expanded digital mode observes a phase's 64-port split", () => {
  const onboardCell = cell("0x200000001", 1);
  const demands = [
    ...Array.from({ length: 50 }, (_, index) => ({
      nci: onboardCell.nci, positionId: `DL-${index}`, direction: "downlink", hasPduOrDrb: true, applied: true,
    })),
    ...Array.from({ length: 30 }, (_, index) => ({
      nci: onboardCell.nci, positionId: `UL-${index}`, direction: "uplink", hasPduOrDrb: true, applied: true,
    })),
  ];
  const digital = createCellBeamSchedule({
    satelliteId: "P01-S01", cell: onboardCell, cellBank: 0, startTimeMs: 20,
    durationMs: BEAM_SLOT_MS, digitalDemands: demands, digitalMode: "expanded",
  }).filter((entry) => entry.beamClass === "digital");
  assert.equal(digital.filter((entry) => entry.direction === "downlink").length, 42);
  assert.equal(digital.filter((entry) => entry.direction === "uplink").length, 22);
  assert.equal(new Set(digital.map((entry) => entry.portId)).size, CELL_DIGITAL_BEAM_PORTS);
});

test("fixed banks do not compact when only upper bank has an active calendar", () => {
  const upperCell = cell("0x200000002", 1);
  const demands = [{
    nci: upperCell.nci, positionId: "DL-0", direction: "downlink", hasPduOrDrb: true, applied: true,
  }];
  const upper = createCellBeamSchedule({
    satelliteId: "P01-S01", cell: upperCell, cellBank: 1, durationMs: BEAM_SLOT_MS, digitalDemands: demands,
  });
  const merged = mergeSatelliteCellSchedules([upper]);
  const analog = merged.filter((entry) => entry.beamClass === "analog");
  const digital = merged.filter((entry) => entry.beamClass === "digital");
  assert.ok(analog.every((entry) => entry.portId >= CELL_ANALOG_BEAM_PORTS));
  assert.ok(digital.every((entry) => entry.portId >= CELL_DIGITAL_BEAM_PORTS));
  assert.ok(Math.max(...analog.map((entry) => entry.portId)) < SATELLITE_ANALOG_BEAM_PORTS);
  assert.ok(Math.max(...digital.map((entry) => entry.portId)) < SATELLITE_DIGITAL_BEAM_PORTS);
});

test("two explicit per-NCI banks map to disjoint satellite 32/128 ports", () => {
  const cells = [cell("0x200000001"), cell("0x200000002")];
  const calendars = cells.map((onboardCell, bank) => {
    const demands = [
      ...Array.from({ length: 50 }, (_, index) => ({
        nci: onboardCell.nci, positionId: `${onboardCell.nci}-DL-${index}`,
        direction: "downlink", hasPduOrDrb: true, applied: true,
      })),
      ...Array.from({ length: 30 }, (_, index) => ({
        nci: onboardCell.nci, positionId: `${onboardCell.nci}-UL-${index}`,
        direction: "uplink", hasPduOrDrb: true, applied: true,
      })),
    ];
    return createCellBeamSchedule({
      satelliteId: "P01-S01", cell: onboardCell, cellBank: bank,
      durationMs: BEAM_SLOT_MS, digitalDemands: demands, digitalMode: "expanded",
    });
  });
  const merged = mergeSatelliteCellSchedules(calendars);
  const analog = merged.filter((entry) => entry.beamClass === "analog");
  const digital = merged.filter((entry) => entry.beamClass === "digital");
  assert.equal(new Set(analog.map((entry) => `${entry.startTimeMs}:${entry.portId}`)).size, analog.length);
  assert.equal(Math.max(...analog.map((entry) => entry.portId)) < SATELLITE_ANALOG_BEAM_PORTS, true);
  assert.equal(digital.length, SATELLITE_DIGITAL_BEAM_PORTS);
  assert.equal(new Set(analog.map((entry) => `${entry.startTimeMs}:${entry.portId}`)).size, analog.length);
  assert.equal(new Set(digital.map((entry) => entry.portId)).size, digital.length);
});

test("candidate inventory is deterministic and never clipped to service capacity", () => {
  const inventory = createCandidateInventory([
    { satelliteId: "P01-S03", elevationDeg: 44, reachesCandidateElevationAtMs: 100_000 },
    { satelliteId: "P01-S02", elevationDeg: 50 },
    { satelliteId: "P01-S01", elevationDeg: 50 },
    { satelliteId: "P01-S04", elevationDeg: 40, reachesCandidateElevationAtMs: 130_000 },
  ], 0);
  assert.deepEqual(inventory.map((candidate) => candidate.satelliteId), ["P01-S01", "P01-S02", "P01-S03"]);
});

test("position transfer permits ready different-PCI overlap and aligns activation to 640 ms", () => {
  assert.equal(alignToActivationEpoch(1_001), 1_280);
  const base = {
    positionId: "A0001",
    sourceSatelliteId: "P01-S01", sourceNci: "0x200000001", sourcePci: 0,
    targetSatelliteId: "P02-S01", targetNci: "0x200000049", targetPci: 48,
    requestedActivationMs: 61_001, sourceElevationDeg: 44, targetElevationDeg: 50,
  };
  const activation = alignToActivationEpoch(base.requestedActivationMs);
  assert.equal(planPositionTransfer({ ...base, nowMs: 0, targetReady: false, targetApplied: false }).state, "candidate");
  assert.equal(planPositionTransfer({ ...base, nowMs: 2_000, targetReady: false, targetApplied: false }).state, "preheated");
  assert.equal(planPositionTransfer({ ...base, nowMs: 2_000, targetReady: true, targetApplied: true }).state, "overlap");
  assert.equal(planPositionTransfer({ ...base, nowMs: activation, targetReady: true, targetApplied: true }).state, "activated");
  assert.equal(planPositionTransfer({
    ...base, nowMs: activation, targetElevationDeg: 44, targetReady: true, targetApplied: true,
  }).state, "delayed");
  assert.equal(planPositionTransfer({
    ...base, nowMs: activation, targetPci: base.sourcePci, targetReady: true, targetApplied: true,
  }).state, "delayed");
  assert.equal(planPositionTransfer({
    ...base, nowMs: activation, targetNci: base.sourceNci, targetReady: true, targetApplied: true,
  }).state, "delayed");
  assert.equal(planPositionTransfer({
    ...base, targetSatelliteId: null, targetNci: null, targetPci: null,
    targetElevationDeg: null, nowMs: activation, sourceElevationDeg: 41, targetReady: false, targetApplied: false,
  }).state, "coverage_failure");
});
