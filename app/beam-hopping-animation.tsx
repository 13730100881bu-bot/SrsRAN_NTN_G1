"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import {
  geoCircle,
  geoGraticule10,
  geoMercator,
  geoPath,
  type GeoPermissibleObjects,
} from "d3-geo";
import {
  createBeamAnimationFrames,
  type BeamAnimationCell,
} from "./beam-hopping-animation-model";
import { createBeamAnimationMapViewport } from "./beam-hopping-map-layout";
import {
  createGlobalPositionHexagon,
  DEFAULT_GLOBAL_POSITION_SPACING_KM,
} from "./global-position-hexagon";
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
  readonly positionSpacingKm?: number;
};

const PLAYBACK_SPEEDS = [1, 2, 4] as const;
const BANK_COLORS = ["#58d2ee", "#ffad55"] as const;

function formatAnimationTime(value: number) {
  return Number.isInteger(value) ? `${value}` : value.toFixed(1);
}

export function BeamHoppingAnimation({
  satellite,
  cells,
  nciByBank,
  landUrl,
  positionSpacingKm = DEFAULT_GLOBAL_POSITION_SPACING_KM,
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
    () => frame?.activePositionIds.map((id) => cellById.get(id)).filter(
      (cell): cell is BeamAnimationCell => Boolean(cell),
    ) ?? [],
    [cellById, frame],
  );
  const mapGeometry = useMemo(() => {
    const spacingKm = Number.isFinite(positionSpacingKm) && positionSpacingKm > 0
      ? positionSpacingKm
      : DEFAULT_GLOBAL_POSITION_SPACING_KM;
    return {
      viewport: createBeamAnimationMapViewport(cells, satellite),
      hexagons: cells.map((cell) => createGlobalPositionHexagon(cell.lat, cell.lon, spacingKm)),
      indexById: new Map(cells.map((cell, index) => [cell.id, index])),
      indexesByBank: [
        cells.flatMap((cell, index) => cell.cellBank === 0 ? [index] : []),
        cells.flatMap((cell, index) => cell.cellBank === 1 ? [index] : []),
      ] as const,
    };
  }, [cells, positionSpacingKm, satellite]);

  useEffect(() => {
    let cancelled = false;
    fetch(landUrl)
      .then((response) => {
        if (!response.ok) throw new Error(`land request failed with ${response.status}`);
        return response.json() as Promise<LandTopology>;
      })
      .then((payload) => {
        if (!cancelled) setLand(landFeatureFromTopology(payload));
      })
      .catch((error) => {
        if (!cancelled) setLandError(error instanceof Error ? error.message : String(error));
      });
    return () => {
      cancelled = true;
    };
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
      const height = Math.max(330, Math.floor(box.height));
      canvas.width = Math.floor(width * ratio);
      canvas.height = Math.floor(height * ratio);
      canvas.style.width = `${width}px`;
      canvas.style.height = `${height}px`;
      const context = canvas.getContext("2d");
      if (!context) return;
      context.setTransform(ratio, 0, 0, ratio, 0, 0);
      context.clearRect(0, 0, width, height);

      const background = context.createLinearGradient(0, 0, 0, height);
      background.addColorStop(0, "#071c25");
      background.addColorStop(1, "#123a43");
      context.fillStyle = background;
      context.fillRect(0, 0, width, height);

      const mapInset = width < 520 ? 10 : 18;
      const mapTop = width < 520 ? 76 : 82;
      const mapBottom = height - (width < 520 ? 54 : 42);
      const projection = geoMercator()
        .rotate([-mapGeometry.viewport.centerLon, 0])
        .center([0, mapGeometry.viewport.centerLat])
        .precision(0.15);
      const focusArea = geoCircle()
        .center([mapGeometry.viewport.centerLon, mapGeometry.viewport.centerLat])
        .radius(mapGeometry.viewport.radiusDeg)
        .precision(0.5)();
      projection.fitExtent(
        [[mapInset, mapTop], [width - mapInset, mapBottom]],
        focusArea,
      );
      projection.clipExtent([[mapInset, mapTop], [width - mapInset, mapBottom]]);
      const path = geoPath(projection, context);

      context.fillStyle = "#0c2d37";
      context.fillRect(mapInset, mapTop, width - mapInset * 2, mapBottom - mapTop);
      if (land) {
        context.beginPath();
        path(land);
        context.fillStyle = "#9fb9ad";
        context.fill();
        context.strokeStyle = "rgba(231, 241, 236, 0.42)";
        context.lineWidth = 0.6;
        context.stroke();
        context.beginPath();
        path(geoGraticule10());
        context.strokeStyle = "rgba(207, 226, 222, 0.18)";
        context.lineWidth = 0.5;
        context.stroke();
      }
      context.strokeStyle = "rgba(112, 207, 238, 0.58)";
      context.lineWidth = 1;
      context.strokeRect(
        mapInset + 0.5,
        mapTop + 0.5,
        width - mapInset * 2 - 1,
        mapBottom - mapTop - 1,
      );

      const drawHexagonLayer = (
        indexes: readonly number[],
        fillStyle: string,
        strokeStyle: string,
        lineWidth: number,
        dashed = false,
      ) => {
        if (indexes.length === 0) return;
        context.save();
        context.beginPath();
        for (const index of indexes) path(mapGeometry.hexagons[index]);
        context.fillStyle = fillStyle;
        context.fill();
        context.strokeStyle = strokeStyle;
        context.lineWidth = lineWidth;
        context.setLineDash(dashed ? [2.5, 2] : []);
        context.stroke();
        context.restore();
      };

      drawHexagonLayer(
        mapGeometry.indexesByBank[0],
        "rgba(26, 113, 135, 0.28)",
        "rgba(126, 214, 238, 0.62)",
        0.75,
      );
      drawHexagonLayer(
        mapGeometry.indexesByBank[1],
        "rgba(142, 91, 33, 0.24)",
        "rgba(255, 187, 102, 0.72)",
        0.75,
        true,
      );

      const activeIndexes = activeCells.flatMap(({ id }) => {
        const index = mapGeometry.indexById.get(id);
        return index === undefined ? [] : [index];
      });
      if (activeIndexes.length > 0) {
        const activeColor = frame.cellBank === 1 ? BANK_COLORS[1] : BANK_COLORS[0];
        context.save();
        context.shadowColor = activeColor;
        context.shadowBlur = 11;
        drawHexagonLayer(
          activeIndexes,
          frame.cellBank === 1 ? "rgba(255, 173, 85, 0.9)" : "rgba(88, 210, 238, 0.92)",
          "#f7fffc",
          1.45,
        );
        context.restore();
      }

      const satelliteX = width / 2;
      const satelliteY = 29;
      const activeColor = frame.cellBank === 1 ? BANK_COLORS[1] : BANK_COLORS[0];
      context.save();
      context.strokeStyle = `${activeColor}b8`;
      context.lineWidth = 1.4;
      context.beginPath();
      context.moveTo(satelliteX - 12, satelliteY + 18);
      context.lineTo(satelliteX - 24, mapTop - 8);
      context.moveTo(satelliteX, satelliteY + 20);
      context.lineTo(satelliteX, mapTop - 5);
      context.moveTo(satelliteX + 12, satelliteY + 18);
      context.lineTo(satelliteX + 24, mapTop - 8);
      context.stroke();
      context.restore();

      context.save();
      context.shadowColor = "rgba(255, 190, 88, 0.42)";
      context.shadowBlur = 9;
      context.fillStyle = "#f4b85e";
      context.fillRect(satelliteX - 13, satelliteY - 6, 26, 12);
      context.fillStyle = "#6ed3ed";
      context.fillRect(satelliteX - 44, satelliteY - 5, 27, 10);
      context.fillRect(satelliteX + 17, satelliteY - 5, 27, 10);
      context.restore();
    };

    let drawFrame = 0;
    const scheduleDraw = () => {
      window.cancelAnimationFrame(drawFrame);
      drawFrame = window.requestAnimationFrame(draw);
    };
    const observer = new ResizeObserver(scheduleDraw);
    observer.observe(parent);
    scheduleDraw();
    return () => {
      observer.disconnect();
      window.cancelAnimationFrame(drawFrame);
    };
  }, [activeCells, frame, land, mapGeometry]);

  const activeBankLabel = frame?.cellBank === null || frame?.cellBank === undefined
    ? "等待安排"
    : `星载小区 ${frame.cellBank === 0 ? "A" : "B"}`;
  const timeLabel = frame
    ? `${formatAnimationTime(frame.startMs)}–${formatAnimationTime(frame.endMs)} ms`
    : "等待日历";
  const activeSlotIndex = frame ? Math.floor(frame.startMs / 10) : 0;

  return (
    <figure className="beam-animation-panel" aria-labelledby="beam-animation-title">
      <header>
        <div>
          <span>80 ms 轮转动画</span>
          <h3 id="beam-animation-title">80 ms内，这颗卫星如何分批广播网络发现信号</h3>
          <p>淡色六边形是本星负责的全部一级波位，亮色六边形是当前 2.5 ms 正在广播的一批。80 ms 内，全部波位都会依次获得一次网络发现信号。</p>
        </div>
        <div className="beam-animation-summary">
          <span>{timeLabel}</span>
          <b>{activeBankLabel}</b>
          <small>第 {frameIndex + 1} / {frames.length || 32} 批 · 第 {activeSlotIndex + 1} / 8 个 10 ms 时间段</small>
        </div>
      </header>

      <div className="beam-animation-facts" aria-label="动画读图说明">
        <article>
          <span>本星负责</span>
          <b>{cells.length}个</b>
          <small>小区A {mapGeometry.indexesByBank[0].length}个 · 小区B {mapGeometry.indexesByBank[1].length}个</small>
        </article>
        <article>
          <span>当前 2.5 ms</span>
          <b>{activeCells.length}个</b>
          <small>亮色区域同时广播</small>
        </article>
        <article>
          <span>完成一轮</span>
          <b>80 ms</b>
          <small>全部波位各安排一次</small>
        </article>
      </div>

      <p className="beam-animation-explanation">
        <b>为什么亮色波位不一定挨着？</b>
        当前软件按波位编号和可用波束分批，同一批无需相邻；日历会直接切换目标，无需沿地图逐格移动。淡色范围中的空隙通常是海面，或由附近其他卫星负责。
      </p>

      <div className={`beam-animation-map ${playing ? "is-playing" : ""}`}>
        <div className="beam-animation-orbit-note" aria-hidden="true">
          <b>{satellite.id}</b>
          <span>当前 2.5 ms 波束指向地图中的亮色区域</span>
        </div>
        <div className="beam-animation-canvas">
          <canvas
            ref={canvasRef}
            role="img"
            aria-label={`${satellite.id}一级波位轮转地图；本星负责${cells.length}个一级波位；${timeLabel}；${activeBankLabel}正在广播${activeCells.length}个一级波位`}
          />
          {cells.length === 0 ? <p className="beam-animation-empty">正在等待本星的一级波位分配结果</p> : null}
          {landError ? <p className="map-error">陆地边界加载失败：{landError}</p> : null}
          <div className="beam-animation-legend" aria-label="跳波束图例">
            <span><i className="cell-a" />本星负责 · 小区 A</span>
            <span><i className="cell-b" />本星负责 · 小区 B</span>
            <span><i className="active-beam" />当前 2.5 ms 正在广播</span>
          </div>
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
          {playing ? "暂停动画" : "播放动画"}
        </button>
        <label>
          <span>当前在 80 ms 一轮中的位置</span>
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
        一轮包含 8 个 10 ms 时间段，每个时间段再分成 4 个 2.5 ms 批次。动画展示软件日历的照射顺序；无线设备接入后，将按同一日历核对天线和空口的实际发射。
      </figcaption>
    </figure>
  );
}
