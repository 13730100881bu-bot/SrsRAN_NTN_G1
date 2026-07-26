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
  const visibleHtml = html.replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, "");
  assert.match(html, /NTN 全球陆地接入方案/);
  assert.match(html, /57°S～57°N陆地/);
  assert.match(html, /最终采用：2,990颗/);
  assert.match(html, /最终方案采用46个轨道面、每面65颗，共2,990颗卫星/);
  assert.match(html, /87 \/ 256/);
  assert.match(html, /方案结论/);
  assert.match(html, /全球覆盖/);
  assert.match(html, /卫星负载/);
  assert.match(html, /跳波束日历/);
  assert.match(html, /最终结论与验收边界/);
  assert.match(html, /720 \/ 720/);
  assert.match(html, /可见.*实际负责数量分开计算/s);
  assert.match(html, /60°倾角表示轨道平面相对赤道的倾斜程度/);
  assert.match(html, /最长80 ms一次/);
  assert.match(html, /每个已分配一级波位在640 ms内各安排一次上行接入机会（PRACH）/);
  assert.match(html, /是否已经接入基站程序/);
  assert.match(html, /尚未接通/);
  assert.match(html, /最终方案参数/);
  assert.match(html, /46个轨道面 × 每面65颗/);
  assert.doesNotMatch(html, /209\s*\/\s*256|距离256个区域上限/);
  assert.doesNotMatch(html, /3,528|历史展示对照|未采用的初始方案|已淘汰的初始排列|不再作为目标规模|动作演示|STEP|当前无选定方案|为什么还不能写|阶段判断|进入候选复核|尚未最终定案|最终数量尚未选定|不代表最终选型/);
  assert.doesNotMatch(html, /64 个地固|地固 NCI|NCI 属于地固|每星 84 个 L1/);
  assert.doesNotMatch(html, /F=0|F=1|<th>F<\/th>/);
  assert.doesNotMatch(visibleHtml, /\bL1\b|\bL2\b|global-land-l1-v1/i);
  assert.doesNotMatch(html, /codex-preview|react-loading-skeleton|Your site is taking shape/i);
});

