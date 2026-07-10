"use client";

import type { CSSProperties, FormEvent, KeyboardEvent } from "react";
import { useEffect, useMemo, useRef, useState } from "react";

type View = "national" | "cluster" | "handover";
type ColorMode = "service" | "reuse" | "ta";

type BeamCell = {
  id: string;
  u: number;
  v: number;
  reuse: number;
  tac: number;
  nci: string;
  region: string;
  satellite: string;
  status: "ready" | "candidate";
};

type CanvasCell = BeamCell & { x: number; y: number; radius: number; guard: boolean };

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
  { name: "A 主用 / B 候选", note: "SAT-B 进入 eligible 集合，尚不承载业务。", a: 1, b: 0.18 },
  { name: "B 预应用", note: "目标资源下发，等待 DU applied feedback。", a: 1, b: 0.46 },
  { name: "A+B 重叠", note: "短时重叠服务，波位身份与邻接关系保持不变。", a: 0.82, b: 0.82 },
  { name: "B 主用 / A 释放", note: "SAT-B 成为 primary，SAT-A 清理旧执行状态。", a: 0.16, b: 1 },
];

const CHINA_OUTLINE = [
  [0.08, 0.35], [0.14, 0.25], [0.12, 0.16], [0.22, 0.12], [0.29, 0.17], [0.39, 0.13],
  [0.49, 0.2], [0.58, 0.18], [0.66, 0.09], [0.79, 0.06], [0.9, 0.13], [0.87, 0.24],
  [0.79, 0.29], [0.88, 0.39], [0.83, 0.49], [0.75, 0.53], [0.71, 0.62], [0.64, 0.65],
  [0.59, 0.76], [0.52, 0.74], [0.48, 0.64], [0.4, 0.61], [0.34, 0.53], [0.23, 0.55],
  [0.17, 0.48], [0.1, 0.47],
] as const;

function mod(value: number, base: number) {
  return ((value % base) + base) % base;
}

function signed(value: number, width = 5) {
  return `${value >= 0 ? "+" : "-"}${Math.abs(value).toString().padStart(width, "0")}`;
}

function makeBeamCell(u: number, v: number, region = "华东 / 沿海核心"): BeamCell {
  const hash = Math.abs((u * 73856093) ^ (v * 19349663));
  return {
    id: `CN-G01-L1-U${signed(u)}-V${signed(v)}`,
    u,
    v,
    reuse: mod(u + 3 * v, 7),
    tac: 31000 + mod(Math.floor(hash / 19), 138),
    nci: `0x${mod(hash, 0xffffff).toString(16).toUpperCase().padStart(6, "0")}`,
    region,
    satellite: mod(u + v, 3) === 0 ? "SAT-B07" : "SAT-A12",
    status: mod(u - v, 11) === 0 ? "candidate" : "ready",
  };
}

function makeL2Identity(parent: Pick<BeamCell, "u" | "v">, position: (typeof L2_POSITIONS)[number]) {
  const centerQ = 3 * parent.u + parent.v;
  const centerR = -parent.u + 2 * parent.v;
  const q = centerQ + position.q;
  const r = centerR + position.r;
  return {
    q,
    r,
    id: `CN-G01-L2-Q${signed(q)}-R${signed(r)}`,
  };
}

function pointInPolygon(x: number, y: number, polygon: readonly (readonly [number, number])[]) {
  let inside = false;
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
    const [xi, yi] = polygon[i];
    const [xj, yj] = polygon[j];
    const intersects = yi > y !== yj > y && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi;
    if (intersects) inside = !inside;
  }
  return inside;
}

