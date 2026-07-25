"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import {
  geoAzimuthalEqualArea,
  geoCircle,
  geoDistance,
  geoGraticule10,
  geoPath,
  type GeoPermissibleObjects,
} from "d3-geo";
import {
  createBeamAnimationFrames,
  type BeamAnimationCell,
} from "./beam-hopping-animation-model";
import { landFeatureFromTopology, type LandTopology } from "./land-topology";

type BeamAnimationSatellite = {
  readonly id: string;
  readonly lat: number;
  readonly lon: number;
};

type BeamHoppingAnimationProps = {
  readonly satellite: BeamAnimationSatellite;
  readonly cells: readonly BeamAnimationCell[];
  readonly nciByBank: readonly [string, string];
  readonly landUrl: string;
};

const PLAYBACK_SPEEDS = [1, 2, 4] as const;
const BANK_COLORS = ["#58d2ee", "#ffad55"] as const;

function formatAnimationTime(value: number) {
  return Number.isInteger(value) ? `${value}` : value.toFixed(1);
}

function drawSatellite(context: CanvasRenderingContext2D, x: number, y: number, label: string) {
  context.save();
  context.shadowColor = "rgba(255, 190, 88, 0.5)";
  context.shadowBlur = 12;
  context.fillStyle = "#f4b85e";
  context.fillRect(x - 16, y - 8, 32, 16);
  context.fillStyle = "#6ed3ed";
  context.fillRect(x - 57, y - 6, 35, 12);
  context.fillRect(x + 22, y - 6, 35, 12);
  context.strokeStyle = "#dcebed";
  context.lineWidth = 1;
  context.strokeRect(x - 16, y - 8, 32, 16);
  context.beginPath();
  context.moveTo(x, y + 8);
  context.lineTo(x, y + 18);
  context.stroke();
  context.restore();

  context.fillStyle = "#eef8f6";
  context.font = "600 11px ui-monospace, monospace";
  context.textAlign = "center";
  context.fillText(label, x, y - 16);
}

