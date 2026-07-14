"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import { l1Catalog, l1CellById, l2CatalogIds } from "./beam-catalog";
import assignmentBaseline from "./l1-assignment-baseline.json";
import {
  BEAM_SLOT_MS,
  CELL_ANALOG_BEAM_PORTS,
  CELL_DIGITAL_BEAM_PORTS,
  CELL_L1_CAPACITY,
  L1_SSB_REVISIT_MS,
  PRACH_REVISIT_MS,
  SATELLITE_ANALOG_BEAM_PORTS,
  SATELLITE_DIGITAL_BEAM_PORTS,
  SATELLITE_L1_CAPACITY,
  SLOT_PHASE_LIMITS,
  SSB_PERIOD_MS,
  SSB_VISITS_PER_DL_PORT_PER_OCCASION,
  createCellBeamSchedule,
  deriveL1Capacity,
  mergeSatelliteCellSchedules,
  type BeamScheduleEntry,
  type CellPositionSet,
  type DigitalBeamDemand,
} from "./beam-hopping-model";
import {
  onboardCellRegistry,
  planL1AssignmentsAtEpoch,
  type L1AssignmentReport,
} from "./satellite-cell-model";
import { baseline, satellites } from "./orbit-model";

type BeamHoppingViewProps = {
  beam: { catalogId: string; childCount: number };
  time: number;
  onTimeChange: (time: number) => void;
  selectedSatelliteId: string;
  onSelectedSatelliteChange: (satelliteId: string) => void;
};

type UiOnboardCell = CellPositionSet & {
  satelliteId: string;
  pci: number;
  bank: 0 | 1;
};

type CellPlan = {
  cell: UiOnboardCell;
  current: BeamScheduleEntry[];
  demands: DigitalBeamDemand[];
  ssbCovered: number;
  prachCovered: number;
  analogBankStart: number;
  digitalBankStart: number;
};

const PLANNING_TICK_SECONDS = 10;

function wrapTime(value: number) {
  return ((value % 86_400) + 86_400) % 86_400;
}

function formatClock(seconds: number) {
  const normalized = Math.floor(wrapTime(seconds));
  const hours = Math.floor(normalized / 3600);
  const minutes = Math.floor((normalized % 3600) / 60);
  const secs = normalized % 60;
  return [hours, minutes, secs].map((value) => String(value).padStart(2, "0")).join(":");
}

function satelliteOrdinal(satelliteId: string) {
  const index = satellites.findIndex((satellite) => satellite.id === satelliteId);
  return index >= 0 ? index : 0;
}

function cellsForSatellite(report: L1AssignmentReport, satelliteId: string): UiOnboardCell[] {
  return onboardCellRegistry
    .filter((cell) => cell.satelliteId === satelliteId)
    .map((cell) => ({
      satelliteId,
      nci: cell.nci,
      pci: cell.pci,
      bank: cell.bank,
      l1Ids: report.assignments
        .filter((assignment) => assignment.satelliteId === satelliteId && assignment.nci === cell.nci)
        .map((assignment) => assignment.positionId)
        .sort(),
    }));
}

function digitalDemands(cell: UiOnboardCell, selectedL1Id: string, enabled: boolean): DigitalBeamDemand[] {
  if (!enabled || cell.l1Ids.length === 0) return [];
  const ordered = [selectedL1Id, ...cell.l1Ids]
    .filter((id, index, values) => cell.l1Ids.includes(id) && values.indexOf(id) === index);
  const source = ordered.length > 0 ? ordered : [...cell.l1Ids];
  const l2Ids = source.flatMap((id) => {
    const l1 = l1CellById(id);
    return l1 ? l2CatalogIds(l1) : [];
  });
  const makePosition = (offset: number) => l2Ids[offset % l2Ids.length];
  return [
    ...Array.from({ length: Math.min(4, l2Ids.length) }, (_, offset) => ({
      nci: cell.nci,
      positionId: makePosition(offset),
      direction: "downlink" as const,
      hasPduOrDrb: true,
      applied: true,
      priority: 20 - offset,
    })),
    ...Array.from({ length: Math.min(2, l2Ids.length) }, (_, offset) => ({
      nci: cell.nci,
      positionId: makePosition(offset + 4),
      direction: "uplink" as const,
      hasPduOrDrb: true,
      applied: true,
      priority: 12 - offset,
    })),
    {
      nci: cell.nci,
      positionId: makePosition(6),
      direction: "downlink" as const,
      hasPduOrDrb: true,
      applied: false,
      priority: 30,
    },
    {
      nci: cell.nci,
      positionId: makePosition(7),
      direction: "uplink" as const,
      hasPduOrDrb: false,
      applied: true,
      priority: 30,
    },
  ];
}

