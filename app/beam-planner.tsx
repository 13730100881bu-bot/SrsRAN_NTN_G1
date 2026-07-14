"use client";

import type { CSSProperties, FormEvent, KeyboardEvent } from "react";
import { useEffect, useMemo, useRef, useState } from "react";
import { axisForL1Cell, CHINA_SERVICE_OUTLINE, l1Catalog, l1CellById, l1CellForAxis, type L1CatalogCell } from "./beam-catalog";
import { BeamHoppingView } from "./beam-hopping-view";
import { OrbitConstellationView } from "./orbit-view";
import { baseline, satellites } from "./orbit-model";

type View = "national" | "cluster" | "handover" | "orbit" | "hopping";
type ColorMode = "region" | "reuse" | "kind";

type BeamCell = {
  id: string;
  u: number;
  v: number;
  reuse: number;
  region: string;
  catalogId: string;
  catalogKind: "full" | "edge";
  childCount: number;
};

type CanvasCell = BeamCell & { x: number; y: number; radius: number };

const REUSE_COLORS = ["#45a8e5", "#ff9b54", "#53c68c", "#ec6f9d", "#9678d3", "#42bbb2", "#e4c64d"];
const REGION_OPTIONS = ["全国", "华北", "华东", "华南", "西部"];
const L2_POSITIONS = [
  { id: "D0", q: 0, r: 0, tx: "0%", ty: "0%" },
  { id: "D1", q: 1, r: 0, tx: "100%", ty: "0%" },
  { id: "D2", q: 1, r: -1, tx: "50%", ty: "-75%" },
  { id: "D3", q: 0, r: -1, tx: "-50%", ty: "-75%" },
  { id: "D4", q: -1, r: 0, tx: "-100%", ty: "0%" },
  { id: "D5", q: -1, r: 1, tx: "-50%", ty: "75%" },
  { id: "D6", q: 0, r: 1, tx: "50%", ty: "75%" },
];

const TAKEOVER_PHASES = [
  { name: "源星持有波位", note: "管理中心保留完整 candidate 全集；当前 L1 仍由源星的星载小区服务。", a: 1, b: 0, source: "position owner", target: "candidate" },
  { name: "目标资源就绪", note: "目标星预留资源并等待 applied；就绪后可用不同 NCI / PCI 广播发现信号，但服务所有权仍在源星。", a: 1, b: 0.45, source: "position owner", target: "discovery RF" },
  { name: "测量重叠与切换", note: "源、目标短时同时可测，连接态 UE 执行跨小区切换；到计划 epoch 后 position_id 的服务所有权转给目标星。", a: 0.55, b: 1, source: "handover source", target: "position owner" },
  { name: "目标星主用", note: "目标星把该 L1 纳入本星两个小区之一；其他 L1 可按各自日历独立接管。", a: 0, b: 1, source: "candidate", target: "position owner" },
];

const CHINA_OUTLINE = CHINA_SERVICE_OUTLINE;

function mod(value: number, base: number) {
  return ((value % base) + base) % base;
}

function signed(value: number, width = 5) {
  return `${value >= 0 ? "+" : "-"}${Math.abs(value).toString().padStart(width, "0")}`;
}

function makeL1ShortId(parent: Pick<BeamCell, "catalogId">) {
  return parent.catalogId;
}

function makeBeamCell(u: number, v: number, region = "华东 / 沿海核心"): BeamCell {
  const catalog = l1CellForAxis(u, v);
  return makeBeamCellForCatalog(catalog, region);
}

function makeBeamCellForCatalog(catalog: L1CatalogCell, region = regionForPoint(catalog.x, catalog.y)): BeamCell {
  const { u, v } = axisForL1Cell(catalog);
  return {
    id: `CN-G01-L1-U${signed(u)}-V${signed(v)}`,
    u,
    v,
    reuse: mod(u + 3 * v, 7),
    region,
    catalogId: catalog.id,
    catalogKind: catalog.kind,
    childCount: catalog.childCount,
  };
}