export function BeamHoppingAnimation({
  satellite,
  cells,
  nciByBank,
  landUrl,
}: BeamHoppingAnimationProps) {
  const [nciA, nciB] = nciByBank;
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [land, setLand] = useState<GeoPermissibleObjects | null>(null);
  const [landError, setLandError] = useState("");
  const [frameIndex, setFrameIndex] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [playbackSpeed, setPlaybackSpeed] = useState<(typeof PLAYBACK_SPEEDS)[number]>(1);
  const frames = useMemo(
    () => createBeamAnimationFrames(cells, satellite.id, [nciA, nciB]),
    [cells, nciA, nciB, satellite.id],
  );
  const cellById = useMemo(() => new Map(cells.map((cell) => [cell.id, cell])), [cells]);
  const frame = frames[frameIndex] ?? frames[0];
  const activeCells = useMemo(
    () => frame?.activePositionIds.map((id) => cellById.get(id)).filter((cell): cell is BeamAnimationCell => Boolean(cell)) ?? [],
    [cellById, frame],
  );

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
    if (!playing || cells.length === 0 || frames.length === 0) return undefined;
    const timer = window.setInterval(
      () => setFrameIndex((current) => (current + 1) % frames.length),
      520 / playbackSpeed,
    );
    return () => window.clearInterval(timer);
  }, [cells.length, frames.length, playbackSpeed, playing]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !frame) return undefined;
    const parent = canvas.parentElement;
    if (!parent) return undefined;

    const draw = () => {
      const box = parent.getBoundingClientRect();
      const ratio = Math.min(window.devicePixelRatio || 1, 2);
      const width = Math.max(320, Math.floor(box.width));
      const height = Math.max(360, Math.floor(box.height));
      canvas.width = Math.floor(width * ratio);
      canvas.height = Math.floor(height * ratio);
      const context = canvas.getContext("2d");
      if (!context) return;
      context.setTransform(ratio, 0, 0, ratio, 0, 0);
      context.clearRect(0, 0, width, height);

      const background = context.createLinearGradient(0, 0, 0, height);
      background.addColorStop(0, "#071c25");
      background.addColorStop(1, "#123a43");
      context.fillStyle = background;
      context.fillRect(0, 0, width, height);

      const maximumDistanceDeg = cells.reduce((maximum, cell) => Math.max(
        maximum,
        geoDistance([satellite.lon, satellite.lat], [cell.lon, cell.lat]) * 180 / Math.PI,
      ), 0);
      // Keep enough surrounding geography visible for the ground positions to be
      // recognizable instead of zooming into an anonymous patch of land.
      const viewRadiusDeg = Math.min(32, Math.max(18, maximumDistanceDeg * 1.25 + 2));
      const mapCenterY = height * 0.62;
      const mapRadius = Math.min(width * 0.43, height * 0.37);
      const scale = mapRadius / Math.max(0.01, 2 * Math.sin(viewRadiusDeg * Math.PI / 360));
      const projection = geoAzimuthalEqualArea()
        .rotate([-satellite.lon, -satellite.lat])
        .translate([width / 2, mapCenterY])
        .scale(scale)
        .clipAngle(viewRadiusDeg);
      const path = geoPath(projection, context);
      const footprint = geoCircle()
        .center([satellite.lon, satellite.lat])
        .radius(viewRadiusDeg)
        .precision(1)();

      context.beginPath();
      path(footprint);
      context.fillStyle = "#0d303a";
      context.fill();
      if (land) {
        context.beginPath();
        path(land);
        context.fillStyle = "#9fb8ab";
        context.fill();
        context.strokeStyle = "rgba(231, 241, 236, 0.35)";
        context.lineWidth = 0.6;
        context.stroke();
        context.beginPath();
        path(geoGraticule10());
        context.strokeStyle = "rgba(207, 226, 222, 0.14)";
        context.lineWidth = 0.5;
        context.stroke();
      }
      context.beginPath();
      path(footprint);
      context.strokeStyle = "rgba(112, 207, 238, 0.52)";
      context.lineWidth = 1;
      context.stroke();

      for (const cell of cells) {
        const point = projection([cell.lon, cell.lat]);
        if (!point) continue;
        context.beginPath();
        context.arc(point[0], point[1], 2.2, 0, Math.PI * 2);
        context.fillStyle = cell.cellBank === 0 ? "rgba(88, 210, 238, 0.48)" : "rgba(255, 173, 85, 0.48)";
        context.fill();
      }

      const satelliteX = width / 2;
      const satelliteY = 52;
      context.save();
      context.setLineDash([5, 5]);
      context.strokeStyle = "rgba(213, 230, 231, 0.42)";
      context.lineWidth = 1;
      context.beginPath();
      context.moveTo(satelliteX - Math.min(150, width * 0.24), satelliteY + 3);
      context.quadraticCurveTo(satelliteX, satelliteY - 22, satelliteX + Math.min(150, width * 0.24), satelliteY + 3);
      context.stroke();
      context.setLineDash([]);
      context.fillStyle = "rgba(213, 230, 231, 0.72)";
      context.beginPath();
      context.moveTo(satelliteX + Math.min(150, width * 0.24), satelliteY + 3);
      context.lineTo(satelliteX + Math.min(150, width * 0.24) - 9, satelliteY - 2);
      context.lineTo(satelliteX + Math.min(150, width * 0.24) - 7, satelliteY + 7);
      context.closePath();
      context.fill();
      context.font = "500 9px system-ui, sans-serif";
      context.textAlign = "center";
      context.fillText("沿轨道持续前进", satelliteX, satelliteY - 27);
      context.restore();

      for (const [index, cell] of activeCells.entries()) {
        const point = projection([cell.lon, cell.lat]);
        if (!point) continue;
        const color = BANK_COLORS[cell.cellBank];
        const beam = context.createLinearGradient(satelliteX, satelliteY, point[0], point[1]);
        beam.addColorStop(0, "rgba(255, 229, 170, 0.18)");
        beam.addColorStop(0.35, `${color}55`);
        beam.addColorStop(1, color);
        context.save();
        context.strokeStyle = beam;
        context.lineWidth = 1.35;
        context.shadowColor = color;
        context.shadowBlur = 7;
        context.beginPath();
        context.moveTo(satelliteX, satelliteY + 17);
        context.lineTo(point[0], point[1]);
        context.stroke();
        context.restore();

        context.save();
        context.strokeStyle = color;
        context.fillStyle = color;
        context.shadowColor = color;
        context.shadowBlur = 13;
        context.beginPath();
        context.arc(point[0], point[1], 6.5, 0, Math.PI * 2);
        context.fill();
        context.beginPath();
        context.arc(point[0], point[1], 10.5, 0, Math.PI * 2);
        context.lineWidth = 1.2;
        context.stroke();
        context.restore();

        if (index < 3) {
          context.fillStyle = "#f4fbf9";
          context.font = "600 9px ui-monospace, monospace";
          context.textAlign = "left";
          context.fillText(cell.id, point[0] + 9, point[1] - 8);
        }
      }

      drawSatellite(context, satelliteX, satelliteY, satellite.id);
    };

    const observer = new ResizeObserver(draw);
    observer.observe(parent);
    draw();
    return () => observer.disconnect();
  }, [activeCells, cells, frame, land, satellite]);

  const activeBankLabel = frame?.cellBank === null || frame?.cellBank === undefined
    ? "等待安排"
    : `星载小区 ${frame.cellBank === 0 ? "A" : "B"}`;
  const timeLabel = frame
    ? `${formatAnimationTime(frame.startMs)}–${formatAnimationTime(frame.endMs)} ms`
    : "等待日历";
  const activeSlotIndex = frame ? Math.floor(frame.startMs / 10) : 0;
  const activePositionSummary = activeCells.slice(0, 4).map(({ id }) => id).join("、");

  return (
    <figure className="beam-animation-panel" aria-labelledby="beam-animation-title">
      <header>
        <div>
          <span>动态演示</span>
          <h3 id="beam-animation-title">卫星如何轮流照向不同的一级波位</h3>
          <p>卫星沿轨道连续运动，跳变的是波束指向。这里播放 80 ms 下行发现轮转；亮起的连线表示当前 2.5 ms 时间段安排照向的一级波位，动画已放慢，便于观察。</p>
        </div>
        <div className="beam-animation-summary">
          <span>{timeLabel}</span>
          <b>{activeBankLabel}</b>
          <small>{frame?.beamCount ?? 0}路下行发现波束 · {activeCells.length}个一级波位</small>
          {activePositionSummary ? <em title={activeCells.map(({ id }) => id).join("、")}>{activePositionSummary}{activeCells.length > 4 ? "…" : ""}</em> : null}
        </div>
      </header>

      <div className="beam-animation-map">
        <canvas
          ref={canvasRef}
          role="img"
          aria-label={`${satellite.id}跳波束动态示意图；${timeLabel}；${activeBankLabel}照向${activeCells.length}个一级波位${activePositionSummary ? `：${activePositionSummary}` : ""}`}
        />
        {cells.length === 0 ? <p className="beam-animation-empty">正在等待本星的一级波位分配结果</p> : null}
        {landError ? <p className="map-error">陆地边界加载失败：{landError}</p> : null}
        <div className="beam-animation-legend" aria-label="跳波束图例">
          <span><i className="cell-a" />小区 A 的一级波位</span>
          <span><i className="cell-b" />小区 B 的一级波位</span>
          <span><i className="active-beam" />当前下行发现波束</span>
        </div>
      </div>

      <div className="beam-animation-controls">
        <button
          type="button"
          className="beam-animation-play"
          aria-pressed={playing}
          disabled={cells.length === 0}
          onClick={() => setPlaying((current) => !current)}
        >
          {playing ? "暂停跳波束动画" : "播放跳波束动画"}
        </button>
        <label>
          <span>80 ms轮转位置</span>
          <input
            type="range"
            min="0"
            max={Math.max(0, frames.length - 1)}
            step="1"
            value={frameIndex}
            disabled={cells.length === 0}
            aria-label="跳波束动画时间位置"
            onChange={(event) => {
              setPlaying(false);
              setFrameIndex(Number(event.target.value));
            }}
          />
          <output>{timeLabel}</output>
        </label>
        <div className="beam-animation-speed" aria-label="跳波束动画速度">
          {PLAYBACK_SPEEDS.map((value) => (
            <button
              type="button"
              key={value}
              className={playbackSpeed === value ? "active" : ""}
              aria-pressed={playbackSpeed === value}
              onClick={() => setPlaybackSpeed(value)}
            >
              {value}×
            </button>
          ))}
        </div>
      </div>

      <div className="beam-slot-track" aria-label={`当前位于第${activeSlotIndex + 1}个10 ms时间段`}>
        {Array.from({ length: 8 }, (_, index) => (
          <span key={index} className={index === activeSlotIndex ? `active bank-${frame?.cellBank ?? 0}` : ""}>
            {index * 10}–{index * 10 + 10}
          </span>
        ))}
      </div>
      <figcaption className="beam-animation-boundary">
        波位位置来自全球陆地波位目录，连线表示当前软件日历计划照向的区域。本图演示 80 ms 下行发现轮转，上行接入按下方 640 ms 指标另行安排。动画经过放慢处理，不代表天线指向、信号功率、波束成形参数或真实无线发射已经执行。
      </figcaption>
    </figure>
  );
}
