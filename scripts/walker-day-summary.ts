import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import {
  createSatelliteCellPlanningContext,
  summarizeL1AssignmentDay,
} from "../app/satellite-cell-model";

const value = process.argv[2];
const identityRegistryPath = process.argv[3];
const match = /^(\d+)[xX](\d+)$/.exec(value ?? "");
if (!match || !identityRegistryPath) {
  throw new Error("Usage: tsx scripts/walker-day-summary.ts PLANESxSATELLITES_PER_PLANE IDENTITY_REGISTRY.json");
}
const config = { planes: Number(match[1]), satellitesPerPlane: Number(match[2]), phaseFactor: 1 };
const identityRegistry: unknown = JSON.parse(readFileSync(resolve(identityRegistryPath), "utf8"));
const startedAt = performance.now();
const planningContext = createSatelliteCellPlanningContext(config, identityRegistry);
const summary = summarizeL1AssignmentDay({ planningContext });
process.stdout.write(`${JSON.stringify({
  ...config,
  satelliteCount: config.planes * config.satellitesPerPlane,
  ...summary,
  failureEpochs: summary.failureEpochs.slice(0, 5),
  elapsedSeconds: Number(((performance.now() - startedAt) / 1_000).toFixed(3)),
})}\n`);
