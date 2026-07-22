/**
 * Incremental, checkpointable NTN assignment timeline.
 *
 * The complete visibility inventory (release threshold, normally 42 degrees)
 * and the candidates allowed to take a new position (entry threshold, normally
 * 45 degrees) are kept separately. Existing owners remain eligible until they
 * leave the complete inventory. A deterministic augmenting-path update repairs
 * only the current matching after each threshold-event group.
 *
 * Equal-cost branching is solved exactly inside its connected candidate/capacity
 * component. Independent positions and satellites are never rebuilt. The legacy
 * `metrics.globalFallbackGroups` counter remains for evidence compatibility and
 * stays zero on this path.
 */

import {matchCandidatesToCapacity} from './constellation_assignment.mjs';

const EVENT_ORDER = Object.freeze({
  release_exit: 0,
  entry_exit: 1,
  release_enter: 2,
  entry_enter: 3
});

const CHECKPOINT_SCHEMA_VERSION = 1;

const FAILURE_REASON = Object.freeze({
  noVisibleCandidate: 'no_visible_candidate',
  scheduleOverflow: 'schedule_overflow',
  cellPartitionOverflow: 'cell_partition_overflow'
});

function fail(message) {
  throw new Error(message);
}

function compareText(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function assertObject(value, context) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    fail(`${context} must be an object`);
  }
}

function assertTime(value, context) {
  if (!Number.isSafeInteger(value)) fail(`${context} must be integer microseconds`);
}

function assertPositiveInteger(value, context) {
  if (!Number.isInteger(value) || value <= 0) fail(`${context} must be a positive integer`);
}

function pairKey(positionId, satelliteId) {
  return `${positionId}\0${satelliteId}`;
}

function validateNci(nci, context) {
  let numeric;
  try {
    if (typeof nci === 'number' && (!Number.isSafeInteger(nci) || nci < 0)) fail(`${context} is invalid`);
    if (typeof nci !== 'number' && (typeof nci !== 'string' || nci.length === 0)) fail(`${context} is invalid`);
    numeric = BigInt(nci);
  } catch {
    fail(`${context} must be a numeric 36-bit NCI`);
  }
  if (numeric < 0n || numeric >= (1n << 36n)) fail(`${context} must fit in 36 bits`);
  return numeric;
}

function normalizePositionIds(positionIds) {
  if (!Array.isArray(positionIds)) fail('positionIds must be an array');
  const ordered = [...positionIds].sort(compareText);
  if (ordered.some((positionId) => typeof positionId !== 'string' || positionId.length === 0)) {
    fail('positionIds must contain non-empty strings');
  }
  if (new Set(ordered).size !== ordered.length) fail('positionIds must be unique');
  return ordered;
}

function normalizeRegistry(registry) {
  const source = Array.isArray(registry) ? registry : registry?.satellites;
  if (!Array.isArray(source)) fail('registry must be an array or contain a satellites array');
  const seenSatellites = new Set();
  const seenNcis = new Set();
  const normalized = source.map((entry, entryIndex) => {
    assertObject(entry, `registry[${entryIndex}]`);
    const satelliteId = entry.satelliteId ?? entry.satellite_id;
    if (typeof satelliteId !== 'string' || satelliteId.length === 0) {
      fail(`registry[${entryIndex}] satellite id must be a non-empty string`);
    }
    if (seenSatellites.has(satelliteId)) fail(`registry contains duplicate satellite '${satelliteId}'`);
    seenSatellites.add(satelliteId);
    if (!Array.isArray(entry.cells) || entry.cells.length !== 2) {
      fail(`registry satellite '${satelliteId}' must contain exactly two cells`);
    }
    const seenBanks = new Set();
    const cells = entry.cells.map((cell, cellIndex) => {
      assertObject(cell, `registry satellite '${satelliteId}' cell ${cellIndex}`);
      if (cell.bank !== 0 && cell.bank !== 1) {
        fail(`registry satellite '${satelliteId}' cell ${cellIndex} bank must be 0 or 1`);
      }
      if (seenBanks.has(cell.bank)) fail(`registry satellite '${satelliteId}' contains duplicate bank ${cell.bank}`);
      seenBanks.add(cell.bank);
      const nciKey = validateNci(cell.nci, `registry satellite '${satelliteId}' cell ${cellIndex} nci`).toString();
      if (seenNcis.has(nciKey)) fail(`registry contains duplicate NCI '${cell.nci}'`);
      seenNcis.add(nciKey);
      if (!Number.isInteger(cell.pci) || cell.pci < 0 || cell.pci > 1007) {
        fail(`registry satellite '${satelliteId}' cell ${cellIndex} pci must be in 0..1007`);
      }
      return {bank: cell.bank, nci: cell.nci, pci: cell.pci};
    }).sort((left, right) => left.bank - right.bank);
    return {satelliteId, cells};
  }).sort((left, right) => compareText(left.satelliteId, right.satelliteId));
  return normalized;
}

function registryMap(registry) {
  return new Map(registry.map((entry) => [entry.satelliteId, entry]));
}

function emptyCandidateMap(positionIds) {
  return new Map(positionIds.map((positionId) => [positionId, new Map()]));
}

function validateScore(value, context) {
  const score = value ?? 0;
  if (!Number.isFinite(score)) fail(`${context} must be finite`);
  return score;
}

function normalizeInitialCandidates(initialVisibility, positionIds, registryBySatellite) {
  if (!Array.isArray(initialVisibility)) fail('initialVisibility must be an array');
  const positionSet = new Set(positionIds);
  const complete = emptyCandidateMap(positionIds);
  const entry = emptyCandidateMap(positionIds);
  const seenPairs = new Set();
  for (const [index, item] of initialVisibility.entries()) {
    assertObject(item, `initialVisibility[${index}]`);
    if (!positionSet.has(item.positionId)) fail(`initialVisibility[${index}].positionId is unknown`);
    if (typeof item.satelliteId !== 'string' || !registryBySatellite.has(item.satelliteId)) {
      fail(`initialVisibility[${index}].satelliteId is unknown`);
    }
    if (typeof item.aboveEntry !== 'boolean' || typeof item.aboveRelease !== 'boolean') {
      fail(`initialVisibility[${index}] threshold states must be boolean`);
    }
    if (item.aboveEntry && !item.aboveRelease) {
      fail(`initialVisibility[${index}] cannot be above entry but below release`);
    }
    const key = pairKey(item.positionId, item.satelliteId);
    if (seenPairs.has(key)) fail(`initialVisibility contains duplicate pair '${item.positionId}/${item.satelliteId}'`);
    seenPairs.add(key);
    const score = validateScore(item.score, `initialVisibility[${index}].score`);
    if (item.aboveRelease) complete.get(item.positionId).set(item.satelliteId, score);
    if (item.aboveEntry) entry.get(item.positionId).set(item.satelliteId, score);
  }
  return {complete, entry};
}

function normalizeEvents(events, positionIds, registryBySatellite, startTimeUs, endTimeUs) {
  if (!Array.isArray(events)) fail('events must be an array');
  const positionSet = new Set(positionIds);
  const normalized = events.map((event, index) => {
    assertObject(event, `events[${index}]`);
    assertTime(event.timeUs, `events[${index}].timeUs`);
    if (event.timeUs < startTimeUs || event.timeUs >= endTimeUs) {
      fail(`events[${index}] lies outside the timeline`);
    }
    if (!Object.hasOwn(EVENT_ORDER, event.kind)) fail(`events[${index}].kind is invalid`);
    if (!positionSet.has(event.positionId)) fail(`events[${index}].positionId is unknown`);
    if (typeof event.satelliteId !== 'string' || !registryBySatellite.has(event.satelliteId)) {
      fail(`events[${index}].satelliteId is unknown`);
    }
    return {
      timeUs: event.timeUs,
      kind: event.kind,
      positionId: event.positionId,
      satelliteId: event.satelliteId,
      score: validateScore(event.score, `events[${index}].score`)
    };
  });
  normalized.sort((left, right) => left.timeUs - right.timeUs
    || EVENT_ORDER[left.kind] - EVENT_ORDER[right.kind]
    || compareText(left.positionId, right.positionId)
    || compareText(left.satelliteId, right.satelliteId));
  const seen = new Set();
  for (const event of normalized) {
    const key = `${event.timeUs}\0${event.kind}\0${event.positionId}\0${event.satelliteId}`;
    if (seen.has(key)) fail('events must not contain duplicate state changes');
    seen.add(key);
  }
  return normalized;
}

function normalizeEventGroupForState(events, state, endTimeUs) {
  if (!Array.isArray(events)) fail('event group must be an array');
  const normalized = events.map((event, index) => {
    assertObject(event, `event group[${index}]`);
    assertTime(event.timeUs, `event group[${index}].timeUs`);
    if (event.timeUs < state.timeUs || event.timeUs >= endTimeUs) {
      fail(`event group[${index}] lies outside the segment`);
    }
    if (!Object.hasOwn(EVENT_ORDER, event.kind)) fail(`event group[${index}].kind is invalid`);
    if (!state.complete.has(event.positionId)) fail(`event group[${index}].positionId is unknown`);
    if (typeof event.satelliteId !== 'string' || !state.registryBySatellite.has(event.satelliteId)) {
      fail(`event group[${index}].satelliteId is unknown`);
    }
    return {
      timeUs: event.timeUs,
      kind: event.kind,
      positionId: event.positionId,
      satelliteId: event.satelliteId,
      score: validateScore(event.score, `event group[${index}].score`)
    };
  });
  normalized.sort((left, right) => EVENT_ORDER[left.kind] - EVENT_ORDER[right.kind]
    || compareText(left.positionId, right.positionId)
    || compareText(left.satelliteId, right.satelliteId));
  const seen = new Set();
  for (const event of normalized) {
    const key = `${event.kind}\0${event.positionId}\0${event.satelliteId}`;
    if (seen.has(key)) fail('event group must not contain duplicate state changes');
    seen.add(key);
  }
  return normalized;
}

