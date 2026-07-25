"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import { catalogGroundPoint, l1CellForAxis, l2CatalogIds } from "./beam-catalog";
import constellationAudit from "./constellation-audit.json";
import {
  assignmentForPoint,
  baseline,
  coverageRadiusKm,
  dailyCoverageStats,
  EARTH_RADIUS_KM,
  ORBIT_PERIOD_SECONDS,
  propagateSatellite,
  satellites,
  visibleStates,
  type GroundPoint,
} from "./orbit-model";

const THRESHOLDS = [25, 40, 50] as const;
const CHINA_GEO_OUTLINE: [number, number][] = [
  [74, 40], [78, 35], [80, 29], [85, 28], [89, 27], [92, 29], [97, 25], [102, 23],
  [108, 21], [112, 22], [116, 23], [120, 27], [122, 31], [124, 39], [128, 42], [134, 48],
  [130, 50], [123, 53], [116, 49], [110, 49], [103, 46], [96, 44], [90, 46], [82, 45], [74, 40],
];
const CONSTELLATION_AUDIT_SCENARIOS = [
  { key: "originalBaseline", eyebrow: "原容量基线", note: "保留为容量缺口对照" },
  { key: "minimumFeasible", eyebrow: "最小可行对照", note: "仅表示本轮采样刚好通过" },
  { key: "recommended", eyebrow: "历史中国样例采用方案", note: "比最小可行对照多 72 星，不等同 N-1 冗余" },
] as const;

function formatClock(seconds: number) {
  const normalized = ((Math.floor(seconds) % 86400) + 86400) % 86400;
  return `${String(Math.floor(normalized / 3600)).padStart(2, "0")}:${String(Math.floor(normalized % 3600 / 60)).padStart(2, "0")}:${String(normalized % 60).padStart(2, "0")}`;
}

function formatDelta(seconds: number | null, empty = "本窗口外") {
  if (seconds === null) return empty;
  if (seconds === 0) return "当前已满足";
  return `T+${Math.floor(seconds / 60)}m ${seconds % 60}s`;
}

function qualityLabel(elevation: number) {
  if (elevation >= 50) return "高质量";
  if (elevation >= 40) return "正常优选";
  if (elevation >= 25) return "硬可用";
  if (elevation >= 22) return "退出滞回";
  return "不可用";
}

function toScenePosition(ecef: [number, number, number], scale: number) {
  return [ecef[0] / EARTH_RADIUS_KM * scale, ecef[2] / EARTH_RADIUS_KM * scale, -ecef[1] / EARTH_RADIUS_KM * scale] as const;
}

