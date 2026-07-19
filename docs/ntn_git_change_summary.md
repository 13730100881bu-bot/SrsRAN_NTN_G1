# NTN fork：官方 srsRAN 基线与 Git 提交序列

Last updated: 2026-07-19

## 权威基线

本分支不再使用本地 `main`、本地 `HEAD` 或
`ai/baseline-before-goal` 作为“srsRAN 原版”。唯一 Git 基线是：

- repository: `https://github.com/srsran/srsRAN_Project.git`
- ref: official `main`
- commit: `4bf1543936d062686d64c10724d2f27a9854f065`
- tree: `c3dee5a8b7408dbeb65218f6603e58fb3f7732f8`
- upstream subject: `readme: add OCUDU notice`

本机已有的官方 `srsRAN_Project-main.zip` 只用作 partial clone 的 blob
缓存。审计逐一计算了官方 tree 的 5,274 个 leaf blob，路径数一致且
SHA mismatch 为 0；分支身份仍由上述 official commit/tree 决定，不由
ZIP 文件名或本地快照决定。

`release_25_10` 对应另一个官方 commit
`d2f4b70dda8e2c557d5b05a0ac5f92dbddda19bc`。旧本地快照虽然声明
`25.10.0`，但其 tree 不等于该 release tag；同时它包含 official `main`
上的 OCUDU notice 和早期本地改动。因此，本次选择实际来源对应的
official `main@4bf1543`，而不是仅凭版本字符串选择 release tag。

## 原始 fork 差异

将整理前成果树 `c4e19d8` 直接与 official tree 比较，原始差异为：

| Metric | Value |
|---|---:|
| changed paths | 497 |
| added paths | 292 |
| modified paths | 205 |
| inserted lines | 123,600 |
| deleted lines | 1,013 |

其中并非全部都是产品代码：大体包括 CU-CP NTN runtime、NGAP/NRPPa/RRC、
F1AP/DU/MAC/scheduler、shared NTN utilities、配置与规划数据、system proof、
研发 harness 和文档。原始统计还包含 snapshot audit、大型 raw diff、运行
结果和 Windows ZIP 导致的 executable-bit 回退，因此不能直接当作净功能
规模。

## 清理后的 official-based 分支

维护分支和 worktree：

- branch: `codex/ntn-upstream-series`
- worktree: `D:\code\srsRAN_Project-upstream-series`
- publish base: `vendor/srsran-main-4bf1543-snapshot@8f2ede2`
- provenance branch: `codex/ntn-upstream-full-history@20ab578`
- ancestry: publish base 的 tree 与 official `4bf1543` 完全相同；由于个人目标
  repository 原有不相关的 README 历史，发布 DAG 不导入完整 official ancestry

`codex/ntn-upstream-full-history` 保留直接以 official `4bf1543` 为父系起点的
本地审计证据。`codex/ntn-upstream-series` 则从 exact-tree snapshot root 重放
同一组 patch，适合推送到当前个人仓库。两者最终产品 tree 在重放后完全一致；
snapshot 方式不声称与 official commit 存在 Git ancestry。

最终净差异统计（包含本报告）为：

| Metric | Value |
|---|---:|
| changed paths | 461 |
| added paths | 275 |
| modified paths | 186 |
| inserted lines | 99,070 |
| deleted lines | 1,013 |

### 提交划分

| Commit | Slice |
|---|---|
| `c89d3c1` | repository LF/binary/ignore policy |
| `0eeab89` | shared beam hopping, orbit and TA primitives with tests |
| `ca72129` | reproducible LEO beam-plan fixtures and generator |
| `2c0a7cb` | isolated ZMQ/Open5GS/UERANSIM synthetic lab profiles |
| `4107aed` | non-NTN `RRC_INACTIVE` and Inter-CU handover improvements |
| `c2fc525` | O-DU → DU/MAC runtime beam-hopping update path |
| `6cd7c29` | initial CU-CP location, beam assignment and mobility runtime |
| `7211d68` | CU-CP inventory, service policy, SIB19, capability and onboard plan |
| `05478ff` | NGAP/NRPPa/RRC positioning and resume flows |
| `fe35e05` | F1AP/DU/MAC/scheduler resource and access-calendar execution |
| `12f3ba6` | source context, task cards, guards and harness templates |
| `8c41786` | split-stack system proof scripts/configuration |
| `21094b9` | NTN architecture, planning and evidence-boundary documents |
| `93861ff` | dated AI/CU-CP evaluation notes, isolated from product code |
| `cd5ea1e` | executable mode for the NTN plan generator |

`RRC_INACTIVE` / Inter-CU handover 被单独列出，因为它们相对 official
upstream 是真实改进，但不是 NTN 功能。产品代码阶段采用已有阶段 tree
逐层重放，避免按目录猜测 `cu_cp_impl.cpp`、`f1ap_cu_impl.cpp` 等 mixed
hub file 的 hunk 归属。

## 当前设计收敛分支

官方差异整理完成后，当前工程在独立分支继续做星载双小区模型收敛：

- branch: `codex/ntn-design-convergence`
- base: `codex/ntn-upstream-series@96f18a9`
- personal remote: `origin` → `13730100881bu-bot/SrsRAN_NTN_G1`
- 当前已推送代码基线: `263db8a`

已经分笔推送的提交为：

