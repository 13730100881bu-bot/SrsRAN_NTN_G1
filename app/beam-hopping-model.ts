export type BeamDirection = "downlink" | "uplink";
export type BeamClass = "analog" | "digital";
export type BeamPurpose = "ssb" | "sib" | "paging" | "rar" | "prach" | "pdu_drb";
export type BeamScheduleState = "planned" | "ready" | "active" | "preheated" | "released";

export const BEAM_SLOT_MS = 10;
export const SSB_PERIOD_MS = 20;
/** Seed assumption: one analog port performs four sequential 2.5 ms visits in a 10 ms access slot. */
export const SSB_VISITS_PER_DL_PORT_PER_OCCASION = 4;
export const SSB_SUBVISIT_MS = BEAM_SLOT_MS / SSB_VISITS_PER_DL_PORT_PER_OCCASION;
export const L1_SSB_REVISIT_MS = 80;
export const PRACH_REVISIT_MS = 640;
export const CELL_ANALOG_BEAM_PORTS = 16;
export const CELL_DIGITAL_BEAM_PORTS = 64;
export const SATELLITE_ANALOG_BEAM_PORTS = 32;
export const SATELLITE_DIGITAL_BEAM_PORTS = 128;
export const DEFAULT_DIGITAL_DOWNLINK_BEAMS = 4;
export const DEFAULT_DIGITAL_UPLINK_BEAMS = 2;
export const CANDIDATE_LOOKAHEAD_MS = 120_000;
export const PREHEAT_LEAD_MS = 60_000;
export const CANDIDATE_ELEVATION_DEG = 45;
export const HOLD_ELEVATION_DEG = 42;

export const SLOT_PHASE_LIMITS = [
  { analog: { downlink: 11, uplink: 5 }, digital: { downlink: 43, uplink: 21 } },
  { analog: { downlink: 11, uplink: 5 }, digital: { downlink: 43, uplink: 21 } },
  { analog: { downlink: 10, uplink: 6 }, digital: { downlink: 42, uplink: 22 } },
] as const;

/** Payload planning limit.  Calendar opportunity may be higher, but this seed deliberately budgets only 128 L1 per cell. */
export const CONFIGURED_CELL_L1_CAPACITY = 128;

export type L1Capacity = {
  readonly revisitMs: number;
  readonly slotsPerWindow: number;
  readonly ssbOccasionsPerWindow: number;
  readonly guaranteedDownlinkPortOccasions: number;
  readonly guaranteedDownlinkVisits: number;
  readonly configuredPerCell: number;
  readonly perCell: number;
  readonly perSatellite: number;
};

/**
 * Derives the schedulable L1 count from the analog-DL calendar itself.
 *
 * A revisit window may start at any of the three phases.  The guarantee is the
 * smallest DL sum over those alignments, not the best or average phase sum.
 */
export function deriveL1Capacity(revisitMs = L1_SSB_REVISIT_MS): L1Capacity {
  if (!Number.isInteger(revisitMs) || revisitMs <= 0 || revisitMs % SSB_PERIOD_MS !== 0) {
    throw new Error(`revisitMs must be a positive multiple of ${SSB_PERIOD_MS} ms`);
  }
  const slotsPerWindow = revisitMs / BEAM_SLOT_MS;
  const ssbOccasionsPerWindow = revisitMs / SSB_PERIOD_MS;
  const phaseSums = SLOT_PHASE_LIMITS.map((_, startPhase) => {
    let sum = 0;
    for (let occasion = 0; occasion < ssbOccasionsPerWindow; occasion += 1) {
      // One cell has an occasion every other 10 ms slot.  The other cell is
      // offset by 10 ms, but both traverse the same three possible alignments.
      sum += SLOT_PHASE_LIMITS[(startPhase + occasion * 2) % SLOT_PHASE_LIMITS.length].analog.downlink;
    }
    return sum;
  });
  const guaranteedDownlinkPortOccasions = Math.min(...phaseSums);
  const guaranteedDownlinkVisits = guaranteedDownlinkPortOccasions * SSB_VISITS_PER_DL_PORT_PER_OCCASION;
  return {
    revisitMs,
    slotsPerWindow,
    ssbOccasionsPerWindow,
    guaranteedDownlinkPortOccasions,
    guaranteedDownlinkVisits,
    configuredPerCell: CONFIGURED_CELL_L1_CAPACITY,
    perCell: Math.min(guaranteedDownlinkVisits, CONFIGURED_CELL_L1_CAPACITY),
    perSatellite: Math.min(guaranteedDownlinkVisits, CONFIGURED_CELL_L1_CAPACITY) * 2,
  };
}

