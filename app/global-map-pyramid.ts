import { normalizeLongitude } from "./global-position-hexagon";

export type GlobalMapPyramidCell = Readonly<{
  lat: number;
  lon: number;
}>;

export type GlobalMapBounds = Readonly<{
  minLon: number;
  maxLon: number;
  minLat: number;
  maxLat: number;
}>;

export type GlobalMapCluster = Readonly<{
  key: string;
  centerLon: number;
  centerLat: number;
  minLon: number;
  maxLon: number;
  minLat: number;
  maxLat: number;
  cellIndexes: readonly number[];
}>;

type GlobalMapPyramidLevel = Readonly<{
  binSizeDeg: number;
  clusters: readonly GlobalMapCluster[];
}>;

export type GlobalMapPyramid = Readonly<{
  cells: readonly GlobalMapPyramidCell[];
  overview: GlobalMapPyramidLevel;
  regional: GlobalMapPyramidLevel;
}>;

export type GlobalMapRenderPlan =
  | Readonly<{
      mode: "overview" | "regional";
      clusters: readonly GlobalMapCluster[];
      cellIndexes: readonly [];
      representedCellCount: number;
      drawItemCount: number;
    }>
  | Readonly<{
      mode: "detail";
      clusters: readonly [];
      cellIndexes: readonly number[];
      representedCellCount: number;
      drawItemCount: number;
    }>;

export const GLOBAL_MAP_REGIONAL_ZOOM = 1.75;
export const GLOBAL_MAP_DETAIL_ZOOM = 3.25;
export const GLOBAL_MAP_MAX_DETAIL_SHAPES = 5000;

const OVERVIEW_BIN_SIZE_DEG = 4;
const REGIONAL_BIN_SIZE_DEG = 2;

function buildLevel(
  cells: readonly GlobalMapPyramidCell[],
  binSizeDeg: number,
): GlobalMapPyramidLevel {
  type MutableCluster = {
    key: string;
    latitudeTotal: number;
    longitudeTotal: number;
    minLon: number;
    maxLon: number;
    minLat: number;
    maxLat: number;
    cellIndexes: number[];
  };

  const buckets = new Map<string, MutableCluster>();
  cells.forEach((cell, index) => {
    const longitude = normalizeLongitude(cell.lon);
    const lonBin = Math.min(
      Math.ceil(360 / binSizeDeg) - 1,
      Math.max(0, Math.floor((longitude + 180) / binSizeDeg)),
    );
    const latBin = Math.min(
      Math.ceil(180 / binSizeDeg) - 1,
      Math.max(0, Math.floor((cell.lat + 90) / binSizeDeg)),
    );
    const key = `${lonBin}:${latBin}`;
    const bucket = buckets.get(key) ?? {
      key,
      latitudeTotal: 0,
      longitudeTotal: 0,
      minLon: longitude,
      maxLon: longitude,
      minLat: cell.lat,
      maxLat: cell.lat,
      cellIndexes: [],
    };
    bucket.latitudeTotal += cell.lat;
    bucket.longitudeTotal += longitude;
    bucket.minLon = Math.min(bucket.minLon, longitude);
    bucket.maxLon = Math.max(bucket.maxLon, longitude);
    bucket.minLat = Math.min(bucket.minLat, cell.lat);
    bucket.maxLat = Math.max(bucket.maxLat, cell.lat);
    bucket.cellIndexes.push(index);
    buckets.set(key, bucket);
  });

  const clusters = [...buckets.values()]
    .map((bucket): GlobalMapCluster => ({
      key: bucket.key,
      centerLon: bucket.longitudeTotal / bucket.cellIndexes.length,
      centerLat: bucket.latitudeTotal / bucket.cellIndexes.length,
      minLon: bucket.minLon,
      maxLon: bucket.maxLon,
      minLat: bucket.minLat,
      maxLat: bucket.maxLat,
      cellIndexes: bucket.cellIndexes,
    }))
    .sort((left, right) => left.key.localeCompare(right.key));

  return { binSizeDeg, clusters };
}