function applyEvent(complete, entry, event) {
  const completeForPosition = complete.get(event.positionId);
  const entryForPosition = entry.get(event.positionId);
  if (event.kind === 'release_exit') {
    entryForPosition.delete(event.satelliteId);
    completeForPosition.delete(event.satelliteId);
    return;
  }
  if (event.kind === 'entry_exit') {
    entryForPosition.delete(event.satelliteId);
    if (completeForPosition.has(event.satelliteId)) completeForPosition.set(event.satelliteId, event.score);
    return;
  }
  if (event.kind === 'release_enter') {
    completeForPosition.set(event.satelliteId, event.score);
    if (entryForPosition.has(event.satelliteId)) entryForPosition.set(event.satelliteId, event.score);
    return;
  }
  completeForPosition.set(event.satelliteId, event.score);
  entryForPosition.set(event.satelliteId, event.score);
}

function normalizeAssignmentHints(value, positionIds, registryBySatellite, context) {
  if (value === undefined) return new Map();
  const positionSet = new Set(positionIds);
  let records;
  if (value instanceof Map) {
    records = [...value].map(([positionId, assignment]) => {
      if (typeof assignment === 'string') return {positionId, satelliteId: assignment};
      return {positionId, ...assignment};
    });
  } else if (Array.isArray(value)) {
    records = value;
  } else {
    fail(`${context} must be a Map or an array`);
  }
  const result = new Map();
  for (const [index, record] of records.entries()) {
    assertObject(record, `${context}[${index}]`);
    if (!positionSet.has(record.positionId)) fail(`${context}[${index}].positionId is unknown`);
    if (result.has(record.positionId)) fail(`${context} contains duplicate position '${record.positionId}'`);
    if (typeof record.satelliteId !== 'string' || !registryBySatellite.has(record.satelliteId)) {
      fail(`${context}[${index}].satelliteId is unknown`);
    }
    if (record.cellBank !== undefined && record.cellBank !== 0 && record.cellBank !== 1) {
      fail(`${context}[${index}].cellBank must be 0 or 1`);
    }
    result.set(record.positionId, {
      positionId: record.positionId,
      satelliteId: record.satelliteId,
      cellBank: record.cellBank
    });
  }
  return result;
}

function eligibleCandidates(positionId, complete, entry, previousAssignments) {
  const incumbent = previousAssignments.get(positionId)?.satelliteId;
  return [...complete.get(positionId)].filter(([satelliteId]) => {
    return entry.get(positionId).has(satelliteId) || satelliteId === incumbent;
  }).map(([satelliteId, score]) => ({satelliteId, score})).sort((left, right) => {
    const incumbentOrder = Number(right.satelliteId === incumbent) - Number(left.satelliteId === incumbent);
    return incumbentOrder || right.score - left.score || compareText(left.satelliteId, right.satelliteId);
  });
}

function incrementalMaximumMatching({
  positionIds,
  complete,
  entry,
  previousAssignments,
  satelliteCapacity,
  initializeGlobally = false
}) {
  const ownerByPosition = new Map();
  const positionsBySatellite = new Map();
  const ensureLoad = (satelliteId) => {
    if (!positionsBySatellite.has(satelliteId)) positionsBySatellite.set(satelliteId, new Set());
    return positionsBySatellite.get(satelliteId);
  };
  const candidatesByPosition = new Map(positionIds.map((positionId) => [
    positionId,
    eligibleCandidates(positionId, complete, entry, previousAssignments)
  ]));

  if (initializeGlobally) {
    const initial = matchCandidatesToCapacity(positionIds.map((positionId) => ({
      positionId,
      candidates: candidatesByPosition.get(positionId)
    })), {satelliteCapacity, previousAssignments});
    for (const [positionId, satelliteId] of initial) {
      ownerByPosition.set(positionId, satelliteId);
      ensureLoad(satelliteId).add(positionId);
    }
  } else {
    // Seed every still-valid incumbent. The previous assignment is already a
    // valid capacity-constrained matching; sorting also makes imported hints safe.
    for (const positionId of positionIds) {
      const satelliteId = previousAssignments.get(positionId)?.satelliteId;
      if (!satelliteId || !candidatesByPosition.get(positionId).some((item) => item.satelliteId === satelliteId)) continue;
      const load = ensureLoad(satelliteId);
      if (load.size >= satelliteCapacity) continue;
      ownerByPosition.set(positionId, satelliteId);
      load.add(positionId);
    }
  }

  const move = (positionId, satelliteId) => {
    const before = ownerByPosition.get(positionId);
    if (before !== undefined) ensureLoad(before).delete(positionId);
    ownerByPosition.set(positionId, satelliteId);
    ensureLoad(satelliteId).add(positionId);
  };
  if (initializeGlobally) return {ownerByPosition, candidatesByPosition};
  const findAugmentingPath = () => {
    // Successive minimum-cost augmenting path. The primary path cost is the
    // number of retained owners lost (restoring an owner has negative cost),
    // and the secondary cost is the number of moved positions. Starting from
    // the still-valid incumbents means every successful augmentation preserves
    // the best feasible owner retention for its new cardinality.
    const satelliteIds = [...new Set([...candidatesByPosition.values()]
      .flatMap((candidates) => candidates.map(({satelliteId}) => satelliteId)))].sort(compareText);
    const infinity = {
      retentionLoss: Number.POSITIVE_INFINITY,
      moves: Number.POSITIVE_INFINITY,
      rootPositionId: undefined
    };
    const positionDistance = new Map(positionIds.map((positionId) => [positionId, {...infinity}]));
    const satelliteDistance = new Map(satelliteIds.map((satelliteId) => [satelliteId, {...infinity}]));
    const positionReachedFromSatellite = new Map();
    const satelliteReachedFromPosition = new Map();
    for (const positionId of positionIds) {
      if (!ownerByPosition.has(positionId)) {
        positionDistance.set(positionId, {retentionLoss: 0, moves: 0, rootPositionId: positionId});
      }
    }
    const isBetter = (candidate, current) => candidate.retentionLoss < current.retentionLoss ||
      (candidate.retentionLoss === current.retentionLoss && candidate.moves < current.moves) ||
      (candidate.retentionLoss === current.retentionLoss && candidate.moves === current.moves &&
       current.rootPositionId !== undefined && compareText(candidate.rootPositionId, current.rootPositionId) < 0);
    const maxRelaxations = positionIds.length + satelliteIds.length - 1;
    for (let iteration = 0; iteration < maxRelaxations; iteration += 1) {
      let relaxed = false;
      for (const positionId of positionIds) {
        const distance = positionDistance.get(positionId);
        if (!Number.isFinite(distance.retentionLoss)) continue;
        const oldSatelliteId = ownerByPosition.get(positionId);
        const incumbentSatelliteId = previousAssignments.get(positionId)?.satelliteId;
        const oldRetained = Number(oldSatelliteId !== undefined && oldSatelliteId === incumbentSatelliteId);
        for (const candidate of candidatesByPosition.get(positionId)) {
          if (candidate.satelliteId === oldSatelliteId) continue;
          const next = {
            retentionLoss: distance.retentionLoss + oldRetained - Number(candidate.satelliteId === incumbentSatelliteId),
            moves: distance.moves + 1,
            rootPositionId: distance.rootPositionId
          };
          if (!isBetter(next, satelliteDistance.get(candidate.satelliteId))) continue;
          satelliteDistance.set(candidate.satelliteId, next);
          satelliteReachedFromPosition.set(candidate.satelliteId, positionId);
          relaxed = true;
        }
      }
      for (const satelliteId of satelliteIds) {
        const distance = satelliteDistance.get(satelliteId);
        const load = ensureLoad(satelliteId);
        if (!Number.isFinite(distance.retentionLoss) || load.size < satelliteCapacity) continue;
        for (const occupant of [...load].sort(compareText)) {
          if (!isBetter(distance, positionDistance.get(occupant))) continue;
          positionDistance.set(occupant, {...distance});
          positionReachedFromSatellite.set(occupant, satelliteId);
          relaxed = true;
        }
      }
      if (!relaxed) break;
    }
    const freeSatelliteId = satelliteIds.filter((satelliteId) => {
      return ensureLoad(satelliteId).size < satelliteCapacity && Number.isFinite(satelliteDistance.get(satelliteId).retentionLoss);
    }).sort((left, right) => {
      const leftDistance = satelliteDistance.get(left);
      const rightDistance = satelliteDistance.get(right);
      return leftDistance.retentionLoss - rightDistance.retentionLoss || leftDistance.moves - rightDistance.moves ||
        compareText(leftDistance.rootPositionId, rightDistance.rootPositionId) ||
        compareText(left, right);
    })[0];
    if (freeSatelliteId === undefined) return null;
    return {
      rootPositionId: satelliteDistance.get(freeSatelliteId).rootPositionId,
      freeSatelliteId,
      distance: satelliteDistance.get(freeSatelliteId),
      positionReachedFromSatellite,
      satelliteReachedFromPosition
    };
  };
  const applyAugmentingPath = (path) => {
    // Apply backwards, ending with the previously unmatched root. Every
    // intermediate move frees exactly the next slot.
    let satelliteId = path.freeSatelliteId;
    let pathLength = 0;
    while (true) {
      const positionId = path.satelliteReachedFromPosition.get(satelliteId);
      if (positionId === undefined || pathLength > positionIds.length) fail('augmenting path reconstruction failed');
      move(positionId, satelliteId);
      if (positionId === path.rootPositionId) break;
      satelliteId = path.positionReachedFromSatellite.get(positionId);
      pathLength += 1;
    }
  };
  // A single multi-source search considers every currently unmatched position,
  // so position order cannot choose a path that displaces more incumbents.
  while (true) {
    const bestPath = findAugmentingPath();
    if (bestPath === null) break;
    applyAugmentingPath(bestPath);
  }

  return {ownerByPosition, candidatesByPosition};
}