export const BASELINE_L1_CAPACITY = Object.freeze(deriveL1Capacity());
export const CELL_L1_CAPACITY = BASELINE_L1_CAPACITY.perCell;
export const SATELLITE_L1_CAPACITY = BASELINE_L1_CAPACITY.perSatellite;

export type CellPositionSet = {
  readonly nci: string;
  readonly l1Ids: readonly string[];
};

export type BeamScheduleEntry = {
  satelliteId: string;
  nci: string;
  /** Fixed physical resource bank; not an NR identity. */
  cellBank?: 0 | 1;
  startTimeMs: number;
  durationMs: number;
  direction: BeamDirection;
  beamClass: BeamClass;
  portId: number;
  positionId: string;
  purpose: BeamPurpose;
  state: BeamScheduleState;
};

export type DigitalBeamDemand = {
  nci: string;
  positionId: string;
  direction: BeamDirection;
  hasPduOrDrb: boolean;
  applied: boolean;
  priority?: number;
};

export type CellBeamScheduleOptions = {
  satelliteId: string;
  cell: CellPositionSet;
  cellBank?: 0 | 1;
  startTimeMs?: number;
  durationMs?: number;
  digitalDemands?: readonly DigitalBeamDemand[];
  digitalMode?: "default" | "expanded";
};

export type PositionSatelliteCandidate = {
  satelliteId: string;
  elevationDeg: number;
  reachesCandidateElevationAtMs?: number;
  dropsBelowHoldElevationAtMs?: number;
  incumbentHoldOnly?: boolean;
  ready?: boolean;
  applied?: boolean;
};

export type PositionTransferState = "candidate" | "preheated" | "overlap" | "activated" | "delayed" | "coverage_failure";

export type PositionTransferEvent = {
  positionId: string;
  sourceSatelliteId: string;
  sourceNci: string;
  sourcePci: number;
  targetSatelliteId: string | null;
  targetNci: string | null;
  targetPci: number | null;
  preheatStartMs: number;
  activationEpochMs: number;
  state: PositionTransferState;
};

export type PlanPositionTransferOptions = Omit<PositionTransferEvent, "preheatStartMs" | "activationEpochMs" | "state"> & {
  nowMs: number;
  requestedActivationMs: number;
  sourceElevationDeg: number;
  targetElevationDeg: number | null;
  targetReady: boolean;
  targetApplied: boolean;
};

export function slotPhase(slotIndex: number) {
  return SLOT_PHASE_LIMITS[((slotIndex % SLOT_PHASE_LIMITS.length) + SLOT_PHASE_LIMITS.length) % SLOT_PHASE_LIMITS.length];
}

function assertSlotAligned(value: number, label: string) {
  if (!Number.isInteger(value) || value < 0 || value % BEAM_SLOT_MS !== 0) {
    throw new Error(`${label} must be a non-negative multiple of ${BEAM_SLOT_MS} ms`);
  }
}

function eligibleDigitalDemands(options: CellBeamScheduleOptions, direction: BeamDirection) {
  return (options.digitalDemands ?? [])
    .filter((demand) =>
      demand.nci.toUpperCase() === options.cell.nci.toUpperCase()
      && demand.direction === direction
      && demand.hasPduOrDrb
      && demand.applied,
    )
    .sort((left, right) => (right.priority ?? 0) - (left.priority ?? 0) || left.positionId.localeCompare(right.positionId));
}

