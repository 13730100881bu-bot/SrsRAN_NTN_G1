import assert from "node:assert/strict";
import test from "node:test";
import {
  parseArguments,
  parseScenario,
  runAudit,
} from "../scripts/audit-global-constellation.mjs";

test("CLI parses all six Walker parameters and rejects invalid F", () => {
  assert.deepEqual(parseScenario("60,42,84,0,7.5,11"), {
    inclinationDeg: 60,
    planes: 42,
    satellitesPerPlane: 84,
    phaseFactor: 0,
    raanOffsetDeg: 7.5,
    phaseOffsetDeg: 11,
  });
  assert.throws(() => parseScenario("60,42,84,42,0,0"), /F must be an integer/);
  assert.throws(() => parseScenario("60,42,84"), /requires inclination,planes,slots,F,RAAN,phase/);
  assert.throws(() => parseArguments(["--scenario"]), /--scenario requires a value/);
});

test("snapshot reads the packaged global catalog and remains an unselectable coarse audit", { timeout: 30_000 }, async () => {
  const options = parseArguments(["--snapshot"]);
  const report = await runAudit(options);
  assert.equal(report.auditLevel, "coarse");
  assert.equal(report.exact, false);
  assert.equal(report.mode, "snapshot");
  assert.equal(report.scenario.satelliteCount, 3528);
  assert.equal(report.catalog.version, "global-land-l1-v1");
  assert.equal(report.catalog.l1Count, 36411);
  assert.equal(report.sampling.epochCount, 1);
  assert.equal(report.epochs.length, 1);
  assert.deepEqual(Object.keys(report.epochs[0]), [
    "timeSeconds",
    "uncoveredL1",
    "minimumCandidates",
    "maximumVisibleL1PerSatellite",
    "satellitesOver256L1",
  ]);
  assert.equal(report.epochs[0].uncoveredL1, 23);
  assert.equal(report.epochs[0].minimumCandidates, 0);
  assert.equal(report.epochs[0].maximumVisibleL1PerSatellite, 206);
  assert.equal(report.epochs[0].satellitesOver256L1, 0);
  assert.equal(Object.hasOwn(report, "selectedScenario"), false);
  assert.match(report.limitations.join(" "), /must not be described as exact/);
});