function partitionToRegistryCells({positionIds, ownerByPosition, previousAssignments, registry, cellCapacity}) {
  const assignments = [];
  const overflow = new Set();
  const cellAllocations = [];
  for (const registryEntry of registry) {
    const owned = positionIds.filter((positionId) => ownerByPosition.get(positionId) === registryEntry.satelliteId);
    const byBank = [[], []];
    const fresh = [];
    for (const positionId of owned) {
      const previous = previousAssignments.get(positionId);
      if (previous?.satelliteId === registryEntry.satelliteId &&
          (previous.cellBank === 0 || previous.cellBank === 1) &&
          byBank[previous.cellBank].length < cellCapacity) {
        byBank[previous.cellBank].push(positionId);
      } else {
        fresh.push(positionId);
      }
    }
    for (const positionId of fresh) {
      let bank;
      if (byBank[0].length >= cellCapacity && byBank[1].length >= cellCapacity) {
        overflow.add(positionId);
        continue;
      } else if (byBank[0].length >= cellCapacity) {
        bank = 1;
      } else if (byBank[1].length >= cellCapacity) {
        bank = 0;
      } else {
        bank = byBank[0].length <= byBank[1].length ? 0 : 1;
      }
      byBank[bank].push(positionId);
    }
    const cells = registryEntry.cells.map((identity) => {
      const assignedPositionIds = [...byBank[identity.bank]].sort(compareText);
      for (const positionId of assignedPositionIds) {
        assignments.push({
          positionId,
          satelliteId: registryEntry.satelliteId,
          cellBank: identity.bank,
          nci: identity.nci,
          pci: identity.pci
        });
      }
      return {...identity, assignedPositionIds};
    });
    cellAllocations.push({satelliteId: registryEntry.satelliteId, cells, overflowPositionIds: owned.filter((id) => overflow.has(id))});
  }
  assignments.sort((left, right) => compareText(left.positionId, right.positionId));
  return {assignments, overflow, cellAllocations};
}

function serializeCandidateMap(candidateMap, positionIds) {
  return positionIds.map((positionId) => ({
    positionId,
    candidates: [...candidateMap.get(positionId)].map(([satelliteId, score]) => ({satelliteId, score}))
      .sort((left, right) => compareText(left.satelliteId, right.satelliteId))
  }));
}

function deserializeCandidateMap(records, positionIds, registryBySatellite, context) {
  if (!Array.isArray(records)) fail(`${context} must be an array`);
  const positionSet = new Set(positionIds);
  const result = emptyCandidateMap(positionIds);
  const seenPositions = new Set();
  for (const [recordIndex, record] of records.entries()) {
    assertObject(record, `${context}[${recordIndex}]`);
    if (!positionSet.has(record.positionId)) fail(`${context}[${recordIndex}].positionId is unknown`);
    if (seenPositions.has(record.positionId)) fail(`${context} contains duplicate position '${record.positionId}'`);
    seenPositions.add(record.positionId);
    if (!Array.isArray(record.candidates)) fail(`${context}[${recordIndex}].candidates must be an array`);
    for (const [candidateIndex, candidate] of record.candidates.entries()) {
      assertObject(candidate, `${context}[${recordIndex}].candidates[${candidateIndex}]`);
      if (typeof candidate.satelliteId !== 'string' || !registryBySatellite.has(candidate.satelliteId)) {
        fail(`${context}[${recordIndex}].candidates[${candidateIndex}].satelliteId is unknown`);
      }
      if (result.get(record.positionId).has(candidate.satelliteId)) {
        fail(`${context}[${recordIndex}] contains duplicate satellite '${candidate.satelliteId}'`);
      }
      result.get(record.positionId).set(
        candidate.satelliteId,
        validateScore(candidate.score, `${context}[${recordIndex}].candidates[${candidateIndex}].score`)
      );
    }
  }
  if (seenPositions.size !== positionIds.length) fail(`${context} must contain every position`);
  return result;
}

function buildPlan({
  positionIds,
  complete,
  entry,
  previousAssignments,
  registry,
  satelliteCapacity,
  cellCapacity,
  initializeGlobally = false
}) {
  const {ownerByPosition, candidatesByPosition} = incrementalMaximumMatching({
    positionIds,
    complete,
    entry,
    previousAssignments,
    satelliteCapacity,
    initializeGlobally
  });
  const partition = partitionToRegistryCells({
    positionIds,
    ownerByPosition,
    previousAssignments,
    registry,
    cellCapacity
  });
  const assignmentByPosition = new Map(partition.assignments.map((assignment) => [assignment.positionId, assignment]));
  const failures = [];
  for (const positionId of positionIds) {
    if (partition.overflow.has(positionId)) {
      failures.push({positionId, reason: FAILURE_REASON.cellPartitionOverflow});
    } else if (!assignmentByPosition.has(positionId)) {
      failures.push({
        positionId,
        reason: candidatesByPosition.get(positionId).length === 0
          ? FAILURE_REASON.noVisibleCandidate
          : FAILURE_REASON.scheduleOverflow
      });
    }
  }
  const failureCounts = {
    [FAILURE_REASON.noVisibleCandidate]: 0,
    [FAILURE_REASON.scheduleOverflow]: 0,
    [FAILURE_REASON.cellPartitionOverflow]: 0
  };
  for (const failure of failures) failureCounts[failure.reason] += 1;
  const failureReason = failureCounts[FAILURE_REASON.cellPartitionOverflow] > 0
    ? FAILURE_REASON.cellPartitionOverflow
    : failureCounts[FAILURE_REASON.scheduleOverflow] > 0
      ? FAILURE_REASON.scheduleOverflow
      : failureCounts[FAILURE_REASON.noVisibleCandidate] > 0
        ? FAILURE_REASON.noVisibleCandidate
        : null;
  return {
    success: failures.length === 0,
    failureReason,
    failureCounts,
    failures,
    unassignedPositionIds: failures.map(({positionId}) => positionId),
    candidateInventory: serializeCandidateMap(complete, positionIds),
    assignmentCandidates: positionIds.map((positionId) => ({positionId, candidates: candidatesByPosition.get(positionId)})),
    satelliteMatches: [...ownerByPosition].map(([positionId, satelliteId]) => ({positionId, satelliteId}))
      .sort((left, right) => compareText(left.positionId, right.positionId)),
    assignments: partition.assignments,
    cellAllocations: partition.cellAllocations,
    satelliteCapacity,
    cellCapacity
  };
}

function cloneFailureCounts(counts) {
  return {
    [FAILURE_REASON.noVisibleCandidate]: counts[FAILURE_REASON.noVisibleCandidate],
    [FAILURE_REASON.scheduleOverflow]: counts[FAILURE_REASON.scheduleOverflow],
    [FAILURE_REASON.cellPartitionOverflow]: counts[FAILURE_REASON.cellPartitionOverflow]
  };
}

function initializeLiveIndexes(state, resetMetrics = true) {
  state.ownerByPosition = new Map(
    state.currentPlan.satelliteMatches.map(({positionId, satelliteId}) => [positionId, satelliteId])
  );
  state.positionsBySatellite = new Map(state.registry.map(({satelliteId}) => [satelliteId, new Set()]));
  for (const [positionId, satelliteId] of state.ownerByPosition) {
    state.positionsBySatellite.get(satelliteId).add(positionId);
  }
  state.assignmentByPosition = assignmentsMap(state.currentPlan.assignments);
  state.cellPositionsBySatellite = new Map(state.registry.map(({satelliteId}) => [
    satelliteId,
    [new Set(), new Set()]
  ]));
  for (const assignment of state.currentPlan.assignments) {
    state.cellPositionsBySatellite.get(assignment.satelliteId)[assignment.cellBank].add(assignment.positionId);
  }
  state.entryPositionsBySatellite = new Map(state.registry.map(({satelliteId}) => [satelliteId, new Set()]));
  for (const positionId of state.positionIds) {
    for (const satelliteId of state.entry.get(positionId).keys()) {
      state.entryPositionsBySatellite.get(satelliteId).add(positionId);
    }
  }
  state.failureByPosition = new Map(
    state.currentPlan.failures.map(({positionId, reason}) => [positionId, reason])
  );
  state.failureCounts = cloneFailureCounts(state.currentPlan.failureCounts);
  if (resetMetrics) {
    state.metrics = {
      eventGroups: 0,
      fastPathGroups: 0,
      matchingRepairGroups: 0,
      globalFallbackGroups: 0,
      conservativeFallbacksAvoided: 0,
      componentFallbackGroups: 0,
      componentFallbackPositions: 0,
      componentFallbackSatellites: 0,
      augmentingSearches: 0,
      augmentingPaths: 0,
      visitedPositions: 0,
      visitedSatellites: 0
    };
  }
  state.planDirty = false;
}