function buildCellPlan(cell: UiOnboardCell, selectedL1Id: string, nowMs: number, demoTraffic: boolean): CellPlan {
  const slotStartMs = Math.floor(nowMs / BEAM_SLOT_MS) * BEAM_SLOT_MS;
  const ssbStartMs = Math.floor(nowMs / L1_SSB_REVISIT_MS) * L1_SSB_REVISIT_MS;
  const prachStartMs = Math.floor(nowMs / PRACH_REVISIT_MS) * PRACH_REVISIT_MS;
  const demands = digitalDemands(cell, selectedL1Id, demoTraffic);
  if (cell.l1Ids.length === 0) {
    return {
      cell,
      current: [],
      demands,
      ssbCovered: 0,
      prachCovered: 0,
      analogBankStart: cell.bank * CELL_ANALOG_BEAM_PORTS,
      digitalBankStart: cell.bank * CELL_DIGITAL_BEAM_PORTS,
    };
  }
  const options = { satelliteId: cell.satelliteId, cell, cellBank: cell.bank };
  const current = createCellBeamSchedule({
    ...options,
    startTimeMs: slotStartMs,
    durationMs: BEAM_SLOT_MS,
    digitalDemands: demands,
  });
  const ssbWindow = createCellBeamSchedule({
    ...options,
    startTimeMs: ssbStartMs,
    durationMs: L1_SSB_REVISIT_MS,
  });
  const prachWindow = createCellBeamSchedule({
    ...options,
    startTimeMs: prachStartMs,
    durationMs: PRACH_REVISIT_MS,
  });
  return {
    cell,
    current,
    demands,
    ssbCovered: new Set(ssbWindow.filter((entry) => entry.purpose === "ssb").map((entry) => entry.positionId)).size,
    prachCovered: new Set(prachWindow.filter((entry) => entry.purpose === "prach").map((entry) => entry.positionId)).size,
    analogBankStart: 0,
    digitalBankStart: 0,
  };
}

function mergeCellPlans(plans: readonly CellPlan[]) {
  const merged = mergeSatelliteCellSchedules(plans.map((plan) => plan.current));
  return plans.map((plan) => {
    const analogBankStart = plan.cell.bank * CELL_ANALOG_BEAM_PORTS;
    const digitalBankStart = plan.cell.bank * CELL_DIGITAL_BEAM_PORTS;
    return {
      ...plan,
      current: merged.filter((entry) => entry.nci === plan.cell.nci),
      analogBankStart,
      digitalBankStart,
    };
  });
}

function directionLabel(direction: "downlink" | "uplink") {
  return direction === "downlink" ? "DL" : "UL";
}