function makeL2Identity(parent: Pick<BeamCell, "u" | "v" | "catalogId">, position: (typeof L2_POSITIONS)[number]) {
  const centerQ = 3 * parent.u + parent.v;
  const centerR = -parent.u + 2 * parent.v;
  const q = centerQ + position.q;
  const r = centerR + position.r;
  return {
    q,
    r,
    id: `CN-G01-L2-Q${signed(q)}-R${signed(r)}`,
    shortId: `${parent.catalogId}-${position.id.slice(1)}`,
  };
}

function regionForPoint(x: number, y: number) {
  if (x > 0.63 && y > 0.42) return "华东 / 沿海核心";
  if (x > 0.58 && y <= 0.42) return "华北 / 核心服务区";
  if (x > 0.48 && y > 0.58) return "华南 / 沿海核心";
  return "西部 / 常规服务区";
}

function regionColor(region: string, alpha = 0.78) {
  if (region.startsWith("华北")) return `rgba(69, 168, 229, ${alpha})`;
  if (region.startsWith("华东")) return `rgba(83, 198, 140, ${alpha})`;
  if (region.startsWith("华南")) return `rgba(239, 173, 85, ${alpha})`;
  return `rgba(150, 120, 211, ${alpha})`;
}

function drawHex(context: CanvasRenderingContext2D, x: number, y: number, radius: number) {
  context.beginPath();
  for (let i = 0; i < 6; i += 1) {
    const angle = (Math.PI / 180) * (60 * i - 30);
    const px = x + radius * Math.cos(angle);
    const py = y + radius * Math.sin(angle);
    if (i === 0) context.moveTo(px, py);
    else context.lineTo(px, py);
  }
  context.closePath();
}

