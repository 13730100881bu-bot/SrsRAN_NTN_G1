export type GlobalSearchTarget =
  | { kind: "satellite"; id: string }
  | { kind: "position"; id: string };

export function parseGlobalSearchTarget(input: string): GlobalSearchTarget | null {
  const normalized = input
    .trim()
    .toUpperCase()
    .replace(/[‐‑‒–—―_/\s]+/g, "-");

  const satellite = /^P(\d{1,2})-?S(\d{1,2})$/.exec(normalized);
  if (satellite) {
    return {
      kind: "satellite",
      id: `P${satellite[1].padStart(2, "0")}-S${satellite[2].padStart(2, "0")}`,
    };
  }

  const position = /^G(\d{1,6})$/.exec(normalized.replaceAll("-", ""));
  if (position) {
    return {
      kind: "position",
      id: `G${position[1].padStart(6, "0")}`,
    };
  }

  return null;
}