function OrbitGlobe({ time, plane, selectedId, onSelect, point }: {
  time: number;
  plane: number | "all";
  selectedId: string;
  onSelect: (id: string) => void;
  point: GroundPoint;
}) {
  const mountRef = useRef<HTMLDivElement>(null);
  const live = useRef({ time, plane, selectedId, point, onSelect });
  const [fallback, setFallback] = useState(false);
  useEffect(() => { live.current = { time, plane, selectedId, point, onSelect }; }, [time, plane, selectedId, point, onSelect]);

  useEffect(() => {
    const mount = mountRef.current;
    if (!mount) return;
    const probe = document.createElement("canvas");
    if (!probe.getContext("webgl2") && !probe.getContext("webgl")) {
      const fallbackFrame = requestAnimationFrame(() => setFallback(true));
      return () => cancelAnimationFrame(fallbackFrame);
    }

    let stopped = false;
    let cleanup = () => {};
    void (async () => {
      try {
        const THREE = await import("three");
        const { OrbitControls } = await import("three/examples/jsm/controls/OrbitControls.js");
        if (stopped) return;
        const scene = new THREE.Scene();
        scene.background = new THREE.Color("#071b24");
        scene.fog = new THREE.Fog("#071b24", 8, 15);
        const camera = new THREE.PerspectiveCamera(38, 1, 0.1, 50);
        camera.position.set(0.2, 2.4, 8.2);
        const renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: "high-performance" });
        renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
        renderer.outputColorSpace = THREE.SRGBColorSpace;
        mount.appendChild(renderer.domElement);
        renderer.domElement.setAttribute("aria-label", `三维地球、${baseline.planes} 个轨道面和 ${satellites.length} 颗卫星`);

        const controls = new OrbitControls(camera, renderer.domElement);
        controls.enableDamping = true;
        controls.enablePan = false;
        controls.minDistance = 5;
        controls.maxDistance = 12;
        controls.autoRotate = true;
        controls.autoRotateSpeed = 0.32;

        scene.add(new THREE.AmbientLight(0x8fcde8, 1.4));
        const sun = new THREE.DirectionalLight(0xffffff, 2.1);
        sun.position.set(4, 4, 6);
        scene.add(sun);
        const globe = new THREE.Mesh(
          new THREE.SphereGeometry(2.25, 48, 32),
          new THREE.MeshPhongMaterial({ color: 0x0f5067, emissive: 0x062a38, shininess: 22, transparent: true, opacity: 0.96 }),
        );
        scene.add(globe);
        scene.add(new THREE.Mesh(
          new THREE.SphereGeometry(2.27, 28, 18),
          new THREE.MeshBasicMaterial({ color: 0x6fb7cc, wireframe: true, transparent: true, opacity: 0.12 }),
        ));

        const orbitGroup = new THREE.Group();
        const satelliteGroup = new THREE.Group();
        scene.add(orbitGroup, satelliteGroup);
        const baseGeometry = new THREE.SphereGeometry(0.033, 7, 5);
        const selectedGeometry = new THREE.SphereGeometry(0.075, 10, 8);
        const markerGeometry = new THREE.SphereGeometry(0.045, 8, 6);
        const planeMaterial = new THREE.LineBasicMaterial({ color: 0x3181a1, transparent: true, opacity: 0.2 });
        const satMaterial = new THREE.MeshBasicMaterial({ color: 0x83cbe5 });
        const mutedSatMaterial = new THREE.MeshBasicMaterial({ color: 0x315867 });
        const selectedMaterial = new THREE.MeshBasicMaterial({ color: 0xffb14a });
        const markerMaterial = new THREE.MeshBasicMaterial({ color: 0x55e6a8 });
        const raycaster = new THREE.Raycaster();
        const pointer = new THREE.Vector2();
        const meshes = new Map<string, InstanceType<typeof THREE.Mesh>>();
        let lastUpdate = -Infinity;

        for (let planeIndex = 1; planeIndex <= baseline.planes; planeIndex += 1) {
          const geometry = new THREE.BufferGeometry();
          const line = new THREE.LineLoop(geometry, planeMaterial.clone());
          line.userData.plane = planeIndex;
          orbitGroup.add(line);
        }
        satellites.forEach((satellite) => {
          const mesh = new THREE.Mesh(baseGeometry, satMaterial);
          mesh.userData.satelliteId = satellite.id;
          satelliteGroup.add(mesh);
          meshes.set(satellite.id, mesh);
        });
        const selectedMarker = new THREE.Mesh(selectedGeometry, selectedMaterial);
        scene.add(selectedMarker);
        const groundMarker = new THREE.Mesh(markerGeometry, markerMaterial);
        scene.add(groundMarker);

        const updateScene = () => {
          const state = live.current;
          orbitGroup.children.forEach((child) => {
            const line = child as InstanceType<typeof THREE.Line>;
            const planeNumber = Number(line.userData.plane);
            const seed = satellites[(planeNumber - 1) * baseline.satellitesPerPlane];
            const points = Array.from({ length: 96 }, (_, index) => {
              const synthetic = { ...seed, phaseDeg: seed.phaseDeg + index * 360 / 96 };
              const position = toScenePosition(propagateSatellite(synthetic, state.time).ecefKm, 2.25);
              return new THREE.Vector3(...position);
            });
            line.geometry.dispose();
            line.geometry = new THREE.BufferGeometry().setFromPoints(points);
            const material = line.material as InstanceType<typeof THREE.LineBasicMaterial>;
            material.color.set(state.plane === planeNumber ? 0xffb14a : 0x3181a1);
            material.opacity = state.plane === "all" || state.plane === planeNumber ? (state.plane === planeNumber ? 0.76 : 0.2) : 0.045;
          });
          satellites.forEach((definition) => {
            const mesh = meshes.get(definition.id)!;
            const stateNow = propagateSatellite(definition, state.time);
            mesh.position.set(...toScenePosition(stateNow.ecefKm, 2.43));
            mesh.material = state.plane === "all" || state.plane === definition.plane ? satMaterial : mutedSatMaterial;
            mesh.visible = definition.id !== state.selectedId;
          });
          const selectedDefinition = satellites.find((item) => item.id === state.selectedId) ?? satellites[0];
          selectedMarker.position.set(...toScenePosition(propagateSatellite(selectedDefinition, state.time).ecefKm, 2.43));
          const observer = [
            Math.cos(state.point.lat * Math.PI / 180) * Math.cos(state.point.lon * Math.PI / 180),
            Math.cos(state.point.lat * Math.PI / 180) * Math.sin(state.point.lon * Math.PI / 180),
            Math.sin(state.point.lat * Math.PI / 180),
          ] as [number, number, number];
          groundMarker.position.set(...toScenePosition(observer.map((value) => value * EARTH_RADIUS_KM) as [number, number, number], 2.29));
        };

        const resize = () => {
          const width = Math.max(1, mount.clientWidth);
          const height = Math.max(1, mount.clientHeight);
          renderer.setSize(width, height, false);
          camera.aspect = width / height;
          camera.updateProjectionMatrix();
        };
        const observer = new ResizeObserver(resize);
        observer.observe(mount);
        resize();

        const click = (event: MouseEvent) => {
          const bounds = renderer.domElement.getBoundingClientRect();
          pointer.set((event.clientX - bounds.left) / bounds.width * 2 - 1, -((event.clientY - bounds.top) / bounds.height) * 2 + 1);
          raycaster.setFromCamera(pointer, camera);
          const hit = raycaster.intersectObjects([...meshes.values()])[0];
          if (hit?.object.userData.satelliteId) live.current.onSelect(hit.object.userData.satelliteId);
        };
        renderer.domElement.addEventListener("click", click);

        let frame = 0;
        const animate = (timestamp: number) => {
          if (stopped) return;
          if (timestamp - lastUpdate >= 450) {
            updateScene();
            lastUpdate = timestamp;
          }
          controls.update();
          renderer.render(scene, camera);
          frame = requestAnimationFrame(animate);
        };
        frame = requestAnimationFrame(animate);

        cleanup = () => {
          cancelAnimationFrame(frame);
          observer.disconnect();
          renderer.domElement.removeEventListener("click", click);
          controls.dispose();
          scene.traverse((object) => {
            const candidate = object as InstanceType<typeof THREE.Mesh>;
            if (candidate.geometry) candidate.geometry.dispose();
            const materials = candidate.material ? (Array.isArray(candidate.material) ? candidate.material : [candidate.material]) : [];
            materials.forEach((material) => material.dispose());
          });
          renderer.dispose();
          renderer.domElement.remove();
        };
      } catch {
        setFallback(true);
      }
    })();
    return () => { stopped = true; cleanup(); };
  }, []);

  return (
    <div className="orbit-globe" ref={mountRef} data-testid="orbit-globe">
      {fallback && <div className="webgl-fallback" role="status"><b>WebGL 不可用</b><span>已切换到 2D 地面轨迹与数据表；轨道查询和接管计算仍可使用。</span></div>}
      <div className="globe-legend"><span><i className="legend-sat" />{satellites.length} 星</span><span><i className="legend-plane" />{baseline.planes} 面</span><span><i className="legend-ground" />当前 L1</span></div>
    </div>
  );
}

