"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import baselineJson from "./orbit-baseline.json";
import constellationAuditJson from "./global-constellation-audit.json";
import snapshotAuditJson from "./global-constellation-snapshot.json";
import f1DayAuditJson from "./global-constellation-f1-day-coarse.json";
import { GlobalCoverageMap, type GlobalMapCell } from "./global-map";
import { GlobalOrbitView } from "./global-orbit-view";
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
  cellBank: 0 | 1;
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
  byCell: readonly [number, number];
  scheduleOverflow: boolean;
};
type AuditScenario = {
  id?: string;
  inclinationDeg?: number;
  planes?: number;
  satellitesPerPlane?: number;
  totalSatellites?: number;
  satelliteCount?: number;
  phaseFactor?: number;
  status?: string;
  auditStatus?: string;
  note?: string;
};
type AuditDocument = {
  planningSeed?: string | AuditScenario;
  selectedScenario?: string | AuditScenario | null;
  exactAudit?: { status?: string; durationDays?: number; method?: string; reportPath?: string | null };
  searchSpace?: { inclinationsDeg?: readonly number[]; planes?: string; satellitesPerPlane?: string; phaseFactor?: string };
  scenarios?: readonly AuditScenario[];
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
const snapshotAudit = snapshotAuditJson as {
  exact: false;
  mode: string;
  summary: {
    maximumUncoveredL1: number;
    maximumVisibleL1PerSatellite: number;
    maximumSatellitesOver256L1: number;
  };
};
const f1DayAudit = f1DayAuditJson as {
  exact: false;
  sampling: { epochCount: number; stepSeconds: number };
  summary: {
    maximumUncoveredL1: number;
    minimumCandidateCount: number;
    maximumVisibleL1PerSatellite: number;
    maximumSatellitesOver256L1: number;
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

function statusText(status?: string) {
  if (status === "exact_pass") return "精确审计通过";
  if (status === "failed") return "审计失败";
  if (status === "coarse_pass") return "仅粗筛通过";
  if (status === "coarse") return "仅离散预筛";
  return "待 7 天连续审计";
}

function scenarioEvidenceText(scenario?: AuditScenario) {
  if (scenario?.id === "global-45-seed-3528") return "单时刻粗筛失败（23个空窗）";
  if (scenario?.id === "global-45-f1-snapshot-candidate") {
    return "一天离散粗筛无空窗（非连续证明）";
  }
  return statusText(scenario?.auditStatus ?? scenario?.status);
}

export function GlobalPlanner() {
  const [view, setView] = useState<View>("audit");
  const [timeSeconds, setTimeSeconds] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState<(typeof TIME_SPEEDS)[number]>(60);
  // F=1 t=0 peak-load satellite and its nearest catalog position keep the
  // initial coverage and calendar views representative of the audited case.
  const [selectedSatelliteId, setSelectedSatelliteId] = useState("P02-S04");
  const [selectedCellId, setSelectedCellId] = useState("G021777");
  const [search, setSearch] = useState("");
  const [catalog, setCatalog] = useState<readonly GlobalMapCell[]>([]);
  const [metadata, setMetadata] = useState<CatalogMetadata>({});
  const [catalogError, setCatalogError] = useState("");
  const [analysis, setAnalysis] = useState<WorkerAnalysis>({ cells: [], visibleCount: 0, byCell: [0, 0], scheduleOverflow: false });
  const [candidateCounts, setCandidateCounts] = useState<readonly number[]>([]);
  const [uncoveredCount, setUncoveredCount] = useState(0);
  const [fleetSnapshot, setFleetSnapshot] = useState({ peakVisible: 0, peakSatelliteId: "", overflowSatellites: 0 });
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
          byCell: message.byCell as readonly [number, number],
          scheduleOverflow: Boolean(message.scheduleOverflow),
        });
      } else if (message.type === "globalCoverage" && Number(message.requestId) === globalRequestRef.current) {
        setCandidateCounts(message.counts as readonly number[]);
        setUncoveredCount(Number(message.uncovered));
        setFleetSnapshot({
          peakVisible: Number(message.peakVisible),
          peakSatelliteId: String(message.peakSatelliteId),
          overflowSatellites: Number(message.overflowSatellites),
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
      radiusKm: entryRadiusKm,
      capacity: SATELLITE_CAPACITY,
      timeline: Array.from({ length: 181 }, (_, index) => {
        const offsetSeconds = (index - 90) * 10;
        const state = propagateSatellite(selectedDefinition, coverageEpoch + offsetSeconds);
        return { offsetSeconds, lat: state.lat, lon: state.lon };
      }),
    });
  }, [analysisSatellite.id, analysisSatellite.lat, analysisSatellite.lon, catalog.length, coverageEpoch, entryRadiusKm, selectedDefinition]);
  useEffect(() => {
    if (!workerRef.current || catalog.length === 0) return;
    globalRequestRef.current += 1;
    workerRef.current.postMessage({
      type: "globalCoverage",
      requestId: globalRequestRef.current,
      radiusKm: entryRadiusKm,
      satelliteSubpoints: satellites.map((definition) => {
        const state = propagateSatellite(definition, coverageEpoch);
        return { id: state.id, lat: state.lat, lon: state.lon };
      }),
    });
  }, [catalog.length, coverageEpoch, entryRadiusKm]);

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
  const selectedScenarioRecord = typeof audit.selectedScenario === "string"
    ? audit.scenarios?.find(({ id }) => id === audit.selectedScenario)
    : audit.selectedScenario;
  const selectedScenario = (selectedScenarioRecord?.auditStatus ?? selectedScenarioRecord?.status) === "exact_pass"
    ? selectedScenarioRecord
    : null;
  const planningSeed = typeof audit.planningSeed === "string"
    ? audit.scenarios?.find(({ id }) => id === audit.planningSeed)
    : audit.planningSeed;
  const engineeringCandidate = audit.scenarios?.find(({ id }) => id === "global-45-f1-snapshot-candidate");
  const displayedScenario = selectedScenario ?? engineeringCandidate ?? planningSeed ?? audit.scenarios?.[0] ?? {
    inclinationDeg: baseline.inclinationDeg,
    planes: baseline.planes,
    satellitesPerPlane: baseline.satellitesPerPlane,
    totalSatellites: baseline.totalSatellites,
    phaseFactor: baseline.phaseFactor,
    status: "pending_exact",
  };

  const chooseSatellite = useCallback((id: string) => setSelectedSatelliteId(id), []);
  const selectBestCandidate = () => {
    const best = entryCandidates[0];
    if (best) setSelectedSatelliteId(best.id);
  };
  const runSearch = () => {
    const value = search.trim().toUpperCase();
    const satellite = satellites.find(({ id }) => id === value);
    if (satellite) {
      setSelectedSatelliteId(satellite.id);
      if (view !== "access") setView("orbit");
      return;
    }
    const cell = catalog.find(({ id }) => id === value);
    if (cell) { setSelectedCellId(cell.id); setView("coverage"); }
  };
  const windowsA = calendarWindows(analysis.cells, 0);
  const windowsB = calendarWindows(analysis.cells, 1);
  const catalogReady = catalog.length > 0;
  const capacityDecision = analysis.scheduleOverflow
    ? "超出日历容量"
    : `容量满足，余量${SATELLITE_CAPACITY - analysis.visibleCount}个L1`;
  const currentJudgement = !catalogReady
    ? "正在载入全球波位目录"
    : view === "coverage"
      ? entryCandidates.length > 0 ? `可接入，${entryCandidates.length}颗候选卫星` : "当前无可接入卫星"
      : capacityDecision;
  const bestEntryCandidate = entryCandidates[0];
  const largestOnboardCell = Math.max(...analysis.byCell, 0);
  const accessCapacityReady = catalogReady
    && !analysis.scheduleOverflow
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
        <div className="audit-chip review"><i />离散覆盖与L1日历可行 · 待连续验收</div>
      </header>

      {view !== "access" ? <section className="global-metrics" aria-label="工程结论指标">
        <article className="metric-recommended"><span>当前推荐候选</span><strong>60° · 42×84 · F=1</strong><small>500 km圆轨道 · 共{baseline.totalSatellites.toLocaleString("en-US")}颗卫星</small></article>
        <article><span>一天离散覆盖检查</span><strong>{f1DayAudit.sampling.epochCount} / {f1DayAudit.sampling.epochCount}</strong><small>120 s步长无空窗 · 全局最少候选{f1DayAudit.summary.minimumCandidateCount}颗</small></article>
        <article><span>单星最大L1负载</span><strong>{f1DayAudit.summary.maximumVisibleL1PerSatellite} / {SATELLITE_CAPACITY}</strong><small>容量余量{SATELLITE_CAPACITY - f1DayAudit.summary.maximumVisibleL1PerSatellite}个L1 · 18.4%</small></article>
        <article className="metric-review"><span>正式连续覆盖验收</span><strong>待执行</strong><small>{audit.exactAudit?.durationDays ?? 7}天事件驱动检查 · 45°区间连续性</small></article>
      </section> : null}

      {view !== "audit" ? <section className="global-toolbar" aria-label="时间与查询控制">
        <button type="button" className="play-button" onClick={() => { if (playing) setCoverageEpoch(timeSeconds); setPlaying((value) => !value); }}>{playing ? "暂停时间" : "播放时间"}</button>
        <label className="time-range"><span>动画时刻 {formatClock(timeSeconds)}</span><input type="range" min="0" max="86399" step="1" value={Math.floor(timeSeconds)} onChange={(event) => { const value = Number(event.target.value); setTimeSeconds(value); setCoverageEpoch(value); }} /></label>
        <div className="speed-control" aria-label="播放速度">{TIME_SPEEDS.map((value) => <button type="button" key={value} className={speed === value ? "active" : ""} onClick={() => setSpeed(value)}>{value}×</button>)}</div>
        <form onSubmit={(event) => { event.preventDefault(); runSearch(); }}><label htmlFor="global-search">定位卫星或 L1</label><div><input id="global-search" value={search} onChange={(event) => setSearch(event.target.value)} placeholder="P01-S01 / G000001" /><button type="submit">定位</button></div></form>
      </section> : null}

      {view !== "audit" && view !== "access" ? <section className="runtime-context" aria-label="当前运行快照">
        <span>F=1运行快照</span>
        <dl>
          <div><dt>动画 / 覆盖快照</dt><dd>{formatClock(timeSeconds)} / {formatClock(coverageEpoch)}</dd></div>
          <div><dt>一级波位</dt><dd>{selectedCell?.id ?? selectedCellId}</dd></div>
          <div><dt>所选卫星</dt><dd>{selectedSatellite.id}</dd></div>
          <div><dt>卫星可见L1</dt><dd>{analysis.visibleCount} / {SATELLITE_CAPACITY}</dd></div>
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
            <header><div><p>工程推荐候选 · F=1</p><h2>星座运行与单星负载</h2></div><span>{baseline.walker} · h={baseline.altitudeKm} km · i={baseline.inclinationDeg}°</span></header>
            <GlobalOrbitView timeSeconds={timeSeconds} selectedSatelliteId={selectedSatelliteId} onSelectSatellite={chooseSatellite} />
          </article>
          <aside className="global-inspector">
            <header><p>已选卫星</p><h2>{selectedSatellite.id}</h2><span className={`position-status ${analysis.scheduleOverflow ? "is-failure" : "is-pass"}`}>{analysis.scheduleOverflow ? "容量超限" : "容量满足"}</span></header>
            <dl><div><dt>轨道面 / 槽位</dt><dd>P{String(selectedSatellite.plane).padStart(2, "0")} / S{String(selectedSatellite.slot).padStart(2, "0")}</dd></div><div><dt>星下点</dt><dd>{selectedSatellite.lat.toFixed(2)}°, {selectedSatellite.lon.toFixed(2)}°</dd></div><div><dt>负载快照时刻</dt><dd>{formatClock(coverageEpoch)}</dd></div><div><dt>45°内完整可见L1</dt><dd>{analysis.visibleCount} / {SATELLITE_CAPACITY}</dd></div><div><dt>当前容量余量</dt><dd className={analysis.scheduleOverflow ? "is-failure" : ""}>{SATELLITE_CAPACITY - analysis.visibleCount}个L1</dd></div><div><dt>两个星载小区</dt><dd>{analysis.byCell[0]} / {analysis.byCell[1]}</dd></div><div><dt>一天预审峰值</dt><dd>{f1DayAudit.summary.maximumVisibleL1PerSatellite} / {SATELLITE_CAPACITY}</dd></div><div><dt>当前超限卫星</dt><dd>{fleetSnapshot.overflowSatellites}颗</dd></div></dl>
            <details className="technical-details"><summary>技术边界</summary><p>轨道位置采用二体圆轨道加地球自转计算；正式工程还需使用TLE/SGP4/J2复核。当前容量判断只针对L1接入日历，不等同于功率、干扰、gateway或RF能力验收。</p></details>
          </aside>
        </section>
      ) : null}

      {view === "access" ? (
        <section className="access-console">
          <header className="access-summary">
            <div>
              <p>跳波束日历 · {selectedSatellite.id} · {formatClock(coverageEpoch)} 快照</p>
              <h2>这颗卫星能否为所有可见地面区域排出接入时段？</h2>
            </div>
            <span className={!catalogReady ? "pending" : accessCapacityReady ? "scheduled" : "failed"}>
              {!catalogReady ? "正在计算" : accessCapacityReady ? "本次安排可行" : "需要调整卫星"}
            </span>
          </header>

          <section className="access-guide" aria-labelledby="access-guide-title">
            <div>
              <span>先看这里</span>
              <h3 id="access-guide-title">日历就是卫星轮流照向不同地面区域的时间安排</h3>
              <p>页面先判断容量是否够，再把可见地面波位分给两个星载小区，最后检查终端能否及时发现网络并发起接入。</p>
            </div>
            <ol className="access-journey">
              <li><b>1</b><span>收集可见波位</span><small>当前{analysis.visibleCount}个地面区域</small></li>
              <li><b>2</b><span>分给两个小区</span><small>{analysis.byCell[0]}个 / {analysis.byCell[1]}个</small></li>
              <li><b>3</b><span>安排下行发现</span><small>每个区域80 ms内有一次计划机会</small></li>
              <li><b>4</b><span>安排上行接入</span><small>每个区域640 ms内有一次计划机会</small></li>
            </ol>
          </section>

          <section className="access-answer-grid" aria-label="跳波束日历核心结论">
            <article className={analysis.scheduleOverflow ? "is-failure" : "is-pass"}>
              <span>容量够不够</span>
              <b>{analysis.visibleCount} / {SATELLITE_CAPACITY}</b>
              <strong>{analysis.scheduleOverflow ? "放不下全部区域" : "可见区域可以全部排入"}</strong>
              <p>256是单颗卫星本阶段最多可安排的地面波位数。</p>
            </article>
            <article className={largestOnboardCell > CELL_CAPACITY ? "is-failure" : "is-pass"}>
              <span>两个小区是否超限</span>
              <b>{analysis.byCell[0]} / {analysis.byCell[1]}</b>
              <strong>{largestOnboardCell > CELL_CAPACITY ? "至少一个小区超限" : "两个小区都在容量内"}</strong>
              <p>每个小区最多负责128个地面波位。</p>
            </article>
            <article className="is-pass">
              <span>多久安排一次网络发现机会</span>
              <b>计划间隔≤80 ms</b>
              <strong>下行发现时段可排</strong>
              <p>每小区安排128项，仍保留{ssbOpportunityHeadroom}次计划机会；真实信号仍需无线验证。</p>
            </article>
            <article className="is-tight">
              <span>多久安排一次接入机会</span>
              <b>计划间隔≤640 ms</b>
              <strong>满足目标，但没有计划余量</strong>
              <p>这是当前最紧张的一项，需要在后续无线实现中重点验证。</p>
            </article>
          </section>

          <section className="access-cell-overview">
            <header>
              <div><span>两个小区如何分工</span><h3>把可见波位分成两组，分别轮流服务</h3></div>
              <p>小区身份跟随卫星保持稳定；地面波位只是在当前时间段被分到其中一组。</p>
            </header>
            <div>
              {([0, 1] as const).map((bank) => {
                const cellCount = analysis.byCell[bank] ?? 0;
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
                const cellCount = analysis.byCell[bank] ?? 0;
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
              <div><table><thead><tr><th>指标</th><th>要求</th><th>当前结果</th><th>结论</th></tr></thead><tbody><tr><td>单星可见L1</td><td>≤{SATELLITE_CAPACITY}</td><td>{analysis.visibleCount} / {SATELLITE_CAPACITY}</td><td><span className={analysis.scheduleOverflow ? "result-failed" : "result-pass"}>{analysis.scheduleOverflow ? "超限" : "满足"}</span></td></tr><tr><td>单个星载小区</td><td>≤{CELL_CAPACITY}</td><td>{analysis.byCell[0]} / {analysis.byCell[1]}</td><td><span className={largestOnboardCell > CELL_CAPACITY ? "result-failed" : "result-pass"}>{largestOnboardCell > CELL_CAPACITY ? "超限" : "满足"}</span></td></tr><tr><td>SSB访问周期</td><td>≤80 ms</td><td>128 / {BASELINE_L1_CAPACITY.guaranteedDownlinkVisits}次机会</td><td><span className="result-pass">时间账本可排</span></td></tr><tr><td>PRACH接入周期</td><td>≤640 ms</td><td>128 / 128次机会</td><td><span className="result-tight">可排，无余量</span></td></tr><tr><td>瞬时模拟波束</td><td>≤{ANALOG_PER_SATELLITE}路</td><td>{ANALOG_PER_SATELLITE}路上限</td><td><span className="result-pass">满足</span></td></tr></tbody></table></div>
            </section>
          </details>

          <details className="access-detail-block visible-table">
            <summary><span><b>查看完整可见波位表</b><small>{selectedSatellite.id}当前共{analysis.visibleCount}个 · {String(metadata.version ?? "载入中")}</small></span><em>原始数据</em></summary>
            <div><table><thead><tr><th>L1</th><th>星载小区</th><th>仰角</th><th>45°可见区间</th><th>仰角时间线</th><th>距星下点</th><th>纬度</th><th>经度</th></tr></thead><tbody>{analysis.cells.map((cell) => <tr key={cell.id}><td>{cell.id}</td><td>{cell.cellBank + 1}</td><td>{cell.elevationDeg?.toFixed(1) ?? "—"}°</td><td>{cell.visibleFromSeconds ?? "—"}…{cell.visibleUntilSeconds ?? "—"} s</td><td>{cell.elevationTimeline?.map((sample) => `${sample.offsetSeconds}:${sample.elevationDeg.toFixed(1)}°`).join(" / ") ?? "—"}</td><td>{cell.distanceKm.toFixed(1)} km</td><td>{cell.lat.toFixed(4)}°</td><td>{cell.lon.toFixed(4)}°</td></tr>)}</tbody></table></div>
          </details>

          <details className="technical-details access-technical"><summary>这项结论还不代表什么</summary><p>当前页面证明的是软件能够排出接入日历。真实广播、终端接入检测、功率、天线和射频切换仍需在后续无线链路中验证；“已安排”不等于信号已经从天线发出。</p></details>
        </section>
      ) : null}

      {view === "audit" ? (
        <section className="decision-console">
          <header className="decision-hero"><div><span className="decision-label">当前工程结论</span><h2>F=1离散覆盖检查无空窗，L1时间账本可排，可作为正式验收输入</h2><p>星座规模保持42面×84星不变，仅采用F=1相位。现有结果支持进入7天连续覆盖与接管验收；完成该验收以及RF、功率和真实轨道复核后，才能签署最终方案。</p></div><div className="decision-stamp"><span>当前决策</span><b>进入下一阶段</b><small>尚未完成最终验收</small></div></header>
          <p className="rejected-line"><b>F=0已淘汰：</b>t=0存在{snapshotAudit.summary.maximumUncoveredL1}个L1没有达到45°的候选卫星，不再作为工程基线。</p>

          <section className="conclusion-table"><header><h3>可行性结论</h3><span>“通过”仅指当前规划计算与已有证据</span></header><div><table><thead><tr><th>工程项</th><th>当前结论</th><th>量化依据</th></tr></thead><tbody><tr><td>F=1覆盖几何</td><td><span className="result-pass">离散检查通过</span></td><td>{f1DayAudit.sampling.epochCount}/{f1DayAudit.sampling.epochCount}个采样时刻无空窗，最少候选{f1DayAudit.summary.minimumCandidateCount}颗</td></tr><tr><td>单星L1日历容量</td><td><span className="result-pass">满足</span></td><td>峰值{f1DayAudit.summary.maximumVisibleL1PerSatellite}/{SATELLITE_CAPACITY}，余量{SATELLITE_CAPACITY - f1DayAudit.summary.maximumVisibleL1PerSatellite}</td></tr><tr><td>双小区容量</td><td><span className="result-pass">满足</span></td><td>峰值按当前平衡分区约105/104，均低于128</td></tr><tr><td>SSB跳波束日历</td><td><span className="result-pass">时间账本可排</span></td><td>80 ms共有{BASELINE_L1_CAPACITY.guaranteedDownlinkVisits}次机会，配置128个L1，余40次</td></tr><tr><td>PRACH接入日历</td><td><span className="result-tight">可排但偏紧</span></td><td>每小区128个L1使用128次640 ms上行机会，无计划余量</td></tr><tr><td>全球连续覆盖</td><td><span className="result-review">待验收</span></td><td>{audit.exactAudit?.durationDays ?? 7}天事件驱动连续检查尚未执行</td></tr></tbody></table></div></section>

          <section className="acceptance-actions"><header><h3>正式定案前必须关闭的三项</h3><span>完成后才能签署“全球连续覆盖通过”</span></header><div><article><b>01</b><h4>连续覆盖</h4><p>执行7天45°区间并集与阈值交叉事件检查，排除固定步长采样之间的短空窗。</p></article><article><b>02</b><h4>接管连续性</h4><p>验证目标星达到45°且ready/applied后接管，源星保持到42°期间全程无双空窗。</p></article><article><b>03</b><h4>载荷与真实轨道</h4><p>完成TLE/SGP4/J2、链路预算、功率、干扰、gateway以及RF执行验证。</p></article></div></section>

          <details className="technical-details decision-details"><summary>计算依据与数据边界</summary><div className="decision-technical"><p>覆盖目录包含±57°陆地的36,411个L1中心采样点；F=1结论来自一天、120 s固定步长检查。正式基线尚未签署，连续事件审计仍未运行。</p><p>80/640 ms日历单测证明128项时间账本可以排入，但不是全球目录、接管、PHY或RF的端到端验收。候选全集不按256容量裁剪。</p><section className="scenario-table"><header><h3>方案对比记录</h3><span>技术字段</span></header><div><table><thead><tr><th>候选</th><th>倾角</th><th>轨道面</th><th>每面卫星</th><th>F</th><th>证据状态</th></tr></thead><tbody>{(audit.scenarios ?? [displayedScenario]).filter(Boolean).map((scenario, index) => <tr key={scenario?.id ?? index}><td>{scenario?.id ?? (index === 0 ? "seed" : `candidate-${index}`)}</td><td>{scenario?.inclinationDeg ?? baseline.inclinationDeg}°</td><td>{scenario?.planes ?? "—"}</td><td>{scenario?.satellitesPerPlane ?? "—"}</td><td>{scenario?.phaseFactor ?? baseline.phaseFactor}</td><td>{scenarioEvidenceText(scenario)}</td></tr>)}</tbody></table></div></section></div></details>
        </section>
      ) : null}

      <footer className="global-footer"><span>波位目录 {String(metadata.version ?? "载入中")} · SHA-256 {String(metadata.integrity?.sha256 ?? metadata.contentHash ?? "pending").slice(0, 16)}</span><p>工程候选：500 km · 60° · 42×84 · F=1 · 45°接入 / 42°保持</p></footer>
    </main>
  );
}
