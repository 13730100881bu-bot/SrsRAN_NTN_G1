"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import baselineJson from "./orbit-baseline.json";
import constellationAuditJson from "./global-constellation-audit.json";
import smallerScreenJson from "./global-constellation-smaller-screen.json";
import { GlobalCoverageMap, type GlobalMapCell } from "./global-map";
import { GlobalOrbitView } from "./global-orbit-view";
import { parseGlobalSearchTarget } from "./global-search";
import { BASELINE_L1_CAPACITY } from "./beam-hopping-model";
import { publicPath } from "./public-path";
import { baselineSatelliteCellPlanningContext } from "./satellite-cell-model";
import {
  EARTH_RADIUS_KM,
  coverageRadiusKm,
  elevationDeg,
  propagateSatellite,
  satellites,
} from "./orbit-model";

type View = "coverage" | "orbit" | "access" | "audit";
type VisibleCell = GlobalMapCell & {
  distanceKm: number;
  cellBank?: 0 | 1;
  elevationDeg?: number;
  visibleFromSeconds?: number | null;
  visibleUntilSeconds?: number | null;
  elevationTimeline?: readonly { offsetSeconds: number; elevationDeg: number }[];
  tableVersion?: string;
};
type CatalogMetadata = {
  version?: string;
  contentHash?: string;
  source?: string;
  l1Count?: number;
  l2Count?: number;
  counts?: { l1?: number; l2?: number; fullL1?: number; edgeL1?: number };
  integrity?: { sha256?: string };
  generation?: { equalAreaGrid?: boolean; maximumRelativeAreaDeviationPct?: number };
  [key: string]: unknown;
};
type WorkerAnalysis = {
  cells: readonly VisibleCell[];
  visibleCount: number;
};
type AssignmentSnapshot = {
  ready: boolean;
  unassignedCount: number;
  peakAssigned: number;
  peakAssignedSatelliteId: string;
  selectedAssignedCount: number;
  selectedAssignedByCell: readonly [number, number];
  selectedAssignmentBanks: readonly { id: string; cellBank: 0 | 1 }[];
};
type AuditDocument = {
  exactAudit?: { status?: string; durationDays?: number; method?: string; reportPath?: string | null };
};
type OrbitBaseline = {
  shell: string;
  walker: string;
  altitudeKm: number;
  inclinationDeg: number;
  planes: number;
  satellitesPerPlane: number;
  totalSatellites: number;
  phaseFactor: number;
  thresholdsDeg: { hard: number; release: number };
  epoch: string;
};

const baseline = baselineJson as OrbitBaseline;
const audit = constellationAuditJson as AuditDocument;
const smallerScreen = smallerScreenJson as {
  exact: false;
  engineeringDecision: {
    status: "final_design_adopted";
    adoptedSatelliteCount: number;
    acceptancePending: true;
  };
  scenario: {
    altitudeKm: number;
    inclinationDeg: number;
    planes: number;
    satellitesPerPlane: number;
    satelliteCount: number;
  };
  sampling: { epochCount: number; stepSeconds: number };
  summary: {
    coveragePassed: true;
    assignmentPassed: true;
    maximumUncoveredL1: number;
    minimumCandidateCount: number;
    maximumVisibleL1PerSatellite: number;
    maximumEntryVisibleL1PerSatellite: number;
    maximumAssignedL1PerSatellite: number;
    maximumBalancedCellLoad: number;
    scheduleOverflowEpochs: number;
    assignmentCheckedEpochs: number;
  };
};
const GLOBAL_CATALOG_URL = publicPath("/data/global-land-l1-v1.json");
const LAND_TOPOLOGY_URL = publicPath("/data/land-50m.json");
const SATELLITE_CAPACITY = 256;
const CELL_CAPACITY = 128;
const ANALOG_PER_SATELLITE = 32;
const ANALOG_PATTERN = ["11 DL / 5 UL", "11 DL / 5 UL", "10 DL / 6 UL"] as const;
const TIME_SPEEDS = [1, 60, 600] as const;
const VIEW_COPY: Record<View, { nav: string }> = {
  audit: { nav: "方案结论" },
  coverage: { nav: "全球覆盖" },
  orbit: { nav: "卫星负载" },
  access: { nav: "跳波束日历" },
};

const formatClock = (seconds: number) => {
  const value = ((Math.floor(seconds) % 86_400) + 86_400) % 86_400;
  const hour = Math.floor(value / 3600);
  const minute = Math.floor(value % 3600 / 60);
  const second = value % 60;
  return `${String(hour).padStart(2, "0")}:${String(minute).padStart(2, "0")}:${String(second).padStart(2, "0")}Z`;
};

function enabledChildren(mask: GlobalMapCell["childMask"]) {
  if (mask === undefined) return 7;
  if (Array.isArray(mask)) return mask.filter(Boolean).length;
  if (typeof mask === "number") {
    let value = mask;
    let count = 0;
    while (value > 0) { count += value & 1; value >>= 1; }
    return count;
  }
  return [...mask].filter((value) => value === "1").length;
}

function phaseLabel(windowIndex: number, bank: 0 | 1) {
  return ANALOG_PATTERN[(bank + windowIndex * 2) % ANALOG_PATTERN.length];
}

function calendarWindows(cells: readonly VisibleCell[], bank: 0 | 1) {
  const members = cells.filter((cell) => cell.cellBank === bank);
  return Array.from({ length: 4 }, (_, windowIndex) => {
    const assigned = members.filter((_, index) => index % 4 === windowIndex);
    return { startMs: windowIndex * 20, assigned, phase: phaseLabel(windowIndex, bank) };
  });
}

