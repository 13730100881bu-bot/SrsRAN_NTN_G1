# NTN 全球星地波位规划台

这个 Web 工程用于推演星载再生 gNB 的全球两级波位与双小区跳波束方案。当前目标服务范围是南纬 `57°` 到北纬 `57°` 的全球陆地区域，海洋和更高纬度不计入连续服务验收。

## 当前工程候选

- 当前 Web 仍以 42 个轨道面、每面 84 星，共 3,528 星作为动画展示基线。它不是最终选型。
- 新一轮粗筛发现了更小的 46 个轨道面、每面 65 星，共 2,990 星候选，比展示基线减少 538 星。一天内每 120 秒一次的 720 个检查时刻均未发现覆盖空缺，并且每个检查时刻都完成了全网唯一分配；实际负责峰值为 87 个一级波位/星。它仍需七天连续事件检查和单星故障检查，不能表述为“2,990 星已经足够”。
- 旧的初始排列已淘汰：保存的起始时刻证据存在 23 个一级波位覆盖空缺。
- 新选/退出仰角：`45° / 42°`；完整可见清单必须保存所有满足几何条件的一级波位，不能被服务容量裁剪。
- 每颗卫星两个长期星载 NR 小区；每小区 `16 analog / 64 digital`，整星 `32 analog / 128 digital`，暂不跨小区借用。
- 日历使用 `10 ms` access slot 和每端口 4 次顺序 `2.5 ms` 子访问。任意 80 ms 窗口的最差模拟下行日历提供 168 次一级波位访问机会；当前软件规划上限为每小区 128、每星 256。这个上限限制的是全网唯一分配后由卫星实际负责的一级波位，不是可见区域数，也不是卫星能够形成的波束数。
- SSB 与 PRACH 都按已分配的一级波位安排；SSB 计划间隔目标不超过 80 ms，PRACH 计划间隔目标不超过 640 ms。
- NCI 属于星载小区并由中心 registry 分配；PCI 依据同时可见、同频小区的冲突图复用，不随波位跳变。

这些值是当前工程候选参数，不是协议常量或载荷定案。[`app/global-constellation-audit.json`](app/global-constellation-audit.json) 仍记录 `selectedScenario=null`、exact audit `status=not_run`，所以当前结论只能是“有更小候选值得继续验证”，不能描述为全球连续覆盖已经通过：

| 场景 | 当前证据 | 准确结论 |
|---|---|---|
| 已淘汰的初始排列 | 起始时刻有 23 个一级波位没有达到 45° 的候选卫星 | 起始时刻已经失败，不再采用 |
| 3,528 星展示基线 | 一天固定步长检查未发现覆盖空缺；单星最多可见约 209 个一级波位 | “可见”不是“实际负责”，不能用 209/256 判断服务负载 |
| 2,990 星较小候选 | 720 个检查时刻未发现空缺且均完成全网唯一分配；实际分配峰值为 87 个一级波位/星 | 只说明候选值得进入下一轮；不是最小值，也不是最终选型 |

较小候选筛选由仓库根目录的 `utils/ntn/constellation_screen.mjs` 生成可复现报告。固定步长粗筛无权写入 `selectedScenario`；只有另行完成的连续事件检查、故障检查和接入日历检查才可用于最终选择。

旧 `Walker Delta 53°:720/30/1`、大陆及海南 `2,620` 个 L1 / `18,208` 个 L2、每星 `42/84` 日历容量和相应一天 120 s 采样结果，均保留为历史中国样例，不是当前全球基线。

## 已生成的全球波位目录

Web 已使用本地 `world-atlas@2.0.2` 打包的 Natural Earth 4.1.0、`1:50m` land 数据生成 `57°S～57°N` 目录：

- `36,411` 个 L1，其中 `33,871` 个 full、`2,540` 个 edge。
- `249,375` 个有效 L2。
- 244 个等 `sin(latitude)` 纬度带；目标球面单元面积 `3,117.691454 km²`，最大面积偏差 `0.1115%`。
- L1 使用稳定顺序号 `G000001`～`G036411`；L2 使用 `<L1>-0`～`<L1>-6`，仅为 `childMask` 中有效的局部槽位生成 ID。
- cells 完整性 SHA-256：`b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a`。

实现与数据入口：

- 生成器：[`scripts/generate-global-land-catalog.mjs`](scripts/generate-global-land-catalog.mjs)
- 紧凑目录：[`public/data/global-land-l1-v1.json`](public/data/global-land-l1-v1.json)
- 本地陆地源：[`public/data/land-50m.json`](public/data/land-50m.json)
- loader/type：[`app/beam-catalog.ts`](app/beam-catalog.ts)
- focused tests：[`tests/beam-catalog.test.mjs`](tests/beam-catalog.test.mjs)

生成方法是等 `sin(latitude)` 分带的球面等面积 quasi-hex 中心格；它满足 `equalAreaGrid=true`，但必须同时保留 `exactRegularSphericalHexagons=false` 和 `exactCoastlineClipping=false`。陆地判定采用中心点包含，不是对海岸线做精确六边形裁剪。因此该目录在 Web 中为`已实现`且 focused 目录测试 `6/6` 已有测试覆盖，但仍不是 CU-CP 运行态目录，也不是正式运营 GIS 冻结成果。

## 本地运行

Windows 下可直接双击 `open_ntn_beam_planner.cmd`。它会启动本机服务并打开：

`http://127.0.0.1:4317`

也可以手动运行：

```bash
npm install
npm run dev -- -H 127.0.0.1 -p 4317
```

生产构建与渲染测试：

```bash
npm run build
npm test
```

目录可重复生成与一致性检查：

```bash
npm run catalog:generate
npm run catalog:check
```

本轮只整理 Web/文档方案，不修改 CU-CP、F1AP、DU、MAC、PHY、RU、C++ API 或运行配置。
