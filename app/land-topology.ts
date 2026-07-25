import type { GeoPermissibleObjects } from "d3-geo";
import { feature } from "topojson-client";

export type LandTopology = {
  type: "Topology";
  objects: Record<string, unknown>;
};

export function landFeatureFromTopology(payload: LandTopology) {
  const object = payload.objects.land ?? Object.values(payload.objects)[0];
  if (!object) throw new Error("land TopoJSON does not contain an object");
  return feature(payload as never, object as never) as unknown as GeoPermissibleObjects;
}