/** Builds one NCI's deterministic calendar within its reserved 16/64 bank. */
export function createCellBeamSchedule(options: CellBeamScheduleOptions): BeamScheduleEntry[] {
  const startTimeMs = options.startTimeMs ?? 0;
  const durationMs = options.durationMs ?? PRACH_REVISIT_MS;
  assertSlotAligned(startTimeMs, "startTimeMs");
  assertSlotAligned(durationMs, "durationMs");
  if (durationMs === 0) return [];
  if (options.cell.l1Ids.length === 0 || options.cell.l1Ids.length > CELL_L1_CAPACITY) {
    throw new Error(`Onboard NR cell ${options.cell.nci} must contain between 1 and ${CELL_L1_CAPACITY} L1 positions`);
  }

  const l1Ids = [...options.cell.l1Ids].sort();
  const downlinkDemands = eligibleDigitalDemands(options, "downlink");
  const uplinkDemands = eligibleDigitalDemands(options, "uplink");
  const entries: BeamScheduleEntry[] = [];
  const firstSlot = startTimeMs / BEAM_SLOT_MS;
  const slotCount = durationMs / BEAM_SLOT_MS;

  for (let localSlot = 0; localSlot < slotCount; localSlot += 1) {
    const absoluteSlot = firstSlot + localSlot;
    const slotStartMs = absoluteSlot * BEAM_SLOT_MS;
    const phase = slotPhase(absoluteSlot);

    const cellBank = options.cellBank ?? 0;
    if (absoluteSlot % (SSB_PERIOD_MS / BEAM_SLOT_MS) === cellBank) {
      const occasionIndex = Math.floor((absoluteSlot - cellBank) / (SSB_PERIOD_MS / BEAM_SLOT_MS));
      const completeCycles = Math.floor(occasionIndex / SLOT_PHASE_LIMITS.length);
      let visitsBefore = completeCycles
        * SLOT_PHASE_LIMITS.reduce((sum, limit) => sum + limit.analog.downlink, 0)
        * SSB_VISITS_PER_DL_PORT_PER_OCCASION;
      for (let previous = completeCycles * SLOT_PHASE_LIMITS.length; previous < occasionIndex; previous += 1) {
        const previousSlot = cellBank + previous * (SSB_PERIOD_MS / BEAM_SLOT_MS);
        visitsBefore += slotPhase(previousSlot).analog.downlink * SSB_VISITS_PER_DL_PORT_PER_OCCASION;
      }
      for (let portId = 0; portId < phase.analog.downlink; portId += 1) {
        for (let subVisit = 0; subVisit < SSB_VISITS_PER_DL_PORT_PER_OCCASION; subVisit += 1) {
          const visitIndex = portId * SSB_VISITS_PER_DL_PORT_PER_OCCASION + subVisit;
          entries.push({
            satelliteId: options.satelliteId,
            nci: options.cell.nci,
            cellBank: options.cellBank,
            startTimeMs: slotStartMs + subVisit * SSB_SUBVISIT_MS,
            durationMs: SSB_SUBVISIT_MS,
            direction: "downlink",
            beamClass: "analog",
            portId,
            positionId: l1Ids[(visitsBefore + visitIndex) % l1Ids.length],
            purpose: "ssb",
            state: "planned",
          });
        }
      }
    }

    // Two receive visits per 10 ms slot cover 128 positions within one 640 ms frame.
    const prachOffset = (absoluteSlot % (PRACH_REVISIT_MS / BEAM_SLOT_MS)) * 2;
    for (let offset = 0; offset < 2 && prachOffset + offset < l1Ids.length; offset += 1) {
      entries.push({
        satelliteId: options.satelliteId,
        nci: options.cell.nci,
        cellBank: options.cellBank,
        startTimeMs: slotStartMs,
        durationMs: BEAM_SLOT_MS,
        direction: "uplink",
        beamClass: "analog",
        portId: phase.analog.downlink + offset,
        positionId: l1Ids[prachOffset + offset],
        purpose: "prach",
        state: "planned",
      });
    }

    const downlinkLimit = options.digitalMode === "expanded"
      ? phase.digital.downlink
      : Math.min(DEFAULT_DIGITAL_DOWNLINK_BEAMS, phase.digital.downlink);
    const uplinkLimit = options.digitalMode === "expanded"
      ? phase.digital.uplink
      : Math.min(DEFAULT_DIGITAL_UPLINK_BEAMS, phase.digital.uplink);

    for (const [offset, demand] of downlinkDemands.slice(0, downlinkLimit).entries()) {
      entries.push({
        satelliteId: options.satelliteId,
        nci: options.cell.nci,
        cellBank: options.cellBank,
        startTimeMs: slotStartMs,
        durationMs: BEAM_SLOT_MS,
        direction: "downlink",
        beamClass: "digital",
        portId: offset,
        positionId: demand.positionId,
        purpose: "pdu_drb",
        state: "ready",
      });
    }
    for (const [offset, demand] of uplinkDemands.slice(0, uplinkLimit).entries()) {
      entries.push({
        satelliteId: options.satelliteId,
        nci: options.cell.nci,
        cellBank: options.cellBank,
        startTimeMs: slotStartMs,
        durationMs: BEAM_SLOT_MS,
        direction: "uplink",
        beamClass: "digital",
        portId: phase.digital.downlink + offset,
        positionId: demand.positionId,
        purpose: "pdu_drb",
        state: "ready",
      });
    }
  }
  return entries;
}