function CellResourceLanes({ plan, phaseIndex, selectedL1Id }: { plan: CellPlan; phaseIndex: number; selectedL1Id: string }) {
  const phase = SLOT_PHASE_LIMITS[phaseIndex];
  const analogEntries = plan.current.filter((entry) => entry.beamClass === "analog");
  const digitalEntries = plan.current.filter((entry) => entry.beamClass === "digital");
  const digitalByPort = new Map(digitalEntries.map((entry) => [entry.portId, entry]));
  const readyDemands = plan.demands.filter((demand) => demand.hasPduOrDrb && demand.applied).length;
  const pendingDemands = plan.demands.filter((demand) => demand.hasPduOrDrb && !demand.applied).length;
  const controlOnly = plan.demands.filter((demand) => !demand.hasPduOrDrb).length;

  return (
    <article className="bh-cell" aria-labelledby={`bh-cell-${plan.cell.nci}`}>
      <header className="bh-cell-header">
        <div>
          <span>星载小区 {plan.cell.bank === 0 ? "A" : "B"} · PCI {plan.cell.pci}</span>
          <h3 id={`bh-cell-${plan.cell.nci}`}>{plan.cell.nci}</h3>
        </div>
        <dl>
          <div><dt>L1</dt><dd>{plan.cell.l1Ids.length}/{CELL_L1_CAPACITY}</dd></div>
          <div><dt>模拟</dt><dd>8 固定</dd></div>
          <div><dt>数字</dt><dd>64 固定</dd></div>
        </dl>
      </header>

      <div className="bh-cell-health" aria-label={`${plan.cell.nci} 周期约束`}>
        <span className={plan.ssbCovered === plan.cell.l1Ids.length ? "is-ok" : "is-miss"}>80 ms SSB：{plan.ssbCovered}/{plan.cell.l1Ids.length} L1</span>
        <span className={plan.prachCovered === plan.cell.l1Ids.length ? "is-ok" : "is-miss"}>640 ms RO：{plan.prachCovered}/{plan.cell.l1Ids.length} L1</span>
        <span>NCI / PCI 随星载小区</span>
      </div>

      <section className="bh-lane" aria-label={`${plan.cell.nci} 模拟波束泳道`}>
        <div className="bh-lane-heading">
          <div><span>ANALOG · 8 PORTS</span><b>SSB / SIB / Paging / PRACH / RAR</b></div>
          <strong>{phase.analog.downlink} DL + {phase.analog.uplink} UL</strong>
        </div>
        <div className="bh-analog-ports">
          {Array.from({ length: CELL_ANALOG_BEAM_PORTS }, (_, localPortId) => {
            const portId = plan.analogBankStart + localPortId;
            const portEntries = analogEntries.filter((item) => item.portId === portId).sort((left, right) => left.startTimeMs - right.startTimeMs);
            const entry = portEntries[0];
            const direction = entry?.direction ?? (localPortId < phase.analog.downlink ? "downlink" : "uplink");
            const label = entry
              ? `${directionLabel(direction)} · ${portEntries.map((item) => item.positionId).join(" → ")}`
              : `${directionLabel(direction)} · guard / idle`;
            return (
              <div key={portId} className={`bh-analog-port ${entry ? (direction === "downlink" ? "is-dl" : "is-ul") : ""} ${portEntries.some((item) => item.positionId === selectedL1Id) ? "is-selected" : ""}`}>
                <i>PHY {String(portId + 1).padStart(2, "0")}</i>
                <b>{label}</b>
                <small>{entry ? `${entry.purpose}${portEntries.length > 1 ? " · 2 × 5 ms retarget" : ""}` : (plan.cell.l1Ids.length === 0 ? "no position lease" : "reserved")}</small>
              </div>
            );
          })}
        </div>
        <div className="bh-direction-bar" aria-label={`本相位 ${phase.analog.downlink} 路下行和 ${phase.analog.uplink} 路上行`}>
          <span className="is-dl" style={{ flex: phase.analog.downlink }}>{phase.analog.downlink} DL</span>
          <span className="is-ul" style={{ flex: phase.analog.uplink }}>{phase.analog.uplink} UL</span>
        </div>
      </section>

      <section className="bh-lane" aria-label={`${plan.cell.nci} 数字波束泳道`}>
        <div className="bh-lane-heading">
          <div><span>DIGITAL · 64 PORTS</span><b>仅 demand + applied 的 L2</b></div>
          <strong>{digitalEntries.length} / 64 active</strong>
        </div>
        <div className="bh-digital-ports" role="img" aria-label={`64个数字端口中，当前${digitalEntries.length}个有已应用业务`}>
          {Array.from({ length: CELL_DIGITAL_BEAM_PORTS }, (_, localPortId) => {
            const portId = plan.digitalBankStart + localPortId;
            const entry = digitalByPort.get(portId);
            return <span key={portId} className={entry ? (entry.direction === "downlink" ? "is-dl" : "is-ul") : ""} aria-label={entry ? `${entry.positionId} ${directionLabel(entry.direction)}` : `端口${portId + 1}空闲`} />;
          })}
        </div>
        <div className="bh-demand-status">
          <span>ready <b>{readyDemands}</b></span>
          <span>pending applied <b>{pendingDemands}</b></span>
          <span>control_only <b>{controlOnly}</b></span>
        </div>
      </section>

      <div className="bh-phase-calendar" aria-label="三个10毫秒相位的端口日历">
        {SLOT_PHASE_LIMITS.map((slot, index) => (
          <div key={index} className={index === phaseIndex ? "is-current" : ""}>
            <span>PHASE {index + 1}</span>
            <b>A {slot.analog.downlink}D/{slot.analog.uplink}U</b>
            <small>D {slot.digital.downlink}D/{slot.digital.uplink}U</small>
          </div>
        ))}
      </div>
    </article>
  );
}

