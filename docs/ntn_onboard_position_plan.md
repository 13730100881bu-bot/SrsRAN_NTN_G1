# NTN 星载双小区版本化波位计划

本文说明管理中心如何把一颗卫星的完整可见 L1 波位表和实际负责子集交给星载 CU-CP，以及 CU-CP 如何校验、划分、审计、等待启用、处理 DU 断开和重启恢复。

这是一条默认关闭的独立 NTN 路径。关闭时，不读取计划文件、不创建恢复文件、不查询或清理 DU 日历，terrestrial 行为保持原样。

## 1. 先看懂整体关系

- 每颗卫星长期拥有两个 NR logical cell；两个小区各自有稳定的 36-bit opaque NCI 和 PCI。
- NCI/PCI 属于星载小区，不属于地面波位；CU-CP 不从卫星编号、坐标或波位编号推导 NCI。
- 地面 L1 使用 `G######`，表示周期访问的信令波位；L2 是其子级数字业务位置。
- 管理中心负责全球目录、轨道传播、可见性、星座搜索和身份 registry；CU-CP 只处理本星收到的版本化输入。
- `visible_l1_positions` 保存管理中心下发的完整本星可见一级波位，允许超过 256 条，不按接入容量裁剪。
- `assigned_l1_position_ids` 是本星在该版本中实际负责的子集，必须全部来自完整可见清单。
- CU-CP 只把实际负责子集确定性地分给两个长期小区，并为这些一级波位生成 SSB/PRACH 接入日历。
- `applied` 只表示匹配的软件 SSB/PRACH gate 已安装，不表示天线、波束成形、PHY、RU 或 RF 已执行。

