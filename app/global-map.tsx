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
import {
  createGlobalPositionHexagon,
  DEFAULT_GLOBAL_POSITION_SPACING_KM,
  EARTH_MEAN_RADIUS_KM,
  globalPositionHexagonRadiusKm,
  wrappedLongitudeDifference,
} from "./global-position-hexagon";
import { landFeatureFromTopology, type LandTopology } from "./land-topology";

export type GlobalMapCell = {
  id: string;
  lat: number;
  lon: number;
  childMask?: number | readonly boolean[] | string;
};

type SatelliteSubpoint = { id: string; lat: number; lon: number };

type BaseMapLayerCache = {
  canvas: HTMLCanvasElement;
  width: number;
  height: number;
  ratio: number;
  mapView: GlobalMapView;
  land: GeoPermissibleObjects | null;
  geometry: object;
};

type GlobalCoverageMapProps = {
  cells: readonly GlobalMapCell[];
  visibleCells: readonly GlobalMapCell[];
  candidateCounts: readonly number[];
  selectedCellId: string;
  satellite: SatelliteSubpoint;
  entryAngularRadiusDeg: number;
  holdAngularRadiusDeg: number;
  positionSpacingKm: number;
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
  positionSpacingKm,
  landUrl,
  onSelectCell,
}: GlobalCoverageMapProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const baseLayerRef = useRef<BaseMapLayerCache | null>(null);
  const mapViewRef = useRef<GlobalMapView>(DEFAULT_GLOBAL_MAP_VIEW);
  const [land, setLand] = useState<GeoPermissibleObjects | null>(null);
  const [landError, setLandError] = useState("");
  const [mapView, setMapView] = useState<GlobalMapView>(DEFAULT_GLOBAL_MAP_VIEW);
  const mapGeometry = useMemo(() => {
    const spacingKm = Number.isFinite(positionSpacingKm) && positionSpacingKm > 0
      ? positionSpacingKm
      : DEFAULT_GLOBAL_POSITION_SPACING_KM;
    const cellIndexById = new Map<string, number>();
    const hexagons = cells.map((cell, index) => {
      cellIndexById.set(cell.id, index);
      return createGlobalPositionHexagon(cell.lat, cell.lon, spacingKm);
    });
    return {
      cellIndexById,
      hexagons,
      allIndexes: cells.map((_, index) => index),
      selectionRadiusDeg: globalPositionHexagonRadiusKm(spacingKm) / EARTH_MEAN_RADIUS_KM * 180 / Math.PI,
    };
  }, [cells, positionSpacingKm]);
  const visibleIndexes = useMemo(() => (
    visibleCells.flatMap(({ id }) => {
      const index = mapGeometry.cellIndexById.get(id);
      return index === undefined ? [] : [index];
    })
  ), [mapGeometry.cellIndexById, visibleCells]);
  const candidateIndexes = useMemo(() => {
    if (candidateCounts.length !== cells.length) return { gaps: [] as number[], single: [] as number[] };
    const gaps: number[] = [];
    const single: number[] = [];
    candidateCounts.forEach((count, index) => {
      if (count === 0) gaps.push(index);
      else if (count === 1) single.push(index);
    });
    return { gaps, single };
  }, [candidateCounts, cells.length]);

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
      const responsiveMinimumHeight = Number.parseFloat(window.getComputedStyle(parent).minHeight);
      const height = Math.max(300, Math.floor(
        Number.isFinite(responsiveMinimumHeight) ? responsiveMinimumHeight : box.height,
      ));
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
      const cached = baseLayerRef.current;
      const cacheMatches = cached
        && cached.width === width
        && cached.height === height
        && cached.ratio === ratio
        && cached.mapView.zoom === mapView.zoom
        && cached.mapView.centerLon === mapView.centerLon
        && cached.mapView.centerLat === mapView.centerLat
        && cached.land === land
        && cached.geometry === mapGeometry;

      if (!cacheMatches) {
        const baseCanvas = document.createElement("canvas");
        baseCanvas.width = Math.floor(width * ratio);
        baseCanvas.height = Math.floor(height * ratio);
        const baseContext = baseCanvas.getContext("2d");
        if (!baseContext) return;
        baseContext.setTransform(ratio, 0, 0, ratio, 0, 0);
        const basePath = geoPath(projection, baseContext);

        baseContext.fillStyle = "#0c2730";
        baseContext.fillRect(0, 0, width, height);
        baseContext.beginPath();
        basePath({ type: "Sphere" });
        baseContext.fillStyle = "#123741";
        baseContext.fill();

        const north = projection([0, 57])?.[1] ?? 0;
        const south = projection([0, -57])?.[1] ?? height;
        baseContext.fillStyle = "rgba(27, 139, 190, 0.09)";
        baseContext.fillRect(0, north, width, south - north);

        if (land) {
          baseContext.beginPath();
          basePath(land);
          baseContext.fillStyle = "#bfd0c6";
          baseContext.fill();
          baseContext.strokeStyle = "rgba(231, 241, 236, 0.38)";
          baseContext.lineWidth = 0.6;
          baseContext.stroke();
        }

        baseContext.fillStyle = "rgba(7, 29, 36, 0.66)";
        baseContext.fillRect(0, 0, width, Math.max(0, north));
        baseContext.fillRect(0, south, width, Math.max(0, height - south));
        baseContext.strokeStyle = "rgba(112, 207, 238, 0.75)";
        baseContext.lineWidth = 1;
        baseContext.beginPath();
        baseContext.moveTo(0, north);
        baseContext.lineTo(width, north);
        baseContext.moveTo(0, south);
        baseContext.lineTo(width, south);
        baseContext.stroke();

        baseContext.strokeStyle = "rgba(198, 220, 215, 0.16)";
        baseContext.lineWidth = 0.5;
        for (let longitude = -150; longitude <= 150; longitude += 30) {
          baseContext.beginPath();
          basePath({ type: "LineString", coordinates: [[longitude, -57], [longitude, 57]] });
          baseContext.stroke();
        }
        for (let latitude = -45; latitude <= 45; latitude += 15) {
          baseContext.beginPath();
          basePath({ type: "LineString", coordinates: [[-180, latitude], [180, latitude]] });
          baseContext.stroke();
        }

        // The frozen catalog is rendered once for each map view and retained as
        // a base layer. Per-second visibility changes only repaint the overlays.
        const boundaryWidth = mapView.zoom >= 4 ? 0.8 : mapView.zoom >= 2 ? 0.55 : 0.35;
        baseContext.beginPath();
        for (const index of mapGeometry.allIndexes) basePath(mapGeometry.hexagons[index]);
        baseContext.fillStyle = "rgba(8, 44, 53, 0.22)";
        baseContext.fill();
        baseContext.strokeStyle = mapView.zoom >= 2
          ? "rgba(235, 244, 240, 0.55)"
          : "rgba(235, 244, 240, 0.34)";
        baseContext.lineWidth = boundaryWidth;
        baseContext.stroke();

        baseLayerRef.current = {
          canvas: baseCanvas,
          width,
          height,
          ratio,
          mapView: { ...mapView },
          land,
          geometry: mapGeometry,
        };
      }

      context.drawImage(baseLayerRef.current!.canvas, 0, 0, width, height);

      const drawHexagonLayer = (
        indexes: readonly number[],
        fillStyle: string,
        strokeStyle: string,
        lineWidth: number,
      ) => {
        if (indexes.length === 0) return;
        context.beginPath();
        for (const index of indexes) path(mapGeometry.hexagons[index]);
        context.fillStyle = fillStyle;
        context.fill();
        context.strokeStyle = strokeStyle;
        context.lineWidth = lineWidth;
        context.stroke();
      };

      const boundaryWidth = mapView.zoom >= 4 ? 0.8 : mapView.zoom >= 2 ? 0.55 : 0.35;
      drawHexagonLayer(candidateIndexes.single, "rgba(229, 164, 79, 0.58)", "#f2be77", boundaryWidth + 0.15);
      drawHexagonLayer(candidateIndexes.gaps, "rgba(232, 95, 79, 0.7)", "#ff8d7f", boundaryWidth + 0.2);
      drawHexagonLayer(visibleIndexes, "rgba(75, 199, 238, 0.52)", "#84e0f8", boundaryWidth + 0.35);

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

      const selectedIndex = mapGeometry.cellIndexById.get(selectedCellId);
      if (selectedIndex !== undefined) {
        drawHexagonLayer([selectedIndex], "rgba(255, 187, 91, 0.34)", "#ffbb5b", 2.2);
      }
    };

    const observer = new ResizeObserver(draw);
    observer.observe(parent);
    draw();
    return () => observer.disconnect();
  }, [candidateIndexes, entryAngularRadiusDeg, holdAngularRadiusDeg, land, mapGeometry, mapView, satellite, selectedCellId, visibleIndexes]);

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
      const dx = wrappedLongitudeDifference(cell.lon, coordinate[0]) * Math.cos(cell.lat * Math.PI / 180);
      const dy = cell.lat - coordinate[1];
      const candidate = dx * dx + dy * dy;
      if (candidate < score) { score = candidate; nearest = cell; }
    }
    if (nearest && score <= mapGeometry.selectionRadiusDeg ** 2) onSelectCell(nearest.id);
  };

  return (
    <div className="global-map-canvas">
      <canvas
        ref={canvasRef}
        onPointerDown={selectNearest}
        aria-label={`全球陆地一级波位六边形地图；${satellite.id} 当前几何可见 ${visibleCells.length} 个一级波位；支持滚轮缩放和点击选择`}
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
        <span><i className="position" />一级波位六边形边界</span>
        <span><i className="entry" />≥45° 可新接入</span>
        <span><i className="hold" />42°～45° 仅保持</span>
        <span><i className="visible" />所选卫星可见一级波位</span>
        <span><i className="gap" />当前时刻无≥45°候选</span>
        <small>每个六边形代表一个一级波位；沿海按中心是否位于陆地保留。</small>
      </div>
    </div>
  );
}