test("audit evidence remains separate from the final engineering decision", async () => {
  const [audit, seedSnapshot, f1Day, planner, globalMap, hexagonModel, orbitView, beamAnimation, beamMapLayout, animationModel, css] = await Promise.all([
    readFile(new URL("../app/global-constellation-audit.json", import.meta.url), "utf8").then(JSON.parse),
    readFile(new URL("../app/global-constellation-snapshot.json", import.meta.url), "utf8").then(JSON.parse),
    readFile(new URL("../app/global-constellation-f1-day-coarse.json", import.meta.url), "utf8").then(JSON.parse),
    readFile(new URL("../app/global-planner.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/global-map.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/global-position-hexagon.ts", import.meta.url), "utf8"),
    readFile(new URL("../app/global-orbit-view.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/beam-hopping-animation.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/beam-hopping-map-layout.ts", import.meta.url), "utf8"),
    readFile(new URL("../app/beam-hopping-animation-model.ts", import.meta.url), "utf8"),
    readFile(new URL("../app/global.css", import.meta.url), "utf8"),
  ]);

  assert.equal(audit.selectedScenario, null);
  assert.equal(seedSnapshot.summary.maximumUncoveredL1, 23);
  assert.equal(f1Day.sampling.epochCount, 720);
  assert.equal(f1Day.summary.maximumUncoveredL1, 0);
  assert.equal(f1Day.summary.maximumVisibleL1PerSatellite, 209);
  assert.equal(f1Day.exact, false);
  assert.doesNotMatch(planner, /3,528颗|历史展示对照|未采用的初始方案|已淘汰的初始排列|不再作为目标规模/);
  assert.match(planner, /现有依据来自一天、每\{smallerScreen\.sampling\.stepSeconds\}秒检查一次/);
  assert.match(planner, /用于标记连续覆盖正式验收进度/);
  assert.match(planner, /每个一级波位的SSB计划间隔/);
  assert.match(planner, /每个一级波位的PRACH计划间隔/);
  assert.match(planner, /满载128 \/ 128次机会/);
  assert.match(planner, /两个星载小区/);
  assert.match(planner, /日历就是卫星轮流照向不同地面区域的时间安排/);
  assert.match(planner, /<BeamHoppingAnimation/);
  assert.match(planner, /cells=\{accessCapacityReady \? animationCells : \[\]\}/);
  assert.match(beamAnimation, /淡色六边形是本星负责的全部一级波位/);
  assert.match(beamAnimation, /80 ms内，这颗卫星如何分批广播网络发现信号/);
  assert.match(beamAnimation, /当前 2\.5 ms 正在广播的一批/);
  assert.match(beamAnimation, /为什么亮色波位不一定挨着/);
  assert.match(beamAnimation, /当前软件按波位编号和可用波束分批/);
  assert.match(beamAnimation, /第 \{frameIndex \+ 1\} \/ \{frames\.length \|\| 32\} 批/);
  assert.match(beamAnimation, /className="beam-animation-facts"/);
  assert.match(beamAnimation, /播放动画/);
  assert.match(beamAnimation, /无线设备接入后，将按同一日历核对天线和空口的实际发射/);
  assert.doesNotMatch(beamAnimation, /不表示天线、波束成形或真实无线发射已经执行/);
  assert.match(beamAnimation, /const \[playing, setPlaying\] = useState\(false\)/);
  assert.match(beamAnimation, /<canvas/);
  assert.match(beamAnimation, /<div className="beam-animation-orbit-note"[\s\S]*?<div className="beam-animation-canvas">/);
  assert.match(beamAnimation, /createGlobalPositionHexagon/);
  assert.match(beamAnimation, /geoMercator/);
  assert.match(beamAnimation, /drawHexagonLayer/);
  assert.match(beamAnimation, /new ResizeObserver\(scheduleDraw\)/);
  assert.match(beamAnimation, /window\.requestAnimationFrame\(draw\)/);
  assert.doesNotMatch(beamAnimation, /geoAzimuthalEqualArea|createLinearGradient\(satelliteX|fillText\(cell\.id/);
  assert.match(beamMapLayout, /createBeamAnimationMapViewport/);
  assert.match(beamMapLayout, /const orderedCells = \[\.\.\.cells\]\.sort/);
  assert.match(animationModel, /createCellBeamSchedule/);
  assert.match(animationModel, /entry\.purpose === "ssb"/);
  assert.match(planner, /这颗卫星如何为实际负责的地面区域安排接入时段/);
  assert.match(planner, /终端多久遇到一次网络发现信号/);
  assert.match(planner, /终端多久获得一次上行接入机会/);
  assert.match(planner, /8表示重复8轮，不是只能服务8个波位/);
  assert.match(planner, /一个640 ms周期包含8轮80 ms发现安排/);
  assert.match(planner, /倾角表示轨道平面相对赤道的倾斜程度/);
  assert.match(planner, /相邻中心约/);
  assert.match(planner, /targetCellAreaKm2/);
  assert.match(planner, /可见候选 · 由其他卫星负责/);
  assert.match(planner, /查看80 ms轮转明细/);
  assert.match(planner, /查看完整可见波位表/);
  assert.match(planner, /基站程序目前只能接收一张/);
  assert.match(planner, /下一步由基站输入接口同时接收两张清单/);
  assert.doesNotMatch(planner, /这项结论还不代表什么|可生成80 ms计划|可生成640 ms计划|这些计划不代表/);
  assert.match(planner, /卫星数量已经确定；如验收不满足，再按结果调整设计/);
  assert.doesNotMatch(planner, /阶段判断|进入候选复核|尚未最终定案|最终数量尚未选定|不代表最终选型/);
  assert.match(planner, /setOrbitFocusRequestId\(\(value\) => value \+ 1\)/);
  assert.match(planner, /focusRequestId=\{orbitFocusRequestId\}/);
  assert.match(planner, /aria-live="polite"/);
  assert.match(planner, /可省略前导零，也可以按 Enter 查找/);
  assert.match(planner, /view !== "access" \? <section className="global-metrics"/);
  assert.doesNotMatch(planner, /部署前连续服务验收|metric-review/);
  assert.match(planner, /view !== "audit" && view !== "access"/);
  assert.match(planner, /<details className="technical-details/);
  assert.doesNotMatch(planner, /<details[^>]*\sopen(?:=|>)/);
  assert.match(orbitView, /卫星轨迹与覆盖关系/);
  assert.match(orbitView, /真实陆地轮廓/);
  assert.match(orbitView, /createEarthTexture/);
  assert.match(orbitView, /geoGraticule10/);
  assert.match(orbitView, /陆地边界已加载/);
  assert.doesNotMatch(orbitView, /WireframeGeometry/);
  assert.doesNotMatch(orbitView, /satellites\.length\.toLocaleString|显示 \$\{satellites\.length\} 颗卫星/);
  assert.match(planner, /landUrl=\{LAND_TOPOLOGY_URL\}/);
  assert.doesNotMatch(`${planner}\n${globalMap}`, /（L1）|（L2）|个L1|个L2|可见L1|负责L1|未分配L1|<th>L1<\/th>/);
  assert.match(globalMap, /滚轮缩放 · 点击选择一级波位/);
  assert.match(globalMap, /addEventListener\("wheel", handleWheel, \{ passive: false \}\)/);
  assert.match(globalMap, /createGlobalMapProjection/);
  assert.match(globalMap, /一级波位六边形边界/);
  assert.match(globalMap, /相邻一级波位合并显示/);
  assert.match(globalMap, /继续放大后会自动展开为单个六边形/);
  assert.match(globalMap, /createGlobalPositionHexagon/);
  assert.match(globalMap, /createGlobalMapPyramid/);
  assert.match(globalMap, /selectGlobalMapRenderPlan/);
  assert.match(globalMap, /drawHexagonLayer/);
  assert.match(globalMap, /baseLayerRef/);
  assert.match(globalMap, /requestAnimationFrame\(draw\)/);
  assert.doesNotMatch(globalMap, /mapGeometry\.allIndexes/);
  assert.match(globalMap, /getComputedStyle\(parent\)\.minHeight/);
  assert.doesNotMatch(globalMap, /fillRect\(point\[0\] - 0\.5/);
  assert.match(hexagonModel, /HEXAGON_BEARINGS_DEG = \[0, 60, 120, 180, 240, 300\]/);
  assert.match(hexagonModel, /spacingKm \/ Math\.sqrt\(3\)/);
  assert.match(planner, /positionSpacingKm=\{positionSpacingKm\}/);
  assert.match(planner, /metadata\.generation\?\.targetCellAreaKm2/);
  assert.match(globalMap, /恢复全图/);

  const navOrder = ["audit", "coverage", "orbit", "access"].map((key) => planner.indexOf(`${key}: {`));
  assert.ok(navOrder.every((index) => index >= 0));
  assert.deepEqual([...navOrder].sort((left, right) => left - right), navOrder);
  assert.match(planner, /aria-pressed=/);

  assert.match(css, /@media \(max-width: 760px\)/);
  assert.match(css, /\.global-header nav \{[^}]*overflow-x:\s*auto/);
  assert.match(css, /\.global-toolbar form > div \{[^}]*grid-template-columns:\s*minmax\(0,\s*1fr\)\s+72px/);
  assert.match(css, /\.global-toolbar form button \{[^}]*min-width:\s*72px[^}]*white-space:\s*nowrap/);
  assert.match(css, /\.global-workspace \{[^}]*grid-template-columns:\s*1fr/);
  assert.match(css, /\.calendar-grid \{[^}]*grid-template-columns:\s*repeat\(2,\s*1fr\)/);
  assert.match(css, /\.access-answer-grid \{[^}]*grid-template-columns:\s*repeat\(2,\s*1fr\)/);
  assert.match(css, /\.beam-animation-map \{[^}]*min-height:\s*430px/);
  assert.match(css, /\.beam-animation-map \{[^}]*display:\s*grid[^}]*grid-template-rows:\s*auto minmax\(0,\s*1fr\)/);
  assert.match(css, /\.beam-animation-canvas \{[^}]*position:\s*relative[^}]*min-height:\s*0/);
  assert.doesNotMatch(css, /\.beam-animation-orbit-note \{[^}]*position:\s*absolute/);
  assert.match(css, /\.beam-animation-controls \{[^}]*grid-template-columns:\s*auto\s+1fr/);
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
