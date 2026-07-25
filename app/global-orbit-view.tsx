"use client";

import { useEffect, useRef, useState } from "react";
import { geoEquirectangular, geoGraticule10, geoPath, type GeoPermissibleObjects } from "d3-geo";
import * as THREE from "three";
import { landFeatureFromTopology, type LandTopology } from "./land-topology";
import {
  EARTH_RADIUS_KM,
  ORBIT_RADIUS_KM,
  propagateSatellite,
  satellites,
  type SatelliteDefinition,
} from "./orbit-model";

type GlobalOrbitViewProps = {
  timeSeconds: number;
  selectedSatelliteId: string;
  focusRequestId: number;
  landUrl: string;
  onSelectSatellite: (id: string) => void;
};

const shellRadius = ORBIT_RADIUS_KM / EARTH_RADIUS_KM;

function createEarthTexture(land: GeoPermissibleObjects) {
  const canvas = document.createElement("canvas");
  canvas.width = 2048;
  canvas.height = 1024;
  const context = canvas.getContext("2d");
  if (!context) throw new Error("2D canvas is unavailable");

  const projection = geoEquirectangular()
    .translate([canvas.width / 2, canvas.height / 2])
    .scale(canvas.width / (2 * Math.PI))
    .precision(0.15);
  const path = geoPath(projection, context);

  const ocean = context.createLinearGradient(0, 0, 0, canvas.height);
  ocean.addColorStop(0, "#071f2d");
  ocean.addColorStop(0.28, "#0c3446");
  ocean.addColorStop(0.5, "#12465a");
  ocean.addColorStop(0.72, "#0c3446");
  ocean.addColorStop(1, "#071f2d");
  context.fillStyle = ocean;
  context.fillRect(0, 0, canvas.width, canvas.height);

  context.beginPath();
  path(geoGraticule10());
  context.strokeStyle = "rgba(174, 214, 220, 0.13)";
  context.lineWidth = 1;
  context.stroke();

  context.save();
  context.beginPath();
  path(land);
  context.clip();
  const terrain = context.createLinearGradient(0, 0, 0, canvas.height);
  terrain.addColorStop(0, "#aeb9a2");
  terrain.addColorStop(0.2, "#809577");
  terrain.addColorStop(0.5, "#54745c");
  terrain.addColorStop(0.8, "#809577");
  terrain.addColorStop(1, "#aeb9a2");
  context.fillStyle = terrain;
  context.fillRect(0, 0, canvas.width, canvas.height);
  context.restore();

  context.beginPath();
  path(land);
  context.strokeStyle = "rgba(226, 235, 209, 0.82)";
  context.lineWidth = 1.4;
  context.stroke();

  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  return texture;
}

