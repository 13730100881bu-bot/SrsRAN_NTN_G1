import type { BeamAnimationCell } from "./beam-hopping-animation-model";

export type BeamAnimationMapViewport = Readonly<{
  centerLat: number;
  centerLon: number;
  radiusDeg: number;
}>;

const MINIMUM_VIEW_RADIUS_DEG = 4.5;
const VIEW_MARGIN_DEG = 1.4;

const radians = (degrees: number) => degrees * Math.PI / 180;
const degrees = (value: number) => value * 180 / Math.PI;

function normalizeLongitude(longitude: number) {
  return ((longitude + 540) % 360) - 180;
}

function angularDistanceDeg(
  left: Pick<BeamAnimationCell, "lat" | "lon">,
  right: Pick<BeamAnimationCell, "lat" | "lon">,
) {
  const leftLat = radians(left.lat);
  const rightLat = radians(right.lat);
  const longitudeDifference = radians(normalizeLongitude(left.lon - right.lon));
  return degrees(Math.acos(Math.min(1, Math.max(-1,
    Math.sin(leftLat) * Math.sin(rightLat)
      + Math.cos(leftLat) * Math.cos(rightLat) * Math.cos(longitudeDifference),
  ))));
}

function sphericalCenter(cells: readonly BeamAnimationCell[]) {
  let x = 0;
  let y = 0;
  let z = 0;
  const orderedCells = [...cells].sort((left, right) =>
    left.id.localeCompare(right.id)
      || left.lat - right.lat
      || left.lon - right.lon,
  );
  for (const cell of orderedCells) {
    const latitude = radians(cell.lat);
    const longitude = radians(cell.lon);
    x += Math.cos(latitude) * Math.cos(longitude);
    y += Math.cos(latitude) * Math.sin(longitude);
    z += Math.sin(latitude);
  }

  const horizontal = Math.hypot(x, y);
  if (horizontal < Number.EPSILON && Math.abs(z) < Number.EPSILON) {
    return { lat: cells[0]?.lat ?? 0, lon: cells[0]?.lon ?? 0 };
  }
  return {
    lat: degrees(Math.atan2(z, horizontal)),
    lon: normalizeLongitude(degrees(Math.atan2(y, x))),
  };
}

/**
 * Frames the complete set of ground positions assigned to the selected
 * satellite. The satellite subpoint is only a fallback for an empty
 * assignment; it is deliberately not the map center once positions exist.
 */
export function createBeamAnimationMapViewport(
  cells: readonly BeamAnimationCell[],
  satellite: Pick<BeamAnimationCell, "lat" | "lon">,
): BeamAnimationMapViewport {
  if (cells.length === 0) {
    return {
      centerLat: satellite.lat,
      centerLon: normalizeLongitude(satellite.lon),
      radiusDeg: MINIMUM_VIEW_RADIUS_DEG,
    };
  }

  const center = sphericalCenter(cells);
  const maximumDistance = cells.reduce(
    (maximum, cell) => Math.max(maximum, angularDistanceDeg(center, cell)),
    0,
  );
  return {
    centerLat: center.lat,
    centerLon: center.lon,
    radiusDeg: Math.max(MINIMUM_VIEW_RADIUS_DEG, maximumDistance + VIEW_MARGIN_DEG),
  };
}

export function isInsideBeamAnimationViewport(
  viewport: BeamAnimationMapViewport,
  cell: Pick<BeamAnimationCell, "lat" | "lon">,
) {
  return angularDistanceDeg(
    { lat: viewport.centerLat, lon: viewport.centerLon },
    cell,
  ) <= viewport.radiusDeg + 1e-9;
}
