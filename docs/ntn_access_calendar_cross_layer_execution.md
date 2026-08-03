# NTN 星载双小区接入日历跨层执行

本文记录 CUCP-036 的实现边界：把 CUCP-035 已完成审计的双小区 `AccessCalendarIntent` 通过 F1AP 下发到 DU/MAC，并在 MAC scheduler 对现有合法 SSB/PRACH opportunity 做版本化软件授权。该功能默认关闭；关闭时 terrestrial 和旧 NTN 路径不变。

## 配置与状态

```yaml
ntn_onboard_position_plan:
  enabled: true
  du_execution_enabled: true
  du_prepare_horizon_ms: 4000
  du_prepare_guard_ms: 1000
  du_apply_timeout_ms: 500
  satellite_id: P01-S01
  plan_json_file: /path/to/management-center-plan.json
  state_file: /path/to/private-recovery-state.json
  expected_catalog_id: global-land-l1-v1
  expected_catalog_hash: "sha256:<catalog hash>"
  expected_identity_registry_version: mc-ntn-onboard-cell-registry-v1
  expected_identity_registry_hash: "sha256:<registry hash>"
  expected_access_profile_id: ntn-access-16a-64d-v1
  expected_access_profile_hash: "sha256:<profile hash>"
  cell_ncis: [4886691841, 4886691842]
  cell_pcis: [101, 101]
```

`du_execution_enabled` 默认是 `false`。开启后，CU-CP 的 plan 状态仍使用：

```text
received -> validated -> calendar_checked -> pending -> active
```

并附带独立 deployment 状态：

```text
not_sent -> preparing -> ready -> applied
                         \-> rejected / unsupported
```

`preparing` 表示 scheduler command 已入队但两个 cell 的 slot thread 尚未全部确认 armed；只有两侧都消费同一 version/hash 后才进入 `ready`。prepare 和 query 的 `preparing/ready/applied` 反馈都必须保持同一 catalog/schedule version、source/calendar hash，并完整回报两个小区的 accepted intent 数；只有匹配的 `applied` feedback 到达后，CU-CP 才允许 pending plan 在 `activation_epoch` 后变为 active。deployment 反馈只允许单调前进且同状态幂等：同一计划已被 query 推到 `ready/applied` 后，较早的 prepare completion 被忽略；guard 前暂时无 response 会回到 `not_sent` 重试。错版本/hash、intent 数不完整、明确终止失败或超过 guard/apply deadline 才 reject 并 clear，新计划失败不改变旧 active。

`du_prepare_horizon_ms`、`du_prepare_guard_ms` 和 `du_apply_timeout_ms` 是可配置软件时限，不是协议常量。CU-CP 只在 plain-SFN 可无歧义映射的 prepare horizon 内下发；guard 前未 armed，或 activation 后 apply timeout 内未两侧 applied，都会回滚。prepare/query/clear 使用独立异步 lane，丢失的 F1 response 不会阻塞 clear。

执行模式接受 schema v2 和 v3 输入，并要求独立 `state_file`。两者都把 catalog、identity registry 和 access profile 的 ID/hash 纳入计划上下文和 `content_hash`；v3 还原子携带完整 `visible_l1_positions` 和实际负责的 `assigned_l1_position_ids`，hash 同时覆盖两份集合。schema v1/v2 按 `assigned=visible` 兼容，v1 仍只允许 dry-run。当前 satellite registry 使用 `Pxx-Syy`，例如 `P01-S01`，CU-CP 不再接受旧文档样例 `P01-S001`。

开启 DU execution 时，启动校验还会提前约束当前实现包络：最多 256 个实际负责 L1、最多 2560 个 intent，并保证一个 PRACH cycle 在最密的 NR numerology `mu=4` 下不超过 scheduler gate 的 16384 slots。完整可见清单不受 256 条容量限制：257 个 visible 加不超过 256 个 assigned 可以接收并完整保存；257 个 assigned 明确返回 `schedule_overflow`，新计划不进入部署。

## 私有 F1AP contract