export function BeamHoppingView({
  beam,
  time,
  onTimeChange,
  selectedSatelliteId,
  onSelectedSatelliteChange,
}: BeamHoppingViewProps) {
  const [playing, setPlaying] = useState(false);
  const [demoTraffic, setDemoTraffic] = useState(false);
  const autoFocusedBeam = useRef("");
  const nowMs = Math.floor(wrapTime(time) * 1000 / BEAM_SLOT_MS) * BEAM_SLOT_MS;
  const absoluteSlot = Math.floor(nowMs / BEAM_SLOT_MS);
  const phaseIndex = ((absoluteSlot % SLOT_PHASE_LIMITS.length) + SLOT_PHASE_LIMITS.length) % SLOT_PHASE_LIMITS.length;
  const assignmentEpoch = Math.floor(wrapTime(time) / PLANNING_TICK_SECONDS) * PLANNING_TICK_SECONDS;
  const previousReport = useMemo(
    () => planL1AssignmentsAtEpoch(wrapTime(assignmentEpoch - PLANNING_TICK_SECONDS)),
    [assignmentEpoch],
  );
  const assignmentReport = useMemo(
    () => planL1AssignmentsAtEpoch(assignmentEpoch, previousReport),
    [assignmentEpoch, previousReport],
  );
  const onboardCells = useMemo(
    () => cellsForSatellite(assignmentReport, selectedSatelliteId),
    [assignmentReport, selectedSatelliteId],
  );
  const plans = useMemo(
    () => mergeCellPlans(onboardCells.map((cell) => buildCellPlan(cell, beam.catalogId, nowMs, demoTraffic))),
    [beam.catalogId, demoTraffic, nowMs, onboardCells],
  );

  useEffect(() => {
    if (!playing) return;
    const timer = window.setInterval(() => onTimeChange(wrapTime(time + 1)), 500);
    return () => window.clearInterval(timer);
  }, [onTimeChange, playing, time]);

  const selectedSatelliteIndex = satelliteOrdinal(selectedSatelliteId);
  const selectedAssignment = assignmentReport.assignments.find((assignment) => assignment.positionId === beam.catalogId);
  const previousAssignment = previousReport.assignments.find((assignment) => assignment.positionId === beam.catalogId);
  const selectedDelta = assignmentReport.reassignmentDeltas.find((delta) => delta.positionId === beam.catalogId);
  const selectedSatelliteLoad = assignmentReport.assignments.filter((assignment) => assignment.satelliteId === selectedSatelliteId).length;
  const loadCounts = new Map<string, number>();
  assignmentReport.assignments.forEach((assignment) => loadCounts.set(assignment.satelliteId, (loadCounts.get(assignment.satelliteId) ?? 0) + 1));
  const activeSatellites = [...loadCounts.values()].filter((count) => count > 0).length;
  const maximumLoad = Math.max(0, ...loadCounts.values());
  const candidateCount = assignmentReport.candidateCountsByPositionId.get(beam.catalogId) ?? 0;
  const servingCandidateCount = assignmentReport.servingCandidateCountsByPositionId.get(beam.catalogId) ?? 0;
  const capacityScenarios = [40, 80, 160].map((revisitMs) => deriveL1Capacity(revisitMs));

  useEffect(() => {
    if (
      autoFocusedBeam.current !== beam.catalogId
      && selectedAssignment
      && selectedSatelliteLoad === 0
      && selectedAssignment.satelliteId !== selectedSatelliteId
    ) {
      autoFocusedBeam.current = beam.catalogId;
      onSelectedSatelliteChange(selectedAssignment.satelliteId);
    }
  }, [beam.catalogId, onSelectedSatelliteChange, selectedAssignment, selectedSatelliteId, selectedSatelliteLoad]);

  const sourceSatellite = selectedDelta?.sourceSatelliteId ?? previousAssignment?.satelliteId ?? "—";
  const targetSatellite = selectedDelta?.targetSatelliteId ?? selectedAssignment?.satelliteId ?? "—";
  const transferSteps = [
    { label: "candidate inventory", detail: `${servingCandidateCount} 颗当前可服务 · ${candidateCount} 颗含120s预览` },
    { label: "activation prediction", detail: "需确认目标在生效 epoch 仍满足仰角" },
    { label: "ready + applied", detail: "执行层反馈后才允许发现与测量重叠" },
    { label: "executed transfer", detail: "真实切换成功后才更新 serving owner" },
  ];
  const transferScope = selectedDelta?.targetSatelliteId === selectedDelta?.sourceSatelliteId
    ? "星内小区切换"
    : "跨星小区切换";

  const stepSatellite = (offset: number) => {
    const next = satellites[(selectedSatelliteIndex + offset + satellites.length) % satellites.length].id;
    onSelectedSatelliteChange(next);
  };

  return (
    <div className="beam-hopping-console bh-console" data-testid="beam-hopping-console">
      <section className="bh-toolbar" aria-label="跳波束时间和卫星选择">
        <div className="bh-time-readout"><span>ORBIT TIME · UTC</span><b>{formatClock(time)}</b></div>
        <button type="button" onClick={() => setPlaying((value) => !value)} aria-pressed={playing}>{playing ? "暂停" : "播放"}</button>
        <button type="button" onClick={() => onTimeChange(wrapTime(time - 10))}>−10 s</button>
        <button type="button" onClick={() => onTimeChange(wrapTime(time + 10))}>+10 s</button>
        <button type="button" onClick={() => setDemoTraffic((value) => !value)} aria-pressed={demoTraffic}>{demoTraffic ? "关闭演示业务" : "演示 L2 业务"}</button>
        <label className="bh-time-slider"><span>0–24 h 受控时间轴</span><input type="range" min="0" max="86400" step="1" value={Math.floor(wrapTime(time))} onChange={(event) => onTimeChange(Number(event.target.value))} /></label>
        <div className="bh-satellite-picker">
          <button type="button" onClick={() => stepSatellite(-1)} aria-label="上一颗卫星">←</button>
          <label><span>当前卫星</span><select value={selectedSatelliteId} onChange={(event) => onSelectedSatelliteChange(event.target.value)} aria-label="卫星短号">{satellites.map((satellite) => <option key={satellite.id} value={satellite.id}>{satellite.id}</option>)}</select></label>
          <button type="button" onClick={() => stepSatellite(1)} aria-label="下一颗卫星">→</button>
        </div>
      </section>

      <header className="bh-overview">
        <div>
          <p>ONBOARD REGENERATIVE GNB · PLANNING SNAPSHOT</p>
          <h2>{selectedSatelliteId} 当前服务 {selectedSatelliteLoad} / {SATELLITE_L1_CAPACITY} 个 L1 波位</h2>
          <span>所选 {beam.catalogId} 当前规划{selectedAssignment ? `由 ${selectedAssignment.satelliteId} · ${selectedAssignment.nci} · PCI ${selectedAssignment.pci} 服务` : "没有满足容量与可见性约束的服务星"}，其 {beam.childCount} 个 L2 随父 L1 继承当前星载小区。</span>
        </div>
        <dl>
          <div><dt>SSB 基准</dt><dd>{SSB_PERIOD_MS} ms</dd></div>
          <div><dt>L1 最迟再访</dt><dd>{L1_SSB_REVISIT_MS} ms</dd></div>
          <div><dt>规划 tick</dt><dd>{PLANNING_TICK_SECONDS} s</dd></div>
        </dl>
      </header>

      <section className={`bh-feasibility ${assignmentReport.success ? "is-pass" : ""}`} role="status" aria-label="当前逐波位分配仿真结果">
        <div><span>当前 epoch 已分配</span><b>{assignmentReport.assignments.length} / {l1Catalog.length}</b><small>未分配 {assignmentReport.unassignedPositionIds.length} 个 L1，失败保持可见</small></div>
        <div><span>同时参与服务</span><b>{activeSatellites} 颗卫星</b><small>单星最高负载 {maximumLoad} / {SATELLITE_L1_CAPACITY}</small></div>
        <div><span>PCI 干扰检查</span><b>{assignmentReport.pciConflicts.length} 个冲突</b><small>NCI 全局登记；PCI 仅在干扰邻域复用</small></div>
        <p><b>现在按单个 L1 分配：</b>不再要求任何预设地面分区整体被同一颗卫星看见；候选全集也不会被 84 的服务容量提前裁掉。</p>
      </section>

      <section className={`bh-day-report ${assignmentBaseline.successfulSamples === assignmentBaseline.sampleCount ? "is-pass" : ""}`} aria-label="24小时逐波位容量仿真报告">
        <div><span>24 h / 2 min 全量成功</span><b>{assignmentBaseline.successfulSamples} / {assignmentBaseline.sampleCount}</b><small>{(assignmentBaseline.successRate * 100).toFixed(2)}% epoch 覆盖全部 L1</small></div>
        <div><span>未分配 L1</span><b>{assignmentBaseline.minimumUnassigned} / {assignmentBaseline.averageUnassigned.toFixed(1)} / {assignmentBaseline.maximumUnassigned}</b><small>最少 / 平均 / 最多</small></div>
        <div><span>同时参与服务</span><b>{assignmentBaseline.minimumActiveSatellites}–{assignmentBaseline.maximumActiveSatellites} 颗</b><small>单星峰值 {assignmentBaseline.peakSatelliteLoad}，PCI 冲突 epoch {assignmentBaseline.pciConflictSamples}</small></div>
        <div><span>旧 owner 变化 / 释放</span><b>{assignmentBaseline.rawReassignments.toLocaleString("en-US")}</b><small>相邻 120 s 独立快照统计；不含恢复分配，也不是实网 HO</small></div>
        <p><b>当前结论：</b>{baseline.planes}×{baseline.satellitesPerPlane} 在一天 720 个采样 epoch 中消除了逐波位容量缺口；但单星峰值仍达到 {assignmentBaseline.peakSatelliteLoad} / {SATELLITE_L1_CAPACITY}。聚合候选容量账本余量 {assignmentBaseline.minimumFleetCapacityHeadroom.toLocaleString("en-US")} 只用于容量核算，不代表几何余量或 N-1 能力；SSB 时序和 PHY/RU 执行也尚未验证。</p>
      </section>

      <section className="bh-resource-contract" aria-label="84个L1容量推导">
        <div><span>整星模拟端口</span><b>{SATELLITE_ANALOG_BEAM_PORTS} = 2 × 8</b><small>瞬时 DL + UL，不是 84 路同时点亮</small></div>
        <div><span>80 ms 保证量</span><b>{SATELLITE_L1_CAPACITY} = 2 × {CELL_L1_CAPACITY}</b><small>每小区 4 次 SSB occasion 的最差相位窗</small></div>
        <div><span>整星数字端口</span><b>{SATELLITE_DIGITAL_BEAM_PORTS} = 2 × 64</b><small>业务端口上限，不决定 SSB 扫描容量</small></div>
        <div className="bh-capacity-warning"><span>重访预算敏感性</span><b>{capacityScenarios.map((item) => `${item.revisitMs} ms→${item.perSatellite}`).join(" · ")}</b><small>假设每20 ms occasion中，每个DL端口有 {SSB_VISITS_PER_DL_PORT_PER_OCCASION} 个顺序子访问；PHY/RU 未验证</small></div>
      </section>

      {demoTraffic && <p className="bh-example-banner"><b>合成 L2 业务已开启：</b>每个非空星载小区只生成最多 4 DL + 2 UL 的演示 demand，并附加一个 pending 与一个 control_only 样本；它们不是实测 UE 流量，关闭后数字端口全部保持空闲。</p>}
      <section className="bh-cells" aria-label={`${selectedSatelliteId} 的两个星载小区资源切片`}>
        {plans.map((plan) => <CellResourceLanes key={plan.cell.nci} plan={plan} phaseIndex={phaseIndex} selectedL1Id={beam.catalogId} />)}
      </section>

      <section className="bh-migration" aria-labelledby="bh-transfer-title">
        <header>
          <div><span>OPTIMIZER SNAPSHOT DELTA</span><h3 id="bh-transfer-title">{beam.catalogId} · {selectedDelta ? transferScope : "同小区跳波束"}</h3></div>
          <div className="bh-rf-owner"><span>本快照规划 owner</span><b>{selectedAssignment?.satelliteId ?? "UNASSIGNED"}</b><small>{selectedAssignment ? `${selectedAssignment.nci} · PCI ${selectedAssignment.pci}` : "显式容量/覆盖失败"}</small></div>
        </header>
        <div className="bh-migration-route">
          <div><span>SOURCE CELL</span><b>{sourceSatellite}</b><small>{selectedDelta ? `${selectedDelta.sourceNci} · PCI ${selectedDelta.sourcePci}` : "—"}</small></div><i aria-hidden="true">→</i><div><span>TARGET CELL</span><b>{targetSatellite}</b><small>{selectedDelta?.targetNci ? `${selectedDelta.targetNci} · PCI ${selectedDelta.targetPci}` : "—"}</small></div>
          <small>{selectedDelta ? "这里只是相邻规划快照差异；未预测activation仰角，也没有ready/applied证据" : "当前没有快照重分配差异；仍在同一星载小区内跳波束"}</small>
        </div>
        <ol className="bh-migration-steps">
          {transferSteps.map((step, index) => <li key={step.label} className={index === 0 ? "is-current" : ""}><i>{String(index + 1).padStart(2, "0")}</i><b>{step.label}</b><span>{step.detail}</span></li>)}
        </ol>
      </section>

      <section className="bh-access-contract" aria-label="接入和业务硬不变量">
        <div><b>空闲 L1 仍发 SSB</b><span>没有 UE 只会让 L2 保持空闲，不能取消 20 ms 小区基准与 80 ms L1 再访承诺。</span></div>
        <div><b>valid RO ⇒ UL beam_on</b><span>每个 L1 在 640 ms 内获得有效 PRACH 机会；PHY/MAC/DU 处理 preamble 与实时 RAR。</span></div>
        <div><b>CU-CP 统一接入策略</b><span>CU-CP 接收结构化接入事件，校验星历、波位表、ownership 与资源上下文，不接收原始 IQ。</span></div>
        <div><b>digital 必须 demand + applied</b><span>PDU/DRB 存在且执行层确认应用后才 ready；control_only 和 pending 均不占 L2 端口。</span></div>
      </section>

      <p className="bh-boundary">当前动画基于 2,620 个地固 L1、{baseline.planes}×{baseline.satellitesPerPlane}（{satellites.length} 星）参考星座和每星两个长期星载 NCI/PCI。NCI/PCI 不随波位移动；跨星时 position_id 保持、服务小区改变。真实 SSB 符号编排、PRACH format、功率/带宽、TA、Doppler、HARQ 与射频切换仍需后续链路和无线实现验证。</p>
    </div>
  );
}
