type CatalogCell = {
  id: string;
  lat: number;
  lon: number;
  childMask?: number | readonly boolean[] | string;
  childIds?: readonly string[];
};

type CatalogPayload = {
  metadata?: Record<string, unknown>;
  cells?: readonly CatalogCell[];
  l1?: readonly CatalogCell[];
  l1Catalog?: readonly CatalogCell[];
};

type LoadMessage = { type: "load"; url: string };
type AnalyzeMessage = {
  type: "analyze";
  requestId: number;
  satelliteId: string;
  latitudeDeg: number;
  longitudeDeg: number;
  radiusKm: number;
  capacity: number;
  timeline?: readonly { offsetSeconds: number; lat: number; lon: number }[];
};
type GlobalCoverageMessage = {
  type: "globalCoverage";
  requestId: number;
  radiusKm: number;
  satelliteSubpoints: readonly { id: string; lat: number; lon: number }[];
};

let catalog: readonly CatalogCell[] = [];
let catalogMetadata: Record<string, unknown> = {};
const earthRadiusKm = 6378.137;
const toRadians = (value: number) => value * Math.PI / 180;
const toDegrees = (value: number) => value * 180 / Math.PI;

function distanceKm(left: Pick<CatalogCell, "lat" | "lon">, right: Pick<CatalogCell, "lat" | "lon">) {
  const dLat = toRadians(right.lat - left.lat);
  const dLon = toRadians(right.lon - left.lon);
  const lat1 = toRadians(left.lat);
  const lat2 = toRadians(right.lat);
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon / 2) ** 2;
  return 2 * earthRadiusKm * Math.asin(Math.min(1, Math.sqrt(a)));
}

function normalizedCells(payload: CatalogPayload) {
  const source = payload.cells ?? payload.l1 ?? payload.l1Catalog ?? [];
  return source
    .map((cell) => ({
      ...cell,
      lat: Number(cell.lat),
      lon: Number(cell.lon),
    }))
    .filter((cell) => /^G\d{6}$/.test(cell.id) && Number.isFinite(cell.lat) && Number.isFinite(cell.lon));
}

function elevationFromGroundDistance(distance: number) {
  const centralAngle = distance / earthRadiusKm;
  const satelliteRadiusKm = earthRadiusKm + 500;
  const lineOfSightKm = Math.sqrt(
    satelliteRadiusKm ** 2 + earthRadiusKm ** 2 - 2 * satelliteRadiusKm * earthRadiusKm * Math.cos(centralAngle),
  );
  return toDegrees(Math.asin((satelliteRadiusKm * Math.cos(centralAngle) - earthRadiusKm) / lineOfSightKm));
}

