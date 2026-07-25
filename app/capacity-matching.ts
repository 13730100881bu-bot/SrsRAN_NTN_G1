export type CapacityCandidate = {
  readonly satelliteId: string;
  readonly elevationDeg: number;
};

export type L1CandidateSet = {
  readonly positionId: string;
  readonly candidates: readonly CapacityCandidate[];
};

type FlowEdge = {
  to: number;
  reverse: number;
  capacity: number;
  satelliteId?: string;
};

/**
 * Deterministic maximum-cardinality matching between earth-fixed L1 positions
 * and serving satellites. Visibility remains an input and is never cropped by
 * this function.
 */
export function matchL1CandidatesToCapacity(
  candidateSets: readonly L1CandidateSet[],
  satelliteCapacity: number,
  previousAssignments: ReadonlyMap<string, string> = new Map(),
) {
  if (!Number.isInteger(satelliteCapacity) || satelliteCapacity <= 0) {
    throw new Error("satelliteCapacity must be a positive integer");
  }
  const satelliteIds = [...new Set(
    candidateSets.flatMap((set) => set.candidates.map((candidate) => candidate.satelliteId)),
  )].sort();
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
  const satelliteNodeById = new Map(
    satelliteIds.map((satelliteId, index) => [satelliteId, firstSatelliteNode + index]),
  );
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

  // Preserve valid previous owners first. Residual paths may still move them
  // when doing so is required to keep another L1 assigned.
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
      // Keep augmenting the current level graph.
    }
  }

  const satelliteByPosition = new Map<string, string>();
  candidateSets.forEach((candidateSet, setIndex) => {
    const positionNode = firstPositionNode + setIndex;
    const matched = graph[positionNode].find(
      (edge) => edge.satelliteId !== undefined && edge.capacity === 0,
    );
    if (matched?.satelliteId) satelliteByPosition.set(candidateSet.positionId, matched.satelliteId);
  });
  return satelliteByPosition;
}
