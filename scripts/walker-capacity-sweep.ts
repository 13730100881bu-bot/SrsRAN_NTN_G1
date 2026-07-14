import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import {
  createSatelliteCellPlanningContext,
  probeL1AssignmentFeasibility,
} from "../app/satellite-cell-model";

const requested = process.argv.slice(2).map((value) => {
  const match = /^(\d+)[xX](\d+)=(.+)$/.exec(value);
  if (!match) throw new Error(`Expected PLANESxSATELLITES_PER_PLANE=IDENTITY_REGISTRY.json, received ${value}`);
  return {
    config: { planes: Number(match[1]), satellitesPerPlane: Number(match[2]), phaseFactor: 1 },
    identityRegistryPath: resolve(match[3]),
  };
});
if (requested.length === 0) {
  throw new Error("At least one PLANESxSATELLITES_PER_PLANE=IDENTITY_REGISTRY.json argument is required");
}
const configurations = requested.sort((left, right) =>
  left.config.planes * left.config.satellitesPerPlane - right.config.planes * right.config.satellitesPerPlane
    || left.config.planes - right.config.planes
    || left.config.satellitesPerPlane - right.config.satellitesPerPlane,
);

let winningSatelliteCount = Number.POSITIVE_INFINITY;
for (const { config, identityRegistryPath } of configurations) {
  const satelliteCount = config.planes * config.satellitesPerPlane;
  if (satelliteCount > winningSatelliteCount) break;
  const startedAt = performance.now();
  const identityRegistry: unknown = JSON.parse(readFileSync(identityRegistryPath, "utf8"));
  const planningContext = createSatelliteCellPlanningContext(config, identityRegistry);
  const probe = probeL1AssignmentFeasibility({ planningContext });
  const result = {
    ...config,
    satelliteCount,
    ...probe,
    elapsedSeconds: Number(((performance.now() - startedAt) / 1_000).toFixed(3)),
  };
  process.stdout.write(`${JSON.stringify(result)}\n`);
  if (probe.success) winningSatelliteCount = satelliteCount;
}