/** Maps two fixed 16/64 NCI banks into disjoint satellite-wide 32/128 ports. */
export function mergeSatelliteCellSchedules(cellSchedules: readonly (readonly BeamScheduleEntry[])[]) {
  const nonEmpty = cellSchedules.filter((schedule) => schedule.length > 0);
  const ncis = [...new Set(nonEmpty.flatMap((schedule) => schedule.map((entry) => entry.nci)))].sort();
  if (ncis.length > 2) throw new Error("A satellite may carry at most two active NCIs");
  const satellites = new Set(nonEmpty.flatMap((schedule) => schedule.map((entry) => entry.satelliteId)));
  if (satellites.size > 1) throw new Error("All merged cell calendars must target the same satellite");
  const explicitBanks = new Map<string, 0 | 1>();
  for (const schedule of nonEmpty) {
    for (const entry of schedule) {
      if (entry.cellBank === undefined) continue;
      const existing = explicitBanks.get(entry.nci);
      if (existing !== undefined && existing !== entry.cellBank) {
        throw new Error(`NCI ${entry.nci} is mapped to two physical resource banks`);
      }
      explicitBanks.set(entry.nci, entry.cellBank);
    }
  }
  let bankByNci: Map<string, number>;
  if (explicitBanks.size === ncis.length) {
    bankByNci = new Map(explicitBanks);
  } else if (ncis.length === 2 && explicitBanks.size === 0) {
    // Legacy two-populated-calendar call: lexical order is deterministic.
    bankByNci = new Map(ncis.map((nci, index) => [nci, index]));
  } else {
    throw new Error("cellBank is required when an empty companion bank is omitted");
  }
  if (new Set(bankByNci.values()).size !== bankByNci.size) {
    throw new Error("Two NCIs cannot occupy the same physical resource bank");
  }
  const merged = nonEmpty.flatMap((schedule) => schedule.map((entry) => ({
    ...entry,
    portId: entry.portId + (bankByNci.get(entry.nci)! * (
      entry.beamClass === "analog" ? CELL_ANALOG_BEAM_PORTS : CELL_DIGITAL_BEAM_PORTS
    )),
  })));
  const occupied = new Set<string>();
  for (const entry of merged) {
    const poolLimit = entry.beamClass === "analog" ? SATELLITE_ANALOG_BEAM_PORTS : SATELLITE_DIGITAL_BEAM_PORTS;
    if (entry.portId < 0 || entry.portId >= poolLimit) throw new Error(`${entry.beamClass} port ${entry.portId} exceeds satellite pool`);
    const key = `${entry.startTimeMs}:${entry.beamClass}:${entry.portId}`;
    if (occupied.has(key)) throw new Error(`Duplicate satellite port occupation ${key}`);
    occupied.add(key);
  }
  return merged;
}