self.addEventListener("message", async (event: MessageEvent<LoadMessage | AnalyzeMessage | GlobalCoverageMessage>) => {
  const message = event.data;
  if (message.type === "load") {
    try {
      const response = await fetch(message.url);
      if (!response.ok) throw new Error(`catalog request failed with ${response.status}`);
      const payload = await response.json() as CatalogPayload;
      catalog = normalizedCells(payload);
      catalogMetadata = payload.metadata ?? {};
      self.postMessage({ type: "loaded", cells: catalog, metadata: catalogMetadata, count: catalog.length });
    } catch (error) {
      self.postMessage({ type: "error", message: error instanceof Error ? error.message : String(error) });
    }
    return;
  }

  if (message.type === "analyze") {
    const subpoint = { lat: message.latitudeDeg, lon: message.longitudeDeg };
    const visible = catalog
      .map((cell) => ({ ...cell, distanceKm: distanceKm(subpoint, cell) }))
      .filter((cell) => cell.distanceKm <= message.radiusKm)
      .sort((left, right) => left.distanceKm - right.distanceKm || left.id.localeCompare(right.id));

    // A deterministic balanced spatial split.  It is a calendar input, not a
    // permanent property of the earth-fixed position.
    const localLongitude = (longitude: number) => ((longitude - message.longitudeDeg + 540) % 360) - 180;
    const byLongitude = [...visible].sort((left, right) =>
      localLongitude(left.lon) - localLongitude(right.lon) || left.lat - right.lat || left.id.localeCompare(right.id),
    );
    const splitIndex = Math.ceil(byLongitude.length / 2);
    const bankById = new Map(byLongitude.map((cell, index) => [cell.id, index < splitIndex ? 0 as const : 1 as const]));
    const cells = visible.map((cell) => {
      const timeline = (message.timeline ?? []).map((sample) => ({
        offsetSeconds: sample.offsetSeconds,
        elevationDeg: elevationFromGroundDistance(distanceKm(cell, sample)),
      }));
      const currentIndex = timeline.findIndex(({ offsetSeconds }) => offsetSeconds === 0);
      let first = currentIndex;
      let last = currentIndex;
      if (currentIndex >= 0) {
        while (first > 0 && timeline[first - 1].elevationDeg >= 45) first -= 1;
        while (last + 1 < timeline.length && timeline[last + 1].elevationDeg >= 45) last += 1;
      }
      return {
        ...cell,
        cellBank: bankById.get(cell.id) ?? 0,
        elevationDeg: elevationFromGroundDistance(cell.distanceKm),
        visibleFromSeconds: currentIndex >= 0 ? timeline[first].offsetSeconds : null,
        visibleUntilSeconds: currentIndex >= 0 ? timeline[last].offsetSeconds : null,
        elevationTimeline: timeline.filter(({ offsetSeconds }) => offsetSeconds === -30 || offsetSeconds === 0 || offsetSeconds === 30),
        tableVersion: catalogMetadata.version ?? "unknown",
      };
    });
    const byCell = [cells.filter((cell) => cell.cellBank === 0).length, cells.filter((cell) => cell.cellBank === 1).length];

    self.postMessage({
      type: "analysis",
      requestId: message.requestId,
      satelliteId: message.satelliteId,
      cells,
      visibleCount: cells.length,
      byCell,
      capacity: message.capacity,
      scheduleOverflow: cells.length > message.capacity || byCell.some((count) => count > message.capacity / 2),
    });
    return;
  }

  if (message.type === "globalCoverage") {
    const binSizeDeg = 6;
    const longitudeBins = Math.ceil(360 / binSizeDeg);
    const key = (latIndex: number, lonIndex: number) => `${latIndex}:${((lonIndex % longitudeBins) + longitudeBins) % longitudeBins}`;
    const bins = new Map<string, Array<{ lat: number; lon: number }>>();
    for (const point of message.satelliteSubpoints) {
      const latIndex = Math.floor((point.lat + 90) / binSizeDeg);
      const lonIndex = Math.floor((point.lon + 180) / binSizeDeg);
      const bucketKey = key(latIndex, lonIndex);
      const bucket = bins.get(bucketKey) ?? [];
      bucket.push(point);
      bins.set(bucketKey, bucket);
    }
    const counts = new Uint16Array(catalog.length);
    let uncovered = 0;
    catalog.forEach((cell, cellIndex) => {
      const latIndex = Math.floor((cell.lat + 90) / binSizeDeg);
      const lonIndex = Math.floor((cell.lon + 180) / binSizeDeg);
      let count = 0;
      for (let latOffset = -1; latOffset <= 1; latOffset += 1) {
        for (let lonOffset = -2; lonOffset <= 2; lonOffset += 1) {
          for (const satellite of bins.get(key(latIndex + latOffset, lonIndex + lonOffset)) ?? []) {
            if (distanceKm(cell, satellite) <= message.radiusKm) count += 1;
          }
        }
      }
      counts[cellIndex] = count;
      if (count === 0) uncovered += 1;
    });
    const catalogBins = new Map<string, CatalogCell[]>();
    for (const cell of catalog) {
      const latIndex = Math.floor((cell.lat + 90) / binSizeDeg);
      const lonIndex = Math.floor((cell.lon + 180) / binSizeDeg);
      const bucketKey = key(latIndex, lonIndex);
      const bucket = catalogBins.get(bucketKey) ?? [];
      bucket.push(cell);
      catalogBins.set(bucketKey, bucket);
    }
    let peakVisible = 0;
    let peakSatelliteId = "";
    let overflowSatellites = 0;
    for (const satellite of message.satelliteSubpoints) {
      const latIndex = Math.floor((satellite.lat + 90) / binSizeDeg);
      const lonIndex = Math.floor((satellite.lon + 180) / binSizeDeg);
      let visible = 0;
      for (let latOffset = -1; latOffset <= 1; latOffset += 1) {
        for (let lonOffset = -2; lonOffset <= 2; lonOffset += 1) {
          for (const cell of catalogBins.get(key(latIndex + latOffset, lonIndex + lonOffset)) ?? []) {
            if (distanceKm(cell, satellite) <= message.radiusKm) visible += 1;
          }
        }
      }
      if (visible > 256) overflowSatellites += 1;
      if (visible > peakVisible) { peakVisible = visible; peakSatelliteId = satellite.id; }
    }
    self.postMessage({
      type: "globalCoverage",
      requestId: message.requestId,
      counts: Array.from(counts),
      uncovered,
      peakVisible,
      peakSatelliteId,
      overflowSatellites,
    });
  }
});

export {};
