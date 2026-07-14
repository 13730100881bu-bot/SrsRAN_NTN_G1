export const POSITION_PLAN_ARTIFACT_KIND = "candidate_test_only" as const;
export const POSITION_PLAN_ACTIVATION_ALIGNMENT_MS = 640;

const MAX_NCI = (BigInt(1) << BigInt(36)) - BigInt(1);
const MAX_PCI = 1007;
const CPP_DOUBLE_MAX_DIGITS10 = 17;

export type PositionPlanOnboardCell = {
  readonly nci: string;
  readonly pci: number;
};

export type PositionPlanVisibleL1 = {
  readonly position_id: string;
  readonly latitude_deg: number;
  readonly longitude_deg: number;
};

/**
 * Reviewed management-center input to the candidate exporter.
 *
 * `artifact_kind` is deliberately input-only. The emitted document exactly
 * matches the CU-CP parser schema and remains a candidate/test artifact; it is
 * not evidence of an exact constellation audit or RF application.
 */
export type PositionPlanCandidateArtifact = {
  readonly artifact_kind: typeof POSITION_PLAN_ARTIFACT_KIND;
  readonly satellite_id: string;
  readonly catalog_version: number;
  readonly schedule_version: number;
  readonly valid_from_unix_ms: number;
  readonly valid_until_unix_ms: number;
  readonly activation_epoch_unix_ms: number;
  readonly onboard_cells: readonly PositionPlanOnboardCell[];
  readonly visible_l1_positions: readonly PositionPlanVisibleL1[];
};

export type VersionedPositionPlanDocument = {
  readonly satellite_id: string;
  readonly catalog_version: number;
  readonly schedule_version: number;
  readonly content_hash: string;
  readonly valid_from_unix_ms: number;
  readonly valid_until_unix_ms: number;
  readonly activation_epoch_unix_ms: number;
  readonly onboard_cells: readonly PositionPlanOnboardCell[];
  readonly visible_l1_positions: readonly PositionPlanVisibleL1[];
};

type HashablePositionPlan = Omit<VersionedPositionPlanDocument, "content_hash">;
type JsonObject = Record<string, unknown>;

function fail(context: string, message: string): never {
  throw new Error(`${context}: ${message}`);
}

function asObject(value: unknown, context: string): JsonObject {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    fail(context, "must be an object");
  }
  return value as JsonObject;
}

function assertExactKeys(value: JsonObject, allowed: readonly string[], context: string) {
  const allowedSet = new Set(allowed);
  const unknown = Object.keys(value).filter((key) => !allowedSet.has(key));
  if (unknown.length > 0) fail(context, `contains unknown field ${unknown.sort()[0]}`);
  const missing = allowed.filter((key) => !Object.hasOwn(value, key));
  if (missing.length > 0) fail(context, `is missing field ${missing[0]}`);
}

function readString(value: unknown, context: string) {
  if (typeof value !== "string" || value.length === 0) fail(context, "must be a non-empty string");
  return value;
}

function readSafePositiveInteger(value: unknown, context: string) {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value <= 0) {
    fail(context, "must be a positive safe integer");
  }
  return value;
}

function readUnixMilliseconds(value: unknown, context: string) {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) {
    fail(context, "must be a non-negative safe integer");
  }
  return value;
}

function readFiniteNumber(value: unknown, context: string) {
  if (typeof value !== "number" || !Number.isFinite(value)) fail(context, "must be a finite number");
  return value;
}

function parseNci(value: unknown, context: string) {
  let numeric: bigint;
  if (typeof value === "number") {
    if (!Number.isSafeInteger(value) || value < 0) fail(context, "must be a non-negative safe integer or string");
    numeric = BigInt(value);
  } else if (typeof value === "string" && (/^0[xX][0-9a-fA-F]+$/.test(value) || /^\d+$/.test(value))) {
    numeric = BigInt(value);
  } else {
    fail(context, "must be a decimal or 0x-prefixed 36-bit NCI");
  }
  if (numeric < BigInt(0) || numeric > MAX_NCI) fail(context, "is outside the 36-bit NCI range");
  return {
    numeric,
    serialized: `0x${numeric.toString(16).toUpperCase().padStart(9, "0")}`,
  };
}

function stripFractionZeros(value: string) {
  return value.includes(".") ? value.replace(/0+$/, "").replace(/\.$/, "") : value;
}

