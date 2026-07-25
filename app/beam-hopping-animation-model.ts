import {
  createCellBeamSchedule,
  L1_SSB_REVISIT_MS,
  SSB_SUBVISIT_MS,
  type BeamScheduleEntry,
} from "./beam-hopping-model";

export type BeamAnimationCell = {
  readonly id: string;
  readonly lat: number;
  readonly lon: number;
  readonly cellBank: 0 | 1;
};

export type BeamAnimationFrame = {
  readonly index: number;
  readonly startMs: number;
  readonly endMs: number;
  readonly cellBank: 0 | 1 | null;
  readonly beamCount: number;
  readonly activePositionIds: readonly string[];
};

export const BEAM_ANIMATION_FRAME_MS = SSB_SUBVISIT_MS;
export const BEAM_ANIMATION_CYCLE_MS = L1_SSB_REVISIT_MS;

function accessEntriesForBank(
  cells: readonly BeamAnimationCell[],
  satelliteId: string,
  nci: string,
  cellBank: 0 | 1,
) {
  const positionIds = cells
    .filter((cell) => cell.cellBank === cellBank)
    .map((cell) => cell.id);
  if (positionIds.length === 0) return [];

  return createCellBeamSchedule({
    satelliteId,
    cell: { nci, l1Ids: positionIds },
    cellBank,
    startTimeMs: 0,
    durationMs: BEAM_ANIMATION_CYCLE_MS,
  }).filter((entry) =>
    entry.beamClass === "analog"
    && entry.direction === "downlink"
    && entry.purpose === "ssb",
  );
}

export function createBeamAnimationFrames(
  cells: readonly BeamAnimationCell[],
  satelliteId: string,
  nciByBank: readonly [string, string],
): readonly BeamAnimationFrame[] {
  const entries = ([
    ...accessEntriesForBank(cells, satelliteId, nciByBank[0], 0),
    ...accessEntriesForBank(cells, satelliteId, nciByBank[1], 1),
  ] as BeamScheduleEntry[]).sort((left, right) =>
    left.startTimeMs - right.startTimeMs
    || (left.cellBank ?? 0) - (right.cellBank ?? 0)
    || left.portId - right.portId
    || left.positionId.localeCompare(right.positionId),
  );

  const entriesByFrame = new Map<number, BeamScheduleEntry[]>();
  for (const entry of entries) {
    const frameIndex = Math.round(entry.startTimeMs / BEAM_ANIMATION_FRAME_MS);
    const frameEntries = entriesByFrame.get(frameIndex) ?? [];
    frameEntries.push(entry);
    entriesByFrame.set(frameIndex, frameEntries);
  }

  const frameCount = Math.round(BEAM_ANIMATION_CYCLE_MS / BEAM_ANIMATION_FRAME_MS);
  return Array.from({ length: frameCount }, (_, index) => {
    const frameEntries = entriesByFrame.get(index) ?? [];
    const banks = new Set<0 | 1>(
      frameEntries
        .map((entry) => entry.cellBank)
        .filter((bank): bank is 0 | 1 => bank !== undefined),
    );
    return {
      index,
      startMs: index * BEAM_ANIMATION_FRAME_MS,
      endMs: (index + 1) * BEAM_ANIMATION_FRAME_MS,
      cellBank: banks.size === 1 ? [...banks][0]! : null,
      beamCount: frameEntries.length,
      activePositionIds: [...new Set(frameEntries.map((entry) => entry.positionId))].sort(),
    };
  });
}