function NationalCanvas({
  mode,
  region,
  showLabels,
  selected,
  onSelect,
}: {
  mode: ColorMode;
  region: string;
  showLabels: boolean;
  selected: BeamCell;
  onSelect: (cell: BeamCell) => void;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const cellsRef = useRef<CanvasCell[]>([]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const parent = canvas.parentElement;
    if (!parent) return;

    const draw = () => {
      const bounds = parent.getBoundingClientRect();
      const width = Math.max(520, Math.floor(bounds.width));
      const height = Math.max(430, Math.floor(bounds.height));
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      canvas.width = width * dpr;
      canvas.height = height * dpr;
      canvas.style.width = `${width}px`;
      canvas.style.height = `${height}px`;
      const context = canvas.getContext("2d");
      if (!context) return;
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
      context.clearRect(0, 0, width, height);

      const marginX = width * 0.055;
      const marginY = height * 0.09;
      const mapWidth = width - marginX * 2;
      const mapHeight = height - marginY * 1.55;
      const polygon = CHINA_OUTLINE.map(([x, y]) => [marginX + x * mapWidth, marginY + y * mapHeight] as [number, number]);

      context.fillStyle = "rgba(15, 30, 39, 0.025)";
      context.strokeStyle = "rgba(15, 30, 39, 0.075)";
      context.lineWidth = 1;
      for (let x = 0; x < width; x += 28) {
        context.beginPath(); context.moveTo(x, 0); context.lineTo(x, height); context.stroke();
      }
      for (let y = 0; y < height; y += 28) {
        context.beginPath(); context.moveTo(0, y); context.lineTo(width, y); context.stroke();
      }

      context.beginPath();
      polygon.forEach(([x, y], index) => index === 0 ? context.moveTo(x, y) : context.lineTo(x, y));
      context.closePath();
      context.fillStyle = "#e8ece8";
      context.fill();
      context.strokeStyle = "#aeb9b7";
      context.lineWidth = 1.4;
      context.stroke();
      context.beginPath();
      context.ellipse(marginX + 0.6 * mapWidth, marginY + 0.83 * mapHeight, 0.04 * mapWidth, 0.032 * mapHeight, 0, 0, Math.PI * 2);
      context.fillStyle = "#e8ece8";
      context.fill();
      context.strokeStyle = "#aeb9b7";
      context.stroke();

      const radius = Math.max(2.15, Math.min(4.25, width / 235));
      const cells: CanvasCell[] = l1Catalog.map((catalog) => {
        const x = marginX + catalog.x * mapWidth;
        const y = marginY + catalog.y * mapHeight;
        const base = makeBeamCellForCatalog(catalog);
        drawHex(context, x, y, radius);
        if (mode === "reuse") context.fillStyle = `${REUSE_COLORS[base.reuse]}c2`;
        else if (mode === "kind") context.fillStyle = catalog.kind === "edge" ? "#efad55d0" : "#50b7a4c8";
        else context.fillStyle = regionColor(base.region);
        context.fill();
        context.strokeStyle = "rgba(255,255,255,.5)";
        context.lineWidth = 0.45;
        context.stroke();
        return { ...base, x, y, radius };
      });
      cellsRef.current = cells;

      const highlightCenter: Record<string, [number, number]> = {
        全国: [0.51, 0.4], 华北: [0.69, 0.28], 华东: [0.75, 0.49], 华南: [0.58, 0.67], 西部: [0.27, 0.39],
      };
      if (region !== "全国") {
        const [hx, hy] = highlightCenter[region];
        context.beginPath();
        context.arc(marginX + hx * mapWidth, marginY + hy * mapHeight, Math.min(width, height) * 0.12, 0, Math.PI * 2);
        context.fillStyle = "rgba(255,255,255,.12)";
        context.fill();
        context.strokeStyle = "#ffffff";
        context.lineWidth = 1.5;
        context.setLineDash([5, 5]);
        context.stroke();
        context.setLineDash([]);
      }

      const selectedCanvas = cells.reduce<CanvasCell | null>((closest, item) => {
        if (!closest) return item;
        if (item.catalogId === selected.catalogId) return item;
        const currentDistance = Math.abs(item.u - selected.u) + Math.abs(item.v - selected.v);
        const closestDistance = Math.abs(closest.u - selected.u) + Math.abs(closest.v - selected.v);
        return currentDistance < closestDistance ? item : closest;
      }, null);
      if (selectedCanvas) {
        drawHex(context, selectedCanvas.x, selectedCanvas.y, radius + 2.2);
        context.strokeStyle = "#0c2430";
        context.lineWidth = 2.4;
        context.stroke();
        context.beginPath();
        context.arc(selectedCanvas.x, selectedCanvas.y, 2.1, 0, Math.PI * 2);
        context.fillStyle = "#ffffff";
        context.fill();
      }

      if (showLabels) {
        const labels = [
          [0.69, 0.27, "华北"], [0.75, 0.49, "华东"], [0.56, 0.68, "华南"], [0.26, 0.39, "西部"],
        ] as const;
        context.font = "600 11px ui-sans-serif, system-ui";
        context.textAlign = "center";
        labels.forEach(([x, y, label]) => {
          const px = marginX + x * mapWidth;
          const py = marginY + y * mapHeight;
          context.fillStyle = "rgba(255,255,255,.86)";
          context.fillRect(px - 19, py - 10, 38, 19);
          context.fillStyle = "#243840";
          context.fillText(label, px, py + 4);
        });
      }
    };

    draw();
    const observer = new ResizeObserver(draw);
    observer.observe(parent);
    return () => observer.disconnect();
  }, [mode, region, selected, showLabels]);

  const selectFromPointer = (event: React.PointerEvent<HTMLCanvasElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect();
    const x = event.clientX - bounds.left;
    const y = event.clientY - bounds.top;
    const closest = cellsRef.current.reduce<CanvasCell | null>((best, item) => {
      if (!best) return item;
      return Math.hypot(item.x - x, item.y - y) < Math.hypot(best.x - x, best.y - y) ? item : best;
    }, null);
    if (closest && Math.hypot(closest.x - x, closest.y - y) < closest.radius * 2.5) onSelect(closest);
  };

  const handleKey = (event: KeyboardEvent<HTMLCanvasElement>) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      const firstReady = cellsRef.current[0];
      if (firstReady) onSelect(firstReady);
    }
  };

  return (
    <canvas
      ref={canvasRef}
      className="national-canvas"
      role="button"
      tabIndex={0}
      aria-label="大陆及海南 2,620 个地固一级波位目录。点击任一六边形查看波位详情。"
      onPointerDown={selectFromPointer}
      onKeyDown={handleKey}
      data-testid="national-grid"
    />
  );
}