/** Matches classic-locale C++ defaultfloat with `setprecision(max_digits10)` for IEEE-754 doubles. */
export function formatCppMaxDigits10(value: number) {
  if (!Number.isFinite(value)) throw new Error("canonical double must be finite");
  if (Object.is(value, -0)) return "-0";
  if (value === 0) return "0";

  const [rawMantissa, rawExponent] = value.toExponential(CPP_DOUBLE_MAX_DIGITS10 - 1).split("e");
  const exponent = Number(rawExponent);
  if (exponent < -4 || exponent >= CPP_DOUBLE_MAX_DIGITS10) {
    const exponentSign = exponent < 0 ? "-" : "+";
    return `${stripFractionZeros(rawMantissa)}e${exponentSign}${String(Math.abs(exponent)).padStart(2, "0")}`;
  }

  const fractionDigits = Math.max(0, CPP_DOUBLE_MAX_DIGITS10 - exponent - 1);
  return stripFractionZeros(value.toFixed(fractionDigits));
}

function normalizeCandidate(value: unknown): HashablePositionPlan {
  const root = asObject(value, "position plan candidate");
  assertExactKeys(root, [
    "artifact_kind",
    "satellite_id",
    "catalog_version",
    "schedule_version",
    "valid_from_unix_ms",
    "valid_until_unix_ms",
    "activation_epoch_unix_ms",
    "onboard_cells",
    "visible_l1_positions",
  ], "position plan candidate");

  if (root.artifact_kind !== POSITION_PLAN_ARTIFACT_KIND) {
    fail("artifact_kind", `must be ${POSITION_PLAN_ARTIFACT_KIND}`);
  }
  const satelliteId = readString(root.satellite_id, "satellite_id");
  if (!/^[A-Za-z0-9][A-Za-z0-9._:-]*$/.test(satelliteId)) {
    fail("satellite_id", "contains unsupported characters");
  }
  const catalogVersion = readSafePositiveInteger(root.catalog_version, "catalog_version");
  const scheduleVersion = readSafePositiveInteger(root.schedule_version, "schedule_version");
  const validFrom = readUnixMilliseconds(root.valid_from_unix_ms, "valid_from_unix_ms");
  const validUntil = readUnixMilliseconds(root.valid_until_unix_ms, "valid_until_unix_ms");
  const activationEpoch = readUnixMilliseconds(root.activation_epoch_unix_ms, "activation_epoch_unix_ms");
  if (validFrom >= validUntil) fail("validity window", "valid_from_unix_ms must be before valid_until_unix_ms");
  if (activationEpoch < validFrom || activationEpoch >= validUntil) {
    fail("activation_epoch_unix_ms", "must be inside the validity window");
  }
  if (activationEpoch % POSITION_PLAN_ACTIVATION_ALIGNMENT_MS !== 0) {
    fail("activation_epoch_unix_ms", `must be aligned to ${POSITION_PLAN_ACTIVATION_ALIGNMENT_MS} ms`);
  }

  if (!Array.isArray(root.onboard_cells) || root.onboard_cells.length !== 2) {
    fail("onboard_cells", "must contain exactly two identities");
  }
  const parsedCells = root.onboard_cells.map((rawCell, index) => {
    const cell = asObject(rawCell, `onboard_cells[${index}]`);
    assertExactKeys(cell, ["nci", "pci"], `onboard_cells[${index}]`);
    const nci = parseNci(cell.nci, `onboard_cells[${index}].nci`);
    if (typeof cell.pci !== "number" || !Number.isInteger(cell.pci) || cell.pci < 0 || cell.pci > MAX_PCI) {
      fail(`onboard_cells[${index}].pci`, `must be an integer in the range 0..${MAX_PCI}`);
    }
    return { ...nci, pci: cell.pci };
  }).sort((left, right) => left.numeric < right.numeric ? -1 : left.numeric > right.numeric ? 1 : left.pci - right.pci);
  if (parsedCells[0].numeric === parsedCells[1].numeric) fail("onboard_cells", "must contain two distinct NCIs");
  const onboardCells = parsedCells.map(({ serialized, pci }) => ({ nci: serialized, pci }));

  if (!Array.isArray(root.visible_l1_positions)) fail("visible_l1_positions", "must be an array");
  const seenIds = new Set<string>();
  const positions = root.visible_l1_positions.map((rawPosition, index) => {
    const position = asObject(rawPosition, `visible_l1_positions[${index}]`);
    assertExactKeys(
      position,
      ["position_id", "latitude_deg", "longitude_deg"],
      `visible_l1_positions[${index}]`,
    );
    const positionId = readString(position.position_id, `visible_l1_positions[${index}].position_id`);
    if (!/^G\d{6}$/.test(positionId)) fail(`visible_l1_positions[${index}].position_id`, "must match G######");
    if (seenIds.has(positionId)) fail("visible_l1_positions", `contains duplicate ${positionId}`);
    seenIds.add(positionId);
    const rawLatitude = readFiniteNumber(position.latitude_deg, `visible_l1_positions[${index}].latitude_deg`);
    const rawLongitude = readFiniteNumber(position.longitude_deg, `visible_l1_positions[${index}].longitude_deg`);
    // JSON.stringify serializes negative zero as zero. Normalize before hashing so the emitted bytes and CU-CP parse agree.
    const latitude = Object.is(rawLatitude, -0) ? 0 : rawLatitude;
    const longitude = Object.is(rawLongitude, -0) ? 0 : rawLongitude;
    if (latitude < -90 || latitude > 90) fail(`visible_l1_positions[${index}].latitude_deg`, "is outside -90..90");
    if (longitude < -180 || longitude > 180) {
      fail(`visible_l1_positions[${index}].longitude_deg`, "is outside -180..180");
    }
    return { position_id: positionId, latitude_deg: latitude, longitude_deg: longitude };
  }).sort((left, right) =>
    left.position_id < right.position_id ? -1
      : left.position_id > right.position_id ? 1
        : left.latitude_deg - right.latitude_deg || left.longitude_deg - right.longitude_deg,
  );

  return {
    satellite_id: satelliteId,
    catalog_version: catalogVersion,
    schedule_version: scheduleVersion,
    valid_from_unix_ms: validFrom,
    valid_until_unix_ms: validUntil,
    activation_epoch_unix_ms: activationEpoch,
    onboard_cells: onboardCells,
    visible_l1_positions: positions,
  };
}