function liveEligibleCandidates(state, positionId) {
  return eligibleCandidates(positionId, state.complete, state.entry, state.assignmentByPosition);
}

function removeLiveOwner(state, positionId) {
  const satelliteId = state.ownerByPosition.get(positionId);
  if (satelliteId === undefined) return undefined;
  state.ownerByPosition.delete(positionId);
  state.positionsBySatellite.get(satelliteId).delete(positionId);
  return satelliteId;
}

function moveLiveOwner(state, positionId, satelliteId) {
  const before = state.ownerByPosition.get(positionId);
  if (before !== undefined) state.positionsBySatellite.get(before).delete(positionId);
  state.ownerByPosition.set(positionId, satelliteId);
  state.positionsBySatellite.get(satelliteId).add(positionId);
}

function ownerRemainsEligible(state, positionId, satelliteId) {
  if (state.entry.get(positionId).has(satelliteId)) return true;
  return state.assignmentByPosition.get(positionId)?.satelliteId === satelliteId &&
    state.complete.get(positionId).has(satelliteId);
}

function discoverUnmatchedRoots(state, seedSatelliteIds) {
  const roots = new Set();
  const visitedSatellites = new Set();
  const queue = [...seedSatelliteIds].filter((satelliteId) => satelliteId !== undefined).sort(compareText);
  for (let index = 0; index < queue.length; index += 1) {
    const satelliteId = queue[index];
    if (visitedSatellites.has(satelliteId)) continue;
    visitedSatellites.add(satelliteId);
    const candidatePositions = [...state.entryPositionsBySatellite.get(satelliteId)].sort(compareText);
    for (const positionId of candidatePositions) {
      const owner = state.ownerByPosition.get(positionId);
      if (owner === satelliteId) continue;
      if (owner === undefined) {
        roots.add(positionId);
      } else if (!visitedSatellites.has(owner)) {
        queue.push(owner);
      }
    }
  }
  return roots;
}

function findLocalAugmentingPath(state, rootPositionIds) {
  const roots = [...rootPositionIds].filter((positionId) => !state.ownerByPosition.has(positionId)).sort(compareText);
  if (roots.length === 0) return null;
  state.metrics.augmentingSearches += 1;
  const positionDistance = new Map();
  const satelliteDistance = new Map();
  const positionReachedFromSatellite = new Map();
  const satelliteReachedFromPosition = new Map();
  let ambiguous = roots.length > 1;
  const queue = [];
  const queued = new Set();
  const enqueue = (kind, id) => {
    const key = `${kind}\0${id}`;
    if (queued.has(key)) return;
    queued.add(key);
    queue.push({kind, id, key});
  };
  const isBetter = (candidate, current) => current === undefined ||
    candidate.retentionLoss < current.retentionLoss ||
    (candidate.retentionLoss === current.retentionLoss && candidate.moves < current.moves) ||
    (candidate.retentionLoss === current.retentionLoss && candidate.moves === current.moves &&
     compareText(candidate.rootPositionId, current.rootPositionId) < 0);
  for (const positionId of roots) {
    positionDistance.set(positionId, {retentionLoss: 0, moves: 0, rootPositionId: positionId});
    enqueue('position', positionId);
  }
  for (let queueIndex = 0; queueIndex < queue.length; queueIndex += 1) {
    const node = queue[queueIndex];
    queued.delete(node.key);
    if (node.kind === 'position') {
      const distance = positionDistance.get(node.id);
      const oldSatelliteId = state.ownerByPosition.get(node.id);
      const incumbentSatelliteId = state.assignmentByPosition.get(node.id)?.satelliteId;
      const oldRetained = Number(oldSatelliteId !== undefined && oldSatelliteId === incumbentSatelliteId);
      const residualCandidates = liveEligibleCandidates(state, node.id).filter((candidate) => {
        return candidate.satelliteId !== oldSatelliteId;
      });
      if (residualCandidates.length > 1) ambiguous = true;
      for (const candidate of residualCandidates) {
        const next = {
          retentionLoss: distance.retentionLoss + oldRetained - Number(candidate.satelliteId === incumbentSatelliteId),
          moves: distance.moves + 1,
          rootPositionId: distance.rootPositionId
        };
        if (!isBetter(next, satelliteDistance.get(candidate.satelliteId))) continue;
        satelliteDistance.set(candidate.satelliteId, next);
        satelliteReachedFromPosition.set(candidate.satelliteId, node.id);
        enqueue('satellite', candidate.satelliteId);
      }
    } else {
      const load = state.positionsBySatellite.get(node.id);
      if (load.size < state.satelliteCapacity) continue;
      const distance = satelliteDistance.get(node.id);
      const occupants = [...load].sort(compareText);
      if (occupants.length > 1) ambiguous = true;
      for (const positionId of occupants) {
        if (!isBetter(distance, positionDistance.get(positionId))) continue;
        positionDistance.set(positionId, {...distance});
        positionReachedFromSatellite.set(positionId, node.id);
        enqueue('position', positionId);
      }
    }
  }
  state.metrics.visitedPositions += positionDistance.size;
  state.metrics.visitedSatellites += satelliteDistance.size;
  const terminalSatelliteId = [...satelliteDistance.keys()].filter((satelliteId) => {
    return state.positionsBySatellite.get(satelliteId).size < state.satelliteCapacity;
  }).sort((left, right) => {
    const leftDistance = satelliteDistance.get(left);
    const rightDistance = satelliteDistance.get(right);
    return leftDistance.retentionLoss - rightDistance.retentionLoss || leftDistance.moves - rightDistance.moves ||
      compareText(leftDistance.rootPositionId, rightDistance.rootPositionId) || compareText(left, right);
  })[0];
  if (terminalSatelliteId === undefined) return null;
  return {
    rootPositionId: satelliteDistance.get(terminalSatelliteId).rootPositionId,
    terminalSatelliteId,
    ambiguous,
    positionReachedFromSatellite,
    satelliteReachedFromPosition
  };
}

function applyLocalAugmentingPath(state, path, touchedPositions) {
  let satelliteId = path.terminalSatelliteId;
  let pathLength = 0;
  while (true) {
    const positionId = path.satelliteReachedFromPosition.get(satelliteId);
    if (positionId === undefined || pathLength > state.positionIds.length) {
      fail('local augmenting path reconstruction failed');
    }
    touchedPositions.add(positionId);
    moveLiveOwner(state, positionId, satelliteId);
    if (positionId === path.rootPositionId) break;
    satelliteId = path.positionReachedFromSatellite.get(positionId);
    pathLength += 1;
  }
  state.metrics.augmentingPaths += 1;
}

function snapshotAssignmentsForSatellites(state, satelliteIds, extraPositionIds) {
  const positions = new Set(extraPositionIds);
  for (const satelliteId of satelliteIds) {
    for (const bank of state.cellPositionsBySatellite.get(satelliteId)) {
      for (const positionId of bank) positions.add(positionId);
    }
    for (const positionId of state.positionsBySatellite.get(satelliteId)) positions.add(positionId);
  }
  return {
    positions,
    assignments: new Map([...positions].map((positionId) => [positionId, state.assignmentByPosition.get(positionId)]))
  };
}

function reconcileLiveCellAssignments(state, touchedPositions) {
  const affectedSatellites = new Set();
  for (const positionId of touchedPositions) {
    const before = state.assignmentByPosition.get(positionId)?.satelliteId;
    const after = state.ownerByPosition.get(positionId);
    if (before !== undefined) affectedSatellites.add(before);
    if (after !== undefined) affectedSatellites.add(after);
  }
  const snapshot = snapshotAssignmentsForSatellites(state, affectedSatellites, touchedPositions);
  for (const positionId of snapshot.positions) {
    const assignment = state.assignmentByPosition.get(positionId);
    if (assignment && state.ownerByPosition.get(positionId) !== assignment.satelliteId) {
      state.cellPositionsBySatellite.get(assignment.satelliteId)[assignment.cellBank].delete(positionId);
      state.assignmentByPosition.delete(positionId);
    }
  }
  for (const satelliteId of [...affectedSatellites].sort(compareText)) {
    const identities = state.registryBySatellite.get(satelliteId).cells;
    const banks = state.cellPositionsBySatellite.get(satelliteId);
    const missing = [...state.positionsBySatellite.get(satelliteId)].filter((positionId) => {
      return !state.assignmentByPosition.has(positionId);
    }).sort(compareText);
    for (const positionId of missing) {
      let bank;
      if (banks[0].size >= state.cellCapacity && banks[1].size >= state.cellCapacity) continue;
      if (banks[0].size >= state.cellCapacity) bank = 1;
      else if (banks[1].size >= state.cellCapacity) bank = 0;
      else bank = banks[0].size <= banks[1].size ? 0 : 1;
      const identity = identities[bank];
      const assignment = {
        positionId,
        satelliteId,
        cellBank: bank,
        nci: identity.nci,
        pci: identity.pci
      };
      banks[bank].add(positionId);
      state.assignmentByPosition.set(positionId, assignment);
      snapshot.positions.add(positionId);
    }
  }
  return snapshot;
}

