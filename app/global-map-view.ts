import { geoEquirectangular } from "d3-geo";
import type { GlobalMapBounds } from "./global-map-pyramid";

export type GlobalMapView = Readonly<{
  zoom: number;
  centerLon: number;
  centerLat: number;
}>;

export const GLOBAL_MAP_MIN_ZOOM = 1;
export const GLOBAL_MAP_MAX_ZOOM = 8;
export const DEFAULT_GLOBAL_MAP_VIEW: GlobalMapView = {
  zoom: GLOBAL_MAP_MIN_ZOOM,
  centerLon: 0,
  centerLat: 0,
};

const BASE_MAP_FILL = 0.97;

function clamp(value: number, minimum: number, maximum: number) {
  return Math.min(maximum, Math.max(minimum, value));
}

export function globalMapScale(width: number, height: number, zoom: number) {
  return Math.min(width / (2 * Math.PI), height / Math.PI) * BASE_MAP_FILL * zoom;
}

export function constrainGlobalMapView(view: GlobalMapView, width: number, height: number): GlobalMapView {
  const zoom = clamp(view.zoom, GLOBAL_MAP_MIN_ZOOM, GLOBAL_MAP_MAX_ZOOM);
  const scale = globalMapScale(width, height, zoom);
  const longitudeLimit = Math.max(0, 180 - (width / (2 * scale)) * 180 / Math.PI);
  const latitudeLimit = Math.max(0, 90 - (height / (2 * scale)) * 180 / Math.PI);

  return {
    zoom,
    centerLon: clamp(view.centerLon, -longitudeLimit, longitudeLimit),
    centerLat: clamp(view.centerLat, -latitudeLimit, latitudeLimit),
  };
}

export function createGlobalMapProjection(width: number, height: number, view: GlobalMapView) {
  const constrained = constrainGlobalMapView(view, width, height);
  return geoEquirectangular()
    .translate([width / 2, height / 2])
    .scale(globalMapScale(width, height, constrained.zoom))
    .center([constrained.centerLon, constrained.centerLat])
    .precision(0.2);
}

export function globalMapViewBounds(
  width: number,
  height: number,
  view: GlobalMapView,
): GlobalMapBounds {
  const constrained = constrainGlobalMapView(view, width, height);
  const scale = globalMapScale(width, height, constrained.zoom);
  const halfLongitude = width * 90 / (Math.PI * scale);
  const halfLatitude = height * 90 / (Math.PI * scale);
  return {
    minLon: Math.max(-180, constrained.centerLon - halfLongitude),
    maxLon: Math.min(180, constrained.centerLon + halfLongitude),
    minLat: Math.max(-90, constrained.centerLat - halfLatitude),
    maxLat: Math.min(90, constrained.centerLat + halfLatitude),
  };
}

export function zoomGlobalMapViewAt(
  view: GlobalMapView,
  requestedZoom: number,
  pointer: readonly [number, number],
  width: number,
  height: number,
): GlobalMapView {
  const current = constrainGlobalMapView(view, width, height);
  const zoom = clamp(requestedZoom, GLOBAL_MAP_MIN_ZOOM, GLOBAL_MAP_MAX_ZOOM);
  if (Math.abs(zoom - current.zoom) < Number.EPSILON) return current;

  const anchor = createGlobalMapProjection(width, height, current).invert?.(pointer);
  if (!anchor) return current;

  const scale = globalMapScale(width, height, zoom);
  return constrainGlobalMapView({
    zoom,
    centerLon: anchor[0] - (pointer[0] - width / 2) * 180 / (Math.PI * scale),
    centerLat: anchor[1] + (pointer[1] - height / 2) * 180 / (Math.PI * scale),
  }, width, height);
}
