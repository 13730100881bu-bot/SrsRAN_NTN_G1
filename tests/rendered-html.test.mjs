import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import test from "node:test";

async function render() {
  const workerUrl = new URL("../dist/server/index.js", import.meta.url);
  workerUrl.searchParams.set("test", `${process.pid}-${Date.now()}`);
  const { default: worker } = await import(workerUrl.href);
  return worker.fetch(
    new Request("http://localhost/", { headers: { accept: "text/html" } }),
    { ASSETS: { fetch: async () => new Response("Not found", { status: 404 }) } },
    { waitUntil() {}, passThroughOnException() {} },
  );
}

test("server-renders the conclusion-first NTN engineering review", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);

  const html = (await response.text()).replaceAll("<!-- -->", "");
  assert.match(html, /NTN 全球陆地接入方案/);
  assert.match(html, /57°S～57°N陆地/);
  assert.match(html, /60° · 42×84 · F=1/);
  assert.match(html, /3,528颗卫星/);
  assert.match(html, /方案结论/);
  assert.match(html, /全球覆盖/);
  assert.match(html, /卫星负载/);
  assert.match(html, /跳波束日历/);
  assert.match(html, /当前方案具备下一阶段验证条件/);
  assert.match(html, /未采用的初始方案/);
  assert.match(html, /720 \/ 720/);
  assert.match(html, /209 \/ 256/);
  assert.match(html, /距离256个区域上限还有47个/);
  assert.match(html, /终端多久能获得接入机会/);
  assert.match(html, /尚未最终定案/);
  assert.doesNotMatch(html, /动作演示|STEP|当前无选定方案|为什么还不能写/);
  assert.doesNotMatch(html, /64 个地固|地固 NCI|NCI 属于地固|每星 84 个 L1/);
  assert.doesNotMatch(html, /codex-preview|react-loading-skeleton|Your site is taking shape/i);
});

test("audit evidence rejects F=0 and keeps the sampled F=1 candidate unselected", async () => {
  const [audit, seedSnapshot, f1Day, planner, css] = await Promise.all([
    readFile(new URL("../app/global-constellation-audit.json", import.meta.url), "utf8").then(JSON.parse),
    readFile(new URL("../app/global-constellation-snapshot.json", import.meta.url), "utf8").then(JSON.parse),
    readFile(new URL("../app/global-constellation-f1-day-coarse.json", import.meta.url), "utf8").then(JSON.parse),
    readFile(new URL("../app/global-planner.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/global.css", import.meta.url), "utf8"),
  ]);

  assert.equal(audit.selectedScenario, null);
  assert.equal(seedSnapshot.summary.maximumUncoveredL1, 23);
  assert.equal(f1Day.sampling.epochCount, 720);
  assert.equal(f1Day.summary.maximumUncoveredL1, 0);
  assert.equal(f1Day.summary.maximumVisibleL1PerSatellite, 209);
  assert.equal(f1Day.exact, false);
  assert.match(planner, /未采用的初始方案/);
  assert.match(planner, /一天、每120 s检查一次的计算/);
  assert.match(planner, /连续事件检查仍未运行/);
  assert.match(planner, /SSB不超过80 ms/);
  assert.match(planner, /PRACH不超过640 ms/);
  assert.match(planner, /128 \/ 128次机会/);
  assert.match(planner, /两个星载小区/);
  assert.match(planner, /日历就是卫星轮流照向不同地面区域的时间安排/);
  assert.match(planner, /这颗卫星能否为所有可见地面区域排出接入时段/);
  assert.match(planner, /多久安排一次网络发现机会/);
  assert.match(planner, /多久安排一次接入机会/);
  assert.match(planner, /查看80 ms轮转明细/);
  assert.match(planner, /查看完整可见波位表/);
  assert.match(planner, /if \(view !== "access"\) setView\("orbit"\)/);
  assert.match(planner, /view !== "access" \? <section className="global-metrics"/);
  assert.match(planner, /view !== "audit" && view !== "access"/);
  assert.match(planner, /<details className="technical-details/);
  assert.doesNotMatch(planner, /<details[^>]*\sopen(?:=|>)/);

  const navOrder = ["audit", "coverage", "orbit", "access"].map((key) => planner.indexOf(`${key}: {`));
  assert.ok(navOrder.every((index) => index >= 0));
  assert.deepEqual([...navOrder].sort((left, right) => left - right), navOrder);
  assert.match(planner, /aria-pressed=/);

  assert.match(css, /@media \(max-width: 760px\)/);
  assert.match(css, /\.global-header nav \{[^}]*overflow-x:\s*auto/);
  assert.match(css, /\.global-workspace \{[^}]*grid-template-columns:\s*1fr/);
  assert.match(css, /\.calendar-grid \{[^}]*grid-template-columns:\s*repeat\(2,\s*1fr\)/);
  assert.match(css, /\.access-answer-grid \{[^}]*grid-template-columns:\s*repeat\(2,\s*1fr\)/);
  assert.match(css, /\.decision-hero \{[^}]*grid-template-columns:\s*1fr/);
  assert.match(css, /@media \(max-width: 430px\)/);
  assert.match(css, /\.global-metrics \{[^}]*grid-template-columns:\s*1fr/);
});

test("active page uses the global planner and retains the local-only Sites shell", async () => {
  const [page, layout, packageJson] = await Promise.all([
    readFile(new URL("../app/page.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/layout.tsx", import.meta.url), "utf8"),
    readFile(new URL("../package.json", import.meta.url), "utf8"),
  ]);

  assert.match(page, /GlobalPlanner/);
  assert.match(layout, /lang="zh-CN"/);
  assert.match(layout, /NTN全球陆地接入方案/);
  assert.doesNotMatch(packageJson, /react-loading-skeleton/);
  await assert.rejects(access(new URL("../app/_sites-preview/SkeletonPreview.tsx", import.meta.url)));
});