/** Exact byte payload hashed by `compute_ntn_position_plan_content_hash()` in CU-CP. */
export function canonicalPositionPlanPayload(plan: HashablePositionPlan) {
  const cells = plan.onboard_cells.map((cell, index) => {
    const nci = parseNci(cell.nci, `onboard_cells[${index}].nci`);
    return { numeric: nci.numeric, pci: cell.pci };
  }).sort((left, right) => left.numeric < right.numeric ? -1 : left.numeric > right.numeric ? 1 : left.pci - right.pci);
  const positions = [...plan.visible_l1_positions].sort((left, right) =>
    left.position_id < right.position_id ? -1
      : left.position_id > right.position_id ? 1
        : left.latitude_deg - right.latitude_deg || left.longitude_deg - right.longitude_deg,
  );

  let payload = "";
  payload += `satellite_id=${plan.satellite_id}\n`;
  payload += `catalog_version=${plan.catalog_version}\n`;
  payload += `schedule_version=${plan.schedule_version}\n`;
  payload += `valid_from_unix_ms=${plan.valid_from_unix_ms}\n`;
  payload += `valid_until_unix_ms=${plan.valid_until_unix_ms}\n`;
  payload += `activation_epoch_unix_ms=${plan.activation_epoch_unix_ms}\n`;
  for (const cell of cells) payload += `cell=${cell.numeric},${cell.pci}\n`;
  for (const position of positions) {
    payload += `l1=${position.position_id},${formatCppMaxDigits10(position.latitude_deg)},`;
    payload += `${formatCppMaxDigits10(position.longitude_deg)}\n`;
  }
  return payload;
}

async function sha256(payload: string) {
  if (!globalThis.crypto?.subtle) throw new Error("Web Crypto SHA-256 is unavailable");
  const digest = await globalThis.crypto.subtle.digest("SHA-256", new TextEncoder().encode(payload));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

/** Validates and emits the exact CU-CP JSON document without clipping the candidate inventory. */
export async function buildVersionedPositionPlan(value: unknown): Promise<VersionedPositionPlanDocument> {
  const normalized = normalizeCandidate(value);
  const contentHash = `sha256:${await sha256(canonicalPositionPlanPayload(normalized))}`;
  return {
    satellite_id: normalized.satellite_id,
    catalog_version: normalized.catalog_version,
    schedule_version: normalized.schedule_version,
    content_hash: contentHash,
    valid_from_unix_ms: normalized.valid_from_unix_ms,
    valid_until_unix_ms: normalized.valid_until_unix_ms,
    activation_epoch_unix_ms: normalized.activation_epoch_unix_ms,
    onboard_cells: normalized.onboard_cells,
    visible_l1_positions: normalized.visible_l1_positions,
  };
}
