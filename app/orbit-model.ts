import baseline from "./orbit-baseline.json";

export const EARTH_RADIUS_KM = 6378.137;
export const EARTH_MU_KM3_S2 = 398600.4418;
export const EARTH_ROTATION_RAD_S = 7.2921159e-5;
export const ORBIT_RADIUS_KM = EARTH_RADIUS_KM + baseline.altitudeKm;
export const ORBIT_PERIOD_SECONDS = 2 * Math.PI * Math.sqrt(ORBIT_RADIUS_KM ** 3 / EARTH_MU_KM3_S2);

export type Vector3Tuple = [number, number, number];
export type GroundPoint = { name: string; lat: number; lon: number };
export type WalkerDeltaConfig = {
  readonly planes: number;
  readonly satellitesPerPlane: number;
  readonly phaseFactor?: number;
  readonly altitudeKm?: number;
  readonly inclinationDeg?: number;
  readonly raanOffsetDeg?: number;
  readonly phaseOffsetDeg?: number;
};
export type ResolvedWalkerDeltaConfig = {
  readonly planes: number;
  readonly satellitesPerPlane: number;
  readonly phaseFactor: number;
  readonly altitudeKm: number;
  readonly inclinationDeg: number;
  readonly raanOffsetDeg: number;
  readonly phaseOffsetDeg: number;
};
export type SatelliteDefinition = {
  id: string;
  plane: number;
  slot: number;
  raanDeg: number;
  phaseDeg: number;
  altitudeKm: number;
  inclinationDeg: number;
};

export type SatelliteState = SatelliteDefinition & {
  ecefKm: Vector3Tuple;
  lat: number;
  lon: number;
};

export type AssignmentSatellite = SatelliteState & {
  elevationDeg: number;
  entrySeconds: number | null;
  exitSeconds: number | null;
  role: "primary" | "candidate" | "preheated" | "released";
};

export type DailyCoverageStats = {
  minimumVisible: number;
  dualCoverageRate: number;
  preferredCoverageRate: number;
  premiumCoverageRate: number;
  primaryChanges: number;
};

export type ConstellationAuditStatus = "seed" | "coarse" | "exact_pass" | "exact_fail";
export type ConstellationAuditScenario = {
  readonly id: string;
  readonly auditStatus: ConstellationAuditStatus;
};

/** Prevents a seed or sampled/coarse run from becoming the accepted scenario. */
export function validateSelectedConstellationScenario(
  scenarios: readonly ConstellationAuditScenario[],
  selectedScenario: string | null,
) {
  if (selectedScenario === null) return null;
  const scenario = scenarios.find((candidate) => candidate.id === selectedScenario);
  if (!scenario) throw new Error(`Unknown selected constellation scenario ${selectedScenario}`);
  if (scenario.auditStatus !== "exact_pass") {
    throw new Error(`Scenario ${selectedScenario} is ${scenario.auditStatus}; only exact_pass may be selected`);
  }
  return scenario;
}

const toRadians = (value: number) => value * Math.PI / 180;
const toDegrees = (value: number) => value * 180 / Math.PI;
const normalizeLongitude = (value: number) => ((value + 540) % 360) - 180;

function positiveInteger(value: number, label: string) {
  if (!Number.isInteger(value) || value <= 0) throw new Error(`${label} must be a positive integer`);
  return value;
}

/**
 * Creates a parameterized circular Walker Delta catalog.  RAAN and phase
 * offsets are explicit so seed/coarse/exact sweeps can be reproduced without
 * silently changing the epoch geometry.
 */
export function createWalkerDelta(config: WalkerDeltaConfig): SatelliteDefinition[] {
  const planes = positiveInteger(config.planes, "planes");
  const satellitesPerPlane = positiveInteger(config.satellitesPerPlane, "satellitesPerPlane");
  const phaseFactor = config.phaseFactor ?? baseline.phaseFactor;
  if (!Number.isInteger(phaseFactor) || phaseFactor < 0 || phaseFactor >= planes) {
    throw new Error("phaseFactor must be an integer in the range 0..planes-1");
  }
  const altitudeKm = config.altitudeKm ?? baseline.altitudeKm;
  const inclinationDeg = config.inclinationDeg ?? baseline.inclinationDeg;
  const raanOffsetDeg = config.raanOffsetDeg ?? baseline.raanOffsetDeg;
  const phaseOffsetDeg = config.phaseOffsetDeg ?? baseline.phaseOffsetDeg;
  if (!Number.isFinite(altitudeKm) || altitudeKm <= 0) throw new Error("altitudeKm must be positive");
  if (!Number.isFinite(inclinationDeg) || inclinationDeg < 0 || inclinationDeg > 180) {
    throw new Error("inclinationDeg must be in the range 0..180");
  }
  if (!Number.isFinite(raanOffsetDeg) || !Number.isFinite(phaseOffsetDeg)) {
    throw new Error("RAAN and phase offsets must be finite");
  }
  const planeDigits = Math.max(2, String(planes).length);
  const slotDigits = Math.max(2, String(satellitesPerPlane).length);
  return Array.from({ length: planes }, (_, planeIndex) =>
    Array.from({ length: satellitesPerPlane }, (_, slotIndex) => ({
      id: `P${String(planeIndex + 1).padStart(planeDigits, "0")}-S${String(slotIndex + 1).padStart(slotDigits, "0")}`,
      plane: planeIndex + 1,
      slot: slotIndex + 1,
      raanDeg: raanOffsetDeg + planeIndex * (360 / planes),
      phaseDeg: phaseOffsetDeg + slotIndex * (360 / satellitesPerPlane)
        + planeIndex * phaseFactor * 360 / (planes * satellitesPerPlane),
      altitudeKm,
      inclinationDeg,
    })),
  ).flat();
}

