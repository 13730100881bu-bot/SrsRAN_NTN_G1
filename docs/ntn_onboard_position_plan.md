# NTN 星载双小区版本化波位计划

本文记录 CU-CP 第一阶段的管理中心输入、校验、日历 dry-run、原子激活和只读观测契约。
该能力是独立、默认关闭的 planning profile，不替换现有 `ntn_location_mobility` 的旧
beam-to-NCI 兼容路径。

## 身份与边界

- 一颗卫星配置两个长期 NR logical cell；NCI 是 36-bit opaque planning ID，PCI 属于星载小区。
- `G######` L1 只保存 `position_id`、纬度和经度，不保存 NCI/PCI。
- CU-CP 不从 `satellite_id`、坐标或 `position_id` 推导 NCI。
- 管理中心负责全球目录、星历、可见性和星座搜索；CU-CP 只接收本星完整可见 L1 inventory。
- 星历继续通过现有 CU-CP satellite-state contract 输入；本 JSON 不在星上重复轨道传播或星座搜索。
- 第一阶段只产生并审计 access calendar intent。`ntn_state` 明确输出 `execution=not_applied`；
  不调用 F1AP、DU、MAC、PHY、RU/RF，也不声称射频波束已经执行。

## 配置入口

`cu_cp.mobility.ntn_onboard_position_plan` 是独立 opt-in 配置：

```yaml
ntn_onboard_position_plan:
  enabled: true
  satellite_id: P01-S001
  plan_json_file: /path/to/management-center-plan.json
  reload_period_ms: 2000
  cell_ncis: [4886691841, 4886691842]
  cell_pcis: [101, 202]
  max_l1_positions_per_cell: 128
  max_l1_positions_per_satellite: 256
  max_analog_ports_per_cell: 16
  max_analog_ports_per_satellite: 32
  access_slot_us: 10000
  subvisit_duration_us: 2500
  max_ssb_interval_ms: 80
  max_prach_interval_ms: 640
  activation_alignment_ms: 640
```

`reload_period_ms: 0` 表示启动时读取一次。非零值使用 CU-CP executor/timer 轮询同一文件；
文件的声明版本、声明 hash 或实际规范内容变化后才重新提交；损坏 hash 修复后不会被旧去重记录
跳过。远于底层 timer 单次安全范围的 reload、activation 和 expiry 会按 24 小时分片重算，
不会缩短计划有效期。默认 `enabled: false`，因此 terrestrial 和旧 NTN 路径保持不变。

本 profile 显式配置恰好两个稳定 NCI/PCI，不从 legacy measurement-cell map、卫星编号、
坐标或 L1 ID 推导。计划中的 `onboard_cells` 必须与本地身份完全一致；顺序不影响校验或
划分结果。两个 NCI 必须不同；按干扰规划复用同一 PCI 是合法的。容量和时序值均为该
planning profile 的可配置参数，不是协议常量。

计划 reload/activation 与 `ntn_state` 只读投影通过同一同步边界访问 controller，避免在
原子切换时读取正在替换的 candidate、pending 或 active 容器。

## 管理中心 JSON

```json
{
  "satellite_id": "P01-S001",
  "catalog_version": 10,
  "schedule_version": 20,
  "content_hash": "sha256:<64 lowercase hex digits>",
  "valid_from_unix_ms": 1780000000000,
  "valid_until_unix_ms": 1780003600000,
  "activation_epoch_unix_ms": 1780000000640,
  "onboard_cells": [
    {"nci": 4886691841, "pci": 101},
    {"nci": 4886691842, "pci": 202}
  ],
  "visible_l1_positions": [
    {"position_id": "G000001", "latitude_deg": 10.0, "longitude_deg": 20.0}
  ]
}
```

NCI 也可使用带 `0x` 前缀的字符串。时间使用 Unix milliseconds。当前规划参数要求
`activation_epoch_unix_ms` 位于有效期内并按 640 ms 对齐；640 ms 是本 profile 的可配置
规划值，不是协议常量。

## Content hash

`content_hash` 是对除 hash 自身外的规范内容计算的 SHA-256：