export function GlobalOrbitView({
  timeSeconds,
  selectedSatelliteId,
  focusRequestId,
  landUrl,
  onSelectSatellite,
}: GlobalOrbitViewProps) {
  const hostRef = useRef<HTMLDivElement>(null);
  const lastFocusRequestRef = useRef(-1);
  const stateRef = useRef<{
    renderer: THREE.WebGLRenderer;
    camera: THREE.PerspectiveCamera;
    scene: THREE.Scene;
    world: THREE.Group;
    instances: THREE.InstancedMesh;
    selected: THREE.Mesh;
    orbitLine: THREE.Line;
    frame: number;
  } | null>(null);
  const [webglAvailable, setWebglAvailable] = useState(true);
  const [earthSurfaceStatus, setEarthSurfaceStatus] = useState<"loading" | "ready" | "error">("loading");

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return undefined;
    let renderer: THREE.WebGLRenderer;
    try {
      renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false, powerPreference: "high-performance" });
    } catch {
      const timer = window.setTimeout(() => setWebglAvailable(false), 0);
      return () => window.clearTimeout(timer);
    }

    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    renderer.setClearColor(0x0b222b, 1);
    host.appendChild(renderer.domElement);
    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(40, 1, 0.1, 100);
    camera.position.set(0, 0.2, 4.2);
    camera.lookAt(0, 0, 0);
    const world = new THREE.Group();
    scene.add(world);

    const earthMaterial = new THREE.MeshStandardMaterial({
      color: 0x173b45,
      roughness: 0.88,
      metalness: 0.02,
    });
    const earth = new THREE.Mesh(
      new THREE.SphereGeometry(1, 96, 64),
      earthMaterial,
    );
    world.add(earth);
    let earthTexture: THREE.CanvasTexture | null = null;
    let cancelled = false;
    const landRequest = new AbortController();
    fetch(landUrl, { signal: landRequest.signal })
      .then((response) => {
        if (!response.ok) throw new Error(`land request failed with ${response.status}`);
        return response.json() as Promise<LandTopology>;
      })
      .then((payload) => {
        if (cancelled) return;
        earthTexture = createEarthTexture(landFeatureFromTopology(payload));
        earthTexture.anisotropy = Math.min(8, renderer.capabilities.getMaxAnisotropy());
        earthMaterial.map = earthTexture;
        earthMaterial.color.set(0xffffff);
        earthMaterial.needsUpdate = true;
        setEarthSurfaceStatus("ready");
      })
      .catch((error: unknown) => {
        if (cancelled) return;
        if (error instanceof DOMException && error.name === "AbortError") return;
        setEarthSurfaceStatus("error");
      });

    const bandGeometry = new THREE.BufferGeometry();
    const bandPoints: THREE.Vector3[] = [];
    for (const latitude of [-57, 57]) {
      const latitudeRad = latitude * Math.PI / 180;
      for (let index = 0; index <= 128; index += 1) {
        const longitude = index / 128 * Math.PI * 2;
        bandPoints.push(new THREE.Vector3(
          Math.cos(latitudeRad) * Math.cos(longitude),
          Math.sin(latitudeRad),
          Math.cos(latitudeRad) * Math.sin(longitude),
        ));
      }
    }
    bandGeometry.setFromPoints(bandPoints);
    const band = new THREE.LineSegments(bandGeometry, new THREE.LineBasicMaterial({ color: 0x5c98a6, transparent: true, opacity: 0.65 }));
    world.add(band);

    const satelliteGeometry = new THREE.IcosahedronGeometry(0.009, 0);
    const satelliteMaterial = new THREE.MeshBasicMaterial({ color: 0x70cfee });
    const instances = new THREE.InstancedMesh(satelliteGeometry, satelliteMaterial, satellites.length);
    instances.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
    world.add(instances);

    const selected = new THREE.Mesh(
      new THREE.IcosahedronGeometry(0.028, 1),
      new THREE.MeshBasicMaterial({ color: 0xffb95c }),
    );
    world.add(selected);

    const orbitLine = new THREE.Line(
      new THREE.BufferGeometry(),
      new THREE.LineBasicMaterial({ color: 0xffb95c, transparent: true, opacity: 0.7 }),
    );
    world.add(orbitLine);

    scene.add(new THREE.AmbientLight(0xffffff, 1.4));
    const key = new THREE.DirectionalLight(0xb8e8ff, 2.2);
    key.position.set(2, 1.5, 3);
    scene.add(key);

    const resize = () => {
      const { width, height } = host.getBoundingClientRect();
      renderer.setSize(Math.max(1, width), Math.max(1, height), false);
      camera.aspect = Math.max(1, width) / Math.max(1, height);
      camera.updateProjectionMatrix();
    };
    const observer = new ResizeObserver(resize);
    observer.observe(host);
    resize();

    let dragging = false;
    let previousX = 0;
    let previousY = 0;
    const down = (event: PointerEvent) => {
      dragging = true;
      previousX = event.clientX;
      previousY = event.clientY;
      renderer.domElement.setPointerCapture(event.pointerId);
    };
    const move = (event: PointerEvent) => {
      if (!dragging) return;
      world.rotation.y += (event.clientX - previousX) * 0.005;
      world.rotation.x = THREE.MathUtils.clamp(world.rotation.x + (event.clientY - previousY) * 0.005, -1.2, 1.2);
      previousX = event.clientX;
      previousY = event.clientY;
    };
    const up = (event: PointerEvent) => {
      if (!dragging) return;
      const movement = Math.hypot(event.clientX - previousX, event.clientY - previousY);
      dragging = false;
      if (movement > 3) return;
      const bounds = renderer.domElement.getBoundingClientRect();
      const pointer = new THREE.Vector2(
        (event.clientX - bounds.left) / bounds.width * 2 - 1,
        -(event.clientY - bounds.top) / bounds.height * 2 + 1,
      );
      const raycaster = new THREE.Raycaster();
      raycaster.setFromCamera(pointer, camera);
      const hit = raycaster.intersectObject(instances, false)[0];
      if (hit?.instanceId !== undefined) onSelectSatellite(satellites[hit.instanceId].id);
    };
    renderer.domElement.addEventListener("pointerdown", down);
    renderer.domElement.addEventListener("pointermove", move);
    renderer.domElement.addEventListener("pointerup", up);

    const render = () => {
      renderer.render(scene, camera);
      if (stateRef.current) stateRef.current.frame = requestAnimationFrame(render);
    };
    stateRef.current = { renderer, camera, scene, world, instances, selected, orbitLine, frame: requestAnimationFrame(render) };

    return () => {
      cancelled = true;
      landRequest.abort();
      observer.disconnect();
      renderer.domElement.removeEventListener("pointerdown", down);
      renderer.domElement.removeEventListener("pointermove", move);
      renderer.domElement.removeEventListener("pointerup", up);
      if (stateRef.current) cancelAnimationFrame(stateRef.current.frame);
      stateRef.current = null;
      world.traverse((object) => {
        if (object instanceof THREE.Mesh || object instanceof THREE.Line || object instanceof THREE.LineSegments) {
          object.geometry.dispose();
          const material = object.material;
          if (Array.isArray(material)) material.forEach((item) => item.dispose());
          else material.dispose();
        }
      });
      earthTexture?.dispose();
      renderer.dispose();
      renderer.domElement.remove();
    };
  }, [landUrl, onSelectSatellite]);

  useEffect(() => {
    const state = stateRef.current;
    if (!state) return;
    const matrix = new THREE.Matrix4();
    satellites.forEach((definition, index) => {
      const propagated = propagateSatellite(definition, timeSeconds);
      matrix.makeTranslation(
        propagated.ecefKm[0] / EARTH_RADIUS_KM,
        propagated.ecefKm[2] / EARTH_RADIUS_KM,
        -propagated.ecefKm[1] / EARTH_RADIUS_KM,
      );
      state.instances.setMatrixAt(index, matrix);
    });
    state.instances.instanceMatrix.needsUpdate = true;

    const definition = satellites.find(({ id }) => id === selectedSatelliteId) ?? satellites[0];
    const propagated = propagateSatellite(definition, timeSeconds);
    state.selected.position.set(
      propagated.ecefKm[0] / EARTH_RADIUS_KM,
      propagated.ecefKm[2] / EARTH_RADIUS_KM,
      -propagated.ecefKm[1] / EARTH_RADIUS_KM,
    );
    if (lastFocusRequestRef.current !== focusRequestId) {
      const selectedDirection = state.selected.position.clone().normalize();
      const cameraDirection = state.camera.position.clone().normalize();
      state.world.quaternion.setFromUnitVectors(selectedDirection, cameraDirection);
      lastFocusRequestRef.current = focusRequestId;
    }

    const points: THREE.Vector3[] = [];
    for (let index = 0; index <= 144; index += 1) {
      const synthetic: SatelliteDefinition = { ...definition, phaseDeg: index / 144 * 360 };
      const sample = propagateSatellite(synthetic, timeSeconds);
      points.push(new THREE.Vector3(
        sample.ecefKm[0] / EARTH_RADIUS_KM,
        sample.ecefKm[2] / EARTH_RADIUS_KM,
        -sample.ecefKm[1] / EARTH_RADIUS_KM,
      ));
    }
    state.orbitLine.geometry.dispose();
    state.orbitLine.geometry = new THREE.BufferGeometry().setFromPoints(points);
  }, [focusRequestId, selectedSatelliteId, timeSeconds]);

  if (!webglAvailable) {
    return (
      <div className="orbit-fallback" role="status">
        <b>WebGL 不可用</b>
        <span>已切换到二维地图和数据表；轨道传播与波位审计仍可查看。</span>
      </div>
    );
  }

  return (
    <div className="global-orbit-canvas" ref={hostRef} aria-label="500 km 轨道运行示意">
      <div className="orbit-overlay"><span>真实陆地轮廓 · 轨道运行示意</span><b>卫星轨迹与覆盖关系</b><small>{earthSurfaceStatus === "ready" ? "陆地边界已加载" : earthSurfaceStatus === "error" ? "陆地边界加载失败，显示基础地球" : "正在加载陆地边界"} · 拖动旋转 · 点击选星</small></div>
      <span className="sr-only">轨道半径为地球半径的 {shellRadius.toFixed(3)} 倍。</span>
    </div>
  );
}