function regionForPoint(x: number, y: number) {
  if (x > 0.63 && y > 0.42) return "华东 / 沿海核心";
  if (x > 0.58 && y <= 0.42) return "华北 / 核心服务区";
  if (x > 0.48 && y > 0.58) return "华南 / 沿海核心";
  return "西部 / 常规服务区";
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
  showGuard,
  showLabels,
  selected,
  onSelect,
}: {
  mode: ColorMode;
  region: string;
  showGuard: boolean;
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

      const radius = Math.max(5.2, Math.min(8.2, width / 112));
      const xStep = Math.sqrt(3) * radius;
      const yStep = radius * 1.5;
      const cells: CanvasCell[] = [];
      let row = 0;
      for (let y = marginY + radius; y < marginY + mapHeight; y += yStep, row += 1) {
        const offset = row % 2 ? xStep / 2 : 0;
        for (let x = marginX + radius + offset; x < marginX + mapWidth; x += xStep) {
          const nx = (x - marginX) / mapWidth;
          const ny = (y - marginY) / mapHeight;
          const inside = pointInPolygon(nx, ny, CHINA_OUTLINE);
          const guard = !inside && showGuard && CHINA_OUTLINE.some(([px, py]) => Math.hypot(nx - px, ny - py) < 0.055);
          if (!inside && !guard) continue;
          const u = Math.round((nx - 0.5) * 310);
          const v = Math.round((ny - 0.48) * 190);
          const base = makeBeamCell(u, v, regionForPoint(nx, ny));
          const cell = { ...base, x, y, radius, guard };
          cells.push(cell);
          drawHex(context, x, y, radius - 0.45);
          if (guard) context.fillStyle = "rgba(115, 129, 132, 0.12)";
          else if (mode === "reuse") context.fillStyle = `${REUSE_COLORS[base.reuse]}b8`;
          else if (mode === "ta") context.fillStyle = mod(base.tac, 2) === 0 ? "#50b7a4a8" : "#efad55a8";
          else context.fillStyle = base.status === "ready" ? "#3ea6d69a" : "#bdc9c9a8";
          context.fill();
          context.strokeStyle = "rgba(255,255,255,.62)";
          context.lineWidth = 0.7;
          context.stroke();
        }
      }
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
        if (item.guard) return closest;
        if (!closest) return item;
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
  }, [mode, region, selected, showGuard, showLabels]);

  const selectFromPointer = (event: React.PointerEvent<HTMLCanvasElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect();
    const x = event.clientX - bounds.left;
    const y = event.clientY - bounds.top;
    const closest = cellsRef.current.reduce<CanvasCell | null>((best, item) => {
      if (item.guard) return best;
      if (!best) return item;
      return Math.hypot(item.x - x, item.y - y) < Math.hypot(best.x - x, best.y - y) ? item : best;
    }, null);
    if (closest && Math.hypot(closest.x - x, closest.y - y) < closest.radius * 2.5) onSelect(closest);
  };

  const handleKey = (event: KeyboardEvent<HTMLCanvasElement>) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      const firstReady = cellsRef.current.find((cell) => !cell.guard);
      if (firstReady) onSelect(firstReady);
    }
  };

  return (
    <canvas
      ref={canvasRef}
      className="national-canvas"
      role="button"
      tabIndex={0}
      aria-label="全国一级波位目录。点击任一六边形查看详细信息。"
      onPointerDown={selectFromPointer}
      onKeyDown={handleKey}
      data-testid="national-grid"
    />
  );
}