1. 依次写入 `satellite_id`、两个 version、三个 Unix millisecond 时间；
2. 两个 cell 按 `(nci,pci)` 排序后写入；
3. L1 按 `position_id,latitude,longitude` 排序，以 C++ `double` 的
   `max_digits10` 精度写入；
4. 每个字段使用实现中的稳定换行格式；输出为 `sha256:<hex>`。

生成方应使用 `compute_ntn_position_plan_content_hash()` 对等实现或以 focused parser/hash
测试向量校验，不能对原始 JSON 字节直接求 hash。

## 生命周期和失败保持

计划依次经过：

```text
received -> validated -> calendar_checked -> pending -> active
```

失败统一进入 `rejected`，并保存机器可读原因，包括 `expired`、`schedule_overflow`、
`identity_mismatch`、`invalid_hash`、`invalid_activation_epoch`、L1 格式/重复、SSB/PRACH
deadline、PRACH RO 缺 UL beam intent 和端口/时间冲突。

新的计划只有在所有校验和 calendar audit 完成后才替换 pending；到达
`activation_epoch` 时一次性替换 active。新计划失败不会修改旧 active，也不会清除一个
已经合法的 pending。active 到达 `valid_until` 后标记为 `expired`。
控制器会保留最高已接受 version 的高水位；即使 active 已过期，也不能重放旧
`schedule_version`。

`candidate_inventory` 在 received 时先保存完整输入，再做容量检查。因此 257 个 L1 会保留
257 条 inventory，同时以 `schedule_overflow` 拒绝激活。

`ntn_state` 分别输出 last-received、active、pending 的 catalog/schedule version、content hash 和
activation epoch；candidate 数量只与 last-received 关联，calendar audit 另带其
`schedule_version`。因此 overflow 或其他拒绝不会把新 inventory 与旧 active 的 hash 混在同一
机器可读记录中。

## 双小区划分

- 首次计划按经纬度的主空间轴确定性二分，两个集合数量差不超过 1；
- 更新优先保持旧 L1 所属 cell，仅在平衡需要时移动最少的已有 L1；
- 新 L1 优先加入空间上更近且仍有目标容量的集合；
- 每个 L1 恰好出现一次；两个 cell 的 NCI/PCI 始终来自本地稳定配置；
- 当前上限为每 cell 128、每 satellite 256，属于可配置 planning profile。

## Access calendar dry-run

当前默认 planning seed：每 cell 16 analog ports、每 satellite 32、10 ms access slot、
2.5 ms sub-visit、最大 SSB 间隔 80 ms、最大 PRACH 间隔 640 ms。每个 L1 在 640 ms
审计周期内获得 8 个 SSB 窗口和 1 个 PRACH RO；每个 RO 必须有同 NCI、L1、时间和时长的
`prach_ul_beam` intent。SIB/Paging 合并到 SSB 窗口，RAR 标记合并到下一 SSB 窗口。

审计检查：

- 每 cell / satellite analog port 上限；
- 每个 L1 的循环最大 SSB/PRACH 间隔；
- `prach_ro_without_beam=0`；
- `(nci,port,time)` 无重叠；
- intent 只引用本计划中分配给相同 NCI 的 L1。

这些结果只证明 CU-CP 离散 intent 模型自洽，不证明 guard、功率、带宽、干扰、PRACH
detector、真实 TDD 映射或 PHY/RU/RF 可执行。

## 兼容性和后续接口

旧 `find_ntn_beam_id_by_nci`、`get_configured_ntn_beam_cells`、repository
`find_by_nci/get_du_for_nci`、beam-derived TAC/TAI/NGAP/Paging 仍维持原语义。新版 L1 不注入
旧 beam table，也不接管这些路径。

下一阶段若要执行日历，最小跨层契约是：CU-CP 下发 versioned calendar intent；DU/RF 返回
按 cell/version/activation epoch 的 `ready/applied/rejected`；只有 applied 后才能声称执行。
PRACH 原始检测继续属于 PHY/DU/MAC，CU-CP 只负责计划、资源授权和 Initial UL 版本/归属校验。