/** Keeps the full visibility inventory; service capacity is applied only by the allocator. */
export function createCandidateInventory(
  candidates: readonly PositionSatelliteCandidate[],
  nowMs: number,
  lookaheadMs = CANDIDATE_LOOKAHEAD_MS,
) {
  return candidates
    .filter((candidate) => {
      if (candidate.elevationDeg >= CANDIDATE_ELEVATION_DEG || candidate.incumbentHoldOnly) return true;
      const entry = candidate.reachesCandidateElevationAtMs;
      return entry !== undefined && entry >= nowMs && entry <= nowMs + lookaheadMs;
    })
    .sort((left, right) =>
      Number(Boolean(right.incumbentHoldOnly)) - Number(Boolean(left.incumbentHoldOnly))
      || right.elevationDeg - left.elevationDeg
      || left.satelliteId.localeCompare(right.satelliteId),
    );
}

export function alignToActivationEpoch(timeMs: number) {
  if (!Number.isFinite(timeMs) || timeMs < 0) throw new Error("timeMs must be non-negative");
  return Math.ceil(timeMs / PRACH_REVISIT_MS) * PRACH_REVISIT_MS;
}

/** Plans one L1 transfer.  Different NCI/PCI permits a bounded RF overlap. */
export function planPositionTransfer(options: PlanPositionTransferOptions): PositionTransferEvent {
  const activationEpochMs = alignToActivationEpoch(options.requestedActivationMs);
  const preheatStartMs = Math.max(0, activationEpochMs - PREHEAT_LEAD_MS);
  let state: PositionTransferState;
  const targetExists = Boolean(options.targetSatelliteId && options.targetNci && options.targetPci !== null && options.targetElevationDeg !== null);
  const targetIdentityIsDistinct = targetExists
    && options.targetNci !== options.sourceNci
    && options.targetPci !== options.sourcePci;
  const targetRadioEligible = targetExists && options.targetElevationDeg! >= CANDIDATE_ELEVATION_DEG;
  if (!targetExists) {
    state = options.sourceElevationDeg < HOLD_ELEVATION_DEG ? "coverage_failure" : "delayed";
  } else if (options.nowMs < preheatStartMs) {
    state = "candidate";
  } else if (!targetIdentityIsDistinct || !targetRadioEligible) {
    state = options.nowMs >= activationEpochMs
      ? options.sourceElevationDeg < HOLD_ELEVATION_DEG ? "coverage_failure" : "delayed"
      : "candidate";
  } else if (!options.targetReady || !options.targetApplied) {
    state = options.nowMs >= activationEpochMs && options.sourceElevationDeg < HOLD_ELEVATION_DEG
      ? "coverage_failure"
      : options.nowMs >= activationEpochMs ? "delayed" : "preheated";
  } else if (options.nowMs < activationEpochMs) {
    state = "overlap";
  } else {
    state = "activated";
  }
  return {
    positionId: options.positionId,
    sourceSatelliteId: options.sourceSatelliteId,
    sourceNci: options.sourceNci,
    sourcePci: options.sourcePci,
    targetSatelliteId: options.targetSatelliteId,
    targetNci: options.targetNci,
    targetPci: options.targetPci,
    preheatStartMs,
    activationEpochMs,
    state,
  };
}