```text
管理中心版本化输入
        |
        +---- visible_l1_positions：完整可见清单，原样保留
        |
        +---- assigned_l1_position_ids：实际负责子集
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
- 保存完整可见清单和实际负责子集；
- 生成双小区划分和接入日历；
- 输出审计结果；
- 不向 DU/MAC 下发，不创建执行证据。

旧 schema v1 只允许在这个模式下使用。v1 缺少完整规划上下文，因此不能开启执行。

### 2.2 Software execution

`du_execution_enabled: true`：

- 接受 schema v2 和 v3；v2 按“全部可见即全部负责”的旧语义处理；
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

## 4. 管理中心 schema v3

```json
{
  "schema_version": 3,
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
    },
    {
      "position_id": "G000002",
      "latitude_deg": 10.2,
      "longitude_deg": 20.3,
      "child_mask": 127
    }
  ],
  "assigned_l1_position_ids": ["G000001"]
}
```

schema v3 把两份集合放在同一份原子计划中：

- `visible_l1_positions` 是本星在该时段的完整可见一级波位，保留 ID、坐标和 `child_mask`；
- `assigned_l1_position_ids` 是本星实际负责的 ID 子集，每个 ID 必须在完整清单中出现；
- 只有实际负责子集进入双小区划分、容量判断和 SSB/PRACH 日历；
- 管理中心 exporter 直接输出这份 schema v3 计划，旧 assignment sidecar 只保留规划端兼容信息，不再是 CU-CP 的负责集合来源。

解析采用 exact-key：缺字段、未知字段、错误类型、重复可见 ID、重复负责 ID、负责 ID 不在可见清单中、非法 `child_mask` 或非法时间都会拒绝。`child_mask` 是 7-bit L2 目录关系；当前阶段只校验和保存，不把它直接变成数字业务运行态。

schema v1/v2 继续可读，并统一解释为 `assigned = visible`。v1 仍只允许 dry-run；v2 可以按原有规划上下文规则执行，已有计划文件不需要立即迁移。

执行模式还会核对：

- `satellite_id` 与本星配置完全一致；
- catalog、identity registry 和 access profile 的 ID/hash 完全一致；
- 两个 NCI/PCI 与本地长期小区身份完全一致；
- `catalog_version` 和 `schedule_version` 单调递增；
- `activation_epoch` 位于有效期内并满足配置的对齐要求；
- `content_hash` 与规范内容一致。

### 4.1 输入规模与原子写入

计划输入使用统一边界：

- 单个 UTF-8 JSON 文件最大 4 MiB；
- `visible_l1_positions` 和 `assigned_l1_position_ids` 各最多 65,536 条；
- `planning_run_id`、catalog ID、identity registry version 和 access profile ID 各最多 256 UTF-8 bytes；
- 执行状态文件最大 16 MiB，状态 schema 继续使用 v3。

CU-CP 分块读取普通文件，并在读取前后检查文件大小和文件身份。计划文件超限、读取期间增长或数组超限统一返回 `input_too_large`，而且在分配大型数组、划分波位、生成日历和写入状态之前停止处理。该拒绝只更新最近拒绝原因；active、pending、accepted version high-water 以及最近一次成功解析的波位清单保持原值。文件路径和底层读取错误仅写日志，不进入只读状态输出。

重启状态文件采用同样的读取前后大小核对：读取期间增长时停止恢复并保持 fail-closed。双小区 assigned ID 数组和 cleanup queue 在 `reserve` 前先检查 65,536 与 66 的上限，损坏文件不能用异常数组触发大额预分配。

管理中心 Node 工具使用相同边界。输出先在目标目录创建临时文件，完整写入并同步后再原子替换目标文件；写入或替换失败时，原计划文件保持完整。限制检查不参与 canonical hash，合法计划的 hash 规则保持不变。

## 5. Content hash

`content_hash` 不是原始 JSON 文件字节的 hash。它对规范化后的逻辑内容计算 SHA-256。schema v3 包含：

- `planning_run_id` 以及 `catalog`、`identity_registry`、`access_profile` 对象中的规范逻辑值；
- satellite、catalog/schedule version 和三个时间字段；
- 按 `(nci,pci)` 排序的两个星载小区；
- 按 `position_id` 排序的全部可见 L1，包括坐标和 `child_mask`；
- 单独排序的全部 `assigned_l1_position_ids`。

所以只要可见清单或实际负责子集任一发生变化，hash 都会变化。输出格式为 `sha256:<hex>`；管理中心 Node producer 与 C++ `compute_ntn_position_plan_content_hash()` 使用同一 golden vector。schema v1/v2 保持原有 canonical hash 规则，不把兼容归一化生成的负责集合重复写入旧 hash。

## 6. 完整可见清单、实际负责子集与 257 条规则

CU-CP 在容量判断前先保存完整 `visible_l1_positions`，容量只检查 `assigned_l1_position_ids`。因此：

- 257 条可见、实际负责不超过 256 条时，完整保存 257 条并正常进入后续处理；
- 257 条实际负责时返回 `schedule_overflow`，可见清单仍不被裁剪，新计划不激活且旧 active plan 不变；
- 每小区 128、每星 256 只约束实际负责子集，不约束几何可见数量；
- 最新输入的只读观测可以高于 accepted version high-water，但不会成为部署权威。

执行模式的状态文件 schema v3 独立保存最新观测中的完整可见清单和实际负责 ID。重启后两者不会混为一份。状态文件 schema v1/v2 仍可读取，读取时按旧语义归一化为 `assigned = visible`；新写入统一使用 schema v3。`received_plan:null` 仍明确表示没有可恢复的最新输入。

## 7. 两个小区如何划分

CU-CP 只划分 `assigned_l1_position_ids` 指向的一级波位。划分保持以下规则：

- 每个实际负责的 L1 恰好出现一次；仅可见但未负责的 L1 不进入任一小区；
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

### 9.1 schema v3 执行结果

- 300 个可见一级波位、87 个实际负责一级波位时，CU-CP 保存完整 300 条可见清单，只将 87 条负责 ID 分给两个小区并下发。默认日历共 870 条：696 条 SSB、87 条 PRACH RO 和 87 条对应的 UL beam。
- DU 完整接受后，计划依次经过 `prepare -> applied -> activation`；到达启用时刻后一次性切换，两个小区的 NCI/PCI 保持不变。
- 重启后先查询当前 DU，再恢复 300 条可见清单、87 条负责 ID、双小区划分和 active 状态。
- 实际负责 257 个一级波位时，完整输入仍被保存并返回 `schedule_overflow`，旧 active 计划继续运行。
- 实际负责集合为空时，CU-CP 向两个小区分别下发空日历，明确关闭该版本的 NTN 软件接入授权。

## 10. 重启和 DU 重连

### 10.1 状态文件

执行模式要求私有 `state_file`。它使用原子替换，保存：

- accepted version high-water；
- active/pending plan、完整双小区划分和 hash；
- activation/validity 和最近的软件下发状态；
- 尚待确认的精确 cleanup obligation；
- 最新成功解析输入的独立只读摘要：catalog/schedule version、content hash、activation epoch、完整可见清单和实际负责 ID；它不是原始 JSON 的逐字段副本。

状态文件当前写入 schema v3，并显式保存这两份集合。读取 schema v1/v2 时，CU-CP 按旧语义补出 `assigned = visible`，随后仍执行相同的身份、hash、有效期和恢复检查。

启动时会重新校验状态和规划上下文。历史 `applied` 先保持隐藏；CU-CP 必须向 live DU 查询同一 version/hash 和两个小区的完整结果，核对成功后才恢复 active/applied 状态。

状态文件用于恢复，不是管理中心真实性证明。替换整个文件为旧的合法副本或删除文件，仍需要独立可信的单调锚点才能检测。

### 10.2 DU 断开

当承载这两个小区的 DU 断开时：

1. CU-CP 立即隐藏旧 `applied` 状态；
2. 保留计划、version high-water 和旧计划恢复副本；
3. 使正在等待的 prepare/query/clear 请求失效；
4. 重连后只接受新连接上、同一请求实例、同一 version/hash 的完整回复；
5. 旧连接迟到的回复只能记录为 stale，不能恢复 active/applied。

如果未来计划已提前 `applied` 但尚未到启用时刻，再次断开仍会保留历史 active fallback。只有未来计划再次核对成功并到点切换，才会清理旧计划；若它失败或过期，CU-CP 回到旧计划的 live-DU reconciliation。

### 10.3 清理不丢失

cleanup queue 记录精确 schedule version、calendar hash 和原因。DU 确认清理后，CU-CP 先把“不含队首”的新状态持久化；只有写盘成功并确认 durability，才从内存队列删除该任务。

如果写盘失败，任务保留在 fail-closed 内存中并阻止继续写入。崩溃后旧状态仍可能让同一 clear 被幂等重试，这是有意的 at-least-once 安全语义：允许安全重复，禁止静默丢失。DU clear 因而必须按 version/hash 幂等。若文件已替换但目录 durability 无法确认，CU-CP 同样保持 blocked，因为崩溃后可能看到旧文件或新文件。

### 10.4 运行映射与小区级寻呼

计划到点启用后，CU-CP 会建立一份只读运行映射。它只包含本星实际负责的一级波位，每个一级波位恰好出现一次，并指向两个稳定星载小区中的一个。同一个 NCI 可以同时负责多个一级波位；按 NCI 查询时，结果始终按 `position_id` 排序。

运行映射只有在计划已经 active、仍在有效期内、DU 已确认同一软件日历为 `applied`，且两个 NCI/PCI 仍能唯一对应到当前 DU 小区时才进入 `ready`。pending 计划不会改变正在使用的映射，新计划失败时旧有效映射继续使用；新计划到达启用时刻后，整份映射一次性替换。DU 断开会立即隐藏映射，重连和进程重启都要先重新查询 DU。映射本身不单独写入状态文件。

位置变化分为四类：同一 `position_id` 为 `no_change`；两个不同一级波位属于同一 NCI 时为 `same_cell`，不触发 handover；所属 NCI 改变时为 `cell_change`；缺少位置或可用映射时为 `unknown`。CUCP-048 已在首次 RRC Setup 前增加默认关闭的注入式 Initial UL 位置校验，但标准 F1AP/DU/MAC 路径尚未提供可信位置数据源；该分类仍用于查询，不接入真实 handover。

每个星载小区的 NCGI 和 TAI 直接取自唯一匹配的 DU served cell：NCGI 使用该小区的 PLMN 和稳定 NCI，TAI 使用同一 PLMN 和 TAC。完整的 `PLMN+TAC` 必须在 NGAP supported TA 中恰好出现一次。缺失、重复或不匹配时，一级波位映射仍可查看，但 CU-CP 不生成该小区的 onboard release location，也不把 Paging 缩小到该小区。

UE 释放前，CU-CP 可以保存当前稳定小区、NCGI、TAI、schedule version、calendar hash 和计划截止时间。收到 Paging 后，只有这些信息仍与当前运行映射完全一致，才推荐对应 NCI；旧版本、已过期或不完整的记录直接失效，普通 TAI Paging 继续执行。实际负责集合为空时，运行映射有效但波位数为零，两个小区都不参与 NTN 定向寻呼。

当前小区路由使用 DU served cell 的 primary NCGI。如果 UE 选择同一小区的 secondary PLMN，CU-CP 不复用 primary route 生成 onboard release location 或 Paging 推荐。这个限制只影响 onboard 定向提示，普通 Paging 路径保持原样。

## 11. 只读观测

`ntn_state` 显示：

- satellite、schema/profile 和 identity authority；
- received、active、pending 的 catalog/schedule version 和 hash；
- 完整 visible 数量、assigned 数量、两个小区的 L1 数量和容量；
- SSB/PRACH/UL beam intent 数量和最大间隔；
- DU 静态机会 expected/matched 结果；
- active calendar hash；
- cleanup queue 深度、队首 version/hash/reason；
- state file schema、generation、hash、保存结果和 write-block 状态；
- `runtime_mapping_stage`、`runtime_mapping_detail`、当前 schedule version 和 calendar hash；
- 已映射一级波位总数，以及两个 NCI/PCI 各自的映射数量；
- 两个小区的 PLMN、TAC 和 TAI 状态；
- Paging 状态、有效 idle context 数量；
- Initial UL 位置校验的 `disabled/audit/strict` 模式、数据源状态和 authority、待处理记录、临时 UE context、结果计数及最近原因；
- 最近拒绝原因。

这些字段是只读诊断，不改变状态。Digital 资源在本阶段只显示规划容量，仍标记为未绑定到真实数字业务运行态。

## 12. 兼容与实现边界

- schema v1 继续兼容 dry-run，schema v2-v4 共用现有 CU-CP 执行、恢复和只读观测路径；这些兼容处理没有修改 F1AP、DU、MAC、PHY、RU/RF、Web/GIS 或 generated ASN.1。
- legacy NTN profile 继续使用旧 `find_ntn_beam_id_by_nci`、beam-derived TAC/TAI/NGAP/Paging 和 per-beam NCI 路径；onboard execution 路径不调用这些一对一查找，也不从一级波位推导 TAC。
- 新 L1 目录不永久保存 NCI/PCI，也不注入 legacy beam table。
- 原始 PRACH detection 仍属于 PHY/DU/MAC；CU-CP 已提供默认关闭的注入式 Initial UL consumer：`audit` 只记录结果，`strict` 只有在启动前注入 ready source 时才允许启用。标准 F1AP/DU/MAC producer 尚未接入。
- 当前没有 `(nci, position_id, cell_local_port, direction) -> hardware_beam_handle` 映射，也没有设备 `prepare_bank/arm_at/cancel/query` 回执。
- software `applied` 的含义固定为软件日历已经安装；`device_applied`、天线控制、RF 输出和全球连续覆盖由后续设备接口及系统验收给出。

## 13. 代码和验证入口

主要实现：

- `lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.*`
- `lib/cu_cp/ntn_mobility/ntn_onboard_position_plan_state.*`
- `lib/cu_cp/ntn_mobility/ntn_onboard_runtime_mapping.*`
- `lib/cu_cp/cu_cp_impl.*`
- `include/srsran/cu_cp/cu_cp_command_handler.h`
- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `utils/ntn/versioned_position_plan_v3.mjs`
- `utils/ntn/constellation_plan_export.mjs`

主要测试：

- `tests/unittests/cu_cp/ntn_mobility/ntn_onboard_position_plan_test.cpp`
- `tests/unittests/cu_cp/ntn_mobility/ntn_onboard_position_plan_state_test.cpp`
- `tests/unittests/cu_cp/ntn_mobility/ntn_onboard_runtime_mapping_test.cpp`
- `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

Focused validation 命令：

```bash
node --test \
  utils/ntn/versioned_position_plan_v3.test.mjs \
  utils/ntn/constellation_plan_export.test.mjs \
  utils/ntn/constellation_replay_plan_export.test.mjs
cmake --build build/ai-clean --target ntn_mobility_test -j1
build/ai-clean/tests/unittests/cu_cp/ntn_mobility/ntn_mobility_test \
  --gtest_filter='ntn_onboard_position_plan.*:ntn_onboard_position_plan_state.*:ntn_onboard_runtime_mapping.*'
cmake --build build/ai-clean --target cu_cp_test cu_cp_unit_config_test srsran_cu_cp -j1
build/ai-clean/tests/unittests/cu_cp/cu_cp_test \
  --gtest_filter='cu_cp_ntn_mobility_test.onboard_runtime_mapping_*:cu_cp_ntn_mobility_test.onboard_paging_*:cu_cp_ntn_mobility_test.default_cu_cp_rejects_ntn_satellite_state_updates'
```

每次交付的实际通过数量和未运行项记录在 [NTN CU-CP Task Change Index](ntn_cucp_task_change_index.md)，不能用历史测试数替代当前验证结果。