实现复用标准 `GNBDUResourceCoordinationRequest/Response` 已有的 opaque OCTET STRING，不新增 3GPP IE，不修改 `include/srsran/asn1/**` 或 generated ASN.1。

私有 payload 支持：

- `prepare`：一次携带两个星载小区及完整 checked calendar；
- `query`：按 version/hash 查询 `preparing/ready/applied/cleared/rejected`；
- `clear`：撤销 superseded、超时或被 CU-CP 拒绝的 deployment；
- result 回显 catalog/schedule version、source/calendar hash、activation slot 和每小区 accepted intent 数。

decoder 限制两个小区、最多 256 个已分配的唯一 `G######`、最多 2560 个 intent，并验证长度、时间、enum、direction/purpose/port 组合和 64-bit 时间范围。完整可见清单留在 CU-CP 计划与恢复状态中，不进入这个下发 payload。两个 NCI 必须不同；两个 PCI 可以按规划复用。

## DU/MAC/scheduler 行为

CU-CP 直接扫描 DU served-cell inventory，以 `(NCI, PCI, DU cell index)` 唯一解析两个星载小区，不使用 legacy beam-to-NCI repository。当前原子 envelope 要求两个小区属于同一 DU；跨 DU 会明确拒绝 `cross_du_calendar_not_supported`。

DU 在一次 MAC 调用前验证两个 cell 都存在、active 且 NCI/PCI 匹配。MAC 把微秒窗口编译成 cell numerology 的 slot mask，再为两个 cell prepare；任一 cell 失败会持久保存 partial-cleanup record，持续 clear 已 prepare 的另一 cell，并在 cleanup 完成前拒绝新 prepare。旧 active plan 不变。零 assigned L1 是合法的显式 deny-all calendar；即使仍有可见但未负责的 L1，也不会退化为 terrestrial allow-all。

微秒 intent 到 scheduler slot request 的转换位于私有纯编译器 `mac_ntn_access_calendar_compiler.h`，production 与 focused test 共用同一实现。它精确校验 wall-clock/slot 映射、validity/cycle/window 对齐、direction/purpose、范围和整数溢出，并只合并完全相同窗口的 purpose mask；不做四舍五入。

scheduler gate 只过滤现有静态配置能够产生的 SSB 和 PRACH opportunity：

- gate 仅在 opt-in prepare 时分配；null/no snapshot 时恒 `allow`，保持默认 terrestrial 调度和内存路径；
- prepare 在 control path 校验并编译固定大小 mask；
- slot path 不做字符串比较、动态分配、日志或锁；
- gate API 可按 future target slot 选择 pending/active；当前集成保持静态 lookahead prefill，只在当前 `sl_tx` result 做 final suppression；
- 两个 cell 都 armed 后按同一 activation epoch 切换；clear 新版本时恢复仍有效的 previous active snapshot；
- 同 version/hash prepare 幂等，支持 F1 timeout 重试；同 version 异 hash 明确拒绝。

plain `slot_point` 只有约 5.12 s 的 wrap-safe activation horizon。CU-CP 会把远期计划保持为 `not_sent`，进入配置的 prepare horizon 后才下发；MAC 仍比较 wall-clock 期望 slot 数与映射后的 `slot_difference`，不会静默映射到错误 SFN。validity 以显式 slot duration 传入，并由 slot thread 转成 `slot_point_extended`，因此一小时有效期可跨 plain SFN wrap；超过 extended half-horizon 会拒绝。

MAC 的 wall-clock/slot mapper 使用纳秒精度；`mu=4` 的 62.5 microsecond slot 不会被截断成 62 microseconds。日历的微秒输入只有在乘以 cell slot rate 后得到整数 slot 时才接受，未对齐窗口会显式拒绝。

## 重启、重连和清理

