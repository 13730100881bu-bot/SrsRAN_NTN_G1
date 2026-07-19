# NTN 星载双小区版本化波位计划

本文说明管理中心如何把一颗卫星的完整可见 L1 波位表交给星载 CU-CP，以及 CU-CP 如何校验、划分、审计、等待启用、处理 DU 断开和重启恢复。

这是一条默认关闭的独立 NTN 路径。关闭时，不读取计划文件、不创建恢复文件、不查询或清理 DU 日历，terrestrial 行为保持原样。

## 1. 先看懂整体关系

- 每颗卫星长期拥有两个 NR logical cell；两个小区各自有稳定的 36-bit opaque NCI 和 PCI。
- NCI/PCI 属于星载小区，不属于地面波位；CU-CP 不从卫星编号、坐标或波位编号推导 NCI。
- 地面 L1 使用 `G######`，表示周期访问的信令波位；L2 是其子级数字业务位置。
- 管理中心负责全球目录、轨道传播、可见性、星座搜索和身份 registry；CU-CP 只处理本星收到的完整输入。
- CU-CP 将所有 L1 确定性地分给两个长期小区，生成 SSB/PRACH 接入日历，并在配置允许时请求 DU/MAC 安装软件日历。
- `applied` 只表示匹配的软件 SSB/PRACH gate 已安装，不表示天线、波束成形、PHY、RU 或 RF 已执行。

```text
管理中心完整输入
        |
        v
格式/身份/版本/时间/hash 校验
        |
        v
两个长期小区的确定性划分
        |
        v
640 ms 接入日历生成与审计
        |
        +---- dry-run：只观察，不下发
        |
        +---- execution：DU 静态机会预检 -> 软件 gate -> 到点切换
```

## 2. 两种运行模式

### 2.1 Dry-run

`enabled: true` 且 `du_execution_enabled: false`：

- 读取并严格校验计划；
- 保存完整候选输入；
- 生成双小区划分和接入日历；
- 输出审计结果；
- 不向 DU/MAC 下发，不创建执行证据。

旧 schema v1 只允许在这个模式下使用。v1 缺少完整规划上下文，因此不能开启执行。

### 2.2 Software execution

`du_execution_enabled: true`：

- 只接受 schema v2；
- 要求配置独立 `state_file`；
- 要求本地 catalog、identity registry 和 access profile 与计划完全匹配；
- 先核对 DU 的两个 NCI/PCI 和静态 SSB/PRACH opportunity；
- 只有完整的 matching feedback 才能进入 `ready` 或 `applied`；
- 到达 `activation_epoch` 后才切换 active plan。

这个模式仍然只是 CU/DU/MAC 软件执行，不是 RF 执行。

## 3. 配置示例

```yaml
ntn_onboard_position_plan:
  enabled: true
  du_execution_enabled: true
  satellite_id: P01-S01
  plan_json_file: /var/lib/srsran/ntn/position-plan.json
  state_file: /var/lib/srsran/ntn/position-plan-state.json

  expected_catalog_id: global-land-l1-v1
  expected_catalog_hash: "sha256:<catalog hash>"
  expected_identity_registry_version: mc-ntn-onboard-cell-registry-v1
  expected_identity_registry_hash: "sha256:<registry hash>"
  expected_access_profile_id: ntn-access-16a-64d-v1
  expected_access_profile_hash: "sha256:<profile hash>"

  cell_ncis: [4886691841, 4886691842]
  cell_pcis: [101, 101]

  max_l1_positions_per_cell: 128
  max_l1_positions_per_satellite: 256
  max_analog_ports_per_cell: 16
  max_analog_ports_per_satellite: 32
  max_digital_ports_per_cell: 64
  max_digital_ports_per_satellite: 128

  access_slot_us: 10000
  subvisit_duration_us: 2500
  max_ssb_interval_ms: 80
  max_prach_interval_ms: 640
  activation_alignment_ms: 640

  reload_period_ms: 2000
  du_prepare_horizon_ms: 4000
  du_prepare_guard_ms: 1000
  du_apply_timeout_ms: 500
```

`Pxx-Syy` 是当前 registry 的规范格式，例如 `P01-S01`。CU-CP 对它做 opaque exact-match；旧样例 `P01-S001` 不再接受。

容量和时序值属于可配置 access profile，不是协议常量。两个小区的配置顺序没有身份含义；实现按 NCI 排序确定日历 parity。两个 NCI 必须不同，PCI 可以按冲突规划复用。

## 4. 管理中心 schema v2