| Commit | 作用 |
|---|---|
| `7870b4e` | 规划输入绑定 catalog、identity registry 和 access profile |
| `38b4440` | 双小区 640 ms 接入日历与三阶段资源模型 |
| `93f1c6f` | DU 静态 SSB/PRACH opportunity 预检 |
| `8dc2299` | active/pending 计划持久化与重启核对 |
| `3081409` | 模型收敛与验证证据文档 |
| `263db8a` | DU 重连后重新核对日历、过期清理和状态观测 |

CUCP-040 在此基础上继续补充：DU 断开后旧软件证据立即失效、旧连接反馈隔离、未来启用时刻前的 fallback、cleanup durable commit，以及状态 schema v2 对最新输入摘要和完整 candidate inventory 的独立保存。该摘要不是原始 JSON 的逐字段副本。代码/测试已作为 `263db8a` 单独推送；说明文档仍作为下一笔提交管理。两笔都使用显式路径暂存，不使用 `git add .`。最终验证结果以 [Task Change Index](ntn_cucp_task_change_index.md) 的 CUCP-040 closeout 为准。

这个分支不会整体合并 `codex/ntn-feature-history` 的 Web/GIS 历史；全球 planner 仍作为独立管理中心输入来源。generated ASN.1、PHY、RU/RF 和 Web/GIS 不属于 CUCP-040 改动范围。

## 明确排除和修正

以下内容没有进入 official-based 历史：

- `ai_harness/audit/**`：旧独立 root snapshot 的 commit-id、path list、
  generated report 和 1 MiB raw diff；raw diff 还嵌入无关 GIS 网站路径；
- `ai_harness/results/**`：运行生成物；
- 19 个 official executable 文件的 `100755 → 100644` Windows mode 回退；
- `docs/lib/cu_cp/ue_manager/cu_cp_ue_impl.h`：0-byte 误产物；
- build、log、pcap、archive、local credential 和独立 Web workspace；
- 旧版 `docs/ntn_git_change_summary.md` 的 local-baseline 结论。

新增的 shell/Python entry points 已按用途设置 `100755`。实验配置中的
`00101` IMSI/K/OPC 是 synthetic lab profile，并被隔离在 `test(lab)` 或
`test(runtime)` 提交；部署时应整体替换，不得当作生产凭据。

`web_replicas/ntn_beam_planner` 是独立 Git repository，不作为 srsRAN
源码提交或 gitlink 混入本分支。它在自己的
`codex/ntn-global-planner-history` 分支管理。

## 与 official upstream 的模块边界

- generated ASN.1：无改动；
- PHY、lower PHY、RU/RF、OFH：无源码改动；
- Web/GIS：无外层 srsRAN tree 改动；
- F1AP/DU/MAC/scheduler：仅保留 NTN resource feedback、SIB19 application、
  RNTI/SR/SRS 和 access-calendar software gate 所需路径；
- CU-CP：保存完整 candidate inventory、两个星载 logical cell 计划、calendar
  audit、atomic activation、UE/resource state 和只读观测。

access calendar 当前仍是 software intent/gate。它证明计划、反馈和软件侧
准入闭环，不证明 RF beam steering、真实 PRACH detector 或空口覆盖已经执行。

## official-based 整理阶段验证

本轮整理不改变最终 C++ 业务内容；官方分支代码路径与整理前已验证成果树
逐 blob 一致。focused validation 记录：

| Validation | Result |
|---|---|
| `cmake --build build/ai-clean --target ntn_mobility_test -j1` | passed in the cache-native WSL environment (36.8 s) |
| `ctest --test-dir build/ai-clean -R "ntn_mobility" --output-on-failure` | matched 153 tests; timed out near test 3 after 304 s and was stopped (exit 124) |
| CU-CP onboard/calendar focused cases | 26/26 passed |
| MAC calendar compiler focused cases | 7/7 passed |
| scheduler access gate focused cases | 10/10 passed |
| F1AP CU resource-coordination cases | 12/12 passed |
| F1AP DU resource-coordination cases | 6/6 passed |
| runtime dry-run self-test | passed |

较大的 dependency-heavy build 若未重跑，应在交付说明中明确列为未运行，不能
把 focused unit/system intent evidence 写成全球连续覆盖或真实 RF 证据。
`build/ai-clean` 是 WSL-native cache；直接从 Windows 调用因 source path
不匹配而立即拒绝，随后在该 cache 的原生 WSL 环境完成了上述 successful
build。超时的 `ctest` 输出被缓冲，不能据此推断前两项在本轮是否通过。

## 常用审查命令

```powershell
git log --oneline vendor/srsran-main-4bf1543-snapshot..codex/ntn-upstream-series
git diff --stat vendor/srsran-main-4bf1543-snapshot codex/ntn-upstream-series
git diff --name-status vendor/srsran-main-4bf1543-snapshot codex/ntn-upstream-series
git rev-parse vendor/srsran-main-4bf1543-snapshot^{tree}
git rev-parse 4bf1543936d062686d64c10724d2f27a9854f065^{tree}
git range-diff 4bf1543..codex/ntn-upstream-full-history `
  vendor/srsran-main-4bf1543-snapshot..codex/ntn-upstream-series
```

两个 `rev-parse` 命令的预期结果均为
`c3dee5a8b7408dbeb65218f6603e58fb3f7732f8`。`range-diff` 应显示 16 笔
patch 一一对应；个人仓库原有 `main` 与本发布分支不相关，不应用作 compare
base。旧
`codex/ntn-feature-history` 只保留为安全备份，不再作为 upstream-relative
交付分支。