export function GlobalPlanner() {
  const [view, setView] = useState<View>("audit");
  const [timeSeconds, setTimeSeconds] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState<(typeof TIME_SPEEDS)[number]>(60);
  // The adjusted plan's t=0 peak-load satellite and its nearest catalog position keep the
  // initial coverage and calendar views representative of the audited case.
  const [selectedSatelliteId, setSelectedSatelliteId] = useState("P02-S04");
  const [selectedCellId, setSelectedCellId] = useState("G021777");
  const [search, setSearch] = useState("");
  const [searchFeedback, setSearchFeedback] = useState<{ tone: "success" | "error" | "info"; message: string } | null>(null);
  const [orbitFocusRequestId, setOrbitFocusRequestId] = useState(0);
  const [catalog, setCatalog] = useState<readonly GlobalMapCell[]>([]);
  const [metadata, setMetadata] = useState<CatalogMetadata>({});
  const [catalogError, setCatalogError] = useState("");
  const [analysis, setAnalysis] = useState<WorkerAnalysis>({ cells: [], visibleCount: 0 });
  const [assignmentSnapshot, setAssignmentSnapshot] = useState<AssignmentSnapshot>({
    ready: false,
    unassignedCount: 0,
    peakAssigned: 0,
    peakAssignedSatelliteId: "",
    selectedAssignedCount: 0,
    selectedAssignedByCell: [0, 0],
    selectedAssignmentBanks: [],
  });
  const [candidateCounts, setCandidateCounts] = useState<readonly number[]>([]);
  const [uncoveredCount, setUncoveredCount] = useState(0);
  const [fleetSnapshot, setFleetSnapshot] = useState({ peakVisible: 0, peakSatelliteId: "" });
  const [coverageEpoch, setCoverageEpoch] = useState(0);
  const workerRef = useRef<Worker | null>(null);
  const requestRef = useRef(0);
  const globalRequestRef = useRef(0);
  const latestTimeRef = useRef(0);

  useEffect(() => {
    if (!playing) return undefined;
    const timer = window.setInterval(() => setTimeSeconds((value) => (value + speed / 10) % 86_400), 100);
    return () => window.clearInterval(timer);
  }, [playing, speed]);

  useEffect(() => { latestTimeRef.current = timeSeconds; }, [timeSeconds]);
  useEffect(() => {
    const timer = window.setInterval(() => setCoverageEpoch(latestTimeRef.current), 1_000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    const worker = new Worker(new URL("./global-catalog.worker.ts", import.meta.url), { type: "module" });
    workerRef.current = worker;
    worker.onmessage = (event: MessageEvent<Record<string, unknown>>) => {
      const message = event.data;
      if (message.type === "loaded") {
        const cells = message.cells as readonly GlobalMapCell[];
        setCatalog(cells);
        setMetadata(message.metadata as CatalogMetadata);
        if (cells.length > 0) setSelectedCellId((current) => cells.some(({ id }) => id === current) ? current : cells[0].id);
      } else if (message.type === "analysis" && Number(message.requestId) === requestRef.current) {
        setAnalysis({
          cells: message.cells as readonly VisibleCell[],
          visibleCount: Number(message.visibleCount),
        });
      } else if (message.type === "globalCoverage" && Number(message.requestId) === globalRequestRef.current) {
        setCandidateCounts(message.counts as readonly number[]);
        setUncoveredCount(Number(message.uncovered));
        setFleetSnapshot({
          peakVisible: Number(message.peakVisible),
          peakSatelliteId: String(message.peakSatelliteId),
        });
        setAssignmentSnapshot({
          ready: true,
          unassignedCount: Number(message.unassignedCount),
          peakAssigned: Number(message.peakAssigned),
          peakAssignedSatelliteId: String(message.peakAssignedSatelliteId),
          selectedAssignedCount: Number(message.selectedAssignedCount),
          selectedAssignedByCell: message.selectedAssignedByCell as readonly [number, number],
          selectedAssignmentBanks: message.selectedAssignmentBanks as readonly { id: string; cellBank: 0 | 1 }[],
        });
      } else if (message.type === "error") {
        setCatalogError(String(message.message));
      }
    };
    worker.postMessage({ type: "load", url: GLOBAL_CATALOG_URL });
    return () => { worker.terminate(); workerRef.current = null; };
  }, []);

  const selectedDefinition = useMemo(
    () => satellites.find(({ id }) => id === selectedSatelliteId) ?? satellites[0],
    [selectedSatelliteId],
  );
  const selectedSatellite = useMemo(
    () => propagateSatellite(selectedDefinition, timeSeconds),
    [selectedDefinition, timeSeconds],
  );
  const analysisSatellite = useMemo(
    () => propagateSatellite(selectedDefinition, coverageEpoch),
    [coverageEpoch, selectedDefinition],
  );
  const entryRadiusKm = coverageRadiusKm(baseline.thresholdsDeg.hard);
  const holdRadiusKm = coverageRadiusKm(baseline.thresholdsDeg.release);

  useEffect(() => {
    if (!workerRef.current || catalog.length === 0) return;
    requestRef.current += 1;
    workerRef.current.postMessage({
      type: "analyze",
      requestId: requestRef.current,
      satelliteId: analysisSatellite.id,
      latitudeDeg: analysisSatellite.lat,
      longitudeDeg: analysisSatellite.lon,
      radiusKm: holdRadiusKm,
      timeline: Array.from({ length: 181 }, (_, index) => {
        const offsetSeconds = (index - 90) * 10;
        const state = propagateSatellite(selectedDefinition, coverageEpoch + offsetSeconds);
        return { offsetSeconds, lat: state.lat, lon: state.lon };
      }),
    });
  }, [analysisSatellite.id, analysisSatellite.lat, analysisSatellite.lon, catalog.length, coverageEpoch, holdRadiusKm, selectedDefinition]);
  useEffect(() => {
    if (!workerRef.current || catalog.length === 0) return;
    globalRequestRef.current += 1;
    workerRef.current.postMessage({
      type: "globalCoverage",
      requestId: globalRequestRef.current,
      entryRadiusKm,
      releaseRadiusKm: holdRadiusKm,
      selectedSatelliteId,
      satelliteSubpoints: satellites.map((definition) => {
        const state = propagateSatellite(definition, coverageEpoch);
        return { id: state.id, lat: state.lat, lon: state.lon };
      }),
    });
  }, [catalog.length, coverageEpoch, entryRadiusKm, holdRadiusKm, selectedSatelliteId]);

  const selectedCell = useMemo(
    () => catalog.find(({ id }) => id === selectedCellId) ?? catalog[0],
    [catalog, selectedCellId],
  );
  const candidateStates = useMemo(() => {
    if (!selectedCell) return [];
    return satellites
      .map((definition) => {
        const state = propagateSatellite(definition, coverageEpoch);
        return { ...state, elevation: elevationDeg(state, { lat: selectedCell.lat, lon: selectedCell.lon }) };
      })
      .filter(({ elevation }) => elevation >= baseline.thresholdsDeg.release)
      .sort((left, right) => right.elevation - left.elevation || left.id.localeCompare(right.id));
  }, [coverageEpoch, selectedCell]);
  const entryCandidates = candidateStates.filter(({ elevation }) => elevation >= baseline.thresholdsDeg.hard);
  const selectedIsVisible = analysis.cells.some(({ id }) => id === selectedCellId);
  const selectedCellIndex = selectedCell ? catalog.indexOf(selectedCell) : -1;
  const selectedGlobalCandidateCount = selectedCellIndex >= 0 ? candidateCounts[selectedCellIndex] ?? 0 : 0;

  const chooseSatellite = useCallback((id: string) => setSelectedSatelliteId(id), []);
  const selectBestCandidate = () => {
    const best = entryCandidates[0];
    if (best) setSelectedSatelliteId(best.id);
  };
  const runSearch = () => {
    const target = parseGlobalSearchTarget(search);
    if (!target) {
      setSearchFeedback({ tone: "error", message: "请输入卫星编号（如 P35-S34）或地面区域编号（如 G021777）。" });
      return;
    }

    if (target.kind === "satellite") {
      const satellite = satellites.find(({ id }) => id === target.id);
      if (!satellite) {
        setSearchFeedback({ tone: "error", message: `未找到卫星 ${target.id}，请检查编号。` });
        return;
      }
      setSearch(satellite.id);
      setSelectedSatelliteId(satellite.id);
      setView("orbit");
      setOrbitFocusRequestId((value) => value + 1);
      setSearchFeedback({ tone: "success", message: `已找到 ${satellite.id}，并将它移到视图中央。` });
      return;
    }

    if (catalog.length === 0) {
      setSearchFeedback({ tone: "info", message: "地面区域目录正在载入，请稍后再试。" });
      return;
    }
    const cell = catalog.find(({ id }) => id === target.id);
    if (!cell) {
      setSearchFeedback({ tone: "error", message: `未找到地面区域 ${target.id}，请检查编号。` });
      return;
    }
    setSearch(cell.id);
    setSelectedCellId(cell.id);
    setView("coverage");
    setSearchFeedback({ tone: "success", message: `已找到 ${cell.id}，右侧显示该区域的详细信息。` });
  };
  const assignmentBankById = useMemo(
    () => new Map(assignmentSnapshot.selectedAssignmentBanks.map(({ id, cellBank }) => [id, cellBank])),
    [assignmentSnapshot.selectedAssignmentBanks],
  );
  const assignedCells = useMemo(
    () => analysis.cells
      .filter(({ id }) => assignmentBankById.has(id))
      .map((cell) => ({ ...cell, cellBank: assignmentBankById.get(cell.id)! })),
    [analysis.cells, assignmentBankById],
  );
  const windowsA = calendarWindows(assignedCells, 0);
  const windowsB = calendarWindows(assignedCells, 1);
  const catalogReady = catalog.length > 0;
  const assignmentReady = assignmentSnapshot.ready;
  const capacityDecision = !assignmentReady
    ? "全网唯一分配正在计算"
    : assignmentSnapshot.unassignedCount > 0
    ? `${assignmentSnapshot.unassignedCount}个区域尚未分配`
    : `当前实际负责${assignmentSnapshot.selectedAssignedCount}个L1`;
  const currentJudgement = !catalogReady
    ? "正在载入全球波位目录"
    : view === "coverage"
      ? entryCandidates.length > 0 ? `可接入，${entryCandidates.length}颗候选卫星` : "当前无可接入卫星"
      : capacityDecision;
  const bestEntryCandidate = entryCandidates[0];
  const largestOnboardCell = Math.max(...assignmentSnapshot.selectedAssignedByCell, 0);
  const accessCapacityReady = catalogReady
    && assignmentReady
    && assignmentSnapshot.unassignedCount === 0
    && largestOnboardCell <= CELL_CAPACITY;
  const ssbOpportunityHeadroom = BASELINE_L1_CAPACITY.guaranteedDownlinkVisits - CELL_CAPACITY;

  return (
    <main className="global-shell">
      <header className="global-header">
        <div className="global-brand"><span>NTN</span><div><p>一期工程评估 · 500 km LEO · 57°S～57°N陆地</p><h1>NTN 全球陆地接入方案</h1></div></div>
        <nav aria-label="主视图">
          {(["audit", "coverage", "orbit", "access"] as const).map((key) => (
            <button type="button" key={key} className={view === key ? "active" : ""} onClick={() => setView(key)} aria-pressed={view === key}>{VIEW_COPY[key].nav}</button>
          ))}
        </nav>
        <div className="audit-chip decision"><i />最终采用：2,990颗</div>
      </header>

      {view !== "access" ? <section className="global-metrics" aria-label="工程结论指标">
        <article className="metric-recommended"><span>最终采用方案</span><strong>{smallerScreen.scenario.planes}轨道面 · 每面{smallerScreen.scenario.satellitesPerPlane}星</strong><small>共{smallerScreen.scenario.satelliteCount.toLocaleString("en-US")}颗 · 服务±57°全球陆地</small></article>
        <article><span>设计依据检查时刻</span><strong>{smallerScreen.sampling.epochCount} / {smallerScreen.sampling.epochCount}</strong><small>一天内每2分钟检查一次 · 未发现采样时刻覆盖空窗</small></article>
        <article><span>单星可见区域峰值</span><strong>{smallerScreen.summary.maximumVisibleL1PerSatellite}</strong><small>表示卫星能看到多少区域，不是实际服务负载</small></article>
        <article><span>采用方案实际负责峰值</span><strong>{smallerScreen.summary.maximumAssignedL1PerSatellite} / {SATELLITE_CAPACITY}</strong><small>{smallerScreen.summary.assignmentCheckedEpochs}个检查时刻均完成唯一分配 · 单小区峰值{smallerScreen.summary.maximumBalancedCellLoad}</small></article>
        <article className="metric-review"><span>部署前连续服务验收</span><strong>继续执行</strong><small>连续检查{audit.exactAudit?.durationDays ?? 7}天，并完成故障与真实无线验证</small></article>
      </section> : null}

      {view !== "audit" ? <section className="global-toolbar" aria-label="时间与查询控制">
        <button type="button" className="play-button" onClick={() => { if (playing) setCoverageEpoch(timeSeconds); setPlaying((value) => !value); }}>{playing ? "暂停时间" : "播放时间"}</button>
        <label className="time-range"><span>动画时刻 {formatClock(timeSeconds)}</span><input type="range" min="0" max="86399" step="1" value={Math.floor(timeSeconds)} onChange={(event) => { const value = Number(event.target.value); setTimeSeconds(value); setCoverageEpoch(value); }} /></label>
        <div className="speed-control" aria-label="播放速度">{TIME_SPEEDS.map((value) => <button type="button" key={value} className={speed === value ? "active" : ""} onClick={() => setSpeed(value)}>{value}×</button>)}</div>
        <form className="location-search" onSubmit={(event) => { event.preventDefault(); runSearch(); }}>
          <label htmlFor="global-search">查找卫星或地面区域</label>
          <div>
            <input
              id="global-search"
              value={search}
              onChange={(event) => { setSearch(event.target.value); setSearchFeedback(null); }}
              placeholder="例如 P35-S34 或 G021777"
              autoComplete="off"
              spellCheck={false}
              aria-describedby="global-search-feedback"
              aria-invalid={searchFeedback?.tone === "error" ? "true" : undefined}
            />
            <button type="submit" disabled={!search.trim()}>查找</button>
          </div>
          <p id="global-search-feedback" className={`location-search-feedback ${searchFeedback?.tone ?? ""}`} aria-live="polite">
            {searchFeedback?.message ?? "可省略前导零，也可以按 Enter 查找。"}
          </p>
        </form>
      </section> : null}

      {view !== "audit" && view !== "access" ? <section className="runtime-context" aria-label="当前运行快照">
        <span>当前方案运行快照</span>
        <dl>
          <div><dt>动画 / 覆盖快照</dt><dd>{formatClock(timeSeconds)} / {formatClock(coverageEpoch)}</dd></div>
          <div><dt>一级波位</dt><dd>{selectedCell?.id ?? selectedCellId}</dd></div>
          <div><dt>所选卫星</dt><dd>{selectedSatellite.id}</dd></div>
          <div><dt>卫星可见L1</dt><dd>{analysis.visibleCount}</dd></div>
          <div><dt>卫星实际负责L1</dt><dd>{assignmentReady ? `${assignmentSnapshot.selectedAssignedCount} / ${SATELLITE_CAPACITY}` : "计算中"}</dd></div>
          <div className={currentJudgement.includes("超出") || currentJudgement.includes("无可接入") ? "is-failure" : "is-pass"}><dt>当前结论</dt><dd>{currentJudgement}</dd></div>
        </dl>
      </section> : null}

      {view === "coverage" ? (
        <section className="global-workspace coverage-workspace">
          <article className="global-stage">
            <header><div><p>45°接入 · 42°保持</p><h2>全球陆地覆盖状态</h2></div><span>{catalog.length.toLocaleString("en-US")}个L1 · {Number(metadata.counts?.l2 ?? 0).toLocaleString("en-US")}个L2 · {String(metadata.version ?? "载入中")}</span></header>
            <GlobalCoverageMap
              cells={catalog}
              visibleCells={analysis.cells}
              candidateCounts={candidateCounts}
              selectedCellId={selectedCellId}
              satellite={analysisSatellite}
              entryAngularRadiusDeg={entryRadiusKm / EARTH_RADIUS_KM * 180 / Math.PI}
              holdAngularRadiusDeg={holdRadiusKm / EARTH_RADIUS_KM * 180 / Math.PI}
              landUrl={LAND_TOPOLOGY_URL}
              onSelectCell={setSelectedCellId}
            />
          </article>
          <aside className="global-inspector">
            <header><p>已选一级波位（L1）</p><h2>{selectedCell?.id ?? "目录载入中"}</h2><span className={`position-status ${entryCandidates.length > 0 ? "is-pass" : "is-failure"}`}>{entryCandidates.length > 0 ? "当前可接入" : "当前不可接入"}</span></header>
            {selectedCell ? <dl><div><dt>地面坐标</dt><dd>{selectedCell.lat.toFixed(4)}°, {selectedCell.lon.toFixed(4)}°</dd></div><div><dt>有效二级波位（L2）</dt><dd>{enabledChildren(selectedCell.childMask)} / 7</dd></div><div><dt>45°以上候选卫星</dt><dd>{selectedGlobalCandidateCount}颗</dd></div><div><dt>最佳候选</dt><dd>{bestEntryCandidate ? `${bestEntryCandidate.id} · ${bestEntryCandidate.elevation.toFixed(1)}°` : "无"}</dd></div><div><dt>所选卫星</dt><dd>{selectedIsVisible ? "可覆盖该波位" : "未达到45°"}</dd></div><div><dt>当前全局空窗</dt><dd className={uncoveredCount > 0 ? "is-failure" : ""}>{uncoveredCount}个L1</dd></div></dl> : null}
            <button type="button" className="candidate-button" disabled={entryCandidates.length === 0} onClick={selectBestCandidate}>选用仰角最高的候选卫星</button>
            <section className="candidate-list">
              <h3>候选卫星（按仰角列前8颗）</h3>
              {candidateStates.slice(0, 8).map((item) => <button type="button" key={item.id} onClick={() => setSelectedSatelliteId(item.id)} className={item.id === selectedSatelliteId ? "active" : ""}><b>{item.id}</b><span>{item.elevation.toFixed(1)}°</span><small>{item.elevation >= 45 ? "可新接入（≥45°）" : "仅可保持已有服务（42°～45°）"}</small></button>)}
              {entryCandidates.length === 0 ? <p className="failure-label">当前没有达到45°的可接入卫星</p> : null}
            </section>
            <details className="candidate-inventory"><summary>全部{candidateStates.length}颗候选（≥42°）</summary><div>{candidateStates.map((item) => <button type="button" key={`all-${item.id}`} onClick={() => setSelectedSatelliteId(item.id)}><span>{item.id}</span><b>{item.elevation.toFixed(1)}°</b></button>)}</div></details>
            {catalogError ? <p className="failure-label">目录加载失败：{catalogError}</p> : null}
          </aside>
        </section>
      ) : null}

      {view === "orbit" ? (
        <section className="global-workspace orbit-workspace">
          <article className="global-stage">
            <header><div><p>轨道运行演示</p><h2>星座运行与单星负载</h2></div><span>交互查看卫星轨迹、区域可见关系和单星负载</span></header>
            <GlobalOrbitView
              timeSeconds={timeSeconds}
              selectedSatelliteId={selectedSatelliteId}
              focusRequestId={orbitFocusRequestId}
              onSelectSatellite={chooseSatellite}
            />
          </article>
          <aside className="global-inspector">
            <header><p>已选卫星</p><h2>{selectedSatellite.id}</h2><span className={`position-status ${largestOnboardCell > CELL_CAPACITY ? "is-failure" : "is-pass"}`}>{largestOnboardCell > CELL_CAPACITY ? "分配超限" : "分配正常"}</span></header>
            <dl>
              <div><dt>轨道面 / 槽位</dt><dd>P{String(selectedSatellite.plane).padStart(2, "0")} / S{String(selectedSatellite.slot).padStart(2, "0")}</dd></div>
              <div><dt>星下点</dt><dd>{selectedSatellite.lat.toFixed(2)}°, {selectedSatellite.lon.toFixed(2)}°</dd></div>
              <div><dt>快照时刻</dt><dd>{formatClock(coverageEpoch)}</dd></div>
              <div><dt>当前能看到的L1</dt><dd>{analysis.visibleCount}个</dd></div>
              <div><dt>当前实际负责的L1</dt><dd>{assignmentReady ? `${assignmentSnapshot.selectedAssignedCount} / ${SATELLITE_CAPACITY}` : "计算中"}</dd></div>
              <div><dt>容量余量</dt><dd>{assignmentReady ? `${SATELLITE_CAPACITY - assignmentSnapshot.selectedAssignedCount}个L1` : "计算中"}</dd></div>
              <div><dt>两个星载小区</dt><dd>{assignmentReady ? `${assignmentSnapshot.selectedAssignedByCell[0]} / ${assignmentSnapshot.selectedAssignedByCell[1]}` : "计算中"}</dd></div>
              <div><dt>全网实际负责峰值</dt><dd>{assignmentReady ? `${assignmentSnapshot.peakAssigned} · ${assignmentSnapshot.peakAssignedSatelliteId}` : "计算中"}</dd></div>
              <div><dt>单星可见峰值</dt><dd>{fleetSnapshot.peakVisible} · {fleetSnapshot.peakSatelliteId || "计算中"}</dd></div>
              <div><dt>当前未分配L1</dt><dd className={assignmentReady && assignmentSnapshot.unassignedCount > 0 ? "is-failure" : ""}>{assignmentReady ? `${assignmentSnapshot.unassignedCount}个` : "计算中"}</dd></div>
            </dl>
            <details className="technical-details"><summary>计算边界</summary><p>“可见”表示卫星在几何上能看到该区域；“实际负责”表示完成全网唯一分配后交给这颗卫星的区域。容量只检查实际负责的区域。轨道模型仍需用真实星历、地面站、功率和干扰条件复核。</p></details>
          </aside>
        </section>
      ) : null}

      {view === "access" ? (
        <section className="access-console">
          <header className="access-summary">
            <div>
              <p>跳波束日历 · {selectedSatellite.id} · {formatClock(coverageEpoch)} 快照</p>
              <h2>这颗卫星如何为实际负责的地面区域安排接入时段？</h2>
            </div>
            <span className={!catalogReady || !assignmentReady ? "pending" : accessCapacityReady ? "scheduled" : "failed"}>
              {!catalogReady || !assignmentReady ? "正在计算" : accessCapacityReady ? "本次安排可行" : "需要调整卫星"}
            </span>
          </header>

          <section className="access-guide" aria-labelledby="access-guide-title">
            <div>
              <span>先看这里</span>
              <h3 id="access-guide-title">日历就是卫星轮流照向不同地面区域的时间安排</h3>
              <p>卫星先保存完整的可见区域清单；全网完成唯一分配后，只有这颗卫星实际负责的区域才进入两个星载小区和接入日历。</p>
              <p><b>当前边界：</b>页面已经能分开计算这两张清单，但基站程序目前只能接收一张；所以下面的日历仍是规划演示，不能直接下发运行。</p>
            </div>
            <ol className="access-journey">
              <li><b>1</b><span>保留可见清单</span><small>当前能看到{analysis.visibleCount}个一级波位</small></li>
              <li><b>2</b><span>确认实际责任</span><small>本星负责{assignmentSnapshot.selectedAssignedCount}个一级波位</small></li>
              <li><b>3</b><span>分给两个小区</span><small>{assignmentSnapshot.selectedAssignedByCell[0]}个 / {assignmentSnapshot.selectedAssignedByCell[1]}个</small></li>
              <li><b>4</b><span>安排网络发现</span><small>每个已分配一级波位80 ms内有一次计划机会</small></li>
              <li><b>5</b><span>安排上行接入</span><small>每个已分配一级波位640 ms内有一次PRACH机会</small></li>
            </ol>
          </section>

          <section className="access-answer-grid" aria-label="跳波束日历核心结论">
            <article className={!assignmentReady ? "is-pending" : assignmentSnapshot.unassignedCount > 0 ? "is-failure" : "is-pass"}>
              <span>实际任务是否超过上限</span>
              <b>{assignmentReady ? `${assignmentSnapshot.selectedAssignedCount} / ${SATELLITE_CAPACITY}` : "计算中"}</b>
              <strong>{!assignmentReady ? "正在完成全网唯一分配" : assignmentSnapshot.unassignedCount > 0 ? "全网仍有区域没有负责人" : "实际负责数量在规划范围内"}</strong>
              <p>256是当前软件规划中单星最多负责的一级波位数，不是卫星能形成的波束数量，也不是可见清单的裁剪线。</p>
            </article>
            <article className={largestOnboardCell > CELL_CAPACITY ? "is-failure" : "is-pass"}>
              <span>两个小区是否超限</span>
              <b>{assignmentSnapshot.selectedAssignedByCell[0]} / {assignmentSnapshot.selectedAssignedByCell[1]}</b>
              <strong>{largestOnboardCell > CELL_CAPACITY ? "至少一个小区超限" : "两个小区都在容量内"}</strong>
              <p>每个稳定星载小区在本阶段最多负责128个一级波位；小区身份不会随波位改变。</p>
            </article>
            <article className="is-pass">
              <span>一级波位多久获得一次网络发现机会</span>
              <b>计划间隔≤80 ms</b>
              <strong>下行发现时段可排</strong>
              <p>当前最忙小区安排{largestOnboardCell}项；按满载128项计算仍保留{ssbOpportunityHeadroom}次计划机会。真实信号仍需无线验证。</p>
            </article>
            <article className="is-tight">
              <span>一级波位多久获得一次PRACH机会</span>
              <b>计划间隔≤640 ms</b>
              <strong>按每个已分配一级波位单独安排</strong>
              <p>满载时每小区128个一级波位对应128次机会；PRACH不是按整颗卫星合并计算，真实接收仍需后续无线验证。</p>
            </article>
          </section>

          <section className="access-cell-overview">
            <header>
              <div><span>两个小区如何分工</span><h3>把本星实际负责的一级波位分成两组</h3></div>
              <p>完整可见清单不会被容量裁剪；只有全网唯一分配给本星的一级波位进入日历。小区身份跟随卫星保持稳定。</p>
            </header>
            <div>
              {([0, 1] as const).map((bank) => {
                const cellCount = assignmentSnapshot.selectedAssignedByCell[bank] ?? 0;
                return (
                  <article key={`overview-${bank}`}>
                    <div><span>星载小区 {bank === 0 ? "A" : "B"}</span><b>{cellCount}个地面波位</b></div>
                    <progress max={CELL_CAPACITY} value={cellCount} aria-label={`星载小区${bank === 0 ? "A" : "B"}容量使用情况`} />
                    <small>已用{cellCount} / {CELL_CAPACITY}，还可安排{CELL_CAPACITY - cellCount}个</small>
                  </article>
                );
              })}
            </div>
          </section>

          <details className="access-detail-block">
            <summary><span><b>查看80 ms轮转明细</b><small>展示两个小区在四个20 ms时间段内访问哪些波位</small></span><em>技术明细</em></summary>
            <div className="access-detail-content">
              {([0, 1] as const).map((bank) => {
                const windows = bank === 0 ? windowsA : windowsB;
                const cellCount = assignmentSnapshot.selectedAssignedByCell[bank] ?? 0;
                const identity = baselineSatelliteCellPlanningContext.identitiesBySatellite.get(selectedSatellite.id)?.[bank];
                return (
                  <section className="cell-calendar" key={bank}>
                    <header>
                      <div><p>星载小区 {bank === 0 ? "A" : "B"}</p><h3>{cellCount} / {CELL_CAPACITY}个L1 · 余量{CELL_CAPACITY - cellCount}</h3></div>
                      <div className="cell-calendar-summary"><span>16路模拟 · 64路数字</span><small>NCI {identity?.nci ?? "未分配"} · PCI {identity?.pci ?? "未分配"} · Registry {baselineSatelliteCellPlanningContext.registryVersion}</small></div>
                    </header>
                    <div className="calendar-grid">
                      {windows.map((window) => (
                        <article key={window.startMs}>
                          <span>{window.startMs}–{window.startMs + 20} ms</span>
                          <b>{window.assigned.length}次波位访问</b>
                          <small>{window.phase} · 4×2.5 ms</small>
                          <em>{window.assigned.slice(0, 2).map(({ id }) => id).join(" · ") || "预留窗口"}{window.assigned.length > 2 ? ` · 另有${window.assigned.length - 2}个` : ""}</em>
                        </article>
                      ))}
                    </div>
                  </section>
                );
              })}
            </div>
          </details>

          <details className="access-detail-block">
            <summary><span><b>查看验算表</b><small>用于核对容量、下行发现、上行接入和模拟波束上限</small></span><em>技术明细</em></summary>
            <section className="engineering-results">
              <header><h3>接入日历工程指标</h3><span>当前快照与单测覆盖的时间账本</span></header>
              <div><table><thead><tr><th>指标</th><th>要求</th><th>当前结果</th><th>结论</th></tr></thead><tbody><tr><td>完整可见一级波位清单</td><td>不得按容量裁剪</td><td>{analysis.visibleCount}个全部保留</td><td><span className="result-pass">清单完整</span></td></tr><tr><td>单星实际负责一级波位</td><td>≤{SATELLITE_CAPACITY}</td><td>{assignmentSnapshot.selectedAssignedCount} / {SATELLITE_CAPACITY}</td><td><span className={assignmentSnapshot.unassignedCount > 0 ? "result-failed" : "result-pass"}>{assignmentSnapshot.unassignedCount > 0 ? "存在未分配区域" : "满足"}</span></td></tr><tr><td>两个星载小区</td><td>各≤{CELL_CAPACITY}</td><td>{assignmentSnapshot.selectedAssignedByCell[0]} / {assignmentSnapshot.selectedAssignedByCell[1]}</td><td><span className={largestOnboardCell > CELL_CAPACITY ? "result-failed" : "result-pass"}>{largestOnboardCell > CELL_CAPACITY ? "超限" : "满足"}</span></td></tr><tr><td>每个一级波位的SSB计划间隔</td><td>≤80 ms</td><td>满载128 / {BASELINE_L1_CAPACITY.guaranteedDownlinkVisits}次机会</td><td><span className="result-pass">软件日历可排</span></td></tr><tr><td>每个一级波位的PRACH计划间隔</td><td>≤640 ms</td><td>满载128 / 128次机会</td><td><span className="result-tight">软件日历可排</span></td></tr><tr><td>瞬时模拟波束</td><td>≤{ANALOG_PER_SATELLITE}路</td><td>{ANALOG_PER_SATELLITE}路上限</td><td><span className="result-pass">满足</span></td></tr></tbody></table></div>
            </section>
          </details>

          <details className="access-detail-block visible-table">
            <summary><span><b>查看完整可见波位表</b><small>{selectedSatellite.id}当前共{analysis.visibleCount}个 · {String(metadata.version ?? "载入中")}</small></span><em>原始数据</em></summary>
            <div><table><thead><tr><th>L1</th><th>当前用途</th><th>仰角</th><th>45°可见区间</th><th>仰角时间线</th><th>距星下点</th><th>纬度</th><th>经度</th></tr></thead><tbody>{analysis.cells.map((cell) => { const bank = assignmentBankById.get(cell.id); return <tr key={cell.id}><td>{cell.id}</td><td>{bank === undefined ? "可见候选 · 由其他卫星负责" : `本星负责 · 小区${bank === 0 ? "A" : "B"}`}</td><td>{cell.elevationDeg?.toFixed(1) ?? "—"}°</td><td>{cell.visibleFromSeconds ?? "—"}…{cell.visibleUntilSeconds ?? "—"} s</td><td>{cell.elevationTimeline?.map((sample) => `${sample.offsetSeconds}:${sample.elevationDeg.toFixed(1)}°`).join(" / ") ?? "—"}</td><td>{cell.distanceKm.toFixed(1)} km</td><td>{cell.lat.toFixed(4)}°</td><td>{cell.lon.toFixed(4)}°</td></tr>; })}</tbody></table></div>
          </details>

          <details className="technical-details access-technical"><summary>这项结论还不代表什么</summary><p>当前页面证明的是离线规划能够排出接入日历。完整可见清单和实际负责清单还不能同时送入基站程序；真实广播、终端接入检测、功率、天线和射频切换也仍需在后续无线链路中验证。“已安排”不等于信号已经从天线发出。</p></details>
        </section>
      ) : null}

      {view === "audit" ? (
        <section className="decision-console">
          <header className="decision-hero"><div><span className="decision-label">最终结论</span><h2>最终方案采用46个轨道面、每面65颗，共2,990颗卫星</h2><p>卫星总数确定为2,990颗。一天720个检查时刻均完成覆盖与唯一分配，单星实际负责峰值为87个一级波位。连续服务、单星故障和真实无线测试列入上线前验收。</p></div><div className="decision-stamp"><span>最终采用方案</span><b>2,990颗</b><small>46个轨道面 × 每面65颗</small></div></header>

          <section className="conclusion-table"><header><h3>最终结论与验收边界</h3><span>卫星数量已经确定，上线条件继续验证</span></header><div><table><thead><tr><th>关注事项</th><th>最终结论</th><th>说明</th></tr></thead><tbody><tr><td>采用多少卫星</td><td><span className="result-pass">2,990颗</span></td><td>{smallerScreen.scenario.planes}个轨道面、每面{smallerScreen.scenario.satellitesPerPlane}颗，轨道高度{smallerScreen.scenario.altitudeKm} km，倾角{smallerScreen.scenario.inclinationDeg}°</td></tr><tr><td>设计依据是否满足</td><td><span className="result-pass">720/720满足</span></td><td>一天内{smallerScreen.sampling.epochCount}个检查时刻未发现空缺；最紧张时每个区域至少有{smallerScreen.summary.minimumCandidateCount}颗可用卫星</td></tr><tr><td>“可见数量”是否等于“实际负载”</td><td><span className="result-review">不是同一概念</span></td><td>单星最多可见{smallerScreen.summary.maximumVisibleL1PerSatellite}个一级波位，只表示几何视野；唯一分配后的实际负责峰值为{smallerScreen.summary.maximumAssignedL1PerSatellite}个/星</td></tr><tr><td>是否已经接入基站程序</td><td><span className="result-review">尚未接通</span></td><td>页面能分别保存完整可见清单和实际负责清单；当前基站程序只能接收一张清单，仍需补充最小输入接口</td></tr><tr><td>终端多久能发现网络</td><td><span className="result-pass">可生成80 ms计划</span></td><td>软件日历为每个已分配一级波位安排SSB机会；“已安排”不等于真实无线信号已经发出</td></tr><tr><td>终端多久能获得接入机会</td><td><span className="result-tight">可生成640 ms计划</span></td><td>PRACH按每个已分配一级波位单独安排；满载时余量较小，仍需真实无线实现验证</td></tr><tr><td>上线前还要完成什么</td><td><span className="result-review">三项验收</span></td><td>{audit.exactAudit?.durationDays ?? 7}天连续事件检查、单星故障检查以及真实功率和干扰验证</td></tr></tbody></table></div></section>

          <section className="acceptance-actions"><header><h3>最终方案上线前必须完成三项验收</h3><span>卫星数量已经确定；如验收不满足，再按结果调整设计</span></header><div><article><b>01</b><h4>连续服务检查</h4><p>连续检查7天，确认两个检查时刻之间也不会出现短暂的服务中断。</p></article><article><b>02</b><h4>卫星接续检查</h4><p>确认一颗卫星离开时，下一颗卫星已经准备好接续服务，交接期间不中断。</p></article><article><b>03</b><h4>真实无线环境验证</h4><p>使用真实轨道、信号功率、干扰、地面站和无线设备完成验证。</p></article></div></section>

          <details className="technical-details decision-details"><summary>查看技术依据和使用边界</summary><div className="decision-technical"><p>最终工程方案采用2,990颗卫星。覆盖目录包含±57°陆地的36,411个一级波位，现有依据来自一天、每{smallerScreen.sampling.stepSeconds}秒检查一次的计算。底层记录中的`exact=false`、`selectedScenario=null`用于标记连续覆盖正式验收进度。</p><p>完整可见清单与唯一服务分配是两层数据：前者不能按256裁剪，后者才受单星256、单小区128的当前软件规划上限约束。SSB和PRACH都针对已分配的一级波位生成计划；这些计划不代表PHY、RF、天线或空口已经执行。</p><section className="scenario-table"><header><h3>最终方案参数</h3><span>2,990颗卫星进入上线前验收</span></header><div><table><thead><tr><th>倾角</th><th>轨道高度</th><th>轨道面</th><th>每面卫星</th><th>卫星总数</th></tr></thead><tbody><tr><td>{smallerScreen.scenario.inclinationDeg}°</td><td>{smallerScreen.scenario.altitudeKm} km</td><td>{smallerScreen.scenario.planes}</td><td>{smallerScreen.scenario.satellitesPerPlane}</td><td>{smallerScreen.scenario.satelliteCount.toLocaleString("en-US")}</td></tr></tbody></table></div></section></div></details>
        </section>
      ) : null}

      <footer className="global-footer"><span>波位目录 {String(metadata.version ?? "载入中")} · SHA-256 {String(metadata.integrity?.sha256 ?? metadata.contentHash ?? "pending").slice(0, 16)}</span><p>最终方案：{smallerScreen.scenario.satelliteCount.toLocaleString("en-US")}颗 · {smallerScreen.scenario.planes}个轨道面 × 每面{smallerScreen.scenario.satellitesPerPlane}颗 · 45°进入 / 42°保持 · 上线前验收继续进行</p></footer>
    </main>
  );
}