function applyLiveAssignmentTransitions(state, snapshot, timeUs, completedIntervals) {
  const changes = [];
  for (const positionId of [...snapshot.positions].sort(compareText)) {
    const oldAssignment = snapshot.assignments.get(positionId);
    const newAssignment = state.assignmentByPosition.get(positionId);
    if (!assignmentChanged(oldAssignment, newAssignment)) continue;
    changes.push({
      positionId,
      before: oldAssignment ? {...oldAssignment} : null,
      after: newAssignment ? {...newAssignment} : null
    });
    const opened = state.open.get(positionId);
    if (opened && timeUs > opened.startTimeUs) {
      completedIntervals.push({...opened.assignment, startTimeUs: opened.startTimeUs, endTimeUs: timeUs});
    }
    state.open.delete(positionId);
    if (oldAssignment && newAssignment && oldAssignment.satelliteId !== newAssignment.satelliteId) {
      state.ownerChangeCount += 1;
      state.handoverCount += 1;
    } else if (oldAssignment && !newAssignment) {
      state.releaseCount += 1;
    } else if (!oldAssignment && newAssignment) {
      state.acquisitionCount += 1;
      const lastOwner = state.lastOwnerByPosition.get(positionId);
      if (lastOwner !== undefined && lastOwner !== newAssignment.satelliteId) state.ownerChangeCount += 1;
    }
    if (newAssignment) {
      state.lastOwnerByPosition.set(positionId, newAssignment.satelliteId);
      state.open.set(positionId, {assignment: newAssignment, startTimeUs: timeUs});
    }
  }
  return changes;
}

function liveFailureReason(state, positionId) {
  if (state.ownerByPosition.has(positionId) && !state.assignmentByPosition.has(positionId)) {
    return FAILURE_REASON.cellPartitionOverflow;
  }
  if (state.assignmentByPosition.has(positionId)) return null;
  return state.entry.get(positionId).size === 0
    ? FAILURE_REASON.noVisibleCandidate
    : FAILURE_REASON.scheduleOverflow;
}

function updateLiveFailures(state, positionIds) {
  const changes = [];
  for (const positionId of [...positionIds].sort(compareText)) {
    const before = state.failureByPosition.get(positionId) ?? null;
    const after = liveFailureReason(state, positionId);
    if (before === after) continue;
    if (before !== null) state.failureCounts[before] -= 1;
    if (after === null) state.failureByPosition.delete(positionId);
    else {
      state.failureByPosition.set(positionId, after);
      state.failureCounts[after] += 1;
    }
    changes.push({positionId, before, after});
  }
  return changes;
}

function materializeLivePlan(state) {
  const candidateInventory = serializeCandidateMap(state.complete, state.positionIds);
  const assignmentCandidates = state.positionIds.map((positionId) => ({
    positionId,
    candidates: liveEligibleCandidates(state, positionId)
  }));
  const satelliteMatches = [...state.ownerByPosition].map(([positionId, satelliteId]) => ({positionId, satelliteId}))
    .sort((left, right) => compareText(left.positionId, right.positionId));
  const assignments = [...state.assignmentByPosition.values()].map((assignment) => ({...assignment}))
    .sort((left, right) => compareText(left.positionId, right.positionId));
  const cellAllocations = state.registry.map((entry) => ({
    satelliteId: entry.satelliteId,
    cells: entry.cells.map((identity) => ({
      ...identity,
      assignedPositionIds: [...state.cellPositionsBySatellite.get(entry.satelliteId)[identity.bank]].sort(compareText)
    })),
    overflowPositionIds: [...state.positionsBySatellite.get(entry.satelliteId)].filter((positionId) => {
      return !state.assignmentByPosition.has(positionId);
    }).sort(compareText)
  }));
  const failures = [...state.failureByPosition].map(([positionId, reason]) => ({positionId, reason}))
    .sort((left, right) => compareText(left.positionId, right.positionId));
  const failureCounts = cloneFailureCounts(state.failureCounts);
  const failureReason = failureCounts[FAILURE_REASON.cellPartitionOverflow] > 0
    ? FAILURE_REASON.cellPartitionOverflow
    : failureCounts[FAILURE_REASON.scheduleOverflow] > 0
      ? FAILURE_REASON.scheduleOverflow
      : failureCounts[FAILURE_REASON.noVisibleCandidate] > 0
        ? FAILURE_REASON.noVisibleCandidate
        : null;
  state.currentPlan = {
    success: failures.length === 0,
    failureReason,
    failureCounts,
    failures,
    unassignedPositionIds: failures.map(({positionId}) => positionId),
    candidateInventory,
    assignmentCandidates,
    satelliteMatches,
    assignments,
    cellAllocations,
    satelliteCapacity: state.satelliteCapacity,
    cellCapacity: state.cellCapacity
  };
  state.planDirty = false;
  return state.currentPlan;
}

function assignmentsMap(assignments) {
  return new Map(assignments.map((assignment) => [assignment.positionId, assignment]));
}

function boundaryAssignments(initialAssignments, positionIds, registryBySatellite, currentAssignments) {
  const normalized = normalizeAssignmentHints(
    initialAssignments,
    positionIds,
    registryBySatellite,
    'initialAssignments'
  );
  const result = new Map();
  for (const [positionId, assignment] of normalized) {
    const current = currentAssignments.get(positionId);
    const cellBank = assignment.cellBank ??
      (current?.satelliteId === assignment.satelliteId ? current.cellBank : null);
    const before = {positionId, satelliteId: assignment.satelliteId, cellBank};
    if (cellBank === 0 || cellBank === 1) {
      const identity = registryBySatellite.get(assignment.satelliteId).cells[cellBank];
      before.nci = identity.nci;
      before.pci = identity.pci;
    }
    result.set(positionId, before);
  }
  return result;
}

function applyBoundaryAssignmentChanges(state, initialAssignments) {
  if (initialAssignments === undefined) return [];
  const baseline = boundaryAssignments(
    initialAssignments,
    state.positionIds,
    state.registryBySatellite,
    state.assignmentByPosition
  );
  state.lastOwnerByPosition = new Map(
    [...baseline].map(([positionId, assignment]) => [positionId, assignment.satelliteId])
  );
  const changes = [];
  for (const positionId of state.positionIds) {
    const before = baseline.get(positionId);
    const after = state.assignmentByPosition.get(positionId);
    if (!assignmentChanged(before, after)) {
      if (after) state.lastOwnerByPosition.set(positionId, after.satelliteId);
      continue;
    }
    changes.push({
      positionId,
      before: before ? {...before} : null,
      after: after ? {...after} : null
    });
    if (before && after && before.satelliteId !== after.satelliteId) {
      state.ownerChangeCount += 1;
      state.handoverCount += 1;
    } else if (before && !after) {
      state.releaseCount += 1;
    } else if (!before && after) {
      state.acquisitionCount += 1;
      const lastOwner = state.lastOwnerByPosition.get(positionId);
      if (lastOwner !== undefined && lastOwner !== after.satelliteId) state.ownerChangeCount += 1;
    }
    if (after) state.lastOwnerByPosition.set(positionId, after.satelliteId);
  }
  return changes;
}

function assignmentChanged(left, right) {
  return left?.satelliteId !== right?.satelliteId || left?.cellBank !== right?.cellBank;
}

function compareIntervals(left, right) {
  return compareText(left.positionId, right.positionId)
    || left.startTimeUs - right.startTimeUs
    || compareText(left.satelliteId, right.satelliteId)
    || left.cellBank - right.cellBank;
}

function finalVisibility(state) {
  const records = [];
  for (const positionId of state.positionIds) {
    for (const [satelliteId, score] of state.complete.get(positionId)) {
      records.push({
        positionId,
        satelliteId,
        aboveEntry: state.entry.get(positionId).has(satelliteId),
        aboveRelease: true,
        score
      });
    }
  }
  return records.sort((left, right) => compareText(left.positionId, right.positionId)
    || compareText(left.satelliteId, right.satelliteId));
}

function checkpointFromState(state) {
  return {
    schemaVersion: CHECKPOINT_SCHEMA_VERSION,
    timeUs: state.timeUs,
    positionIds: [...state.positionIds],
    registry: state.registry.map((entry) => ({
      satelliteId: entry.satelliteId,
      cells: entry.cells.map((cell) => ({...cell}))
    })),
    satelliteCapacity: state.satelliteCapacity,
    cellCapacity: state.cellCapacity,
    completeCandidates: serializeCandidateMap(state.complete, state.positionIds),
    entryCandidates: serializeCandidateMap(state.entry, state.positionIds),
    assignments: state.currentPlan.assignments.map((assignment) => ({...assignment})),
    satelliteMatches: state.currentPlan.satelliteMatches.map((match) => ({...match})),
    openIntervals: [...state.open].map(([positionId, opened]) => ({
      positionId,
      startTimeUs: opened.startTimeUs,
      assignment: {...opened.assignment}
    })).sort((left, right) => compareText(left.positionId, right.positionId)),
    lastOwners: [...state.lastOwnerByPosition].map(([positionId, satelliteId]) => ({positionId, satelliteId}))
      .sort((left, right) => compareText(left.positionId, right.positionId)),
    counters: {
      ownerChangeCount: state.ownerChangeCount,
      handoverCount: state.handoverCount,
      releaseCount: state.releaseCount,
      acquisitionCount: state.acquisitionCount
    },
    metrics: {...state.metrics}
  };
}