function CoverageMap({ time, threshold, selectedId, point, plane }: {
  time: number;
  threshold: number;
  selectedId: string;
  point: GroundPoint;
  plane: number | "all";
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const selected = satellites.find((item) => item.id === selectedId) ?? satellites[0];
  const selectedState = propagateSatellite(selected, time);
  const visible = visibleStates(time, point, threshold).filter((item) => plane === "all" || item.plane === plane);

  useEffect(() => {
    const canvas = canvasRef.current;
    const parent = canvas?.parentElement;
    if (!canvas || !parent) return;
    const draw = () => {
      const bounds = parent.getBoundingClientRect();
      const width = Math.max(320, Math.floor(bounds.width));
      const height = Math.max(245, Math.floor(bounds.height));
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      canvas.width = width * dpr;
      canvas.height = height * dpr;
      canvas.style.width = `${width}px`;
      canvas.style.height = `${height}px`;
      const context = canvas.getContext("2d");
      if (!context) return;
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
      context.fillStyle = "#eef1ed";
      context.fillRect(0, 0, width, height);
      const project = (lon: number, lat: number) => [
        18 + (lon - 72) / 64 * (width - 36),
        14 + (55 - lat) / 39 * (height - 28),
      ] as const;
      context.strokeStyle = "#d1d8d4";
      context.lineWidth = 1;
      for (let lon = 80; lon <= 130; lon += 10) {
        const [x] = project(lon, 20); context.beginPath(); context.moveTo(x, 0); context.lineTo(x, height); context.stroke();
      }
      for (let lat = 20; lat <= 50; lat += 10) {
        const [, y] = project(80, lat); context.beginPath(); context.moveTo(0, y); context.lineTo(width, y); context.stroke();
      }
      context.beginPath();
      CHINA_GEO_OUTLINE.forEach(([lon, lat], index) => {
        const [x, y] = project(lon, lat);
        if (index === 0) context.moveTo(x, y);
        else context.lineTo(x, y);
      });
      context.closePath(); context.fillStyle = "#dfe7df"; context.fill(); context.strokeStyle = "#748b87"; context.lineWidth = 1.3; context.stroke();
      const [hx, hy] = project(110.1, 19.2);
      context.beginPath(); context.ellipse(hx, hy, 5.5, 3.6, -0.2, 0, Math.PI * 2); context.fill(); context.stroke();

      const [satX, satY] = project(selectedState.lon, selectedState.lat);
      [...THRESHOLDS].reverse().forEach((level) => {
        const radius = coverageRadiusKm(level) / 111 * (height - 28) / 39;
        context.beginPath(); context.arc(satX, satY, radius, 0, Math.PI * 2);
        context.fillStyle = level === threshold ? "rgba(22,138,195,.12)" : "rgba(22,138,195,.035)";
        context.fill(); context.strokeStyle = level === threshold ? "#168ac3" : "rgba(22,138,195,.42)";
        context.lineWidth = level === threshold ? 2 : 1; context.setLineDash(level === threshold ? [] : [4, 4]); context.stroke();
      });
      context.setLineDash([]);
      context.fillStyle = "#ef8a3e"; context.beginPath(); context.arc(satX, satY, 4, 0, Math.PI * 2); context.fill();
      const [groundX, groundY] = project(point.lon, point.lat);
      context.fillStyle = "#10252d"; context.beginPath(); context.arc(groundX, groundY, 4.5, 0, Math.PI * 2); context.fill();
      context.strokeStyle = "#55c894"; context.lineWidth = 2; context.beginPath(); context.arc(groundX, groundY, 8, 0, Math.PI * 2); context.stroke();
      context.font = "700 10px ui-monospace, monospace"; context.fillStyle = "#10252d"; context.fillText(point.name, groundX + 10, groundY - 8);
    };
    draw();
    const observer = new ResizeObserver(draw); observer.observe(parent);
    return () => observer.disconnect();
  }, [point, selectedState.lat, selectedState.lon, threshold]);

  return (
    <div className="coverage-map" data-testid="coverage-map">
      <canvas ref={canvasRef} aria-label="大陆及海南二维卫星覆盖图" />
      <div className="coverage-count"><strong>{visible.length}</strong><span>{threshold}° 可见星</span><small>{qualityLabel(visible[0]?.elevationDeg ?? -90)}</small></div>
    </div>
  );
}

export function OrbitConstellationView({
  beam,
  time,
  onTimeChange,
  selectedId,
  onSelectedIdChange,
}: {
  beam: { id: string; u: number; v: number };
  time: number;
  onTimeChange: (time: number) => void;
  selectedId: string;
  onSelectedIdChange: (id: string) => void;
}) {
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState(60);
  const [threshold, setThreshold] = useState<number>(25);
  const [plane, setPlane] = useState<number | "all">("all");
  const [query, setQuery] = useState("P01-S01");
  const [gatewayUnavailable, setGatewayUnavailable] = useState<Set<string>>(() => new Set());
  const catalogCell = useMemo(() => l1CellForAxis(beam.u, beam.v), [beam.u, beam.v]);
  const point = useMemo(() => catalogGroundPoint(catalogCell), [catalogCell]);
  const dailyStats = useMemo(() => dailyCoverageStats(point), [point]);
  const childIds = useMemo(() => l2CatalogIds(catalogCell), [catalogCell]);
  const assignments = useMemo(() => assignmentForPoint(time, point, gatewayUnavailable), [time, point, gatewayUnavailable]);
  const selectedState = useMemo(() => propagateSatellite(satellites.find((item) => item.id === selectedId) ?? satellites[0], time), [selectedId, time]);
  const gatewayAvailable = !gatewayUnavailable.has(selectedId);

  useEffect(() => {
    if (!playing) return;
    const timer = window.setInterval(() => onTimeChange((time + speed * 0.25) % 86400), 250);
    return () => window.clearInterval(timer);
  }, [onTimeChange, playing, speed, time]);

  const locate = () => {
    const normalized = query.trim().toUpperCase();
    const satellite = satellites.find((item) => item.id === normalized);
    const planeMatch = normalized.match(/^P(\d{1,2})$/);
    if (satellite) {
      onSelectedIdChange(satellite.id); setPlane(satellite.plane); setQuery(satellite.id);
    } else if (planeMatch && Number(planeMatch[1]) >= 1 && Number(planeMatch[1]) <= baseline.planes) {
      const selectedPlane = Number(planeMatch[1]); setPlane(selectedPlane); onSelectedIdChange(`P${String(selectedPlane).padStart(2, "0")}-S01`);
    }
  };

  return (
    <div className="orbit-console" data-testid="orbit-console">
      <div className="orbit-toolbar">
        <div className="time-control">
          <button type="button" onClick={() => setPlaying((value) => !value)} aria-pressed={playing}>{playing ? "暂停" : "播放"}</button>
          <b>{formatClock(time)}</b>
          <input type="range" min="0" max="86400" step="10" value={Math.floor(time)} onChange={(event) => onTimeChange(Number(event.target.value))} aria-label="0 到 24 小时时间轴" />
        </div>
        <div className="orbit-segments" role="group" aria-label="播放速度">
          {[1, 60, 600].map((value) => <button type="button" key={value} className={speed === value ? "active" : ""} onClick={() => setSpeed(value)} aria-pressed={speed === value}>{value}×</button>)}
        </div>
        <div className="orbit-segments" role="group" aria-label="最低仰角">
          {THRESHOLDS.map((value) => <button type="button" key={value} className={threshold === value ? "active" : ""} onClick={() => setThreshold(value)} aria-pressed={threshold === value}>{value}°</button>)}
        </div>
        <label>轨道面<select value={plane} onChange={(event) => setPlane(event.target.value === "all" ? "all" : Number(event.target.value))}><option value="all">全部 {baseline.planes} 面</option>{Array.from({ length: baseline.planes }, (_, index) => <option key={index + 1} value={index + 1}>P{String(index + 1).padStart(2, "0")}</option>)}</select></label>
        <div className="orbit-search"><input list="satellite-ids" value={query} onChange={(event) => setQuery(event.target.value)} onKeyDown={(event) => { if (event.key === "Enter") locate(); }} aria-label="按卫星或轨道面定位" /><datalist id="satellite-ids">{satellites.map((satellite) => <option key={satellite.id} value={satellite.id} />)}</datalist><button type="button" onClick={locate}>定位</button></div>
      </div>

      <section className="constellation-audit" aria-labelledby="constellation-audit-title">
        <header><div><p>CONSTELLATION CAPACITY AUDIT</p><h3 id="constellation-audit-title">为什么从 {constellationAudit.originalBaseline.satelliteCount} 星增加到 {constellationAudit.recommended.satelliteCount} 星</h3></div><span>24 h · 120 s 采样 · 2,620 L1</span></header>
        <div className="constellation-audit-grid">
          {CONSTELLATION_AUDIT_SCENARIOS.map((scenario) => {
            const data = constellationAudit[scenario.key];
            const isRecommended = scenario.key === "recommended";
            const isFailed = data.successfulSamples < constellationAudit.model.sampleCount;
            return <article key={scenario.key} className={`${isRecommended ? "is-recommended" : ""} ${isFailed ? "is-failed" : ""}`}>
              <div><span>{scenario.eyebrow}</span><b>{isRecommended ? "当前" : isFailed ? "未通过" : "通过"}</b></div>
              <h4>{data.planes} × {data.satellitesPerPlane} <small>= {data.satelliteCount} 星</small></h4>
              <dl><div><dt>全量成功</dt><dd>{data.successfulSamples} / {constellationAudit.model.sampleCount}</dd></div><div><dt>未分配 L1</dt><dd>{data.minimumUnassigned} / {data.averageUnassigned.toFixed(1)} / {data.maximumUnassigned}</dd></div></dl>
              <p>{scenario.note} · 参与服务 {data.minimumActiveSatellites}–{data.maximumActiveSatellites} 星</p>
            </article>;
          })}
        </div>
        <p className="constellation-audit-note"><b>选择 {constellationAudit.recommended.planes} × {constellationAudit.recommended.satellitesPerPlane}：</b>增加轨道面更贴合空间容量缺口；Walker 相位改变会让结果不随总星数单调增长。{constellationAudit.recommended.successfulSamples} / {constellationAudit.model.sampleCount} 只是一天离线采样通过；聚合候选容量账本余量 {constellationAudit.recommended.minimumFleetCapacityHeadroom.toLocaleString("en-US")} 不代表几何或 N-1 余量。</p>
      </section>

      <div className="orbit-visual-grid">
        <section className="orbit-card globe-card"><header><div><p>ORBIT SHELL · {baseline.shell}</p><h3>{baseline.walker}</h3></div><span>{baseline.altitudeKm} km · 周期 {(ORBIT_PERIOD_SECONDS / 60).toFixed(1)} min</span></header><OrbitGlobe time={time} plane={plane} selectedId={selectedId} onSelect={(id) => { onSelectedIdChange(id); setQuery(id); }} point={point} /></section>
        <section className="orbit-card map-card"><header><div><p>SERVICE MASK</p><h3>大陆及海南覆盖</h3></div><span>台湾 / 南海诸岛不计验收</span></header><CoverageMap time={time} threshold={threshold} selectedId={selectedId} point={point} plane={plane} /></section>
      </div>

      <div className="orbit-detail-grid">
        <section className="orbit-detail selected-satellite"><p>当前卫星</p><h3>{selectedId}</h3><dl><div><dt>轨道面 / 槽位</dt><dd>P{String(satellites.find((item) => item.id === selectedId)?.plane ?? 1).padStart(2, "0")} / S{String(satellites.find((item) => item.id === selectedId)?.slot ?? 1).padStart(2, "0")}</dd></div><div><dt>星下点</dt><dd>{selectedState.lat.toFixed(2)}° / {selectedState.lon.toFixed(2)}°</dd></div><div><dt>Gateway / feeder</dt><dd><button type="button" className={`gateway-state ${gatewayAvailable ? "available" : ""}`} onClick={() => setGatewayUnavailable((current) => { const next = new Set(current); if (next.has(selectedId)) next.delete(selectedId); else next.add(selectedId); return next; })}>{gatewayAvailable ? "外部确认可用" : "外部标记不可用"}</button></dd></div></dl></section>
        <section className="orbit-detail assignment-panel"><p>候选星 · {point.lat.toFixed(2)}°N {point.lon.toFixed(2)}°E</p><div className="assignment-cards">{assignments.slice(0, 3).map((item) => <button type="button" key={item.id} onClick={() => { onSelectedIdChange(item.id); setQuery(item.id); }} className={item.id === selectedId ? "selected" : ""}><span>{item.role}</span><b>{item.id}</b><strong>{item.elevationDeg.toFixed(1)}°</strong><small>进入 {formatDelta(item.entrySeconds)} · 退出 {formatDelta(item.exitSeconds)}</small></button>)}</div><ol className="takeover-timeline"><li className="done"><b>candidate</b><span>当前 L1 的候选全集不受服务容量裁剪</span></li><li className={assignments.some((item) => item.role === "preheated") ? "active" : ""}><b>resource ready</b><span>目标端口 applied 后可用不同 NCI / PCI 广播发现信号</span></li><li><b>measurement overlap</b><span>源、目标短时同时可测，UE 执行跨小区切换</span></li><li><b>position transfer</b><span>计划 epoch 上服务所有权转给目标，源星随后清理该波位</span></li></ol></section>
      </div>

      <section className="beam-constellation-plan" aria-label="星座约束下的波位规划">
        <header><div><p>CONSTELLATION × SELECTED L1</p><h3>所选 L1 的几何候选与调度输入</h3></div><span className={dailyStats.minimumVisible >= 1 && dailyStats.dualCoverageRate >= 0.97 ? "pass" : "review"}>{dailyStats.minimumVisible >= 1 && dailyStats.dualCoverageRate >= 0.97 ? "单点通过" : "单点复核"}</span></header>
        <div className="beam-plan-body">
          <article className="catalog-identity"><span>L1 信令短号</span><strong>{catalogCell.id}</strong><small>{catalogCell.kind === "full" ? "完整 7 波位组" : `边缘 ${catalogCell.childCount} 波位组`} · 机器坐标保留在导出属性</small><div className="child-id-row">{childIds.map((id) => <b key={id}>{id}</b>)}</div></article>
          <div className="coverage-kpis"><article><span>25° 最少可见星</span><strong>{dailyStats.minimumVisible}</strong><small>必须 ≥ 1</small></article><article><span>双星覆盖率</span><strong>{(dailyStats.dualCoverageRate * 100).toFixed(1)}%</strong><small>核心目标 ≥ 97%</small></article><article><span>40° 优选覆盖</span><strong>{(dailyStats.preferredCoverageRate * 100).toFixed(1)}%</strong><small>质量统计</small></article><article><span>50° 高质量覆盖</span><strong>{(dailyStats.premiumCoverageRate * 100).toFixed(1)}%</strong><small>质量统计</small></article><article><span>24h 最优星变化</span><strong>{dailyStats.primaryChanges}</strong><small>不是最终接管次数</small></article></div>
        </div>
        <div className="planning-layers"><div><b>固定目录</b><span>{catalogCell.id} 与 {catalogCell.childCount} 个 L2 只保存地固 position_id</span></div><i>→</i><div><b>逐波位候选</b><span>{baseline.planes}×{baseline.satellitesPerPlane} 星座按 L1 的 25°/22° 门限生成全集</span></div><i>→</i><div><b>星载双小区</b><span>先分配 position_id，再在每颗卫星内划入两个 NCI / PCI</span></div></div>
        <p className="orbit-method-note"><b>联合仿真口径：</b>调度对象改为单个 L1，不再把一组地面波位捆绑成整体候选。每星 84 个 L1 依赖“20 ms SSB occasion 内每个 DL 模拟端口可顺序访问两个波位”的一期假设，仍须 PHY/RU 验证。</p>
      </section>

      <details className="orbit-data-table"><summary>无 WebGL 数据表 / 当前候选详情</summary><div className="table-scroll"><table><thead><tr><th>角色</th><th>卫星</th><th>轨道面</th><th>仰角</th><th>质量</th><th>预计进入</th><th>预计退出</th></tr></thead><tbody>{assignments.map((item) => <tr key={`${item.id}-${item.role}`}><td>{item.role}</td><td>{item.id}</td><td>P{String(item.plane).padStart(2, "0")}</td><td>{item.elevationDeg.toFixed(1)}°</td><td>{qualityLabel(item.elevationDeg)}</td><td>{formatDelta(item.entrySeconds)}</td><td>{formatDelta(item.exitSeconds)}</td></tr>)}</tbody></table></div></details>
      <p className="orbit-method-note">传播模型：二体圆轨道 + 地球自转。TLE / SGP4 / J2、gateway 物理选址、碰撞分析与真实 Doppler / TA / HARQ 执行为后续外部工程。</p>
    </div>
  );
}