export function createGlobalMapPyramid(
  cells: readonly GlobalMapPyramidCell[],
): GlobalMapPyramid {
  return {
    cells,
    overview: buildLevel(cells, OVERVIEW_BIN_SIZE_DEG),
    regional: buildLevel(cells, REGIONAL_BIN_SIZE_DEG),
  };
}

function clusterIntersectsBounds(
  cluster: GlobalMapCluster,
  binSizeDeg: number,
  bounds: GlobalMapBounds,
) {
  const margin = Math.min(0.5, binSizeDeg / 4);
  return cluster.maxLon >= bounds.minLon - margin
    && cluster.minLon <= bounds.maxLon + margin
    && cluster.maxLat >= bounds.minLat - margin
    && cluster.minLat <= bounds.maxLat + margin;
}

function clustersInside(
  level: GlobalMapPyramidLevel,
  bounds: GlobalMapBounds,
) {
  return level.clusters.filter((cluster) => (
    clusterIntersectsBounds(cluster, level.binSizeDeg, bounds)
  ));
}

/**
 * Selects the smallest useful set of shapes for the current view.
 *
 * The complete catalog remains in memory. Only drawing changes:
 * - full-earth views use 4-degree groups;
 * - regional views use 2-degree groups;
 * - close views draw the individual catalog hexagons inside the viewport.
 */
export function selectGlobalMapRenderPlan(
  pyramid: GlobalMapPyramid,
  zoom: number,
  bounds: GlobalMapBounds,
  detailMarginDeg = 1,
): GlobalMapRenderPlan {
  if (zoom < GLOBAL_MAP_REGIONAL_ZOOM) {
    const clusters = clustersInside(pyramid.overview, bounds);
    const representedCellCount = clusters.reduce(
      (total, cluster) => total + cluster.cellIndexes.length,
      0,
    );
    return {
      mode: "overview",
      clusters,
      cellIndexes: [],
      representedCellCount,
      drawItemCount: clusters.length,
    };
  }

  if (zoom < GLOBAL_MAP_DETAIL_ZOOM) {
    const clusters = clustersInside(pyramid.regional, bounds);
    const representedCellCount = clusters.reduce(
      (total, cluster) => total + cluster.cellIndexes.length,
      0,
    );
    return {
      mode: "regional",
      clusters,
      cellIndexes: [],
      representedCellCount,
      drawItemCount: clusters.length,
    };
  }

  const expandedBounds = {
    minLon: Math.max(-180, bounds.minLon - detailMarginDeg),
    maxLon: Math.min(180, bounds.maxLon + detailMarginDeg),
    minLat: Math.max(-90, bounds.minLat - detailMarginDeg),
    maxLat: Math.min(90, bounds.maxLat + detailMarginDeg),
  };
  const candidateClusters = clustersInside(pyramid.regional, expandedBounds);
  const cellIndexes = candidateClusters.flatMap((cluster) => (
    cluster.cellIndexes.filter((index) => {
      const cell = pyramid.cells[index];
      const longitude = normalizeLongitude(cell.lon);
      return longitude >= expandedBounds.minLon
        && longitude <= expandedBounds.maxLon
        && cell.lat >= expandedBounds.minLat
        && cell.lat <= expandedBounds.maxLat;
    })
  ));

  if (cellIndexes.length > GLOBAL_MAP_MAX_DETAIL_SHAPES) {
    return {
      mode: "regional",
      clusters: candidateClusters,
      cellIndexes: [],
      representedCellCount: candidateClusters.reduce(
        (total, cluster) => total + cluster.cellIndexes.length,
        0,
      ),
      drawItemCount: candidateClusters.length,
    };
  }

  return {
    mode: "detail",
    clusters: [],
    cellIndexes,
    representedCellCount: cellIndexes.length,
    drawItemCount: cellIndexes.length,
  };
}