function createState({
  startTimeUs,
  positionIds,
  initialVisibility,
  registry,
  initialAssignments,
  satelliteCapacity,
  cellCapacity
}) {
  assertTime(startTimeUs, 'startTimeUs');
  assertPositiveInteger(satelliteCapacity, 'satelliteCapacity');
  assertPositiveInteger(cellCapacity, 'cellCapacity');
  const orderedPositions = normalizePositionIds(positionIds);
  const normalizedRegistry = normalizeRegistry(registry);
  const bySatellite = registryMap(normalizedRegistry);
  const {complete, entry} = normalizeInitialCandidates(initialVisibility, orderedPositions, bySatellite);
  const previousAssignments = normalizeAssignmentHints(
    initialAssignments,
    orderedPositions,
    bySatellite,
    'initialAssignments'
  );
  const currentPlan = buildPlan({
    positionIds: orderedPositions,
    complete,
    entry,
    previousAssignments,
    registry: normalizedRegistry,
    satelliteCapacity,
    cellCapacity,
    initializeGlobally: true
  });
  const open = new Map(currentPlan.assignments.map((assignment) => [
    assignment.positionId,
    {assignment, startTimeUs}
  ]));
  const state = {
    timeUs: startTimeUs,
    positionIds: orderedPositions,
    registry: normalizedRegistry,
    registryBySatellite: bySatellite,
    satelliteCapacity,
    cellCapacity,
    complete,
    entry,
    currentPlan,
    open,
    lastOwnerByPosition: new Map(currentPlan.assignments.map((assignment) => [
      assignment.positionId,
      assignment.satelliteId
    ])),
    ownerChangeCount: 0,
    handoverCount: 0,
    releaseCount: 0,
    acquisitionCount: 0
  };
  initializeLiveIndexes(state);
  return state;
}

function sameAssignment(left, right) {
  return left.positionId === right.positionId && left.satelliteId === right.satelliteId &&
    left.cellBank === right.cellBank && BigInt(left.nci) === BigInt(right.nci) && left.pci === right.pci;
}

function restoreState(checkpoint) {
  assertObject(checkpoint, 'checkpoint');
  if (checkpoint.schemaVersion !== CHECKPOINT_SCHEMA_VERSION) fail('checkpoint.schemaVersion is unsupported');
  assertTime(checkpoint.timeUs, 'checkpoint.timeUs');
  assertPositiveInteger(checkpoint.satelliteCapacity, 'checkpoint.satelliteCapacity');
  assertPositiveInteger(checkpoint.cellCapacity, 'checkpoint.cellCapacity');
  const positionIds = normalizePositionIds(checkpoint.positionIds);
  const registry = normalizeRegistry(checkpoint.registry);
  const bySatellite = registryMap(registry);
  const complete = deserializeCandidateMap(checkpoint.completeCandidates, positionIds, bySatellite, 'checkpoint.completeCandidates');
  const entry = deserializeCandidateMap(checkpoint.entryCandidates, positionIds, bySatellite, 'checkpoint.entryCandidates');
  for (const positionId of positionIds) {
    for (const satelliteId of entry.get(positionId).keys()) {
      if (!complete.get(positionId).has(satelliteId)) {
        fail(`checkpoint entry candidate '${positionId}/${satelliteId}' is absent from complete candidates`);
      }
    }
  }
  const assignmentHints = normalizeAssignmentHints(checkpoint.assignments, positionIds, bySatellite, 'checkpoint.assignments');
  if (!Array.isArray(checkpoint.satelliteMatches)) fail('checkpoint.satelliteMatches must be an array');
  const matchingHints = new Map();
  const positionSetForMatches = new Set(positionIds);
  for (const [index, match] of checkpoint.satelliteMatches.entries()) {
    assertObject(match, `checkpoint.satelliteMatches[${index}]`);
    if (!positionSetForMatches.has(match.positionId) || !bySatellite.has(match.satelliteId) ||
        matchingHints.has(match.positionId)) {
      fail(`checkpoint.satelliteMatches[${index}] is invalid`);
    }
    matchingHints.set(match.positionId, {
      positionId: match.positionId,
      satelliteId: match.satelliteId,
      cellBank: assignmentHints.get(match.positionId)?.cellBank
    });
  }
  const currentPlan = buildPlan({
    positionIds,
    complete,
    entry,
    previousAssignments: matchingHints,
    registry,
    satelliteCapacity: checkpoint.satelliteCapacity,
    cellCapacity: checkpoint.cellCapacity
  });
  if (!Array.isArray(checkpoint.assignments) || checkpoint.assignments.length !== currentPlan.assignments.length ||
      !checkpoint.assignments.every((assignment, index) => sameAssignment(assignment, currentPlan.assignments[index]))) {
    fail('checkpoint assignments are inconsistent with candidates, capacity, or registry identity');
  }
  if (checkpoint.satelliteMatches.length !== currentPlan.satelliteMatches.length ||
      !checkpoint.satelliteMatches.every((match, index) => {
        const restored = currentPlan.satelliteMatches[index];
        return match?.positionId === restored?.positionId && match?.satelliteId === restored?.satelliteId;
      })) {
    fail('checkpoint satellite matches are inconsistent with candidates or capacity');
  }
  if (!Array.isArray(checkpoint.openIntervals)) fail('checkpoint.openIntervals must be an array');
  const open = new Map();
  const currentByPosition = assignmentsMap(currentPlan.assignments);
  for (const [index, record] of checkpoint.openIntervals.entries()) {
    assertObject(record, `checkpoint.openIntervals[${index}]`);
    assertTime(record.startTimeUs, `checkpoint.openIntervals[${index}].startTimeUs`);
    if (record.startTimeUs > checkpoint.timeUs) fail(`checkpoint.openIntervals[${index}] starts after checkpoint`);
    const current = currentByPosition.get(record.positionId);
    if (!current || !sameAssignment(record.assignment, current) || open.has(record.positionId)) {
      fail(`checkpoint.openIntervals[${index}] is inconsistent with current assignments`);
    }
    open.set(record.positionId, {assignment: current, startTimeUs: record.startTimeUs});
  }
  if (open.size !== currentPlan.assignments.length) fail('checkpoint must contain one open interval per assignment');
  if (!Array.isArray(checkpoint.lastOwners)) fail('checkpoint.lastOwners must be an array');
  const lastOwnerByPosition = new Map();
  const positionSet = new Set(positionIds);
  for (const [index, record] of checkpoint.lastOwners.entries()) {
    assertObject(record, `checkpoint.lastOwners[${index}]`);
    if (!positionSet.has(record.positionId) || !bySatellite.has(record.satelliteId) ||
        lastOwnerByPosition.has(record.positionId)) {
      fail(`checkpoint.lastOwners[${index}] is invalid`);
    }
    lastOwnerByPosition.set(record.positionId, record.satelliteId);
  }
  for (const assignment of currentPlan.assignments) {
    if (lastOwnerByPosition.get(assignment.positionId) !== assignment.satelliteId) {
      fail(`checkpoint last owner for '${assignment.positionId}' is inconsistent with its assignment`);
    }
  }
  assertObject(checkpoint.counters, 'checkpoint.counters');
  for (const name of ['ownerChangeCount', 'handoverCount', 'releaseCount', 'acquisitionCount']) {
    if (!Number.isSafeInteger(checkpoint.counters[name]) || checkpoint.counters[name] < 0) {
      fail(`checkpoint.counters.${name} must be a non-negative safe integer`);
    }
  }
  const state = {
    timeUs: checkpoint.timeUs,
    positionIds,
    registry,
    registryBySatellite: bySatellite,
    satelliteCapacity: checkpoint.satelliteCapacity,
    cellCapacity: checkpoint.cellCapacity,
    complete,
    entry,
    currentPlan,
    open,
    lastOwnerByPosition,
    ownerChangeCount: checkpoint.counters.ownerChangeCount,
    handoverCount: checkpoint.counters.handoverCount,
    releaseCount: checkpoint.counters.releaseCount,
    acquisitionCount: checkpoint.counters.acquisitionCount
  };
  initializeLiveIndexes(state);
  if (checkpoint.metrics !== undefined) {
    assertObject(checkpoint.metrics, 'checkpoint.metrics');
    for (const name of Object.keys(state.metrics)) {
      if (!Number.isSafeInteger(checkpoint.metrics[name]) || checkpoint.metrics[name] < 0) {
        fail(`checkpoint.metrics.${name} must be a non-negative safe integer`);
      }
      state.metrics[name] = checkpoint.metrics[name];
    }
  }
  return state;
}