```json
{
  "schema_version": 2,
  "planning_run_id": "global-audit-2026-07-18T00:00:00Z",
  "catalog": {
    "id": "global-land-l1-v1",
    "sha256": "sha256:<64 lowercase hex digits>"
  },
  "identity_registry": {
    "version": "mc-ntn-onboard-cell-registry-v1",
    "sha256": "sha256:<64 lowercase hex digits>"
  },
  "access_profile": {
    "id": "ntn-access-16a-64d-v1",
    "sha256": "sha256:<64 lowercase hex digits>"
  },
  "satellite_id": "P01-S01",
  "catalog_version": 10,
  "schedule_version": 20,
  "content_hash": "sha256:<64 lowercase hex digits>",
  "valid_from_unix_ms": 1780000000000,
  "valid_until_unix_ms": 1780003600000,
  "activation_epoch_unix_ms": 1780000000640,
  "onboard_cells": [
    {"nci": 4886691841, "pci": 101},
    {"nci": 4886691842, "pci": 101}
  ],
  "visible_l1_positions": [
    {
      "position_id": "G000001",
      "latitude_deg": 10.0,
      "longitude_deg": 20.0,
      "child_mask": 127
    }
  ]
}
```

解析采用 exact-key：缺字段、未知字段、错误类型、重复 L1、非法 `child_mask` 或非法时间都会拒绝。`child_mask` 是 7-bit L2 目录关系；当前阶段只校验和保存，不把它直接变成数字业务运行态。

执行模式还会核对：

- `satellite_id` 与本星配置完全一致；
- catalog、identity registry 和 access profile 的 ID/hash 完全一致；
- 两个 NCI/PCI 与本地长期小区身份完全一致；
- `catalog_version` 和 `schedule_version` 单调递增；
- `activation_epoch` 位于有效期内并满足配置的对齐要求；
- `content_hash` 与规范内容一致。

## 5. Content hash

`content_hash` 不是原始 JSON 文件字节的 hash。它对规范化后的逻辑内容计算 SHA-256，包含：

- schema v2 的 `planning_run_id` 以及 `catalog`、`identity_registry`、`access_profile` 对象中的规范逻辑值；
- satellite、catalog/schedule version 和三个时间字段；
- 按 `(nci,pci)` 排序的两个星载小区；
- 按 `position_id` 排序的全部 L1，包括坐标和 `child_mask`。

输出格式为 `sha256:<hex>`。管理中心 producer 应与 C++ `compute_ntn_position_plan_content_hash()` 使用同一 golden vector。

## 6. 完整候选输入与 257 条规则

CU-CP 在容量判断前先保存完整 `visible_l1_positions`。因此：

- 256 条可以进入双小区日历审计；
- 257 条仍完整保存 257 条；
- 257 条返回 `schedule_overflow`，不激活、不截断、不改变旧 active plan；
- 最新输入的只读观测可以高于 accepted version high-water，但不会成为部署权威。

执行模式的状态文件 schema v2 独立保存这份最新观测。即使旧计划随后被清理并再次重启，257 条也不会退回成旧 active plan 的较小 inventory。schema v1 状态文件仍可读取；只有 v1 会从其 active/pending snapshot 补出旧式观测，v2 的 `received_plan:null` 明确表示没有观测。

## 7. 两个小区如何划分

划分保持以下规则：

- 每个 L1 恰好出现一次；
- 相同输入得到相同结果；
- 首次划分优先保持空间紧凑和两边数量平衡；
- 更新优先保持已有 L1 的所属小区，只为容量和平衡移动必要的最少位置；
- 两个小区的 NCI/PCI 始终来自本地身份配置，不随划分改变；
- 当前 profile 每小区最多 128 L1，整星最多 256 L1。

持久化的 sticky partition 用于减少小更新和重启后的波位抖动。它不是新的小区编号，也不引入 `onboard_cell_index` 或绑定对象。

## 8. 接入日历

默认 profile 使用 640 ms supercycle 和 10 ms access slot。按排序后的 NCI：较小 NCI 使用偶数 slot，另一个使用奇数 slot。三阶段资源分别为：

| 阶段 | 每小区 analog DL/UL | 每小区 digital DL/UL |
|---|---:|---:|
| 0 | `11 / 5` | `43 / 21` |
| 1 | `11 / 5` | `43 / 21` |
| 2 | `10 / 6` | `42 / 22` |

资源不跨小区借用。每个 L1：

- 每个 80 ms 子窗口恰好一个 SSB intent；
- 每 640 ms 恰好一个 PRACH RO；
- 每个 PRACH RO 必须有同 NCI、position、时间、方向和端口的 UL beam intent。

256 L1 满载时共有 2,560 个 intent：2,048 个 SSB、256 个 PRACH RO、256 个 UL beam。未使用的机会保持空闲，不用伪造 L1 填满资源。

审计会拒绝：SSB/PRACH 间隔超限、重复或缺失 RO/UL 配对、孤立 UL beam、方向错误、端口越界、同小区端口时间冲突以及静默丢弃 L1。

## 9. 更新和启用

计划主状态为：

```text
received -> validated -> calendar_checked -> pending -> active
```

执行模式另有软件下发状态：

```text
not_sent -> preparing -> ready -> applied
                         \-> rejected / unsupported
```