function ClusterView({ parent, selectedChild, onSelect }: { parent: BeamCell; selectedChild: number; onSelect: (index: number) => void }) {
  return (
    <div className="cluster-stage" data-testid="cluster-stage">
      <div className="cluster-caption">
        <span>L1 信令服务轮廓</span>
        <strong>1 个 access cell · 7 个数字业务位置</strong>
      </div>
      <div className="cluster-grid">
        <div className="parent-ring" aria-hidden="true" />
        {L2_POSITIONS.map((position, index) => {
          const identity = makeL2Identity(parent, position);
          return (
            <button
              key={position.id}
              type="button"
              className={`hex-button ${selectedChild === index ? "is-selected" : ""}`}
              style={{ "--hex-tx": position.tx, "--hex-ty": position.ty, "--hex-color": REUSE_COLORS[mod(position.q + 3 * position.r, 7)] } as CSSProperties}
              onClick={() => onSelect(index)}
              aria-pressed={selectedChild === index}
              aria-label={`${position.id}，正式 ID ${identity.id}，复用色 ${mod(position.q + 3 * position.r, 7)}`}
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
        {L2_POSITIONS.map((position, index) => {
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

function HandoverView({ phase, onPhase }: { phase: number; onPhase: (phase: number) => void }) {
  const current = TAKEOVER_PHASES[phase];
  return (
    <div className="handover-stage" data-testid="handover-stage">
      <div className="satellite-row">
        <div className="satellite-card sat-a" style={{ opacity: 0.35 + current.a * 0.65 }}>
          <span className="sat-symbol">SAT</span><b>SAT-A12</b><small>{phase === 3 ? "released" : "primary"}</small>
        </div>
        <div className="handover-arrow"><span>assignment</span><b>→</b></div>
        <div className="satellite-card sat-b" style={{ opacity: 0.35 + current.b * 0.65 }}>
          <span className="sat-symbol">SAT</span><b>SAT-B07</b><small>{phase === 3 ? "primary" : "candidate"}</small>
        </div>
      </div>
      <div className="beam-takeover">
        <div className="signal-line line-a" style={{ opacity: current.a }} />
        <div className="signal-line line-b" style={{ opacity: current.b }} />
        <div className="takeover-hex">
          <span>地固 L1</span><b>U+00125 / V-00037</b><small>NCI · TAC · 7 个 L2 均不变</small>
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
  const [colorMode, setColorMode] = useState<ColorMode>("reuse");
  const [region, setRegion] = useState("全国");
  const [showGuard, setShowGuard] = useState(true);
  const [showLabels, setShowLabels] = useState(true);
  const [selected, setSelected] = useState(() => makeBeamCell(125, -37));
  const [selectedChild, setSelectedChild] = useState(0);
  const [phase, setPhase] = useState(0);
  const [search, setSearch] = useState("");
  const [searchError, setSearchError] = useState("");

  const selectedL2 = L2_POSITIONS[selectedChild];
  const selectedL2Identity = makeL2Identity(selected, selectedL2);
  const selectedL2Reuse = mod(selectedL2.q + 3 * selectedL2.r, 7);
  const activeMetric = view === "national" ? "全国目录" : view === "cluster" ? "两级结构" : "多星接管";

  const submitSearch = (event: FormEvent) => {
    event.preventDefault();
    const match = search.trim().match(/^CN-G01-L1-U([+-]\d+)-V([+-]\d+)$/i);
    if (!match) {
      setSearchError("请输入完整 L1 ID，例如 U+00125-V-00037");
      return;
    }
    setSelected(makeBeamCell(Number(match[1]), Number(match[2])));
    setSearchError("");
    setView("cluster");
  };

  const metrics = useMemo(() => [
    { label: "规划 L1", value: "≈ 2,620", note: "信令跳变单元" },
    { label: "规划 L2", value: "≈ 18,340", note: "数字业务位置" },
    { label: "Tracking Area", value: "≈ 138", note: "约 19 个 L1 / TA" },
    { label: "当前视图", value: activeMetric, note: "CN-G01 · v0.3" },
  ], [activeMetric]);

  return (
    <main className="planner-shell">
      <header className="topbar">
        <div className="brand-block">
          <div className="brand-mark">CN</div>
          <div><p>NTN BEAM PLANNING</p><h1>星地波位规划台</h1></div>
        </div>
        <nav className="view-tabs" aria-label="主视图">
          <button type="button" className={view === "national" ? "active" : ""} onClick={() => setView("national")} aria-pressed={view === "national"}>全国目录</button>
          <button type="button" className={view === "cluster" ? "active" : ""} onClick={() => setView("cluster")} aria-pressed={view === "cluster"}>L1 / L2 编排</button>
          <button type="button" className={view === "handover" ? "active" : ""} onClick={() => setView("handover")} aria-pressed={view === "handover"}>多星接管</button>
        </nav>
        <div className="version-chip"><span />规划草案 v0.3</div>
      </header>

      <section className="metric-strip" aria-label="方案规模">
        {metrics.map((metric) => <article key={metric.label}><span>{metric.label}</span><strong>{metric.value}</strong><small>{metric.note}</small></article>)}
      </section>

      <div className="workspace-grid">
        <aside className="control-panel" aria-label="规划控制">
          <div className="panel-heading"><span>01</span><div><p>PLANNING SCOPE</p><h2>规划范围</h2></div></div>
          <div className="region-list" role="group" aria-label="区域筛选">
            {REGION_OPTIONS.map((item) => <button type="button" key={item} className={region === item ? "active" : ""} onClick={() => setRegion(item)} aria-pressed={region === item}><span>{item}</span><b>{item === "全国" ? "2,620" : item === "华东" ? "684" : item === "西部" ? "892" : item === "华北" ? "536" : "508"}</b></button>)}
          </div>

          <div className="control-section">
            <label className="section-label">着色方式</label>
            <div className="segmented-control" role="group" aria-label="着色方式">
              <button type="button" className={colorMode === "service" ? "active" : ""} onClick={() => setColorMode("service")} aria-pressed={colorMode === "service"}>状态</button>
              <button type="button" className={colorMode === "reuse" ? "active" : ""} onClick={() => setColorMode("reuse")} aria-pressed={colorMode === "reuse"}>复用</button>
              <button type="button" className={colorMode === "ta" ? "active" : ""} onClick={() => setColorMode("ta")} aria-pressed={colorMode === "ta"}>TA</button>
            </div>
            {colorMode === "reuse" && <div className="reuse-legend" aria-label="复用色图例">{REUSE_COLORS.map((color, index) => <span key={color} style={{ background: color }} title={`复用色 ${index}`}>{index}</span>)}</div>}
          </div>

          <div className="control-section toggles">
            <label><span><b>完整 guard ring</b><small>边界外保留一圈 L1</small></span><input type="checkbox" checked={showGuard} onChange={(event) => setShowGuard(event.target.checked)} /></label>
            <label><span><b>区域标签</b><small>只影响显示，不改变目录</small></span><input type="checkbox" checked={showLabels} onChange={(event) => setShowLabels(event.target.checked)} /></label>
          </div>

          <form className="beam-search" onSubmit={submitSearch}>
            <label htmlFor="beam-id">定位一级波位</label>
            <div><input id="beam-id" value={search} onChange={(event) => setSearch(event.target.value)} placeholder="CN-G01-L1-U+00125-V-00037" /><button type="submit">定位</button></div>
            {searchError && <p role="alert">{searchError}</p>}
          </form>

          <div className="projection-card"><p>投影基线</p><b>Lambert Conformal Conic</b><span>105°E · 35°N · GRS80</span><small>标准纬线 23.5°N / 49.25°N</small></div>
        </aside>

        <section className="stage-panel" aria-label="波位画布">
          <div className="stage-header">
            <div><p>{view === "national" ? "NATIONAL CATALOG" : view === "cluster" ? "LOCAL HIERARCHY" : "RUNTIME ASSIGNMENT"}</p><h2>{view === "national" ? "全国地固波位目录" : view === "cluster" ? "一级与二级波位编排" : "卫星接管不改变波位身份"}</h2></div>
            <div className="stage-status"><span className="pulse" />{view === "handover" ? TAKEOVER_PHASES[phase].name : `${region} · ${showGuard ? "含 guard" : "仅 service"}`}</div>
          </div>
          <div className={`visual-stage view-${view}`}>
            {view === "national" && <NationalCanvas mode={colorMode} region={region} showGuard={showGuard} showLabels={showLabels} selected={selected} onSelect={setSelected} />}
            {view === "cluster" && <ClusterView parent={selected} selectedChild={selectedChild} onSelect={setSelectedChild} />}
            {view === "handover" && <HandoverView phase={phase} onPhase={setPhase} />}
          </div>
          <div className="stage-footer">
            <span><b>固定几何</b>波位 ID 与卫星无关</span><span><b>完整父子关系</b>每个 L1 固定 7 个 L2</span><span><b>运行态分离</b>assignment 单独管理</span>
          </div>
        </section>

        <aside className="inspector-panel" aria-label="波位详情">
          <div className="panel-heading"><span>02</span><div><p>SELECTION</p><h2>当前选择</h2></div></div>
          <div className="selection-id"><span>L1 · {selected.status.toUpperCase()}</span><h3>{selected.id}</h3><button type="button" onClick={() => setView("cluster")}>展开 7 个 L2 →</button></div>
          <dl className="detail-grid">
            <div><dt>NCI</dt><dd>{selected.nci}</dd></div><div><dt>TAC</dt><dd>{selected.tac}</dd></div>
            <div><dt>轴坐标</dt><dd>U {signed(selected.u)} / V {signed(selected.v)}</dd></div><div><dt>L1 复用色</dt><dd><i style={{ background: REUSE_COLORS[selected.reuse] }} />R{selected.reuse}</dd></div>
            <div><dt>服务区域</dt><dd>{selected.region}</dd></div><div><dt>运行卫星</dt><dd>{selected.satellite}</dd></div>
          </dl>

          <div className="l2-inspector">
            <div><span>正式二级波位 ID · {selectedL2.id} 为局部槽位</span><b>{selectedL2Identity.id}</b></div>
            <div className="mini-hex" style={{ "--hex-color": REUSE_COLORS[selectedL2Reuse] } as CSSProperties}>{selectedL2.id}</div>
            <dl><div><dt>全局轴坐标</dt><dd>Q {signed(selectedL2Identity.q)} / R {signed(selectedL2Identity.r)}</dd></div><div><dt>名义半径</dt><dd>15 km</dd></div><div><dt>复用色</dt><dd>R{selectedL2Reuse}</dd></div><div><dt>服务资源</dt><dd>PDU / DRB · SR/SRS</dd></div></dl>
          </div>

          <div className="invariant-box"><p>当前不变量</p><ul><li>小区身份不绑定 satellite_id</li><li>同 L1 的 L2 切换不触发 HO</li><li>DU applied feedback 后才视为 ready</li></ul></div>
        </aside>
      </div>

      <section className="decision-strip">
        <div><span>DECISION 01</span><b>全国一张连续网格</b><p>省界和运营区只作为属性，不切割波位几何。</p></div>
        <div><span>DECISION 02</span><b>L1 是信令服务小区</b><p>NCI、TAC、SIB19、PRACH 与 Paging 在这里稳定。</p></div>
        <div><span>DECISION 03</span><b>L2 是业务跳变位置</b><p>服务 PDU/DRB、QoS 与 SR/SRS，按需求激活。</p></div>
        <div><span>DECISION 04</span><b>多星只改变执行者</b><p>接管更新 assignment，不重编号、不改变邻接。</p></div>
      </section>
      <footer><span>CN-G01 · CHINA NATIONAL NTN GRID</span><p>规划值用于方案推演，不是协议常量；正式 service mask 需接入运营边界数据。</p></footer>
    </main>
  );
}