function discoverMatchingComponent(state, rootPositionIds) {
  const positions = new Set();
  const satellites = new Set();
  const queue = [...rootPositionIds].sort(compareText).map((id) => ({kind: 'position', id}));
  for (let index = 0; index < queue.length; index += 1) {
    const node = queue[index];
    if (node.kind === 'position') {
      if (positions.has(node.id)) continue;
      positions.add(node.id);
      for (const {satelliteId} of liveEligibleCandidates(state, node.id)) {
        if (!satellites.has(satelliteId)) queue.push({kind: 'satellite', id: satelliteId});
      }
      continue;
    }
    if (satellites.has(node.id)) continue;
    satellites.add(node.id);
    for (const positionId of state.entryPositionsBySatellite.get(node.id)) {
      if (!positions.has(positionId)) queue.push({kind: 'position', id: positionId});
    }
    for (const positionId of state.positionsBySatellite.get(node.id)) {
      if (!positions.has(positionId)) queue.push({kind: 'position', id: positionId});
    }
  }
  return {
    positionIds: [...positions].sort(compareText),
    satelliteIds: [...satellites].sort(compareText)
  };
}

function applyComponentAmbiguityFallback(state, roots, eventPositions, timeUs, completedIntervals) {
  const component = discoverMatchingComponent(state, roots);
  const componentPositionSet = new Set(component.positionIds);
  const satelliteSet = new Set(component.satelliteIds);
  const previousAssignments = new Map(component.positionIds.flatMap((positionId) => {
    const assignment = state.assignmentByPosition.get(positionId);
    return assignment === undefined ? [] : [[positionId, assignment]];
  }));
  const nextPlan = buildPlan({
    positionIds: component.positionIds,
    complete: state.complete,
    entry: state.entry,
    previousAssignments,
    registry: state.registry.filter(({satelliteId}) => satelliteSet.has(satelliteId)),
    satelliteCapacity: state.satelliteCapacity,
    cellCapacity: state.cellCapacity,
    initializeGlobally: true
  });
  const beforeAssignments = new Map(component.positionIds.map((positionId) => [
    positionId,
    state.assignmentByPosition.get(positionId)
  ]));
  const failurePositions = new Set([...component.positionIds, ...eventPositions]);
  const beforeFailures = new Map([...failurePositions].map((positionId) => [
    positionId,
    state.failureByPosition.get(positionId) ?? null
  ]));

  for (const positionId of component.positionIds) {
    const owner = state.ownerByPosition.get(positionId);
    if (owner !== undefined) state.positionsBySatellite.get(owner).delete(positionId);
    state.ownerByPosition.delete(positionId);
    const assignment = state.assignmentByPosition.get(positionId);
    if (assignment !== undefined) {
      state.cellPositionsBySatellite.get(assignment.satelliteId)[assignment.cellBank].delete(positionId);
      state.assignmentByPosition.delete(positionId);
    }
  }
  for (const {positionId, satelliteId} of nextPlan.satelliteMatches) {
    state.ownerByPosition.set(positionId, satelliteId);
    state.positionsBySatellite.get(satelliteId).add(positionId);
  }
  for (const assignment of nextPlan.assignments) {
    state.assignmentByPosition.set(assignment.positionId, assignment);
    state.cellPositionsBySatellite.get(assignment.satelliteId)[assignment.cellBank].add(assignment.positionId);
  }
  const nextFailures = new Map(nextPlan.failures.map(({positionId, reason}) => [positionId, reason]));
  const failureChanges = [];
  for (const positionId of failurePositions) {
    const before = beforeFailures.get(positionId);
    const after = nextFailures.has(positionId)
      ? nextFailures.get(positionId)
      : componentPositionSet.has(positionId)
        ? null
        : liveFailureReason(state, positionId);
    if (before === after) continue;
    if (before !== null) state.failureCounts[before] -= 1;
    if (after === null) state.failureByPosition.delete(positionId);
    else {
      state.failureByPosition.set(positionId, after);
      state.failureCounts[after] += 1;
    }
    failureChanges.push({positionId, before, after});
  }
  const assignmentChanges = applyLiveAssignmentTransitions(state, {
    positions: new Set(component.positionIds),
    assignments: beforeAssignments
  }, timeUs, completedIntervals);
  state.metrics.componentFallbackGroups += 1;
  state.metrics.componentFallbackPositions += component.positionIds.length;
  state.metrics.componentFallbackSatellites += component.satelliteIds.length;
  state.planDirty = true;
  return {
    timeUs,
    success: Object.values(state.failureCounts).every((count) => count === 0),
    failureCounts: cloneFailureCounts(state.failureCounts),
    assignmentChanges,
    failureChanges
  };
}

function processLiveEventGroup(state, group, timeUs, completedIntervals) {
  state.metrics.eventGroups += 1;
  const eventPositions = new Set();
  const initialEntryByPair = new Map();
  for (const event of group) {
    eventPositions.add(event.positionId);
    const key = pairKey(event.positionId, event.satelliteId);
    if (!initialEntryByPair.has(key)) {
      initialEntryByPair.set(key, {
        positionId: event.positionId,
        satelliteId: event.satelliteId,
        aboveEntry: state.entry.get(event.positionId).has(event.satelliteId)
      });
    }
    const beforeEntry = state.entry.get(event.positionId).has(event.satelliteId);
    applyEvent(state.complete, state.entry, event);
    const afterEntry = state.entry.get(event.positionId).has(event.satelliteId);
    if (beforeEntry !== afterEntry) {
      const reverse = state.entryPositionsBySatellite.get(event.satelliteId);
      if (afterEntry) reverse.add(event.positionId);
      else reverse.delete(event.positionId);
    }
  }
  state.planDirty = true;

  const touchedPositions = new Set();
  const freedSatelliteIds = new Set();
  for (const positionId of eventPositions) {
    const owner = state.ownerByPosition.get(positionId);
    if (owner !== undefined && !ownerRemainsEligible(state, positionId, owner)) {
      freedSatelliteIds.add(removeLiveOwner(state, positionId));
      touchedPositions.add(positionId);
    }
  }

  const roots = new Set(touchedPositions);
  const reverseSeeds = new Set(freedSatelliteIds);
  const hasUnmatchedPositions = state.ownerByPosition.size < state.positionIds.length;
  for (const record of initialEntryByPair.values()) {
    const nowAboveEntry = state.entry.get(record.positionId).has(record.satelliteId);
    if (record.aboveEntry || !nowAboveEntry) continue;
    const owner = state.ownerByPosition.get(record.positionId);
    if (owner === undefined) roots.add(record.positionId);
    else if (hasUnmatchedPositions && owner !== record.satelliteId &&
             state.positionsBySatellite.get(owner).size >= state.satelliteCapacity) {
      // A non-full incumbent satellite is already an augmenting-path terminal.
      // With a maximum-cardinality live matching, no unmatched position can
      // reach it. Moving this assigned position would therefore only lose
      // stickiness without increasing coverage.
      reverseSeeds.add(owner);
    }
  }
  for (const root of discoverUnmatchedRoots(state, reverseSeeds)) roots.add(root);

  let searched = false;
  while (true) {
    const path = findLocalAugmentingPath(state, roots);
    if (path === null) break;
    if (path.ambiguous) {
      state.metrics.conservativeFallbacksAvoided += 1;
      state.metrics.matchingRepairGroups += 1;
      return applyComponentAmbiguityFallback(state, roots, eventPositions, timeUs, completedIntervals);
    }
    searched = true;
    applyLocalAugmentingPath(state, path, touchedPositions);
  }
  if (searched || freedSatelliteIds.size > 0 || roots.size > 0) state.metrics.matchingRepairGroups += 1;

  let assignmentChanges = [];
  let transitionPositions = new Set();
  if (touchedPositions.size > 0) {
    const snapshot = reconcileLiveCellAssignments(state, touchedPositions);
    transitionPositions = snapshot.positions;
    assignmentChanges = applyLiveAssignmentTransitions(state, snapshot, timeUs, completedIntervals);
  }
  const failurePositions = new Set([...eventPositions, ...transitionPositions]);
  const failureChanges = updateLiveFailures(state, failurePositions);
  const semanticallyChanged = assignmentChanges.length > 0 || failureChanges.length > 0;
  if (!semanticallyChanged) state.metrics.fastPathGroups += 1;
  return semanticallyChanged ? {
    timeUs,
    success: Object.values(state.failureCounts).every((count) => count === 0),
    failureCounts: cloneFailureCounts(state.failureCounts),
    assignmentChanges,
    failureChanges
  } : null;
}

function runState(state, endTimeUs, events, {emitInitialEpoch, initialAssignmentChanges = [], finalize}) {
  assertTime(endTimeUs, 'endTimeUs');
  if (endTimeUs <= state.timeUs) fail('endTimeUs must be after the current timeline time');
  const orderedEvents = normalizeEvents(
    events,
    state.positionIds,
    state.registryBySatellite,
    state.timeUs,
    endTimeUs
  );
  const completedIntervals = [];
  const epochs = emitInitialEpoch ? [{
    timeUs: state.timeUs,
    success: state.currentPlan.success,
    failureCounts: {...state.currentPlan.failureCounts},
    failures: state.currentPlan.failures.map((failure) => ({...failure})),
    assignmentChanges: initialAssignmentChanges.map((change) => ({
      positionId: change.positionId,
      before: change.before ? {...change.before} : null,
      after: change.after ? {...change.after} : null
    })),
    failureChanges: []
  }] : [];
  for (let index = 0; index < orderedEvents.length;) {
    const timeUs = orderedEvents[index].timeUs;
    const group = [];
    while (index < orderedEvents.length && orderedEvents[index].timeUs === timeUs) {
      group.push(orderedEvents[index]);
      index += 1;
    }
    const epoch = processLiveEventGroup(state, group, timeUs, completedIntervals);
    if (epoch !== null) epochs.push(epoch);
  }
  return finishStateSegment(state, endTimeUs, completedIntervals, epochs, finalize);
}