`state_file` 原子保存 accepted version high-water、active/pending 计划、双小区划分、source/calendar hash、启用时间、历史软件下发状态和仍需确认的清理任务。状态 schema v3 还独立保存最新成功解析输入的只读摘要：catalog/schedule version、content hash、activation epoch、完整可见清单和实际负责 ID。它不是原始 JSON 的逐字段副本。读取状态 schema v1/v2 时按旧语义归一化为 `assigned=visible`，后续新写入统一使用 schema v3。257 个 visible 加不超过 256 个 assigned 可正常恢复；被 `schedule_overflow` 拒绝的 257 个 assigned 也保持完整只读观测，不会成为 active/pending 或 DU 下发权威。

启动时，历史 `applied` 只表示上次进程看到的软件状态。CU-CP 会隐藏它，并查询 live DU 的同一 version/hash；只有两个小区的完整 matching result 才恢复 `active/applied`。

DU 断开时，CU-CP 在 served-cell context 被删除前让该连接上的应用证据失效。prepare 使用 DU connection generation 和计划身份保护；query/clear 还使用独立 request id。旧连接迟到的反馈不能改变新连接上的状态。未来计划即使提前得到 `applied`，仍需等待 `activation_epoch`，其历史 active fallback 会跨再次断开保留；未来计划失败或过期时，fallback 重新向 live DU 核对。

clear 采用先落盘、后删内存队首的顺序。若 DU 已确认 clear，但不含该队首的新状态无法 durable 保存，CU-CP 保留原 cleanup obligation 并阻止继续写入。崩溃后它可能按同一 version/hash 幂等重试，这是有意的 at-least-once 语义：允许安全重复，禁止静默丢失。状态文件仍不是可信单调锚点：整文件被旧合法副本替换或删除，需要外部可信存储才能检测。

## 证据边界

当前 `applied` 的准确含义是：

```text
ssb_prach_software_gate_applied_no_position_or_rf_evidence
```

它不表示：

- `position_id` 或 `port_id` 已转换为模拟波束；
- SIB/Paging/RAR 已按每个窗口执行；这些仍是 coalescing intent；
- FAPI/PHY 已携带 beam token；
- OFH `BeamId` 已配置；
- SDR/ZMQ 或真实 O-RU 已切换波束；
- RF 已应用或已有硬件 telemetry。

SSB/PRACH 保持静态 future prefill，只在当前 `sched_result` 交 MAC/PHY 前做 final suppression；因此 clear 在下一 cell slot 可恢复 previous active 的实际 PDU，不受 lookahead cache 污染。两个软件 slot thread 均 armed 且共享 activation epoch，但真实 RF 的原子 bank switch 仍没有证据，不能声称瞬时 RF rollback。

仓库级下沉审计也确认当前没有可复用的设备闭环：FAPI PRACH/SSB beamforming 尚未填充，OFH section type 1/type 3 的 `BeamId` 仍固定为 0，`ru_controller` 没有 beam bank、定时 arm/query 或 applied telemetry。MAC 在 software gate 编译时只保留 slot window 和 purpose mask，尚未把 `position_id`/`port_id` 转为硬件句柄。因此本阶段没有修改 generated ASN.1、PHY、OFH 或 RU/RF；在缺少设备映射时向这些层增加占位字段不能构成运行证据。

这里的 `port_id` 是每小区 0..15 的可复用模拟资源槽；同一端口会在不同窗口服务不同 `position_id`。它不是 eAxC、OFH `BeamId` 或阵列权重索引，不能直接下沉。仓库内可以继续增加 `ru_ntn_beam_controller` 契约、FAPI/OFH `BeamId` plumbing 和 dummy/spy backend，并将证据提升到 `command_sent`；但在管理中心映射和设备回执缺失时，仍不得返回 `device_applied`。

## Initial UL active-plan 审计边界

CUCP-037 在 CU-CP 私有 position-plan controller 中增加了无副作用审计器。对完整的 proposed sideband 测试输入，它检查：

- `satellite_id`、catalog/schedule version、source/calendar hash；
- 两个长期星载小区之一的稳定 NCI/PCI；
- `G######` 是否属于该小区；
- absolute occasion 映射到配置的 PRACH cycle（当前 profile 为 640 ms）后是否落入该 L1 的 PRACH RO；
- 同一窗口是否有配对 `prach_ul_beam`，且 cell-local `port_id` 匹配；
- external-execution profile 是否保留当前 active plan 的 software-gate applied snapshot。

