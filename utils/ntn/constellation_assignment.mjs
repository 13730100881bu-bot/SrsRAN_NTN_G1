/**
 * Dependency-free management-center helpers for capacity-constrained NTN L1 assignment.
 *
 * The complete candidate inventory and the candidates eligible for the current
 * assignment epoch are deliberately separate inputs. Capacity never trims the
 * complete inventory.
 */

export const DEFAULT_SATELLITE_CAPACITY = 256;
export const DEFAULT_CELL_CAPACITY = 128;

export const assignmentFailureReason = Object.freeze({
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

function assertPositiveInteger(value, name) {
  if (!Number.isInteger(value) || value <= 0) {
    fail(`${name} must be a positive integer`);
  }
}

function assertMap(value, name) {
  if (!(value instanceof Map)) {
    fail(`${name} must be a Map`);
  }
}

function candidateQuality(candidate) {
  const quality = candidate.elevationDeg ?? candidate.score ?? 0;
  if (!Number.isFinite(quality)) {
    fail(`candidate '${candidate.satelliteId}' quality must be finite`);
  }
  return quality;
}

function normalizeCandidateSets(candidateSets, context) {
  if (!Array.isArray(candidateSets)) {
    fail(`${context} must be an array`);
  }
  const seenPositions = new Set();
  const normalized = candidateSets.map((candidateSet, setIndex) => {
    if (candidateSet === null || typeof candidateSet !== 'object' || Array.isArray(candidateSet)) {
      fail(`${context}[${setIndex}] must be an object`);
    }
    const {positionId} = candidateSet;
    if (typeof positionId !== 'string' || positionId.length === 0) {
      fail(`${context}[${setIndex}].positionId must be a non-empty string`);
    }
    if (seenPositions.has(positionId)) {
      fail(`${context} contains duplicate positionId '${positionId}'`);
    }
    seenPositions.add(positionId);
    if (!Array.isArray(candidateSet.candidates)) {
      fail(`${context}[${setIndex}].candidates must be an array`);
    }
    const seenSatellites = new Set();
    const candidates = candidateSet.candidates.map((candidate, candidateIndex) => {
      if (candidate === null || typeof candidate !== 'object' || Array.isArray(candidate)) {
        fail(`${context}[${setIndex}].candidates[${candidateIndex}] must be an object`);
      }
      if (typeof candidate.satelliteId !== 'string' || candidate.satelliteId.length === 0) {
        fail(`${context}[${setIndex}].candidates[${candidateIndex}].satelliteId must be a non-empty string`);
      }
      if (seenSatellites.has(candidate.satelliteId)) {
        fail(`${context}[${setIndex}] contains duplicate candidate satellite '${candidate.satelliteId}'`);
      }
      seenSatellites.add(candidate.satelliteId);
      candidateQuality(candidate);
      return {...candidate, satelliteId: candidate.satelliteId};
    }).sort((left, right) => compareText(left.satelliteId, right.satelliteId));
    return {positionId, candidates};
  });
  normalized.sort((left, right) => compareText(left.positionId, right.positionId));
  return normalized;
}

function previousSatelliteId(previousAssignments, positionId) {
  const previous = previousAssignments.get(positionId);
  return typeof previous === 'string' ? previous : previous?.satelliteId;
}

function previousCellBank(previousAssignments, positionId) {
  const previous = previousAssignments.get(positionId);
  return typeof previous === 'object' && previous !== null ? previous.cellBank : undefined;
}

/**
 * Deterministic maximum-cardinality bipartite b-matching.
 *
 * Every position has capacity one and every satellite has satelliteCapacity.
 * Valid previous owners are seeded first, but residual augmenting paths may move
 * them when that is necessary to avoid leaving another position unmatched.
 */
export function matchCandidatesToCapacity(
  candidateSets,
  {satelliteCapacity = DEFAULT_SATELLITE_CAPACITY, previousAssignments = new Map()} = {}
) {
  assertPositiveInteger(satelliteCapacity, 'satelliteCapacity');
  assertMap(previousAssignments, 'previousAssignments');
  const sets = normalizeCandidateSets(candidateSets, 'candidateSets');
  if (sets.length === 0) {
    return new Map();
  }

  const satelliteIds = [...new Set(sets.flatMap((set) => set.candidates.map((candidate) => candidate.satelliteId)))].sort();
  const source = 0;
  const firstPositionNode = 1;
  const firstSatelliteNode = firstPositionNode + sets.length;
  const sink = firstSatelliteNode + satelliteIds.length;
  const graph = Array.from({length: sink + 1}, () => []);
  const addEdge = (from, to, capacity, satelliteId = undefined) => {
    const forward = {to, reverse: graph[to].length, capacity, satelliteId};
    const reverse = {to: from, reverse: graph[from].length, capacity: 0, satelliteId: undefined};
    graph[from].push(forward);
    graph[to].push(reverse);
    return forward;
  };

  const satelliteNodeById = new Map(
    satelliteIds.map((satelliteId, index) => [satelliteId, firstSatelliteNode + index])
  );
  const sourceEdgeByPositionId = new Map();
  const candidateEdgeByKey = new Map();
  const sinkEdgeBySatelliteId = new Map();

  sets.forEach((candidateSet, setIndex) => {
    const positionNode = firstPositionNode + setIndex;
    sourceEdgeByPositionId.set(candidateSet.positionId, addEdge(source, positionNode, 1));
    const incumbent = previousSatelliteId(previousAssignments, candidateSet.positionId);
    const orderedCandidates = [...candidateSet.candidates].sort((left, right) => {
      const incumbentOrder = Number(right.satelliteId === incumbent) - Number(left.satelliteId === incumbent);
      return incumbentOrder || candidateQuality(right) - candidateQuality(left) ||
        compareText(left.satelliteId, right.satelliteId);
    });
    for (const candidate of orderedCandidates) {
      candidateEdgeByKey.set(
        `${candidateSet.positionId}\0${candidate.satelliteId}`,
        addEdge(positionNode, satelliteNodeById.get(candidate.satelliteId), 1, candidate.satelliteId)
      );
    }
  });
  for (const satelliteId of satelliteIds) {
    sinkEdgeBySatelliteId.set(
      satelliteId,
      addEdge(satelliteNodeById.get(satelliteId), sink, satelliteCapacity)
    );
  }

  const consume = (edge) => {
    edge.capacity -= 1;
    graph[edge.to][edge.reverse].capacity += 1;
  };
  const seededLoad = new Map();
  for (const candidateSet of sets) {
    const incumbent = previousSatelliteId(previousAssignments, candidateSet.positionId);
    if (!incumbent || (seededLoad.get(incumbent) ?? 0) >= satelliteCapacity) {
      continue;
    }
    const sourceEdge = sourceEdgeByPositionId.get(candidateSet.positionId);
    const candidateEdge = candidateEdgeByKey.get(`${candidateSet.positionId}\0${incumbent}`);
    const sinkEdge = sinkEdgeBySatelliteId.get(incumbent);
    if (!sourceEdge || !candidateEdge || !sinkEdge || sinkEdge.capacity <= 0) {
      continue;
    }
    consume(sourceEdge);
    consume(candidateEdge);
    consume(sinkEdge);
    seededLoad.set(incumbent, (seededLoad.get(incumbent) ?? 0) + 1);
  }

  const levels = Array(graph.length).fill(-1);
  const cursors = Array(graph.length).fill(0);
  const buildLevels = () => {
    levels.fill(-1);
    levels[source] = 0;
    const queue = [source];
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
  const send = (node, flow) => {
    if (node === sink) {
      return flow;
    }
    for (; cursors[node] < graph[node].length; cursors[node] += 1) {
      const edge = graph[node][cursors[node]];
      if (edge.capacity <= 0 || levels[edge.to] !== levels[node] + 1) {
        continue;
      }
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
    cursors.fill(0);
    while (send(source, Number.MAX_SAFE_INTEGER) > 0) {
      // Continue augmenting the current level graph.
    }
  }

  const satelliteByPositionId = new Map();
  sets.forEach((candidateSet, setIndex) => {
    const positionNode = firstPositionNode + setIndex;
    const matched = graph[positionNode].find((edge) => edge.satelliteId !== undefined && edge.capacity === 0);
    if (matched?.satelliteId !== undefined) {
      satelliteByPositionId.set(candidateSet.positionId, matched.satelliteId);
    }
  });
  return satelliteByPositionId;
}

function validateNci(nci, context) {
  let numeric;
  try {
    if (typeof nci === 'number' && (!Number.isSafeInteger(nci) || nci < 0)) {
      fail(`${context} must be a non-negative safe integer or numeric string`);
    }
    if (typeof nci !== 'number' && (typeof nci !== 'string' || nci.length === 0)) {
      fail(`${context} must be a non-negative safe integer or numeric string`);
    }
    numeric = BigInt(nci);
  } catch {
    fail(`${context} must be a numeric 36-bit NCI`);
  }
  if (numeric < 0n || numeric >= (1n << 36n)) {
    fail(`${context} must fit in 36 bits`);
  }
  return numeric;
}

function normalizeRegistry(registry) {
  const entries = Array.isArray(registry) ? registry : registry?.satellites;
  if (!Array.isArray(entries)) {
    fail('registry must be an array or an object with a satellites array');
  }
  const bySatellite = new Map();
  const seenNcis = new Set();
  for (const [entryIndex, entry] of entries.entries()) {
    const satelliteId = entry?.satelliteId ?? entry?.satellite_id;
    if (typeof satelliteId !== 'string' || satelliteId.length === 0) {
      fail(`registry[${entryIndex}] satellite id must be a non-empty string`);
    }
    if (bySatellite.has(satelliteId)) {
      fail(`registry contains duplicate satellite '${satelliteId}'`);
    }
    if (!Array.isArray(entry.cells) || entry.cells.length !== 2) {
      fail(`registry satellite '${satelliteId}' must contain exactly two cells`);
    }
    const cells = [];
    for (const [cellIndex, cell] of entry.cells.entries()) {
      if (cell?.bank !== 0 && cell?.bank !== 1) {
        fail(`registry satellite '${satelliteId}' cell ${cellIndex} bank must be 0 or 1`);
      }
      if (cells.some((existing) => existing.bank === cell.bank)) {
        fail(`registry satellite '${satelliteId}' contains duplicate bank ${cell.bank}`);
      }
      const numericNci = validateNci(cell.nci, `registry satellite '${satelliteId}' cell ${cellIndex} nci`);
      const nciKey = numericNci.toString();
      if (seenNcis.has(nciKey)) {
        fail(`registry contains duplicate NCI '${cell.nci}'`);
      }
      seenNcis.add(nciKey);
      if (!Number.isInteger(cell.pci) || cell.pci < 0 || cell.pci > 1007) {
        fail(`registry satellite '${satelliteId}' cell ${cellIndex} pci must be in 0..1007`);
      }
      cells.push({satelliteId, bank: cell.bank, nci: cell.nci, pci: cell.pci});
    }
    cells.sort((left, right) => left.bank - right.bank);
    bySatellite.set(satelliteId, {satelliteId, cells});
  }
  return new Map([...bySatellite].sort(([left], [right]) => compareText(left, right)));
}

/**
 * Sticky deterministic split of satellite matches across two registry-owned cells.
 * Existing feasible banks are retained first; new positions fill the less-loaded bank.
 * Overflow is explicit and never silently discarded.
 */
export function partitionAssignmentsToTwoCells({
  satelliteId,
  positionIds,
  registryCells,
  previousAssignments = new Map(),
  cellCapacity = DEFAULT_CELL_CAPACITY
}) {
  if (typeof satelliteId !== 'string' || satelliteId.length === 0) {
    fail('satelliteId must be a non-empty string');
  }
  if (!Array.isArray(positionIds)) {
    fail('positionIds must be an array');
  }
  assertPositiveInteger(cellCapacity, 'cellCapacity');
  assertMap(previousAssignments, 'previousAssignments');
  const registry = normalizeRegistry([{satelliteId, cells: registryCells}]);
  const identities = registry.get(satelliteId).cells;
  const ordered = [...positionIds].sort(compareText);
  if (ordered.some((positionId) => typeof positionId !== 'string' || positionId.length === 0)) {
    fail('positionIds must contain non-empty strings');
  }
  if (new Set(ordered).size !== ordered.length) {
    fail('positionIds must be unique');
  }

  const byBank = [[], []];
  const fresh = [];
  for (const positionId of ordered) {
    const previousOwner = previousSatelliteId(previousAssignments, positionId);
    const previousBank = previousCellBank(previousAssignments, positionId);
    if (previousOwner === satelliteId && (previousBank === 0 || previousBank === 1) &&
        byBank[previousBank].length < cellCapacity) {
      byBank[previousBank].push(positionId);
    } else {
      fresh.push(positionId);
    }
  }

  const overflowPositionIds = [];
  for (const positionId of fresh) {
    let bank;
    if (byBank[0].length >= cellCapacity && byBank[1].length >= cellCapacity) {
      overflowPositionIds.push(positionId);
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

  return {
    satelliteId,
    cells: identities.map((identity) => ({
      ...identity,
      assignedPositionIds: [...byBank[identity.bank]].sort(compareText)
    })),
    overflowPositionIds: overflowPositionIds.sort(compareText)
  };
}

/**
 * Plans one assignment epoch without conflating visibility inventory with capacity.
 *
 * completeCandidateSets contains the full management-center inventory. The
 * assignmentCandidateSets argument contains only candidates eligible to own the
 * position at this epoch. The latter must be a subset of the former.
 */
export function planAssignmentEpoch({
  completeCandidateSets,
  assignmentCandidateSets,
  registry,
  previousAssignments = new Map(),
  satelliteCapacity = DEFAULT_SATELLITE_CAPACITY,
  cellCapacity = DEFAULT_CELL_CAPACITY
}) {
  assertPositiveInteger(satelliteCapacity, 'satelliteCapacity');
  assertPositiveInteger(cellCapacity, 'cellCapacity');
  assertMap(previousAssignments, 'previousAssignments');
  const complete = normalizeCandidateSets(completeCandidateSets, 'completeCandidateSets');
  const eligible = normalizeCandidateSets(assignmentCandidateSets, 'assignmentCandidateSets');
  const registryBySatellite = normalizeRegistry(registry);
  const completeByPosition = new Map(complete.map((entry) => [entry.positionId, entry]));
  const eligibleByPosition = new Map(eligible.map((entry) => [entry.positionId, entry]));

  for (const entry of eligible) {
    const inventory = completeByPosition.get(entry.positionId);
    if (!inventory) {
      fail(`assignment position '${entry.positionId}' is absent from the complete inventory`);
    }
    const inventorySatellites = new Set(inventory.candidates.map((candidate) => candidate.satelliteId));
    for (const candidate of entry.candidates) {
      if (!inventorySatellites.has(candidate.satelliteId)) {
        fail(`assignment candidate '${entry.positionId}/${candidate.satelliteId}' is absent from the complete inventory`);
      }
    }
  }

  for (const entry of complete) {
    for (const candidate of entry.candidates) {
      if (!registryBySatellite.has(candidate.satelliteId)) {
        fail(`complete inventory references unknown satellite '${candidate.satelliteId}'`);
      }
    }
  }
  for (const entry of eligible) {
    for (const candidate of entry.candidates) {
      if (!registryBySatellite.has(candidate.satelliteId)) {
        fail(`assignment candidates reference unknown satellite '${candidate.satelliteId}'`);
      }
    }
  }

  const candidateInventoryByPositionId = new Map(
    complete.map((entry) => [entry.positionId, entry.candidates.map((candidate) => ({...candidate}))])
  );
  const completeInventoryBySatellite = new Map(
    [...registryBySatellite.keys()].map((satelliteId) => [satelliteId, []])
  );
  for (const entry of complete) {
    for (const candidate of entry.candidates) {
      completeInventoryBySatellite.get(candidate.satelliteId).push({positionId: entry.positionId, ...candidate});
    }
  }
  for (const inventory of completeInventoryBySatellite.values()) {
    inventory.sort((left, right) => compareText(left.positionId, right.positionId));
  }

  const matchingInput = complete.map((entry) => ({
    positionId: entry.positionId,
    candidates: eligibleByPosition.get(entry.positionId)?.candidates ?? []
  }));
  const satelliteMatchesByPositionId = matchCandidatesToCapacity(matchingInput, {
    satelliteCapacity,
    previousAssignments
  });
  const positionsBySatellite = new Map([...registryBySatellite.keys()].map((satelliteId) => [satelliteId, []]));
  for (const [positionId, satelliteId] of satelliteMatchesByPositionId) {
    positionsBySatellite.get(satelliteId).push(positionId);
  }

  const cellAllocationsBySatellite = new Map();
  const overflowByPositionId = new Map();
  for (const [satelliteId, registryEntry] of registryBySatellite) {
    const allocation = partitionAssignmentsToTwoCells({
      satelliteId,
      positionIds: positionsBySatellite.get(satelliteId),
      registryCells: registryEntry.cells,
      previousAssignments,
      cellCapacity
    });
    cellAllocationsBySatellite.set(satelliteId, allocation);
    for (const positionId of allocation.overflowPositionIds) {
      overflowByPositionId.set(positionId, satelliteId);
    }
  }

  const assignments = [];
  for (const [satelliteId, allocation] of cellAllocationsBySatellite) {
    for (const cell of allocation.cells) {
      for (const positionId of cell.assignedPositionIds) {
        assignments.push({
          positionId,
          satelliteId,
          cellBank: cell.bank,
          nci: cell.nci,
          pci: cell.pci
        });
      }
    }
  }
  assignments.sort((left, right) => compareText(left.positionId, right.positionId));
  const assignmentByPositionId = new Map(assignments.map((assignment) => [assignment.positionId, assignment]));

  const failures = [];
  for (const entry of complete) {
    if (overflowByPositionId.has(entry.positionId)) {
      failures.push({positionId: entry.positionId, reason: assignmentFailureReason.cellPartitionOverflow});
      continue;
    }
    if (assignmentByPositionId.has(entry.positionId)) {
      continue;
    }
    const eligibleCandidates = eligibleByPosition.get(entry.positionId)?.candidates ?? [];
    failures.push({
      positionId: entry.positionId,
      reason: eligibleCandidates.length === 0
        ? assignmentFailureReason.noVisibleCandidate
        : assignmentFailureReason.scheduleOverflow
    });
  }
  failures.sort((left, right) => compareText(left.positionId, right.positionId));
  const failureCounts = {
    [assignmentFailureReason.noVisibleCandidate]: 0,
    [assignmentFailureReason.scheduleOverflow]: 0,
    [assignmentFailureReason.cellPartitionOverflow]: 0
  };
  for (const failure of failures) {
    failureCounts[failure.reason] += 1;
  }
  const failureReason = failureCounts[assignmentFailureReason.cellPartitionOverflow] > 0
    ? assignmentFailureReason.cellPartitionOverflow
    : failureCounts[assignmentFailureReason.scheduleOverflow] > 0
      ? assignmentFailureReason.scheduleOverflow
      : failureCounts[assignmentFailureReason.noVisibleCandidate] > 0
        ? assignmentFailureReason.noVisibleCandidate
        : null;

  return {
    success: failures.length === 0,
    failureReason,
    failureCounts,
    failures,
    unassignedPositionIds: failures.map((failure) => failure.positionId),
    candidateInventoryByPositionId,
    completeInventoryBySatellite,
    satelliteMatchesByPositionId,
    assignments,
    assignmentByPositionId,
    cellAllocationsBySatellite,
    satelliteCapacity,
    cellCapacity
  };
}
