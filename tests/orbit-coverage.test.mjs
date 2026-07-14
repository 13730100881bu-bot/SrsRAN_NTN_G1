import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import {
  ORBIT_PERIOD_SECONDS,
  coverageRadiusKm,
  createWalkerDelta,
  propagateSatellite,
  validateSelectedConstellationScenario,
} from "../app/orbit-model.ts";

const baseline = JSON.parse(await readFile(new URL("../app/orbit-baseline.json", import.meta.url), "utf8"));
const audit = JSON.parse(await readFile(new URL("../app/global-constellation-audit.json", import.meta.url), "utf8"));

test("global 45-degree engineering display uses the F=1 candidate", () => {
  const definitions = createWalkerDelta({
    planes: baseline.planes,
    satellitesPerPlane: baseline.satellitesPerPlane,
    phaseFactor: baseline.phaseFactor,
    altitudeKm: baseline.altitudeKm,
    inclinationDeg: baseline.inclinationDeg,
    raanOffsetDeg: baseline.raanOffsetDeg,
    phaseOffsetDeg: baseline.phaseOffsetDeg,
  });
  assert.equal(baseline.walker, "Walker Delta 60°:3528/42/1");
  assert.equal(baseline.designStatus, "coarse_candidate");
  assert.equal(baseline.phaseFactor, 1);
  assert.equal(definitions.length, 3528);
  assert.equal(new Set(definitions.map(({ id }) => id)).size, 3528);
  assert.equal(definitions[84].raanDeg - definitions[0].raanDeg, 360 / 42);
  assert.equal(definitions[1].phaseDeg - definitions[0].phaseDeg, 360 / 84);
  assert.ok(Math.abs(definitions[84].phaseDeg - 360 / (42 * 84)) < 1e-12);
  assert.equal(definitions.every((satellite) => satellite.altitudeKm === 500 && satellite.inclinationDeg === 60), true);
  assert.ok(Math.abs(ORBIT_PERIOD_SECONDS / 60 - 94.6) < 0.1);
  assert.ok(coverageRadiusKm(42) > coverageRadiusKm(45));
});

test("Walker generator parameterizes inclination, RAAN, phase, altitude, plane count, slots and F", () => {
  const catalog = createWalkerDelta({
    planes: 3, satellitesPerPlane: 4, phaseFactor: 2,
    altitudeKm: 600, inclinationDeg: 70, raanOffsetDeg: 7, phaseOffsetDeg: 11,
  });
  assert.equal(catalog.length, 12);
  assert.deepEqual(catalog[0], {
    id: "P01-S01", plane: 1, slot: 1, raanDeg: 7, phaseDeg: 11,
    altitudeKm: 600, inclinationDeg: 70,
  });
  assert.equal(catalog[4].raanDeg, 127);
  assert.equal(catalog[4].phaseDeg, 71);
  assert.notDeepEqual(propagateSatellite(catalog[0], 100).ecefKm, propagateSatellite({ ...catalog[0], inclinationDeg: 20 }, 100).ecefKm);
  assert.throws(() => createWalkerDelta({ planes: 0, satellitesPerPlane: 4 }), /positive integer/);
  assert.throws(() => createWalkerDelta({ planes: 3, satellitesPerPlane: 4, phaseFactor: 3 }), /phaseFactor/);
});

test("seed/coarse audit cannot be selected as an exact pass", () => {
  assert.equal(audit.selectedScenario, null);
  assert.equal(audit.scenarios[0].auditStatus, "seed");
  assert.equal(audit.scenarios[0].sevenDayEventAudit, "not_run");
  assert.equal(validateSelectedConstellationScenario(audit.scenarios, null), null);
  assert.throws(() => validateSelectedConstellationScenario(audit.scenarios, audit.scenarios[0].id), /only exact_pass/);
  assert.equal(validateSelectedConstellationScenario([
    { id: "candidate", auditStatus: "exact_pass" },
  ], "candidate").id, "candidate");
});