新计划只有全部校验和日历审计通过后才替换 pending。即使 DU 提前报告 `applied`，计划也必须等到 `activation_epoch` 才能变成 active；旧计划的恢复副本在此之前继续保留。新计划失败、过期或反馈不完整时，旧有效计划不变。

执行前还会核对 DU 的实际静态配置是否能覆盖全部 SSB/PRACH opportunity。反馈包含 expected/matched 数、实际最大间隔和失败明细；部分匹配不能进入 `ready`。

## 10. 重启和 DU 重连

### 10.1 状态文件

执行模式要求私有 `state_file`。它使用原子替换，保存：

- accepted version high-water；
- active/pending plan、完整双小区划分和 hash；
- activation/validity 和最近的软件下发状态；
- 尚待确认的精确 cleanup obligation；
- 最新成功解析输入的独立只读摘要：catalog/schedule version、content hash、activation epoch 和完整 candidate inventory；它不是原始 JSON 的逐字段副本。

启动时会重新校验状态和规划上下文。历史 `applied` 不会直接恢复成当前证据；CU-CP 必须向 live DU 查询同一 version/hash 和两个小区的完整结果。

状态文件用于恢复，不是管理中心真实性证明。替换整个文件为旧的合法副本或删除文件，仍需要独立可信的单调锚点才能检测。

### 10.2 DU 断开

当承载这两个小区的 DU 断开时：

1. CU-CP 立即隐藏旧 `applied` 证据；
2. 保留计划、version high-water 和旧计划恢复副本；
3. 使正在等待的 prepare/query/clear 请求失效；
4. 重连后只接受新连接上、同一请求实例、同一 version/hash 的完整回复；
5. 旧连接迟到的回复只能记录为 stale，不能恢复 active/applied。

如果未来计划已提前 `applied` 但尚未到启用时刻，再次断开仍会保留历史 active fallback。只有未来计划再次核对成功并到点切换，才会清理旧计划；若它失败或过期，CU-CP 回到旧计划的 live-DU reconciliation。

### 10.3 清理不丢失

cleanup queue 记录精确 schedule version、calendar hash 和原因。DU 确认清理后，CU-CP 先把“不含队首”的新状态持久化；只有写盘成功并确认 durability，才从内存队列删除该任务。

如果写盘失败，任务保留在 fail-closed 内存中并阻止继续写入。崩溃后旧状态仍可能让同一 clear 被幂等重试，这是有意的 at-least-once 安全语义：允许安全重复，禁止静默丢失。DU clear 因而必须按 version/hash 幂等。若文件已替换但目录 durability 无法确认，CU-CP 同样保持 blocked，因为崩溃后可能看到旧文件或新文件。

## 11. 只读观测

`ntn_state` 显示：

- satellite、schema/profile 和 identity authority；
- received、active、pending 的 catalog/schedule version 和 hash；
- 完整 candidate 数量、两个小区的 L1 数量和容量；
- SSB/PRACH/UL beam intent 数量和最大间隔；
- DU 静态机会 expected/matched 结果；
- active calendar hash；
- cleanup queue 深度、队首 version/hash/reason；
- state file schema、generation、hash、保存结果和 write-block 状态；
- 最近拒绝原因。

这些字段是只读诊断，不改变状态。Digital 资源在本阶段只显示规划容量，仍标记为未绑定到真实数字业务运行态。

## 12. 兼容与证据边界

- 旧 `find_ntn_beam_id_by_nci`、beam-derived TAC/TAI/NGAP/Paging 和 per-beam NCI 路径继续作为默认关闭 profile 之外的兼容实现。
- 新 L1 目录不永久保存 NCI/PCI，也不注入 legacy beam table。
- 原始 PRACH detection 仍属于 PHY/DU/MAC；CU-CP 只管理计划、资源授权和可用 metadata 的 Initial UL 审计。
- 当前没有 `(nci, position_id, cell_local_port, direction) -> hardware_beam_handle` 映射，也没有设备 `prepare_bank/arm_at/cancel/query` 回执。
- 因此 software `applied` 不能写成 `device_applied`、RF 已执行或全球连续覆盖已经通过。

## 13. 代码和验证入口

主要实现：

- `lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.*`
- `lib/cu_cp/ntn_mobility/ntn_onboard_position_plan_state.*`
- `lib/cu_cp/cu_cp_impl.*`
- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`

主要测试：

- `tests/unittests/cu_cp/ntn_mobility/ntn_onboard_position_plan_test.cpp`
- `tests/unittests/cu_cp/ntn_mobility/ntn_onboard_position_plan_state_test.cpp`
- `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

阶段性命令：

```bash
cmake --build build/ai-clean --target ntn_mobility_test -j1
ctest --test-dir build/ai-clean -R "ntn_mobility" --output-on-failure
cmake --build build/ai-clean --target srsran_cu_cp cu_cp_test cu_cp_unit_config_test -j1
```

每次交付的实际通过数量和未运行项记录在 [NTN CU-CP Task Change Index](ntn_cucp_task_change_index.md)，不能用历史测试数替代当前验证结果。
