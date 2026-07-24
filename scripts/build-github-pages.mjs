import { spawnSync } from "node:child_process";
import { access, readdir, readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const repositoryBasePath = "/SrsRAN_NTN_G1";
const projectDirectory = fileURLToPath(new URL("..", import.meta.url));
const vinextCli = fileURLToPath(
  new URL("../node_modules/vinext/dist/cli.js", import.meta.url),
);
const result = spawnSync(process.execPath, [vinextCli, "build"], {
  cwd: projectDirectory,
  env: {
    ...process.env,
    NTN_GITHUB_PAGES: "1",
    VITE_NTN_PUBLIC_BASE_PATH: repositoryBasePath,
  },
  stdio: "inherit",
});

const windowsVinextCleanupStatus = 3221226505;
const completedBeforeWindowsCleanupAssertion =
  process.platform === "win32" && result.status === windowsVinextCleanupStatus;

if (result.status !== 0 && !completedBeforeWindowsCleanupAssertion) {
  process.exit(result.status ?? 1);
}
if (completedBeforeWindowsCleanupAssertion) {
  console.warn("vinext completed the static export before a known Windows libuv cleanup assertion.");
}

const clientDirectory = new URL("../dist/client/", import.meta.url);
const indexFile = new URL("index.html", clientDirectory);
await access(indexFile);
await writeFile(new URL(".nojekyll", clientDirectory), "");

const html = await readFile(indexFile, "utf8");
const requiredPaths = [
  `${repositoryBasePath}/assets/`,
  `${repositoryBasePath}/data/`,
  `${repositoryBasePath}/favicon.svg`,
];
for (const requiredPath of requiredPaths) {
  if (!html.includes(requiredPath) && requiredPath !== `${repositoryBasePath}/data/`) {
    throw new Error(`Static export is missing ${requiredPath}`);
  }
}

const assetDirectory = new URL("assets/", clientDirectory);
const assetDirectoryPath = fileURLToPath(assetDirectory);
const javascriptFiles = (await readdir(assetDirectory)).filter((name) => name.endsWith(".js"));
const javascript = (
  await Promise.all(javascriptFiles.map((name) => readFile(join(assetDirectoryPath, name), "utf8")))
).join("\n");

if (!javascript.includes(repositoryBasePath)) {
  throw new Error(`Client bundle is missing the ${repositoryBasePath} public base path`);
}

for (const requiredDataPath of [
  "/data/global-land-l1-v1.json",
  "/data/land-50m.json",
]) {
  if (!javascript.includes(requiredDataPath)) {
    throw new Error(`Client bundle is missing ${requiredDataPath}`);
  }
}

if (/["'`]\/assets\//.test(`${html}\n${javascript}`)) {
  throw new Error("Static export still contains a root-relative asset URL");
}

console.log(`GitHub Pages artifact ready: ${fileURLToPath(clientDirectory)}`);