export const baselineWalkerDelta: Readonly<ResolvedWalkerDeltaConfig> = Object.freeze({
  planes: baseline.planes,
  satellitesPerPlane: baseline.satellitesPerPlane,
  phaseFactor: baseline.phaseFactor,
  altitudeKm: baseline.altitudeKm,
  inclinationDeg: baseline.inclinationDeg,
  raanOffsetDeg: baseline.raanOffsetDeg,
  phaseOffsetDeg: baseline.phaseOffsetDeg,
});

export const satellites: SatelliteDefinition[] = createWalkerDelta(baselineWalkerDelta);

export function coverageRadiusKm(elevationDeg: number, altitudeKm = baseline.altitudeKm) {
  const elevation = toRadians(elevationDeg);
  const orbitRadiusKm = EARTH_RADIUS_KM + altitudeKm;
  const centralAngle = Math.acos(EARTH_RADIUS_KM / orbitRadiusKm * Math.cos(elevation)) - elevation;
  return EARTH_RADIUS_KM * centralAngle;
}

export function propagateSatellite(definition: SatelliteDefinition, secondsSinceEpoch: number): SatelliteState {
  const inclination = toRadians(definition.inclinationDeg);
  const orbitRadiusKm = EARTH_RADIUS_KM + definition.altitudeKm;
  const orbitPeriodSeconds = 2 * Math.PI * Math.sqrt(orbitRadiusKm ** 3 / EARTH_MU_KM3_S2);
  const raan = toRadians(definition.raanDeg);
  const argument = toRadians(definition.phaseDeg) + secondsSinceEpoch * 2 * Math.PI / orbitPeriodSeconds;
  const cosRaan = Math.cos(raan);
  const sinRaan = Math.sin(raan);
  const cosArgument = Math.cos(argument);
  const sinArgument = Math.sin(argument);
  const cosInclination = Math.cos(inclination);
  const sinInclination = Math.sin(inclination);

  const xEci = orbitRadiusKm * (cosRaan * cosArgument - sinRaan * sinArgument * cosInclination);
  const yEci = orbitRadiusKm * (sinRaan * cosArgument + cosRaan * sinArgument * cosInclination);
  const zEci = orbitRadiusKm * sinArgument * sinInclination;
  const earthAngle = EARTH_ROTATION_RAD_S * secondsSinceEpoch;
  const cosEarth = Math.cos(earthAngle);
  const sinEarth = Math.sin(earthAngle);
  const x = cosEarth * xEci + sinEarth * yEci;
  const y = -sinEarth * xEci + cosEarth * yEci;
  const z = zEci;
  const lat = toDegrees(Math.asin(z / orbitRadiusKm));
  const lon = normalizeLongitude(toDegrees(Math.atan2(y, x)));

  return { ...definition, ecefKm: [x, y, z], lat, lon };
}

export function groundEcef(point: Pick<GroundPoint, "lat" | "lon">): Vector3Tuple {
  const lat = toRadians(point.lat);
  const lon = toRadians(point.lon);
  return [
    EARTH_RADIUS_KM * Math.cos(lat) * Math.cos(lon),
    EARTH_RADIUS_KM * Math.cos(lat) * Math.sin(lon),
    EARTH_RADIUS_KM * Math.sin(lat),
  ];
}

export function elevationDeg(state: SatelliteState, point: Pick<GroundPoint, "lat" | "lon">) {
  const observer = groundEcef(point);
  const los: Vector3Tuple = [
    state.ecefKm[0] - observer[0],
    state.ecefKm[1] - observer[1],
    state.ecefKm[2] - observer[2],
  ];
  const distance = Math.hypot(...los);
  const up = observer.map((value) => value / EARTH_RADIUS_KM) as Vector3Tuple;
  return toDegrees(Math.asin((los[0] * up[0] + los[1] * up[1] + los[2] * up[2]) / distance));
}

export function visibleStates(secondsSinceEpoch: number, point: GroundPoint, minimumElevationDeg = baseline.thresholdsDeg.release) {
  return satellites
    .map((definition) => {
      const state = propagateSatellite(definition, secondsSinceEpoch);
      return { ...state, elevationDeg: elevationDeg(state, point) };
    })
    .filter((state) => state.elevationDeg >= minimumElevationDeg)
    .sort((left, right) => right.elevationDeg - left.elevationDeg);
}

