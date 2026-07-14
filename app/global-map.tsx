"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import { geoCircle, geoEquirectangular, geoPath, type GeoPermissibleObjects } from "d3-geo";
import { feature } from "topojson-client";

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

type TopologyLike = {
  type: "Topology";
  objects: Record<string, unknown>;
};

function topologyFeature(payload: TopologyLike) {
  const object = payload.objects.land ?? Object.values(payload.objects)[0];
  if (!object) throw new Error("land TopoJSON does not contain an object");
  return feature(payload as never, object as never) as unknown as GeoPermissibleObjects;
}

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
  const [land, setLand] = useState<GeoPermissibleObjects | null>(null);
  const [landError, setLandError] = useState("");
  const visibleIds = useMemo(() => new Set(visibleCells.map(({ id }) => id)), [visibleCells]);

  useEffect(() => {
    let cancelled = false;
    fetch(landUrl)
      .then((response) => {
        if (!response.ok) throw new Error(`land request failed with ${response.status}`);
        return response.json() as Promise<TopologyLike>;
      })
      .then((payload) => { if (!cancelled) setLand(topologyFeature(payload)); })
      .catch((error) => { if (!cancelled) setLandError(error instanceof Error ? error.message : String(error)); });
    return () => { cancelled = true; };
  }, [landUrl]);

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

      const projection = geoEquirectangular()
        .translate([width / 2, height / 2])
        .scale(Math.min(width / (2 * Math.PI), height / Math.PI) * 0.97)
        .precision(0.2);
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
  }, [candidateCounts, cells, entryAngularRadiusDeg, holdAngularRadiusDeg, land, satellite, selectedCellId, visibleCells, visibleIds]);

  const selectNearest = (event: React.PointerEvent<HTMLCanvasElement>) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const bounds = canvas.getBoundingClientRect();
    const projection = geoEquirectangular()
      .translate([bounds.width / 2, bounds.height / 2])
      .scale(Math.min(bounds.width / (2 * Math.PI), bounds.height / Math.PI) * 0.97);
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
        aria-label={`全球陆地波位地图；${satellite.id} 当前几何可见 ${visibleCells.length} 个 L1`}
      />
      {landError ? <p className="map-error">陆地边界加载失败：{landError}</p> : null}
      <div className="map-legend" aria-label="地图图例">
        <span><i className="entry" />≥45° 可新接入</span>
        <span><i className="hold" />42°～45° 仅保持</span>
        <span><i className="visible" />所选卫星可见L1</span>
        <span><i className="gap" />当前时刻无≥45°候选</span>
      </div>
    </div>
  );
}