function ClusterView({ parent, selectedChild, onSelect }: { parent: BeamCell; selectedChild: number; onSelect: (index: number) => void }) {
  const visiblePositions = L2_POSITIONS.slice(0, parent.childCount);
  return (
    <div className="cluster-stage" data-testid="cluster-stage">
      <div className="cluster-caption">
        <span>L1 信令服务轮廓</span>
        <strong>{parent.catalogId} · {parent.childCount} 个 L2 业务位置{parent.catalogKind === "edge" ? "（边缘裁剪）" : ""} · 星载小区运行时归属</strong>
      </div>
      <div className="cluster-grid">
        <div className="parent-ring" aria-hidden="true" />
        {visiblePositions.map((position, index) => {
          const identity = makeL2Identity(parent, position);
          return (
            <button
              key={position.id}
              type="button"
              className={`hex-button ${selectedChild === index ? "is-selected" : ""}`}
              style={{ "--hex-tx": position.tx, "--hex-ty": position.ty, "--hex-color": REUSE_COLORS[mod(position.q + 3 * position.r, 7)] } as CSSProperties}
              onClick={() => onSelect(index)}
              aria-pressed={selectedChild === index}
              aria-label={`${position.id}，短号 ${identity.shortId}，复用色 ${mod(position.q + 3 * position.r, 7)}`}
              data-testid={`l2-${position.id}`}
            >
              <b>{position.id}</b>
              <span>R{mod(position.q + 3 * position.r, 7)}</span>
            </button>
          );
        })}
      </div>
      <div className="cluster-scale"><span /> 15 km 名义半径</div>
      <div className="cluster-rule rule-intra">同 L1：资源重配</div>
      <div className="cluster-rule rule-inter">跨 L1：目标 ready 后 HO</div>
      <div className="l2-id-strip" aria-label="当前一级波位的七个二级波位 ID">
        {visiblePositions.map((position, index) => {
          const identity = makeL2Identity(parent, position);
          return (
            <button type="button" key={position.id} className={selectedChild === index ? "active" : ""} onClick={() => onSelect(index)} aria-pressed={selectedChild === index}>
              <span>{position.id}</span><b>Q{signed(identity.q)}</b><b>R{signed(identity.r)}</b>
            </button>
          );
        })}
      </div>
    </div>
  );
}

function HandoverView({ phase, onPhase, parent }: { phase: number; onPhase: (phase: number) => void; parent: BeamCell }) {
  const current = TAKEOVER_PHASES[phase];
  return (
    <div className="handover-stage" data-testid="handover-stage">
      <div className="satellite-row">
        <div className="satellite-card sat-a" style={{ opacity: 0.35 + current.a * 0.65 }}>
          <span className="sat-symbol">SAT</span><b>P08-S12</b><small>{current.source}</small>
        </div>
        <div className="handover-arrow"><span>assignment</span><b>→</b></div>
        <div className="satellite-card sat-b" style={{ opacity: 0.35 + current.b * 0.65 }}>
          <span className="sat-symbol">SAT</span><b>P09-S07</b><small>{current.target}</small>
        </div>
      </div>
      <div className="beam-takeover">
        <div className="signal-line line-a" style={{ opacity: current.a }} />
        <div className="signal-line line-b" style={{ opacity: current.b }} />
        <div className="takeover-hex">
          <span>地固波位 position_id</span><b>{parent.catalogId}</b><small>源、目标分别使用各自星载 NCI / PCI</small>
        </div>
      </div>
      <div className="phase-tabs" role="group" aria-label="接管阶段">
        {TAKEOVER_PHASES.map((item, index) => (
          <button key={item.name} type="button" className={phase === index ? "active" : ""} onClick={() => onPhase(index)} aria-pressed={phase === index}>
            <i>{index + 1}</i><span>{item.name}</span>
          </button>
        ))}
      </div>
      <input className="phase-range" type="range" min="0" max="3" step="1" value={phase} onChange={(event) => onPhase(Number(event.target.value))} aria-label="接管阶段滑块" />
      <p className="phase-note"><b>{current.name}</b>{current.note}</p>
    </div>
  );
}

