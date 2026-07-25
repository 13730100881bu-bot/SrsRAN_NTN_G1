export type GeographicCoordinate = [longitude: number, latitude: number];

export type GlobalPositionHexagon = {
  type: "Polygon";
  coordinates: [GeographicCoordinate[]];
};

export const EARTH_MEAN_RADIUS_KM = 6371.0088;
export const DEFAULT_GLOBAL_POSITION_SPACING_KM = 60;

const HEXAGON_BEARINGS_DEG = [0, 60, 120, 180, 240, 300] as const;

const radians = (degrees: number) => degrees * Math.PI / 180;
const degrees = (value: number) => value * 180 / Math.PI;

export function normalizeLongitude(longitude: number) {
  return ((longitude + 540) % 360) - 180;
}

export function wrappedLongitudeDifference(left: number, right: number) {
  return normalizeLongitude(left - right);
}

export function globalPositionHexagonRadiusKm(spacingKm: number) {
  if (!Number.isFinite(spacingKm) || spacingKm <= 0) {
    throw new RangeError("position spacing must be a positive finite number");
  }
  return spacingKm / Math.sqrt(3);
}

function destinationPoint(
  latitudeDeg: number,
  longitudeDeg: number,
  bearingDeg: number,
  distanceKm: number,
): GeographicCoordinate {
  const angularDistance = distanceKm / EARTH_MEAN_RADIUS_KM;
  const latitude = radians(latitudeDeg);
  const longitude = radians(longitudeDeg);
  const bearing = radians(bearingDeg);
  const destinationLatitude = Math.asin(
    Math.sin(latitude) * Math.cos(angularDistance)
      + Math.cos(latitude) * Math.sin(angularDistance) * Math.cos(bearing),
  );
  const destinationLongitude = longitude + Math.atan2(
    Math.sin(bearing) * Math.sin(angularDistance) * Math.cos(latitude),
    Math.cos(angularDistance) - Math.sin(latitude) * Math.sin(destinationLatitude),
  );
  return [
    normalizeLongitude(degrees(destinationLongitude)),
    degrees(destinationLatitude),
  ];
}

/**
 * Builds the point-up spherical hexagon implied by the catalog's staggered
 * 60 km center grid. The catalog is land-center based, so coastal polygons are
 * intentionally not clipped to the shoreline.
 */
export function createGlobalPositionHexagon(
  latitudeDeg: number,
  longitudeDeg: number,
  spacingKm = DEFAULT_GLOBAL_POSITION_SPACING_KM,
): GlobalPositionHexagon {
  if (!Number.isFinite(latitudeDeg) || latitudeDeg < -90 || latitudeDeg > 90) {
    throw new RangeError("latitude must be between -90 and 90 degrees");
  }
  if (!Number.isFinite(longitudeDeg)) {
    throw new RangeError("longitude must be finite");
  }

  const radiusKm = globalPositionHexagonRadiusKm(spacingKm);
  const vertices = HEXAGON_BEARINGS_DEG.map((bearing) => (
    destinationPoint(latitudeDeg, longitudeDeg, bearing, radiusKm)
  ));
  return {
    type: "Polygon",
    coordinates: [[...vertices, [...vertices[0]] as GeographicCoordinate]],
  };
}
