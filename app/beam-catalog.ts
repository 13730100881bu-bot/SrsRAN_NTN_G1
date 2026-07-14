export const NATIONAL_L1_TARGET = 2620;
export const DIGITAL_BEAM_RADIUS_KM = 15;
export const L1_CELL_RADIUS_NORMALIZED = 0.00709;

export const GLOBAL_CATALOG_URL = "/data/global-land-l1-v1.json";

export type GlobalL1Cell = {
  id: string;
  lat: number;
  lon: number;
  /** Seven-bit mask for L2 IDs `${id}-0` through `${id}-6`. */
  childMask: number;
};

export type GlobalL1CatalogMetadata = {
  version: string;
  scope: {
    longitudeDeg: [number, number];
    latitudeDeg: [number, number];
    surface: string;
  };
  source: {
    dataset: string;
    naturalEarthVersion: string;
    scale: string;
    package: string;
    packageVersion: string;
    file: string;
    publicPath: string;
    packageLicense: string;
    sha256: string;
    url: string;
  };
  generation: {
    algorithm: string;
    landTest: string;
    exactCoastlineClipping: boolean;
    equalAreaGrid: boolean;
    equalAreaConstruction: string;
    exactRegularSphericalHexagons: boolean;
    quasiHexagonalCenters: boolean;
    earthRadiusKm: number;
    l1NominalSpacingKm: number;
    targetCellAreaKm2: number;
    latitudeBandCount: number;
    minimumCellAreaKm2: number;
    maximumCellAreaKm2: number;
    maximumRelativeAreaError: number;
    l2NominalRadiusKm: number;
    l2RingDistanceKm: number;
    l2BearingsDeg: Array<number | null>;
    coordinatePrecisionDeg: number;
    ordering: string;
    idFormat: string;
    generatedBy: string;
  };
  counts: { l1: number; l2: number; fullL1: number; edgeL1: number };
  integrity: { algorithm: "SHA-256"; scope: string; sha256: string };
};

export type GlobalL1Catalog = {
  metadata: GlobalL1CatalogMetadata;
  cells: GlobalL1Cell[];
};

export function globalL2CatalogIds(cell: Pick<GlobalL1Cell, "id" | "childMask">) {
  return Array.from({ length: 7 }, (_, index) => index)
    .filter((index) => (cell.childMask & (1 << index)) !== 0)
    .map((index) => `${cell.id}-${index}`);
}

export async function loadGlobalL1Catalog(
  url = GLOBAL_CATALOG_URL,
  fetchCatalog: typeof fetch = fetch,
): Promise<GlobalL1Catalog> {
  const response = await fetchCatalog(url);
  if (!response.ok) throw new Error(`Global L1 catalog request failed: ${response.status}`);
  const catalog = await response.json() as GlobalL1Catalog;
  if (catalog.metadata?.version !== "global-land-l1-v1" || !Array.isArray(catalog.cells)) {
    throw new Error("Global L1 catalog has an unsupported or malformed payload");
  }
  return catalog;
}

export type L1CatalogCell = {
  id: string;
  index: number;
  x: number;
  y: number;
  row: number;
  column: number;
  childCount: number;
  kind: "full" | "edge";
};

export const CHINA_SERVICE_OUTLINE = [
  [0.08, 0.35], [0.14, 0.25], [0.12, 0.16], [0.22, 0.12], [0.29, 0.17], [0.39, 0.13],
  [0.49, 0.2], [0.58, 0.18], [0.66, 0.09], [0.79, 0.06], [0.9, 0.13], [0.87, 0.24],
  [0.79, 0.29], [0.88, 0.39], [0.83, 0.49], [0.75, 0.53], [0.71, 0.62], [0.64, 0.65],
  [0.59, 0.76], [0.52, 0.74], [0.48, 0.64], [0.4, 0.61], [0.34, 0.53], [0.23, 0.55],
  [0.17, 0.48], [0.1, 0.47],
] as const;

export function pointInPolygon(x: number, y: number, polygon = CHINA_SERVICE_OUTLINE) {
  let inside = false;
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
    const [xi, yi] = polygon[i];
    const [xj, yj] = polygon[j];
    const intersects = yi > y !== yj > y && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi;
    if (intersects) inside = !inside;
  }
  return inside;
}

export function insideServiceMask(x: number, y: number) {
  const insideHainan = ((x - 0.6) / 0.035) ** 2 + ((y - 0.83) / 0.025) ** 2 <= 1;
  return pointInPolygon(x, y) || insideHainan;
}

const L2_LOCAL_OFFSETS = [
  [0, 0], [1, 0], [0.5, -0.75], [-0.5, -0.75], [-1, 0], [-0.5, 0.75], [0.5, 0.75],
] as const;

function createCatalog() {
  const cells: Omit<L1CatalogCell, "id" | "index">[] = [];
  const horizontalStep = Math.sqrt(3) * L1_CELL_RADIUS_NORMALIZED;
  const verticalStep = 1.5 * L1_CELL_RADIUS_NORMALIZED;
  let row = 0;
  for (let y = L1_CELL_RADIUS_NORMALIZED; y < 1; y += verticalStep, row += 1) {
    const offset = row % 2 ? horizontalStep / 2 : 0;
    let column = 0;
    for (let x = L1_CELL_RADIUS_NORMALIZED + offset; x < 1; x += horizontalStep, column += 1) {
      if (!insideServiceMask(x, y)) continue;
      const childOffset = L1_CELL_RADIUS_NORMALIZED * 0.52;
      const childCount = L2_LOCAL_OFFSETS.filter(([dx, dy]) => insideServiceMask(x + dx * childOffset, y + dy * childOffset)).length;
      cells.push({ x, y, row, column, childCount, kind: childCount === 7 ? "full" : "edge" });
    }
  }
  return cells
    .sort((left, right) => left.y - right.y || left.x - right.x)
    .map<L1CatalogCell>((cell, index) => ({ ...cell, index: index + 1, id: `A${String(index + 1).padStart(4, "0")}` }));
}

export const l1Catalog = createCatalog();

if (l1Catalog.length !== NATIONAL_L1_TARGET) {
  throw new Error(`L1 catalog count drifted: expected ${NATIONAL_L1_TARGET}, got ${l1Catalog.length}`);
}

const catalogById = new Map(l1Catalog.map((cell) => [cell.id, cell]));

export function l1CellById(id: string) {
  return catalogById.get(id.toUpperCase());
}

export function nearestL1Cell(x: number, y: number) {
  return l1Catalog.reduce((best, cell) => {
    const distance = (cell.x - x) ** 2 + (cell.y - y) ** 2;
    const bestDistance = (best.x - x) ** 2 + (best.y - y) ** 2;
    return distance < bestDistance ? cell : best;
  }, l1Catalog[0]);
}

export function l1CellForAxis(u: number, v: number) {
  return nearestL1Cell(0.5 + u / 310, 0.48 + v / 190);
}

export function axisForL1Cell(cell: L1CatalogCell) {
  return { u: Math.round((cell.x - 0.5) * 310), v: Math.round((cell.y - 0.48) * 190) };
}

export function l2CatalogIds(cell: L1CatalogCell) {
  return L2_LOCAL_OFFSETS.slice(0, cell.childCount).map((_, index) => `${cell.id}-${index}`);
}

export function catalogGroundPoint(cell: L1CatalogCell) {
  return {
    name: cell.id,
    lat: 55 - cell.y * 39,
    lon: 72 + cell.x * 64,
  };
}
