#!/usr/bin/env node

import { readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { buildVersionedPositionPlan } from "../app/position-plan-model.ts";

const DISCLAIMER = "candidate/test artifact only; this is not exact/global coverage or DU/RF application evidence";

export function parseArguments(argv) {
  const options = { input: "", output: "" };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--help" || argument === "-h") return { ...options, help: true };
    if (argument !== "--input" && argument !== "--output") throw new Error(`unknown argument ${argument}`);
    const value = argv[index + 1];
    if (!value || value.startsWith("--")) throw new Error(`${argument} requires a path`);
    index += 1;
    if (argument === "--input") options.input = value;
    else options.output = value;
  }
  if (!options.input) throw new Error("--input is required");
  return options;
}

export function usage() {
  return [
    "Usage: npm run plan:export -- --input candidate.json [--output plan.json]",
    "",
    "The input must declare artifact_kind=candidate_test_only and explicitly provide",
    "versions, validity/activation times, two registry identities and the complete L1 inventory.",
    "No Web calendar is exported. Without --output, the CU-CP JSON document is written to stdout.",
  ].join("\n");
}

export async function exportCandidatePlan(options) {
  const inputText = await readFile(resolve(options.input), "utf8");
  let candidate;
  try {
    candidate = JSON.parse(inputText);
  } catch (error) {
    throw new Error(`invalid candidate JSON: ${error instanceof Error ? error.message : String(error)}`);
  }
  const plan = await buildVersionedPositionPlan(candidate);
  const serialized = `${JSON.stringify(plan, null, 2)}\n`;
  if (options.output) await writeFile(resolve(options.output), serialized, "utf8");
  return { plan, serialized };
}

export async function main(argv = process.argv.slice(2)) {
  const options = parseArguments(argv);
  if (options.help) {
    process.stdout.write(`${usage()}\n`);
    return;
  }
  const result = await exportCandidatePlan(options);
  process.stderr.write(`NTN position plan: ${DISCLAIMER}\n`);
  if (!options.output) process.stdout.write(result.serialized);
}

const isMain = process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) {
  main().catch((error) => {
    process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    process.exitCode = 1;
  });
}