结果使用 `accept/reject/audit_only` 和机器可读原因。`accept` 只说明“提供的 metadata 与当前 CU-CP active plan/software-gate snapshot 匹配”，不验证发送方身份，也没有接收时刻 freshness/anti-replay，不说明 position steering 或 RF 已执行。DU 断连会立即隐藏 `active_has_external_apply_evidence`，只有当前连接的 matching query 才能恢复；这解决本地连接代次污染，但仍不是经过认证的跨重连 telemetry。

标准 F1AP Initial UL 目前只有 CGI、C-RNTI 和 RRC container，不携带上述 position/version/hash/RO/port 证据。生产 `handle_ue_setup_request()` 因而没有接入这个审计器，也绝不能通过 legacy beam-to-NCI table 推导 `G######`。下一步若要成为真实 admission gate，需要一个明确授权、版本化且可鉴别来源的最小 sideband；RAR 低时延路径仍留在 DU/MAC，原始 PRACH 检测仍留在 PHY/DU。

相关的 C-RNTI lease key 已改为 `(DU, DU cell index, PCI, C-RNTI)`。这允许两个长期星载小区按规划复用 PCI，但当前 DU RNTI table 仍按 C-RNTI 扁平索引，所以同一 DU 的两个 cell 不能复用同一个 C-RNTI 值；不同 DU 可以复用。CUCP-038 进一步要求 generation 与完整 ACK 集合原子匹配，未知 ACK 只用原 generation 修复，普通 in-flight pool 不叠加新 generation；ICS 后释放模拟接入归属和 `control_only`/L2 规则不变。

## 管理中心 plan producer

`utils/ntn/constellation_plan_export.mjs` 和 replay exporter 输出 candidate/test-only schema v3 计划：

- 在同一份 plan 中输出完整 `visible_l1_positions` 和 `assigned_l1_position_ids`，前者不按 256 条裁剪；
- schema v3 使用 exact-key，canonical SHA-256 同时覆盖两份排序后的集合，并与 C++ `compute_ntn_position_plan_content_hash()` 共享 golden vector；
- `assignment_sidecar` 仅保留规划端 bank 等兼容明细，CU-CP 不依赖它确定负责集合；
- baseline 使用版本化管理中心 registry `app/onboard-cell-identity-registry.json`（`mc-ntn-onboard-cell-registry-v1`），显式保存 3528 星/7056 cell 的 opaque NCI、PCI 和 bank；运行时已删除 `centralNci`/ordinal 派生路径；
- 每星必须恰好两 cell、全局 NCI 唯一且在 36-bit 范围内；PCI 保留管理中心输入并允许复用，只做 topology conflict audit，不在 Web 运行时重算；
- 非 baseline Walker audit 必须显式提供匹配的 registry 文件，缺失、额外或错误 identity fail closed；
- 不导出 Web preview calendar，F1AP 只承载 CU-CP audited calendar；
- 不接入 GIS runtime，也不把 coarse/exact=false 结果表述为全球连续覆盖。

## 下一阶段最小硬件接口

真实波束跳变至少需要管理中心提供：

```text
  (nci, position_id, cell_local_port, direction)
    -> {sector, eAxC, BeamId or opaque hardware_beam_handle/weights}
```

并需要 capability-negotiated adapter：

```text
prepare_bank(version, hash, activation, entries)
arm_at(clock_domain, absolute_slot_and_symbol)
cancel(version, hash)
query_status() -> prepared / armed / command_sent / device_applied / rejected
```

设备能力必须包含并发 beam 数、UL/DL 支持、切换粒度与 guard、时钟域、atomic bank switch。若走 OFH，还需 O-RU M-plane beam table、非零 type-1/type-3 BeamId、PRACH capture/token 回传及设备 telemetry。只有设备回执或硬件 loopback 才能把状态提升为 `device_applied`。
