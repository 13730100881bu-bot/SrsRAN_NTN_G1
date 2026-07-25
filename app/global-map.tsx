"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import { geoCircle, geoPath, type GeoPermissibleObjects } from "d3-geo";
import {
  createGlobalMapProjection,
  DEFAULT_GLOBAL_MAP_VIEW,
  GLOBAL_MAP_MAX_ZOOM,
  GLOBAL_MAP_MIN_ZOOM,
  zoomGlobalMapViewAt,
  type GlobalMapView,
} from "./global-map-view";
import { landFeatureFromTopology, type LandTopology } from "./land-topology";

export type GlobalMapCell = {
  id: string;
  lat: number;
  lon: number;
  childMask?: number | readonly boolean[] | string;
};

type SatelliteSubpoint = { id: string; lat: number; lon: number };

type GlobalCoverageMapProps = {
  cells: readonly GlobalMapCell[];
  visibleCells: readonly GlobalMapCell[];
  candidateCounts: readonly number[];
  selectedCellId: string;
  satellite: SatelliteSubpoint;
  entryAngularRadiusDeg: number;
  holdAngularRadiusDeg: number;
  landUrl: string;
  onSelectCell: (id: string) => void;
};

export function GlobalCoverageMap({
  cells,
  visibleCells,
  candidateCounts,
  selectedCellId,
  satellite,
  entryAngularRadiusDeg,
  holdAngularRadiusDeg,
  landUrl,
  onSelectCell,
}: GlobalCoverageMapProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const mapViewRef = useRef<GlobalMapView>(DEFAULT_GLOBAL_MAP_VIEW);
  const [land, setLand] = useState<GeoPermissibleObjects | null>(null);
  const [landError, setLandError] = useState("");
  const [mapView, setMapView] = useState<GlobalMapView>(DEFAULT_GLOBAL_MAP_VIEW);
  const visibleIds = useMemo(() => new Set(visibleCells.map(({ id }) => id)), [visibleCells]);

  useEffect(() => {
    let cancelled = false;
    fetch(landUrl)
      .then((response) => {
        if (!response.ok) throw new Error(`land request failed with ${response.status}`);
        return response.json() as Promise<LandTopology>;
      })
      .then((payload) => { if (!cancelled) setLand(landFeatureFromTopology(payload)); })
      .catch((error) => { if (!cancelled) setLandError(error instanceof Error ? error.message : String(error)); });
    return () => { cancelled = true; };
  }, [landUrl]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return undefined;

    const handleWheel = (event: WheelEvent) => {
      event.preventDefault();
      const bounds = canvas.getBoundingClientRect();
      const deltaPixels = event.deltaY * (event.deltaMode === WheelEvent.DOM_DELTA_LINE
        ? 16
        : event.deltaMode === WheelEvent.DOM_DELTA_PAGE ? bounds.height : 1);
      const current = mapViewRef.current;
      const next = zoomGlobalMapViewAt(
        current,
        current.zoom * Math.exp(-deltaPixels * 0.0015),
        [event.clientX - bounds.left, event.clientY - bounds.top],
        bounds.width,
        bounds.height,
      );
      if (next.zoom === current.zoom && next.centerLon === current.centerLon && next.centerLat === current.centerLat) return;

      mapViewRef.current = next;
      setMapView(next);
    };

    canvas.addEventListener("wheel", handleWheel, { passive: false });
    return () => canvas.removeEventListener("wheel", handleWheel);
  }, []);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return undefined;
    const parent = canvas.parentElement;
    if (!parent) return undefined;

    const draw = () => {
      const box = parent.getBoundingClientRect();
      const ratio = Math.min(window.devicePixelRatio || 1, 2);
      const width = Math.max(320, Math.floor(box.width));
      const height = Math.max(300, Math.floor(box.height));
      canvas.width = Math.floor(width * ratio);
      canvas.height = Math.floor(height * ratio);
      canvas.style.width = `${width}px`;
      canvas.style.height = `${height}px`;
      const context = canvas.getContext("2d");
      if (!context) return;
      context.setTransform(ratio, 0, 0, ratio, 0, 0);
      context.clearRect(0, 0, width, height);

      const projection = createGlobalMapProjection(width, height, mapView);
      const path = geoPath(projection, context);

      context.fillStyle = "#0c2730";
      context.fillRect(0, 0, width, height);
      context.beginPath();
      path({ type: "Sphere" });
      context.fillStyle = "#123741";
      context.fill();

      const north = projection([0, 57])?.[1] ?? 0;
      const south = projection([0, -57])?.[1] ?? height;
      context.fillStyle = "rgba(27, 139, 190, 0.09)";
      context.fillRect(0, north, width, south - north);

      if (land) {
        context.beginPath();
        path(land);
        context.fillStyle = "#bfd0c6";
        context.fill();
        context.strokeStyle = "rgba(231, 241, 236, 0.38)";
        context.lineWidth = 0.6;
        context.stroke();
      }

      context.fillStyle = "rgba(7, 29, 36, 0.66)";
      context.fillRect(0, 0, width, Math.max(0, north));
      context.fillRect(0, south, width, Math.max(0, height - south));
      context.strokeStyle = "rgba(112, 207, 238, 0.75)";
      context.lineWidth = 1;
      context.beginPath();
      context.moveTo(0, north);
      context.lineTo(width, north);
      context.moveTo(0, south);
      context.lineTo(width, south);
      context.stroke();

      context.strokeStyle = "rgba(198, 220, 215, 0.16)";
      context.lineWidth = 0.5;
      for (let longitude = -150; longitude <= 150; longitude += 30) {
        context.beginPath();
        path({ type: "LineString", coordinates: [[longitude, -57], [longitude, 57]] });
        context.stroke();
      }
      for (let latitude = -45; latitude <= 45; latitude += 15) {
        context.beginPath();
        path({ type: "LineString", coordinates: [[-180, latitude], [180, latitude]] });
        context.stroke();
      }

      // Render the frozen catalog first, then overwrite the selected satellite's
      // complete visible table.  The display never truncates at the 256 ceiling.
      for (let index = 0; index < cells.length; index += 1) {
        const cell = cells[index];
        const point = projection([cell.lon, cell.lat]);
        if (!point) continue;
        const candidateCount = candidateCounts[index] ?? 0;
        context.fillStyle = candidateCounts.length !== cells.length
          ? "rgba(12, 54, 64, 0.38)"
          : candidateCount === 0 ? "#e85f4f" : candidateCount === 1 ? "#e5a44f" : "rgba(12, 54, 64, 0.38)";
        context.fillRect(point[0] - 0.5, point[1] - 0.5, 1, 1);
      }
      context.fillStyle = "#4bc7ee";
      for (const cell of visibleCells) {
        const point = projection([cell.lon, cell.lat]);
        if (!point) continue;
        context.beginPath();
        context.arc(point[0], point[1], 1.6, 0, Math.PI * 2);
        context.fill();
      }

      const drawFootprint = (radius: number, stroke: string, dashed: boolean) => {
        context.beginPath();
        path(geoCircle().center([satellite.lon, satellite.lat]).radius(radius).precision(1)());
        context.strokeStyle = stroke;
        context.lineWidth = 1.5;
        context.setLineDash(dashed ? [5, 4] : []);
        context.stroke();
        context.setLineDash([]);
      };
      drawFootprint(holdAngularRadiusDeg, "#ef9b52", true);
      drawFootprint(entryAngularRadiusDeg, "#42c2ed", false);

      const satellitePoint = projection([satellite.lon, satellite.lat]);
      if (satellitePoint) {
        context.fillStyle = "#ffbb5b";
        context.beginPath();
        context.arc(satellitePoint[0], satellitePoint[1], 4, 0, Math.PI * 2);
        context.fill();
        context.fillStyle = "#f6fbf8";
        context.font = "11px ui-monospace, monospace";
        context.fillText(satellite.id, satellitePoint[0] + 7, satellitePoint[1] - 6);
      }

      const selected = cells.find(({ id }) => id === selectedCellId);
      if (selected) {
        const point = projection([selected.lon, selected.lat]);
        if (point) {
          context.strokeStyle = "#ffbb5b";
          context.lineWidth = 2;
          context.beginPath();
          context.arc(point[0], point[1], visibleIds.has(selected.id) ? 6 : 5, 0, Math.PI * 2);
          context.stroke();
        }
      }
    };

    const observer = new ResizeObserver(draw);
    observer.observe(parent);
    draw();
    return () => observer.disconnect();
  }, [candidateCounts, cells, entryAngularRadiusDeg, holdAngularRadiusDeg, land, mapView, satellite, selectedCellId, visibleCells, visibleIds]);

  const changeZoom = (requestedZoom: number) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const bounds = canvas.getBoundingClientRect();
    const next = zoomGlobalMapViewAt(
      mapViewRef.current,
      requestedZoom,
      [bounds.width / 2, bounds.height / 2],
      bounds.width,
      bounds.height,
    );
    mapViewRef.current = next;
    setMapView(next);
  };

  const resetMapView = () => {
    mapViewRef.current = DEFAULT_GLOBAL_MAP_VIEW;
    setMapView(DEFAULT_GLOBAL_MAP_VIEW);
  };

  const selectNearest = (event: React.PointerEvent<HTMLCanvasElement>) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const bounds = canvas.getBoundingClientRect();
    const projection = createGlobalMapProjection(bounds.width, bounds.height, mapViewRef.current);
    const coordinate = projection.invert?.([event.clientX - bounds.left, event.clientY - bounds.top]);
    if (!coordinate) return;
    let nearest: GlobalMapCell | undefined;
    let score = Number.POSITIVE_INFINITY;
    for (const cell of cells) {
      const dx = Math.abs(cell.lon - coordinate[0]) * Math.cos(cell.lat * Math.PI / 180);
      const dy = cell.lat - coordinate[1];
      const candidate = dx * dx + dy * dy;
      if (candidate < score) { score = candidate; nearest = cell; }
    }
    if (nearest && score < 1) onSelectCell(nearest.id);
  };

  return (
    <div className="global-map-canvas">
      <canvas
        ref={canvasRef}
        onPointerDown={selectNearest}
        aria-label={`全球陆地一级波位地图；${satellite.id} 当前几何可见 ${visibleCells.length} 个一级波位；支持滚轮缩放和点击选择`}
      />
      {landError ? <p className="map-error">陆地边界加载失败：{landError}</p> : null}
      <div className="map-zoom-panel">
        <span>滚轮缩放 · 点击选择一级波位</span>
        <div aria-label="地图缩放控制">
          <button type="button" aria-label="缩小地图" disabled={mapView.zoom <= GLOBAL_MAP_MIN_ZOOM} onClick={() => changeZoom(mapView.zoom / 1.5)}>−</button>
          <output aria-live="polite" aria-label={`当前地图缩放比例 ${Math.round(mapView.zoom * 100)}%`}>{Math.round(mapView.zoom * 100)}%</output>
          <button type="button" aria-label="放大地图" disabled={mapView.zoom >= GLOBAL_MAP_MAX_ZOOM} onClick={() => changeZoom(mapView.zoom * 1.5)}>+</button>
          <button type="button" className="map-reset-button" disabled={mapView.zoom <= GLOBAL_MAP_MIN_ZOOM} onClick={resetMapView}>恢复全图</button>
        </div>
      </div>
      <div className="map-legend" aria-label="地图图例">
        <span><i className="entry" />≥45° 可新接入</span>
        <span><i className="hold" />42°～45° 仅保持</span>
        <span><i className="visible" />所选卫星可见一级波位</span>
        <span><i className="gap" />当前时刻无≥45°候选</span>
      </div>
    </div>
  );
}