function secondsToThreshold(definition: SatelliteDefinition, point: GroundPoint, time: number, threshold: number, direction: "entry" | "exit", horizon = 1800) {
  let previous = elevationDeg(propagateSatellite(definition, time), point);
  for (let delta = 5; delta <= horizon; delta += 5) {
    const current = elevationDeg(propagateSatellite(definition, time + delta), point);
    if (direction === "entry" && previous < threshold && current >= threshold) return delta;
    if (direction === "exit" && previous >= threshold && current < threshold) return delta;
    previous = current;
  }
  return null;
}

export function assignmentForPoint(secondsSinceEpoch: number, point: GroundPoint, unavailableSatelliteIds: ReadonlySet<string> = new Set()): AssignmentSatellite[] {
  const hard = baseline.thresholdsDeg.hard;
  const release = baseline.thresholdsDeg.release;
  const holdEpoch = Math.floor(secondsSinceEpoch / 300) * 300;
  const held = visibleStates(holdEpoch, point, release).find((state) => !unavailableSatelliteIds.has(state.id));
  const current = visibleStates(secondsSinceEpoch, point, -90).filter((state) => !unavailableSatelliteIds.has(state.id));
  const currentById = new Map(current.map((state) => [state.id, state]));
  let primary = held ? currentById.get(held.id) : undefined;
  const released = primary && primary.elevationDeg < release ? primary : undefined;
  if (!primary || primary.elevationDeg < release) primary = current.find((state) => state.elevationDeg >= hard);

  const eligible = current.filter((state) => state.id !== primary?.id && (
    state.elevationDeg >= hard || secondsToThreshold(state, point, secondsSinceEpoch, hard, "entry", 120) !== null
  ));
  const target = eligible[0];
  const primaryExit = primary ? secondsToThreshold(primary, point, secondsSinceEpoch, release, "exit") : null;
  const preheated = Boolean(target && primaryExit !== null && primaryExit <= 60);

  const decorate = (state: typeof current[number], role: AssignmentSatellite["role"]): AssignmentSatellite => ({
    ...state,
    role,
    entrySeconds: state.elevationDeg >= hard ? 0 : secondsToThreshold(state, point, secondsSinceEpoch, hard, "entry", 1800),
    exitSeconds: state.elevationDeg >= hard ? secondsToThreshold(state, point, secondsSinceEpoch, hard, "exit", 1800) : null,
  });

  const result: AssignmentSatellite[] = [];
  if (primary) result.push(decorate(primary, "primary"));
  eligible.slice(0, 2).forEach((state, index) => result.push(decorate(
    state,
    index === 0 && preheated ? "preheated" : "candidate",
  )));
  if (released && released.id !== primary?.id && result.length < 4) result.push(decorate(released, "released"));
  return result;
}

export function dailyCoverageStats(point: GroundPoint): DailyCoverageStats {
  const samples = 24 * 60 / 2;
  let minimumVisible = satellites.length;
  let dualSamples = 0;
  let preferredSamples = 0;
  let premiumSamples = 0;
  let primaryChanges = 0;
  let previousPrimary = "";
  for (let sample = 0; sample < samples; sample += 1) {
    const time = sample * 120;
    let visible = 0;
    let preferred = 0;
    let premium = 0;
    let primary = "";
    let primaryElevation = -90;
    satellites.forEach((definition) => {
      const state = propagateSatellite(definition, time);
      const elevation = elevationDeg(state, point);
      if (elevation >= baseline.thresholdsDeg.hard) visible += 1;
      if (elevation >= baseline.thresholdsDeg.preferred) preferred += 1;
      if (elevation >= baseline.thresholdsDeg.premium) premium += 1;
      if (elevation > primaryElevation) {
        primaryElevation = elevation;
        primary = definition.id;
      }
    });
    minimumVisible = Math.min(minimumVisible, visible);
    if (visible >= 2) dualSamples += 1;
    if (preferred >= 1) preferredSamples += 1;
    if (premium >= 1) premiumSamples += 1;
    if (previousPrimary && primary !== previousPrimary) primaryChanges += 1;
    previousPrimary = primary;
  }
  return {
    minimumVisible,
    dualCoverageRate: dualSamples / samples,
    preferredCoverageRate: preferredSamples / samples,
    premiumCoverageRate: premiumSamples / samples,
    primaryChanges,
  };
}

export function beamToGroundPoint(beam: { u: number; v: number; id?: string }): GroundPoint {
  return {
    name: beam.id ?? "当前 L1",
    lat: Math.max(18.2, Math.min(52.5, 35 - beam.v * 0.08)),
    lon: Math.max(75.8, Math.min(134.5, 105 + beam.u * 0.1)),
  };
}

export { baseline };