export function BeamPlanner() {
  const [view, setView] = useState<View>("national");
  const [colorMode, setColorMode] = useState<ColorMode>("region");
  const [region, setRegion] = useState("全国");
  const [showLabels, setShowLabels] = useState(true);
  const [selected, setSelected] = useState(() => makeBeamCellForCatalog(l1CellById("A1970") ?? l1Catalog[0]));
  const [selectedChild, setSelectedChild] = useState(0);
  const [phase, setPhase] = useState(0);
  const [search, setSearch] = useState("");
  const [searchError, setSearchError] = useState("");
  const [orbitTime, setOrbitTime] = useState(0);
  const [selectedSatelliteId, setSelectedSatelliteId] = useState("P01-S01");

  const selectedL2 = L2_POSITIONS[Math.min(selectedChild, selected.childCount - 1)];
  const selectedL2Identity = makeL2Identity(selected, selectedL2);
  const selectedL2Reuse = mod(selectedL2.q + 3 * selectedL2.r, 7);

  const submitSearch = (event: FormEvent) => {
    event.preventDefault();
    const value = search.trim();
    const catalogMatch = value.match(/^A\d{4}$/i);
    if (catalogMatch) {
      const catalog = l1CellById(value);
      if (catalog) {
        const axis = axisForL1Cell(catalog);
        setSelected(makeBeamCell(axis.u, axis.v));
        setSelectedChild(0);
        setSearchError("");
        setView("cluster");
        return;
      }
    }
    const match = value.match(/^CN1\.1\.([+-]\d+)\.([+-]\d+)$/i) ?? value.match(/^CN-G01-L1-U([+-]\d+)-V([+-]\d+)$/i);
    if (!match) {
      setSearchError("请输入 L1 短号（如 A1427）或完整 position_id");
      return;
    }
    setSelected(makeBeamCell(Number(match[1]), Number(match[2])));
    setSelectedChild(0);
    setSearchError("");
    setView("cluster");
  };

  const metrics = useMemo(() => [
    { label: "规划 L1", value: l1Catalog.length.toLocaleString("en-US"), note: "固定信令跳变目录" },
    { label: "80 ms SSB 容量", value: "≤ 84 L1", note: "双小区日历推导值，非协议常量" },
    { label: "每星小区", value: "2 NCI", note: "每小区 8 模拟 / 64 数字资源" },
    { label: "参考星座", value: `${satellites.length} 星`, note: `${baseline.planes} 面 × ${baseline.satellitesPerPlane} 星 · 一期离线仿真` },
  ], []);

  return (
    <main className="planner-shell">
      <header className="topbar">
        <div className="brand-block">
          <div className="brand-mark">L1</div>
          <div><p>ONBOARD NTN BEAM PLANNING</p><h1>星载双小区与跳波束规划台</h1></div>
        </div>
        <nav className="view-tabs" aria-label="主视图">
          <button type="button" className={view === "national" ? "active" : ""} onClick={() => setView("national")} aria-pressed={view === "national"}>全国目录</button>
          <button type="button" className={view === "cluster" ? "active" : ""} onClick={() => setView("cluster")} aria-pressed={view === "cluster"}>L1 / L2 编排</button>
          <button type="button" className={view === "handover" ? "active" : ""} onClick={() => setView("handover")} aria-pressed={view === "handover"}>多星接管</button>
          <button type="button" className={view === "orbit" ? "active" : ""} onClick={() => setView("orbit")} aria-pressed={view === "orbit"}>轨道星座</button>
          <button type="button" className={view === "hopping" ? "active" : ""} onClick={() => setView("hopping")} aria-pressed={view === "hopping"}>跳波束接入</button>
        </nav>
        <div className="version-chip"><span />双小区仿真 v0.7</div>
      </header>

      <section className="metric-strip" aria-label="方案规模">
        {metrics.map((metric) => <article key={metric.label}><span>{metric.label}</span><strong>{metric.value}</strong><small>{metric.note}</small></article>)}
      </section>

      <div className={`workspace-grid ${view === "orbit" || view === "hopping" ? "workspace-full" : ""}`}>
        <aside className="control-panel" aria-label="规划控制">
          <div className="panel-heading"><span>01</span><div><p>PLANNING SCOPE</p><h2>规划范围</h2></div></div>
          <div className="region-list" role="group" aria-label="区域筛选">
            {REGION_OPTIONS.map((item) => <button type="button" key={item} className={region === item ? "active" : ""} onClick={() => setRegion(item)} aria-pressed={region === item}><span>{item}</span><b>{item === "全国" ? "2,620" : item === "华东" ? "684" : item === "西部" ? "892" : item === "华北" ? "536" : "508"}</b></button>)}
          </div>

          <div className="control-section">
            <label className="section-label">着色方式</label>
            <div className="segmented-control" role="group" aria-label="着色方式">
              <button type="button" className={colorMode === "region" ? "active" : ""} onClick={() => setColorMode("region")} aria-pressed={colorMode === "region"}>区域</button>
              <button type="button" className={colorMode === "reuse" ? "active" : ""} onClick={() => setColorMode("reuse")} aria-pressed={colorMode === "reuse"}>复用</button>
              <button type="button" className={colorMode === "kind" ? "active" : ""} onClick={() => setColorMode("kind")} aria-pressed={colorMode === "kind"}>完整 / 边缘</button>
            </div>
            {colorMode === "reuse" && <div className="reuse-legend" aria-label="复用色图例">{REUSE_COLORS.map((color, index) => <span key={color} style={{ background: color }} title={`复用色 ${index}`}>{index}</span>)}</div>}
          </div>

          <div className="control-section toggles">
            <label><span><b>区域标签</b><small>只影响显示，不改变目录</small></span><input type="checkbox" checked={showLabels} onChange={(event) => setShowLabels(event.target.checked)} /></label>
          </div>

          <form className="beam-search" onSubmit={submitSearch}>
            <label htmlFor="beam-id">定位一级波位</label>
            <div><input id="beam-id" value={search} onChange={(event) => setSearch(event.target.value)} placeholder="A1427 / CN-G01-L1-U…" /><button type="submit">定位</button></div>
            {searchError && <p role="alert">{searchError}</p>}
          </form>

          <div className="projection-card"><p>投影基线</p><b>Lambert Conformal Conic</b><span>105°E · 35°N · GRS80</span><small>标准纬线 23.5°N / 49.25°N</small></div>
        </aside>

        <section className="stage-panel" aria-label="波位画布">
          <div className="stage-header">
            <div><p>{view === "national" ? "GROUND-FIXED POSITION CATALOG" : view === "cluster" ? "L1 / L2 HIERARCHY" : view === "handover" ? "POSITION OWNERSHIP TRANSFER" : view === "orbit" ? "ORBIT CONSTELLATION" : "DUAL-CELL BEAM CALENDAR"}</p><h2>{view === "national" ? "大陆及海南地固 L1 波位目录" : view === "cluster" ? "一级与二级波位平铺编排" : view === "handover" ? "跨星 L1 所有权与小区切换" : view === "orbit" ? "大陆及海南一期参考星座" : "星载双小区跳波束资源日历"}</h2></div>
            <div className="stage-status"><span className="pulse" />{view === "handover" ? TAKEOVER_PHASES[phase].name : view === "orbit" ? `${baseline.shell} · ${baseline.planes} × ${baseline.satellitesPerPlane}` : view === "hopping" ? `${selectedSatelliteId} · 2 × (8 / 64)` : `${region} · ${l1Catalog.length.toLocaleString("en-US")} L1`}</div>
          </div>
          <div className={`visual-stage view-${view}`}>
            {view === "national" && <NationalCanvas mode={colorMode} region={region} showLabels={showLabels} selected={selected} onSelect={(cell) => { setSelected(cell); setSelectedChild(0); }} />}
            {view === "cluster" && <ClusterView parent={selected} selectedChild={selectedChild} onSelect={setSelectedChild} />}
            {view === "handover" && <HandoverView phase={phase} onPhase={setPhase} parent={selected} />}
            {view === "orbit" && <OrbitConstellationView beam={selected} time={orbitTime} onTimeChange={setOrbitTime} selectedId={selectedSatelliteId} onSelectedIdChange={setSelectedSatelliteId} />}
            {view === "hopping" && <BeamHoppingView beam={selected} time={orbitTime} onTimeChange={setOrbitTime} selectedSatelliteId={selectedSatelliteId} onSelectedSatelliteChange={setSelectedSatelliteId} />}
          </div>
          <div className="stage-footer">
            <span><b>固定几何</b>2,620 个 L1 与其 L2 position_id 保持地固</span><span><b>{view === "orbit" ? "一期范围" : view === "hopping" ? "接入硬约束" : "运行时归属"}</b>{view === "orbit" ? "大陆及海南连续覆盖" : view === "hopping" ? "空闲 L1 仍需 SSB，RO 必须配 UL 波束" : "L1 由当前卫星划入两个星载小区"}</span><span><b>小区身份</b>{view === "hopping" ? "星载 CU-CP 决策、无线执行层落地" : "NCI 与 PCI 跟随星载小区"}</span>
          </div>
        </section>

        <aside className="inspector-panel" aria-label="波位详情">
          <div className="panel-heading"><span>02</span><div><p>SELECTION</p><h2>当前选择</h2></div></div>
          <div className="selection-id">
            <span>L1 · GROUND FIXED</span>
            <h3>{makeL1ShortId(selected)}</h3>
            <button type="button" onClick={() => setView("cluster")}>展开 {selected.childCount} 个 L2 →</button>
            <details className="export-key"><summary>查看导出键</summary><code>{selected.id}</code></details>
          </div>
          <dl className="detail-grid">
            <div><dt>运行时小区</dt><dd>由当前星载双小区划分</dd></div><div><dt>NCI / PCI</dt><dd>跟随星载小区</dd></div>
            <div><dt>轴坐标</dt><dd>U {signed(selected.u)} / V {signed(selected.v)}</dd></div><div><dt>L1 复用色</dt><dd><i style={{ background: REUSE_COLORS[selected.reuse] }} />R{selected.reuse}</dd></div>
            <div><dt>服务区域</dt><dd>{selected.region}</dd></div><div><dt>目录类型</dt><dd>{selected.catalogKind === "full" ? "完整 7 波位组" : `边缘 ${selected.childCount} 波位组`}</dd></div>
          </dl>

          <div className="l2-inspector">
            <div><span>L2 短号 · {selectedL2.id} 为局部槽位</span><b>{selectedL2Identity.shortId}</b></div>
            <div className="mini-hex" style={{ "--hex-color": REUSE_COLORS[selectedL2Reuse] } as CSSProperties}>{selectedL2.id}</div>
            <details className="export-key"><summary>查看导出键</summary><code>{selectedL2Identity.id}</code></details>
            <dl><div><dt>全局轴坐标</dt><dd>Q {signed(selectedL2Identity.q)} / R {signed(selectedL2Identity.r)}</dd></div><div><dt>名义半径</dt><dd>15 km</dd></div><div><dt>复用色</dt><dd>R{selectedL2Reuse}</dd></div><div><dt>服务资源</dt><dd>PDU / DRB · SR/SRS</dd></div></dl>
          </div>

          <div className="invariant-box"><p>当前不变量</p><ul><li>{selected.catalogId} 的 position_id 与地面几何保持不变</li><li>每颗卫星固定运行两个星载 NR 小区</li><li>波位跨星接管时，服务 NCI / PCI 随目标星小区改变</li><li>数字 L2 收到 applied feedback 后才视为 ready</li></ul></div>
        </aside>
      </div>

      <section className="decision-strip">
        <div><span>DECISION 01</span><b>地面只固定波位</b><p>L1/L2 保存稳定 position_id；不再预设全国固定分区。</p></div>
        <div><span>DECISION 02</span><b>NCI / PCI 属于星载小区</b><p>每星两个小区长期持有身份，波位按当前日历动态归属。</p></div>
        <div><span>DECISION 03</span><b>84 是带假设的日历保证量</b><p>80 ms 含 4 次 SSB occasion；最差 21 个 DL 端口机会各假设 2 次顺序子访问，得到每小区 42、每星 84。</p></div>
        <div><span>DECISION 04</span><b>按波位跨星接管</b><p>目标资源 ready/applied 后切换 position_id；终端执行跨 NCI / PCI 移动。</p></div>
      </section>
      <footer><span>CHINA MAINLAND + HAINAN · GROUND-FIXED BEAM POSITIONS</span><p>20/80/640 ms、84 L1 与 8/64 资源均为一期仿真参数或推导值，不是协议常量或已实现的无线能力。</p></footer>
    </main>
  );
}