function finishStateSegment(state, endTimeUs, completedIntervals, epochs, finalize) {
  state.timeUs = endTimeUs;
  if (state.planDirty) materializeLivePlan(state);
  const intervals = [...completedIntervals];
  if (finalize) {
    for (const opened of state.open.values()) {
      if (endTimeUs > opened.startTimeUs) {
        intervals.push({...opened.assignment, startTimeUs: opened.startTimeUs, endTimeUs});
      }
    }
  }
  intervals.sort(compareIntervals);
  return {
    intervals,
    epochs,
    ownerChangeCount: state.ownerChangeCount,
    handoverCount: state.handoverCount,
    releaseCount: state.releaseCount,
    acquisitionCount: state.acquisitionCount,
    metrics: {...state.metrics},
    finalPlan: state.currentPlan,
    finalAssignments: state.currentPlan.assignments.map((assignment) => ({...assignment})),
    finalVisibility: finalVisibility(state),
    checkpoint: checkpointFromState(state)
  };
}

export class StreamingAssignmentSession {
  #state;
  #emitInitialEpoch;
  #initialAssignmentChanges;
  #finalized = false;

  constructor(state, {emitInitialEpoch = false, initialAssignmentChanges = []} = {}) {
    this.#state = state;
    this.#emitInitialEpoch = emitInitialEpoch;
    this.#initialAssignmentChanges = initialAssignmentChanges;
  }

  /** Advances the live matching without rebuilding it from the JSON checkpoint. */
  advance({endTimeUs, events = [], finalize = false}) {
    if (this.#finalized) fail('streaming assignment session is already finalized');
    if (typeof finalize !== 'boolean') fail('finalize must be boolean');
    const result = runState(this.#state, endTimeUs, events, {
      emitInitialEpoch: this.#emitInitialEpoch,
      initialAssignmentChanges: this.#initialAssignmentChanges,
      finalize
    });
    this.#emitInitialEpoch = false;
    this.#initialAssignmentChanges = [];
    this.#finalized = finalize;
    return result;
  }

  /**
   * Advances from an already time-ordered async iterable of same-time groups.
   * This avoids collecting or sorting a full global slab in memory.
   */
  async advanceEventGroups({endTimeUs, eventGroups, finalize = false}) {
    if (this.#finalized) fail('streaming assignment session is already finalized');
    if (typeof finalize !== 'boolean') fail('finalize must be boolean');
    assertTime(endTimeUs, 'endTimeUs');
    if (endTimeUs <= this.#state.timeUs) fail('endTimeUs must be after the current timeline time');
    if (eventGroups === null || eventGroups === undefined ||
        typeof eventGroups[Symbol.asyncIterator] !== 'function' &&
        typeof eventGroups[Symbol.iterator] !== 'function') {
      fail('eventGroups must be an iterable of same-time event arrays');
    }
    const completedIntervals = [];
    const epochs = this.#emitInitialEpoch ? [{
      timeUs: this.#state.timeUs,
      success: this.#state.currentPlan.success,
      failureCounts: {...this.#state.currentPlan.failureCounts},
      failures: this.#state.currentPlan.failures.map((failure) => ({...failure})),
      assignmentChanges: this.#initialAssignmentChanges.map((change) => ({
        positionId: change.positionId,
        before: change.before ? {...change.before} : null,
        after: change.after ? {...change.after} : null
      })),
      failureChanges: []
    }] : [];
    let previousTimeUs;
    for await (const rawGroup of eventGroups) {
      const group = normalizeEventGroupForState(rawGroup, this.#state, endTimeUs);
      if (group.length === 0) continue;
      const timeUs = group[0].timeUs;
      if (group.some((event) => event.timeUs !== timeUs) ||
          previousTimeUs !== undefined && timeUs <= previousTimeUs) {
        fail('eventGroups must contain strictly ordered same-time groups');
      }
      const epoch = processLiveEventGroup(this.#state, group, timeUs, completedIntervals);
      if (epoch !== null) epochs.push(epoch);
      previousTimeUs = timeUs;
    }
    this.#emitInitialEpoch = false;
    this.#initialAssignmentChanges = [];
    this.#finalized = finalize;
    return finishStateSegment(this.#state, endTimeUs, completedIntervals, epochs, finalize);
  }
}

/** Restores the expensive live indexes once, then reuses them across many slabs. */
export function restoreStreamingAssignmentSession(checkpoint) {
  return new StreamingAssignmentSession(restoreState(checkpoint));
}

/** Creates a reusable session without collecting the first slab's events. */
export function createStreamingAssignmentSession({
  startTimeUs,
  positionIds,
  initialVisibility = [],
  startEvents = [],
  registry,
  initialAssignments = undefined,
  satelliteCapacity = 256,
  cellCapacity = 128
}) {
  assertTime(startTimeUs, 'startTimeUs');
  if (startTimeUs >= Number.MAX_SAFE_INTEGER) fail('startTimeUs is too large to validate boundary events');
  const normalizedRegistry = normalizeRegistry(registry);
  const orderedPositions = normalizePositionIds(positionIds);
  const bySatellite = registryMap(normalizedRegistry);
  const orderedStartEvents = normalizeEvents(
    startEvents,
    orderedPositions,
    bySatellite,
    startTimeUs,
    startTimeUs + 1
  );
  if (orderedStartEvents.some(({timeUs}) => timeUs !== startTimeUs)) {
    fail('startEvents must all occur at startTimeUs');
  }
  const initialCandidateState = normalizeInitialCandidates(initialVisibility, orderedPositions, bySatellite);
  for (const event of orderedStartEvents) applyEvent(initialCandidateState.complete, initialCandidateState.entry, event);
  const effectiveVisibility = [];
  for (const positionId of orderedPositions) {
    for (const [satelliteId, score] of initialCandidateState.complete.get(positionId)) {
      effectiveVisibility.push({
        positionId,
        satelliteId,
        aboveEntry: initialCandidateState.entry.get(positionId).has(satelliteId),
        aboveRelease: true,
        score
      });
    }
  }
  const state = createState({
    startTimeUs,
    positionIds: orderedPositions,
    initialVisibility: effectiveVisibility,
    registry: normalizedRegistry,
    initialAssignments,
    satelliteCapacity,
    cellCapacity
  });
  const initialAssignmentChanges = applyBoundaryAssignmentChanges(state, initialAssignments);
  return new StreamingAssignmentSession(state, {emitInitialEpoch: true, initialAssignmentChanges});
}

/**
 * Starts one reusable session and processes its first segment. Start-boundary
 * events are applied before the first plan, while supplied sticky assignments
 * remain available for transition evidence and counters.
 */
export function startStreamingAssignmentSession({
  startTimeUs,
  endTimeUs,
  positionIds,
  initialVisibility = [],
  events = [],
  registry,
  initialAssignments = undefined,
  satelliteCapacity = 256,
  cellCapacity = 128,
  finalize = true
}) {
  assertTime(endTimeUs, 'endTimeUs');
  if (endTimeUs <= startTimeUs) fail('endTimeUs must be after startTimeUs');
  const normalizedRegistry = normalizeRegistry(registry);
  const orderedPositions = normalizePositionIds(positionIds);
  const bySatellite = registryMap(normalizedRegistry);
  const orderedEvents = normalizeEvents(events, orderedPositions, bySatellite, startTimeUs, endTimeUs);
  let initialEnd = 0;
  while (initialEnd < orderedEvents.length && orderedEvents[initialEnd].timeUs === startTimeUs) initialEnd += 1;
  const session = createStreamingAssignmentSession({
    startTimeUs,
    positionIds: orderedPositions,
    initialVisibility,
    startEvents: orderedEvents.slice(0, initialEnd),
    registry: normalizedRegistry,
    initialAssignments,
    satelliteCapacity,
    cellCapacity
  });
  if (typeof finalize !== 'boolean') fail('finalize must be boolean');
  return {
    session,
    result: session.advance({endTimeUs, events: orderedEvents.slice(initialEnd), finalize})
  };
}

/**
 * Replays an unordered event collection after normalizing it into deterministic
 * same-time groups. The returned result and checkpoint contain JSON values only.
 */
export function replayStreamingAssignmentTimeline(options) {
  return startStreamingAssignmentSession(options).result;
}

/**
 * Continues a JSON-round-tripped checkpoint. Use finalize=false for an
 * intermediate chunk: only intervals completed in that chunk are returned,
 * while open interval starts remain in the bounded checkpoint.
 */
export function resumeStreamingAssignmentTimeline({checkpoint, endTimeUs, events = [], finalize = true}) {
  return restoreStreamingAssignmentSession(checkpoint).advance({endTimeUs, events, finalize});
}
