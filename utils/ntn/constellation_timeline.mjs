import {planAssignmentEpoch} from './constellation_assignment.mjs';

const EVENT_ORDER = Object.freeze({
  release_exit: 0,
  entry_exit: 1,
  release_enter: 2,
  entry_enter: 3
});

function fail(message) {
  throw new Error(message);
}

function compareText(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function assertTime(value, context) {
  if (!Number.isSafeInteger(value)) fail(`${context} must be integer microseconds`);
}

function pairKey(positionId, satelliteId) {
  return `${positionId}\0${satelliteId}`;
}

function validatePositionIds(positionIds) {
  if (!Array.isArray(positionIds)) fail('positionIds must be an array');
  const ordered = [...positionIds].sort(compareText);
  if (ordered.some((id) => typeof id !== 'string' || id.length === 0)) {
    fail('positionIds must contain non-empty strings');
  }
  if (new Set(ordered).size !== ordered.length) fail('positionIds must be unique');
  return ordered;
}

function validateVisibilityRecord(record, context, positionSet) {
  if (record === null || typeof record !== 'object' || Array.isArray(record)) fail(`${context} must be an object`);
  if (!positionSet.has(record.positionId)) fail(`${context}.positionId is unknown`);
  if (typeof record.satelliteId !== 'string' || record.satelliteId.length === 0) {
    fail(`${context}.satelliteId must be a non-empty string`);
  }
  if (typeof record.aboveEntry !== 'boolean' || typeof record.aboveRelease !== 'boolean') {
    fail(`${context} threshold states must be boolean`);
  }
  if (record.aboveEntry && !record.aboveRelease) fail(`${context} cannot be above entry but below release`);
  const score = record.score ?? 0;
  if (!Number.isFinite(score)) fail(`${context}.score must be finite`);
  return {
    positionId: record.positionId,
    satelliteId: record.satelliteId,
    aboveEntry: record.aboveEntry,
    aboveRelease: record.aboveRelease,
    score
  };
}

function normalizeInitialVisibility(initialVisibility, positionSet) {
  if (!Array.isArray(initialVisibility)) fail('initialVisibility must be an array');
  const states = new Map();
  for (const [index, item] of initialVisibility.entries()) {
    const state = validateVisibilityRecord(item, `initialVisibility[${index}]`, positionSet);
    const key = pairKey(state.positionId, state.satelliteId);
    if (states.has(key)) fail(`initialVisibility contains duplicate pair '${state.positionId}/${state.satelliteId}'`);
    states.set(key, state);
  }
  return states;
}

function normalizeEvents(events, positionSet, startTimeUs, endTimeUs) {
  if (!Array.isArray(events)) fail('events must be an array');
  const normalized = events.map((event, index) => {
    if (event === null || typeof event !== 'object' || Array.isArray(event)) fail(`events[${index}] must be an object`);
    assertTime(event.timeUs, `events[${index}].timeUs`);
    if (event.timeUs < startTimeUs || event.timeUs >= endTimeUs) fail(`events[${index}] lies outside the timeline`);
    if (!Object.hasOwn(EVENT_ORDER, event.kind)) fail(`events[${index}].kind is invalid`);
    if (!positionSet.has(event.positionId)) fail(`events[${index}].positionId is unknown`);
    if (typeof event.satelliteId !== 'string' || event.satelliteId.length === 0) {
      fail(`events[${index}].satelliteId must be a non-empty string`);
    }
    const score = event.score ?? 0;
    if (!Number.isFinite(score)) fail(`events[${index}].score must be finite`);
    return {...event, score};
  });
  normalized.sort((left, right) => left.timeUs - right.timeUs
    || EVENT_ORDER[left.kind] - EVENT_ORDER[right.kind]
    || compareText(left.positionId, right.positionId)
    || compareText(left.satelliteId, right.satelliteId));
  const keys = new Set();
  for (const event of normalized) {
    const key = `${event.timeUs}\0${event.kind}\0${event.positionId}\0${event.satelliteId}`;
    if (keys.has(key)) fail('events must not contain duplicate state changes');
    keys.add(key);
  }
  return normalized;
}

function normalizePreviousAssignments(value) {
  if (value === undefined) return new Map();
  if (!(value instanceof Map)) fail('initialAssignments must be a Map');
  return new Map(value);
}

function applyEvent(states, event) {
  const key = pairKey(event.positionId, event.satelliteId);
  const state = states.get(key) ?? {
    positionId: event.positionId,
    satelliteId: event.satelliteId,
    aboveEntry: false,
    aboveRelease: false,
    score: event.score
  };
  state.score = event.score;
  if (event.kind === 'release_exit') {
    state.aboveEntry = false;
    state.aboveRelease = false;
  } else if (event.kind === 'entry_exit') {
    state.aboveEntry = false;
  } else if (event.kind === 'release_enter') {
    state.aboveRelease = true;
  } else {
    state.aboveEntry = true;
    state.aboveRelease = true;
  }
  if (!state.aboveEntry && !state.aboveRelease) states.delete(key);
  else states.set(key, state);
}

function candidateSets(positionIds, states, previousAssignments) {
  const complete = new Map(positionIds.map((positionId) => [positionId, []]));
  const eligible = new Map(positionIds.map((positionId) => [positionId, []]));
  for (const state of states.values()) {
    if (!state.aboveRelease) continue;
    const candidate = {satelliteId: state.satelliteId, score: state.score};
    complete.get(state.positionId).push(candidate);
    const incumbent = previousAssignments.get(state.positionId);
    const incumbentSatelliteId = typeof incumbent === 'string' ? incumbent : incumbent?.satelliteId;
    if (state.aboveEntry || incumbentSatelliteId === state.satelliteId) {
      eligible.get(state.positionId).push(candidate);
    }
  }
  const normalize = (source) => positionIds.map((positionId) => ({
    positionId,
    candidates: source.get(positionId).sort((left, right) => compareText(left.satelliteId, right.satelliteId))
  }));
  return {completeCandidateSets: normalize(complete), assignmentCandidateSets: normalize(eligible)};
}

function planAtTime({positionIds, states, previousAssignments, registry, satelliteCapacity, cellCapacity}) {
  return planAssignmentEpoch({
    ...candidateSets(positionIds, states, previousAssignments),
    registry,
    previousAssignments,
    satelliteCapacity,
    cellCapacity
  });
}

function assignmentChanged(left, right) {
  return left?.satelliteId !== right?.satelliteId || left?.cellBank !== right?.cellBank;
}

function mapAssignments(plan) {
  return new Map(plan.assignments.map((assignment) => [assignment.positionId, assignment]));
}

function baselineAssignments(previousAssignments, currentAssignments, positionIds) {
  const result = new Map();
  for (const positionId of positionIds) {
    const value = previousAssignments.get(positionId);
    if (value === undefined || value === null) continue;
    const satelliteId = typeof value === 'string' ? value : value.satelliteId;
    if (typeof satelliteId !== 'string' || satelliteId.length === 0) continue;
    const current = currentAssignments.get(positionId);
    result.set(positionId, {
      positionId,
      satelliteId,
      cellBank: typeof value === 'object' && (value.cellBank === 0 || value.cellBank === 1)
        ? value.cellBank
        : current?.satelliteId === satelliteId ? current.cellBank : null,
      ...typeof value === 'object' && Object.hasOwn(value, 'nci') ? {nci: value.nci} : {},
      ...typeof value === 'object' && Object.hasOwn(value, 'pci') ? {pci: value.pci} : {}
    });
  }
  return result;
}

function mapFailures(plan) {
  return new Map(plan.failures.map((failure) => [failure.positionId, failure.reason]));
}

function updateAssignmentPeaks(plan, peakBySatellite, peakByNci) {
  const bySatellite = new Map();
  const byNci = new Map();
  for (const assignment of plan.assignments) {
    bySatellite.set(assignment.satelliteId, (bySatellite.get(assignment.satelliteId) ?? 0) + 1);
    const nci = String(assignment.nci);
    byNci.set(nci, (byNci.get(nci) ?? 0) + 1);
  }
  for (const [satelliteId, count] of bySatellite) {
    peakBySatellite.set(satelliteId, Math.max(peakBySatellite.get(satelliteId) ?? 0, count));
  }
  for (const [nci, count] of byNci) peakByNci.set(nci, Math.max(peakByNci.get(nci) ?? 0, count));
}

/**
 * Replays threshold events into deterministic, capacity-checked assignment intervals.
 *
 * A new owner must be above the entry threshold. The incumbent may stay while it
 * remains above the lower release threshold. Exit events at one timestamp are
 * applied before entries, then the whole timestamp is planned atomically.
 */
export function replayAssignmentTimeline({
  startTimeUs,
  endTimeUs,
  positionIds,
  initialVisibility = [],
  events = [],
  registry,
  initialAssignments = undefined,
  satelliteCapacity = 256,
  cellCapacity = 128
}) {
  assertTime(startTimeUs, 'startTimeUs');
  assertTime(endTimeUs, 'endTimeUs');
  if (endTimeUs <= startTimeUs) fail('endTimeUs must be after startTimeUs');
  const orderedPositions = validatePositionIds(positionIds);
  const positionSet = new Set(orderedPositions);
  const states = normalizeInitialVisibility(initialVisibility, positionSet);
  const orderedEvents = normalizeEvents(events, positionSet, startTimeUs, endTimeUs);
  let previousAssignments = normalizePreviousAssignments(initialAssignments);
  let eventIndex = 0;
  while (eventIndex < orderedEvents.length && orderedEvents[eventIndex].timeUs === startTimeUs) {
    applyEvent(states, orderedEvents[eventIndex]);
    eventIndex += 1;
  }
  let currentPlan = planAtTime({
    positionIds: orderedPositions,
    states,
    previousAssignments,
    registry,
    satelliteCapacity,
    cellCapacity
  });
  let currentAssignments = mapAssignments(currentPlan);
  let currentFailures = mapFailures(currentPlan);
  const open = new Map();
  for (const assignment of currentPlan.assignments) open.set(assignment.positionId, {assignment, startTimeUs});
  const openFailures = new Map(
    [...currentFailures].map(([positionId, reason]) => [positionId, {reason, startTimeUs}])
  );
  const intervals = [];
  const failureIntervals = [];
  const transitions = [];
  const epochs = [{
    timeUs: startTimeUs,
    success: currentPlan.success,
    failureCounts: {...currentPlan.failureCounts},
    failures: currentPlan.failures.map((failure) => ({...failure}))
  }];
  const peakBySatellite = new Map(
    [...currentPlan.cellAllocationsBySatellite.keys()].map((satelliteId) => [satelliteId, 0])
  );
  const peakByNci = new Map();
  for (const allocation of currentPlan.cellAllocationsBySatellite.values()) {
    for (const cell of allocation.cells) peakByNci.set(String(cell.nci), 0);
  }
  updateAssignmentPeaks(currentPlan, peakBySatellite, peakByNci);
  const hasInitialAssignmentBaseline = initialAssignments !== undefined;
  const boundaryBaseline = hasInitialAssignmentBaseline
    ? baselineAssignments(previousAssignments, currentAssignments, orderedPositions)
    : new Map();
  const lastOwnerByPosition = new Map(
    [...(hasInitialAssignmentBaseline ? boundaryBaseline : currentAssignments)]
      .map(([positionId, assignment]) => [positionId, assignment.satelliteId])
  );
  let ownerChangeCount = 0;
  let handoverCount = 0;
  let releaseCount = 0;
  let acquisitionCount = 0;

  if (hasInitialAssignmentBaseline) {
    for (const positionId of orderedPositions) {
      const before = boundaryBaseline.get(positionId);
      const after = currentAssignments.get(positionId);
      if (!assignmentChanged(before, after)) continue;
      const kind = before && after
        ? before.satelliteId === after.satelliteId ? 'cell_rebalance' : 'handover'
        : before ? 'release' : 'acquisition';
      transitions.push({timeUs: startTimeUs, positionId, kind, before: before ?? null, after: after ?? null});
      if (before && after && before.satelliteId !== after.satelliteId) {
        ownerChangeCount += 1;
        handoverCount += 1;
      } else if (before && !after) {
        releaseCount += 1;
      } else if (!before && after) {
        acquisitionCount += 1;
        const lastOwner = lastOwnerByPosition.get(positionId);
        if (lastOwner !== undefined && lastOwner !== after.satelliteId) ownerChangeCount += 1;
      }
      if (after) lastOwnerByPosition.set(positionId, after.satelliteId);
    }
  }

  for (let index = eventIndex; index < orderedEvents.length;) {
    const timeUs = orderedEvents[index].timeUs;
    while (index < orderedEvents.length && orderedEvents[index].timeUs === timeUs) {
      applyEvent(states, orderedEvents[index]);
      index += 1;
    }
    previousAssignments = currentAssignments;
    const nextPlan = planAtTime({
      positionIds: orderedPositions,
      states,
      previousAssignments,
      registry,
      satelliteCapacity,
      cellCapacity
    });
    const nextAssignments = mapAssignments(nextPlan);
    const nextFailures = mapFailures(nextPlan);
    for (const positionId of orderedPositions) {
      const before = currentAssignments.get(positionId);
      const after = nextAssignments.get(positionId);
      if (assignmentChanged(before, after)) {
        const opened = open.get(positionId);
        if (opened && timeUs > opened.startTimeUs) {
          intervals.push({...opened.assignment, startTimeUs: opened.startTimeUs, endTimeUs: timeUs});
        }
        open.delete(positionId);
        const kind = before && after
          ? before.satelliteId === after.satelliteId ? 'cell_rebalance' : 'handover'
          : before ? 'release' : 'acquisition';
        transitions.push({
          timeUs,
          positionId,
          kind,
          before: before ? {...before} : null,
          after: after ? {...after} : null
        });
        if (before && after && before.satelliteId !== after.satelliteId) {
          ownerChangeCount += 1;
          handoverCount += 1;
        } else if (before && !after) {
          releaseCount += 1;
        } else if (!before && after) {
          acquisitionCount += 1;
          const lastOwner = lastOwnerByPosition.get(positionId);
          if (lastOwner !== undefined && lastOwner !== after.satelliteId) ownerChangeCount += 1;
        }
        if (after) lastOwnerByPosition.set(positionId, after.satelliteId);
        if (after && timeUs < endTimeUs) open.set(positionId, {assignment: after, startTimeUs: timeUs});
      }

      const beforeFailure = currentFailures.get(positionId);
      const afterFailure = nextFailures.get(positionId);
      if (beforeFailure !== afterFailure) {
        const openedFailure = openFailures.get(positionId);
        if (openedFailure && timeUs > openedFailure.startTimeUs) {
          failureIntervals.push({
            positionId,
            reason: openedFailure.reason,
            startTimeUs: openedFailure.startTimeUs,
            endTimeUs: timeUs
          });
        }
        openFailures.delete(positionId);
        if (afterFailure !== undefined) openFailures.set(positionId, {reason: afterFailure, startTimeUs: timeUs});
      }
    }
    currentPlan = nextPlan;
    currentAssignments = nextAssignments;
    currentFailures = nextFailures;
    updateAssignmentPeaks(currentPlan, peakBySatellite, peakByNci);
    epochs.push({
      timeUs,
      success: currentPlan.success,
      failureCounts: {...currentPlan.failureCounts},
      failures: currentPlan.failures.map((failure) => ({...failure}))
    });
  }
  for (const opened of open.values()) {
    if (endTimeUs > opened.startTimeUs) {
      intervals.push({...opened.assignment, startTimeUs: opened.startTimeUs, endTimeUs});
    }
  }
  for (const [positionId, opened] of openFailures) {
    if (endTimeUs > opened.startTimeUs) {
      failureIntervals.push({positionId, reason: opened.reason, startTimeUs: opened.startTimeUs, endTimeUs});
    }
  }
  intervals.sort((left, right) => compareText(left.positionId, right.positionId)
    || left.startTimeUs - right.startTimeUs
    || compareText(left.satelliteId, right.satelliteId)
    || left.cellBank - right.cellBank);
  failureIntervals.sort((left, right) => compareText(left.positionId, right.positionId)
    || left.startTimeUs - right.startTimeUs || compareText(left.reason, right.reason));

  return {
    intervals,
    failureIntervals,
    transitions,
    epochs,
    ownerChangeCount,
    handoverCount,
    releaseCount,
    acquisitionCount,
    peakAssignedBySatellite: [...peakBySatellite].map(([satelliteId, count]) => ({satelliteId, count}))
      .sort((left, right) => compareText(left.satelliteId, right.satelliteId)),
    peakAssignedByNci: [...peakByNci].map(([nci, count]) => ({nci, count}))
      .sort((left, right) => compareText(left.nci, right.nci)),
    finalPlan: currentPlan,
    finalAssignments: currentAssignments,
    finalVisibility: [...states.values()].sort((left, right) => compareText(left.positionId, right.positionId)
      || compareText(left.satelliteId, right.satelliteId))
  };
}
