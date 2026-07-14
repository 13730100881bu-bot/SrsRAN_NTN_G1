# srsRAN LEO 500 km CU-CP NTN 完整分阶段计划

> 本文保留 2026-05-23 开始的 Phase 0～6 演进记录，部分状态和参数早于后续双层波束、RNTI/SR/SRS 资源闭环、SIB19 和 UE capability 工作。文内“单星、透明载荷”等表述属于已被星载再生 CU-CP 目标架构取代的历史假设，不得作为当前方案结论。当前方案总览请先阅读 [NTN CU-CP 中文主方案](ntn_cucp_solution_overview.md)，需要追溯早期阶段设计时再查本文。

记录日期：2026-05-23

## 1. 总体目标

本计划面向 LEO 500 km、单星、透明载荷、CU-CP-only 的 NTN 第一阶段能力建设。目标不是一次性实现完整 NTN 空口，而是先在 CU-CP 建立一条可审计、可测试、可逐步扩展的控制面闭环：

```text
satellite state
  -> visible beams
  -> beam placement plan
  -> active served beam set
  -> UE location mobility
  -> handover trigger/result
  -> standard NGAP LocationReport
```

核心原则：

- CU-CP 负责控制面策略、拓扑关系、移动性判断和核心网上报。
- DU/RU/PHY/MAC/SIB19 只作为本阶段依赖，不在本计划第一阶段修改。
- 第一版使用 500 km circular orbit 作为确定性基线。
- UE 位置输入先假定已被上游解码为经纬度，不实现真实 RRC opaque location 解码。
- NGAP 只使用标准 LocationReport / UserLocationInformationNR / NRNTNTAIInformation，不添加私有经纬度扩展。

## 2. 分阶段路线图

| 阶段 | 名称 | 目标 | 结束门槛 |
|---|---|---|---|
| Phase 0 | 基线冻结与资料-代码对齐 | 说明协议要求、现有代码能力和 CU-CP 边界 | 文档、矩阵、缺口表完成 |
| Phase 1 | LEO 500 km 轨道驱动服务波位 | 从卫星状态稳定得到 served beam candidates | circular orbit / served beam tests 通过 |
| Phase 2 | CU-CP 波位到 DU 规划 | 将 visible beams 变成 active/candidate/draining plan | beam placement tests 通过 |
| Phase 3 | UE 位置驱动移动性 | 由 UE 位置稳定触发 NTN handover | location mobility tests 通过 |
| Phase 4 | NGAP 位置上报与 AMF 控制 | accepted UE location 触发标准 NGAP report | NGAP location tests 通过 |
| Phase 5 | CU-CP 集成与回归保护 | 串联 Phase 1-4 并保护非 NTN 路径 | CU-CP 集成/回归测试通过 |
| Phase 6 | 后续增强 | SGP4、真实 RRC 位置、DU/RU 联动 | 不阻塞 CU-CP v1 |

推荐执行顺序：Phase 0 -> Phase 1 -> Phase 2 -> Phase 3 -> Phase 4 -> Phase 5。Phase 6 只记录路线，不进入当前 CU-CP v1 的完成标准。

## 3. 本轮落地状态

| 项目 | 状态 | 说明 |
|---|---|---|
| 完整分阶段计划 | 已落地 | 本文档替代早期资料索引型说明，作为后续执行路线图 |
| LEO 500 km 最小 CU-CP 配置 | 已落地 | `configs/leo_500km_cucp_ntn.yml` |
| LEO 500 km 静态 beam table | 已落地 | `configs/leo_500km_beam_table.json` |
| LEO beam table 离线生成器 | 已补充 | `utils/ntn/generate_leo_beam_table.py` 可按中心点、15 km beam 半径、区域半径或 500 km/10 deg 足迹估算生成静态 JSON |
| beam table 输入校验 | 已补充 | JSON 解析阶段拒绝非法经纬度、非正覆盖半径、重复 beam id/NCI |
| DU full -> candidate 测试 | 已补充 | `ntn_beam_placement_planner` 覆盖 UE 水位超限场景 |
| runtime DU load watermark | 已补充 | placement planner 支持 DU 当前非 NTN UE/DRB 水位叠加 beam load，超限 beam 保持 candidate |
| DU 后上线 placement 重试 | 已补充 | visible beam 未全部 active 时不缓存 scheduler accepted set，后续相同卫星状态可重试 |
| DU capacity 变化 placement 重算 | 已补充 | satellite candidate set 未变化时仍会重算 placement，避免 DU 掉线/容量变化后保留旧 active set |
| active served beam 查询语义 | 已收敛 | `get_current_ntn_served_beam_ids()` 返回 active placement set，不暴露 candidate/draining |
| runtime beam placement 状态查询 | 已补充 | `get_current_ntn_beam_status()` 暴露 active/candidate/draining/inactive 快照，用于多波束跳波束调试和联调 |
| runtime beam placement CLI | 已补充 | `ntn_beams` 默认打印 active/candidate/draining/inactive 汇总；`ntn_beams window 64`、`ntn_beams active 64` 等过滤命令打印 beam/state/window/DU/NCI/elevation/UE/DRB，明细行数上限 1024 |
| CU-CP 用户驱动 SR/SRS 时隙计划 | 已补充 | placement planner 仅给 active/draining 且已有 UE/DRB 负载的服务波位分配 SR/SRS slot offset/period；按 UE 数分配连续服务 slot 块，SR 放在块起点，SRS 放在块终点，DRB-only 负载兜底 1 个 slot，空 active beam 不占 SR/SRS 周期资源 |
| 多波束方案可视化 | 已补充 | `docs/ntn_cucp_multibeam_visualization.md` 嵌入 PNG/SVG 图片，展示 CU-CP 控制面闭环、beam 状态机、hopping window、UE 位置 HO 和责任边界 |
| runtime placement 摘要日志 | 已补充 | CU-CP placement 变化时只输出状态计数和前 32 个非 inactive 条目，避免 15 km 万级波位池生成超长 debug 字符串；plan 未变化时不重复打印大池摘要 |
| satellite state 注入 CLI | 已补充 | `ntn_sat <ecef_x_m> <ecef_y_m> <ecef_z_m>` 手动注入卫星 ECEF 状态，用于 CU-CP-only 跳波束联调 |
| satellite geodetic 注入 CLI | 已补充 | `ntn_sat_geo <lat_deg> <lon_deg> [alt_m]` 按 WGS84 转 ECEF 后注入，默认 `alt_m=500000`，便于 LEO 500 km 人工联调 |
| served beam window candidate reset | 已补充 | active served beam set 变化时只清目标 beam 已离开的 UE NTN candidate；目标仍在新 active set 内的 TTT/连续报告累计保留 |
| DU served-cell 能力映射 | 已补充 | runtime placement 只把 beam 分配给实际承载对应 NCI 的 DU，不支持的 beam 保持 candidate |
| 多波束/跳波束 CU-CP 控制面 | 已补充 | `served_beam_hopping_enabled=true` 且 visible pool 大于 `max_nof_served_beams` 时，按 dwell 周期轮转 active hopping window；window 外 visible beam 保持 candidate/draining；不涉及 PHY slot 级 beam hopping |
| hopping dwell 保护 | 已补充 | `served_beam_hopping_dwell_updates` 控制每个 active window 至少保持若干卫星状态更新，避免 UE location TTT/连续报告累计被过快轮转打断 |
| visible pool 成员判定 | 已补充 | scheduler 只在 visible beam 成员集合变化时重置 hopping 起点；同一批 beam 的仰角排序变化不会打断 15 km 大波位池轮转 |
| hopping dwell 排序抖动保护 | 已补充 | 同一 visible pool 在 dwell 未满时保持当前 active window beam IDs，仰角排序变化只更新 candidate elevation，不强迫换 window |
| active served beam 成员判定 | 已补充 | location mobility 只在 active beam 成员集合变化时评估 UE candidate；目标 beam 仍在新集合时保留 TTT/连续报告累计，目标离开时才清理 |
| 大规模 placement 查询 | 已补充 | active beam 提取、DU supported beam 判断、mobility HO trigger placement 校验和 NGAP core-reportable NCI gating 均改为索引/集合查询，避免随全量 beam table 做重复线性扫描 |
| draining 生命周期保护 | 已补充 | previous active/draining beam 只要仍有 UE/DRB load 就继续保持 draining；load 清空后退役为 inactive/candidate |
| accepted UE location -> NGAP LocationReport | 已补充测试 | CU-CP 集成测试覆盖本地 forwarding 到标准 NGAP LocationReport |
| NGAP reporting app 配置 | 已补充 | YAML/CLI 可配置 local forwarding、AMF control、min report interval |
| AMF change-of-serving-cell 事件语义 | 已补充 | 建立 serving cell 基线，仅在服务小区变化时发送 LocationReport，stop 后停止上报 |
| AMF direct 无可用 UE 位置 failure | 已补充 | direct LocationReportingControl 在没有 accepted UE location 时返回 failure |
| NGAP LocationReport core-reportable beam 保护 | 已补充 | UE location 可缓存，但本地/AMF 上报前必须确认 serving NCI 属于当前 active 或 draining beam；candidate/inactive 不上报 |
| LEO 500 km 示例 cell/beam 对齐 | 已补充 | 示例 5 个 beam 均有 mobility cell 配置，避免 4/5 号 beam 静默无法 HO |
| Phase 1-4 核心代码 | 已有原型 | 现有代码已包含轨道、服务波位、beam placement、UE 位置移动性、NGAP 位置上报 |
| Phase 5 完整 CU-CP 端到端测试 | 已补强 | 已补充 `satellite update -> active beams -> UE location -> F1AP HO trigger` 的 CU-CP intra-DU / inter-DU 集成测试，覆盖 inter-DU HO 失败后的结果清理与重试；同时增加 NTN 开启时传统 RSRP measurement 不直接触发 HO、NTN 关闭时传统 RSRP measurement 仍可触发 HO 的 CU-CP 回归保护 |
| Phase 6 后续增强 | 不进入本阶段 | SGP4、真实 RRC 解码、DU/RU 联动、PHY/MAC 专项 |

## 4. Phase 0: 基线冻结与资料-代码对齐

目标：把“协议要求什么、srsRAN 已有什么、CU-CP 该做什么”固定下来，避免后续把空口定时、DU 调度或 PHY Doppler 误塞进 CU-CP。

输入：

- 3GPP NTN Overview。
- TS 38.300、TS 38.331、TS 38.304、TS 38.413、TR 38.821。
- srsRAN NTN Tutorial。
- OAI NTN MR !2722 / !2723。
- 当前 srsRAN CU-CP NTN 代码。

任务：

1. 协议映射：整理 NTN architecture、LEO/NGSO、transparent payload、earth-fixed/earth-moving beams、UE location assistance、idle/inactive reselection、NGAP LocationReport 相关内容。
2. 开源对照：区分 srsRAN/OAI 当前公开 NTN 工作中哪些偏 DU/UE 空口定时，哪些能借鉴到 CU-CP 移动性。
3. 代码审计：检查 `ntn_mobility`、`cell_meas_manager`、`mobility_manager`、`ngap`、`o_cu_cp` 配置链路。
4. 功能归属：把每项 NTN 功能标记为 `CU-CP负责`、`CU-CP依赖DU/RU` 或 `暂不做`。
5. 缺口排序：输出 P0/P1/P2 表，避免后续实现顺序被“看起来有趣”的 PHY/DU 任务打乱。

交付物：

- `docs/ntn_cucp_leo_500km_plan.md`。
- CU-CP 功能矩阵。
- P0/P1/P2 缺口表。
- `configs/leo_500km_cucp_ntn.yml` 和 `configs/leo_500km_beam_table.json` 作为最小联调入口；示例包含 5 个静态 beam 和 5 个对应 mobility cells。

验收标准：

- 文档能回答 LEO 500 km 场景假设、CU-CP 职责边界、协议条款位置、现有代码模块映射、测试覆盖和后续优先级。
- 所有“不做”的内容都有明确归属，不被解释成遗漏。

## 5. Phase 1: LEO 500 km 轨道驱动服务波位基线

目标：CU-CP 能根据 LEO 500 km 卫星状态稳定地产生当前可服务波位集合。

关键接口：

- `ntn_satellite_state_update_config`
- `ntn_served_beam_scheduler`
- `ntn_served_beam_candidate`
- `cu_cp_ntn_command_handler::handle_ntn_satellite_state_update`

默认配置：

```yaml
satellite_state_source: circular_orbit
circular_orbit_altitude_m: 500000.0
served_beam_min_elevation_deg: 50.0
max_nof_served_beams: 3
served_beam_hopping_enabled: true
served_beam_hopping_dwell_updates: 3
satellite_state_update_period_ms: 1000
```

15 km 波位尺度估算：

| 最小仰角 | 500 km LEO 地面足迹半径 | 足迹面积 | 15 km 圆形波位粗略数量 |
|---:|---:|---:|---:|
| 5 deg | 约 1950 km | 约 11.85 million km2 | 约 16.8k |
| 10 deg | 约 1564 km | 约 7.64 million km2 | 约 10.8k |
| 20 deg | 约 1044 km | 约 3.42 million km2 | 约 4.8k |
| 50 deg | 约 380 km | 约 0.45 million km2 | 约 0.6k |

当前 LEO 500 km CU-CP-only 示例配置采用 `served_beam_min_elevation_deg: 50.0`，优先保证第一阶段联调使用高仰角、链路质量更可控的服务波位；10 deg 仍保留为大足迹/压力测试参考。
| 30 deg | 约 732 km | 约 1.68 million km2 | 约 2.4k |

上述数量是用足迹面积除以单个 15 km 圆形波位面积得到的粗略估算，偏向理想下限。真实工程如果采用六边形中心规划、边缘保护、相邻波束重叠和不可服务区域裁剪，10 度仰角下的配置规模很可能从约 1.08 万继续上浮。因此 `configs/leo_500km_beam_table.json` 只作为 5 个 15 km 波位的最小联调切片，不代表完整卫星足迹。完整系统应由外部 O&M/规划工具生成千级到万级 earth-fixed beam table；CU-CP 负责加载、筛选 visible pool，并只把小规模 active hopping window 暴露给移动性。`utils/ntn/generate_leo_beam_table.py` 提供一个轻量离线生成器，便于先生成区域级或整足迹级静态 JSON，再按需要扩展 mobility cell/neighbor 配置。

任务：

1. 保持 circular orbit 作为第一版确定性轨道源。
2. 将 satellite ECEF state 转成 visible beam candidates。
3. 根据最小仰角选出完整 visible beam pool，并按 elevation 排序。
4. 根据 `max_nof_served_beams` 从 visible pool 中标记当前 active hopping window；LEO 500 km 示例默认最多 3 个 active beams。
5. 当 `served_beam_hopping_enabled=false` 时，相同 satellite state 不重复通知 CU-CP；当 `served_beam_hopping_enabled=true` 且 visible pool 大于 active window 时，相同 satellite state 的周期 tick 会在达到 `served_beam_hopping_dwell_updates` 后轮转 window。
6. visible pool 重置依据是 beam 成员集合，而不是按仰角排序后的列表；LEO 运动导致相同成员的仰角排名交换时，不应把 hopping 起点清零。
7. selector 支持千级到万级静态 beam table；单元测试覆盖 15 km 密集 beam pool 中选取最高仰角 active window。
8. 当无 beam 可见时，允许 served beam set 变为空。
9. TLE 只保留为接口能力；真实精度需求进入 Phase 6 SGP4。

验收标准：

- 500 km circular orbit ECEF 半径约为 `6378137 + 500000` m。
- 非 hopping 模式下，相同 satellite state 不重复触发 served beam update。
- hopping 模式下，同一 visible pool 可按 dwell 周期轮转 active window，使长期可见但未进入上一窗口的 beam 获得候选服务机会。
- 同一 visible pool 仅发生仰角排序变化时，hopping 起点保持不变，避免 15 km 大规模波位池因为卫星小步移动反复回到最高仰角窗口。
- 同一 active served beam 成员集合仅发生顺序变化时，不清 UE candidate，不通知移动性管理器。
- 15 km dense beam pool 下，CU-CP 能从大量 visible candidates 中稳定选择最高仰角 active window。
- 大规模 visible pool 下保持明确 contract：scheduler 决定 hopping window，placement planner 只允许 window 内 beam 进入 active；window 外可见 beam 保持 candidate，上一轮已服务且仍有 UE/DRB load 的 beam 才进入 draining。
- 卫星过境导致可见波位变化时，served beam set 正确更新。
- `max_nof_served_beams > 1` 时，候选 beam 按 elevation 排序形成多波束 hopping pool，active window 大小受 `max_nof_served_beams` 限制。
- 空可见集不会导致崩溃，且能清空 served beam set。

主要风险：

- circular orbit 不能代表真实星历误差。
- 500 km LEO 过境窗口更短，真实系统需要更快更新和更强时间一致性验证。

## 6. Phase 2: CU-CP 波位到 DU 的运行时规划

目标：CU-CP 把 visible beams 转成可调度的 beam placement plan，并只把 active beams 暴露给移动性模块。

关键接口：

- `ntn_beam_placement_request`
- `ntn_beam_placement_plan`
- `ntn_beam_assignment_state`
- `get_active_ntn_beam_ids`
- `cu_cp_ntn_command_handler::get_current_ntn_beam_status`

波位状态：

| 状态 | 含义 |
|---|---|
| `inactive` | 静态存在，但当前不可见或不可用 |
| `candidate` | 当前可见，但不在本轮 hopping window 内，或没有可用 DU capacity |
| `active` | 当前由某个 DU 承载，可用于 UE 移动性目标 |
| `draining` | 上一轮 active，本轮离开可见窗口、离开 hopping window，或应退出服务 |

任务：

1. 输入静态 beam table、visible beams、DU capacity、beam load、previous assignments。
2. 优先保持 previous DU ownership，减少频繁 DU 归属变化。
3. DU 不存在、DU 不支持该 beam、DU active beam 数满、当前 DU 非 NTN UE/DRB 水位叠加 beam load 后超限时，beam 保持 `candidate`。
4. 离开可见窗口或离开本轮 hopping window 的 previous active/draining beam，如仍有 UE/DRB load，则保持 `draining`；load 清空后退役。
5. 从 placement plan 中提取 `active` beam IDs，作为 location mobility controller 的 served beam set。
6. 对多波束 hopping window 支持部分成功：本轮 window 内可承载 beam 进入 active，window 外或 DU 不支持/容量不足的 beam 保持 candidate/draining，不暴露给 UE 移动性目标选择。
7. `max_nof_served_beams` 作为 CU-CP 全局 active beam 上限下发到 placement planner，即使多个 DU 同时可承载，也不会超过当前 hopping window 尺寸。
8. 为人工联调和后续 northbound/CLI 暴露运行时 beam placement 快照，包含 beam id、NCI、DU、state、是否属于当前 hopping window、elevation、UE/DRB 水位；移动性仍只消费 active subset。
9. 即使 satellite state 对应的 visible candidates 不变，也要在每次状态更新时重新评估 DU placement，使 DU 掉线、DU 恢复、capacity/load 变化能收敛到新的 active/candidate 集合。
10. 有 UE/DRB load 但不在本轮 hopping window 的 visible beam 不得被提升为 active；只有 previous active/draining 且仍有负载的 beam 才能保留为 draining，并且 draining 负载预占原 DU 的 beam/UE/DRB 水位。
11. 在 O-CU-CP 命令行暴露只读 `ntn_beams` 命令，用于人工确认多波束 hopping window 的 active/candidate/draining 状态；默认只输出状态计数，支持 `summary|all|window|active|candidate|draining|inactive` 过滤和行数限制，避免 15 km 万级波位池刷屏。
12. active served beam set 变化时精准清理 UE 侧 pending NTN candidate：目标 beam 已离开新 active set 时清理；目标仍在新 active set 内时保留 TTT/连续报告累计。
13. 在 O-CU-CP 命令行暴露 `ntn_sat` 和 `ntn_sat_geo` 手动注入入口，便于不等待 circular orbit timer 时直接验证 `satellite state -> placement -> active served beams`。
14. 静态 beam table 在解析阶段做几何和唯一性校验，避免错误 JSON 进入 runtime hopping window。
15. CU-CP 在 placement plan 内生成用户驱动的 SR/SRS slot plan：仅对 active/draining 且已有 UE/DRB load 的服务波位分配时隙，空 active beam 保持 `nof_antenna_slots=0` 且 `sr_slot_period/srs_slot_period=0`；多个有负载波位按 UE 数、DRB 数、仰角、beam id 排序后形成重复 slot period，并按 UE 数给连续 slot 块。SR 使用服务块起始 offset，SRS 使用服务块末尾 offset。

验收标准：

- active/candidate/draining/inactive 均有测试覆盖。
- no DU、DU unavailable、DU full、unsupported beam、当前 DU load watermark 均有测试覆盖。
- candidate beam 不进入 served beam set，不作为 UE handover target。
- CU-CP runtime 根据 DU F1 served-cell NCI 构造 `supported_beam_ids`，避免把 beam 分配给不承载该小区的 DU。
- 多 active beams 可同时暴露给 location mobility，served beam snapshot 保持当前 hopping window 的 active 子集。
- 多 DU 场景下 active beam 总数受 `max_nof_served_beams` 全局约束，溢出或不在本轮 window 的 visible beams 进入 candidate/draining。
- `get_current_ntn_beam_status()` 能显示 candidate/draining beam 以及 `in_hopping_window`，便于区分“窗口外轮候”和“窗口内但 DU/容量不满足”；`get_current_ntn_served_beam_ids()` 仍只返回 active beam。
- `get_current_ntn_beam_status()` 和 `ntn_beams` 能显示 `slot_index/slot_period`、`nof_antenna_slots`、`sr_slot_offset/period` 与 `srs_slot_offset/period`；多 slot 波位在 CLI 中显示为 `start-end/period`，`ntn_beams scheduled` 只列出当前获得 SR/SRS 调度意图的波位。
- `served_beam_hopping_dwell_updates=3` 时，一个 active window 至少保持 3 个已接受的卫星状态更新周期，下一次重复更新才轮转，有利于 UE 位置连续报告和 TTT 累计完成。
- window 外有 load 但没有 previous service 的 visible beam 保持 candidate，不破坏 scheduler hopping window；previous active/draining 的 retained draining 会占用原 DU 容量，避免新窗口超配。
- visible pool 大于 active window 时，连续 satellite update 可观察到 active window 轮转，window 外 beam 不会被 location mobility 当作 handover target。
- unchanged satellite candidates 下 DU capacity 变化也能触发 placement 重算，旧 active beam 不会因为 scheduler 去重而残留。
- served beam set 变化会重置 UE candidate，目标 beam 离开再回来时必须重新满足连续报告和 TTT。
- `ntn_sat_geo 31.2304 121.4737` / `ntn_sat` + `ntn_beams` 可完成人工闭环：手动注入卫星状态后先用 `ntn_beams` 看状态汇总，再用 `ntn_beams window 16`、`ntn_beams active 16`、`ntn_beams candidate 32`、`ntn_beams draining 16` 查看明细。

主要风险：

- 当前 DU capacity 仍是 CU-CP 抽象快照，真实 DU beam activation 能力需要 Phase 6 联动。
- 本阶段的“跳波束”是 CU-CP 控制面 beam-set hopping：随卫星状态周期更新 active served beam set；PHY slot 级 beam hopping、RU precoding、DU beam activation 仍属于 Phase 6。

## 7. Phase 3: UE 位置驱动 NTN 移动性

目标：CU-CP 根据 UE 位置、服务波位几何和邻区/Xn 关系，稳定触发 NTN handover。

关键接口：

- `ntn_location_mobility_config`
- `ntn_ue_location_report`
- `ntn_location_handover_trigger`
- `ntn_handover_result`

默认策略：

```yaml
required_consecutive_location_reports: 3
time_to_trigger_ms: 1000
max_report_gap_ms: 750
boundary_hysteresis_m: 5000
handover_retry_timeout_ms: 2000
```

任务：

1. 接收已解码 UE 经纬度报告。
2. 过滤 invalid、unknown UE、unknown cell、stale、inaccurate、out-of-order、duplicate 报告。
3. UE 仍在 serving beam 覆盖内时清理 candidate，不触发 HO。
4. UE 进入 active target beam 后，按连续报告数和 TTT 累计 candidate。
5. 目标 beam 必须属于当前 active served beam set。
6. 目标 cell 必须存在，且与 serving cell 存在允许 handover 的邻区/Xn 关系。
7. HO trigger 被接受后记录 attempt id。
8. HO 成功或失败都清理 candidate；失败后按后续位置报告重新累计。

验收标准：

- UE 在服务波位内：不触发 HO。
- UE 进入可服务目标波位：满足连续报告和 TTT 后触发 HO。
- UE 离开所有 active beams：不触发 HO。
- 目标无邻区/Xn、目标 cell 配置不完整：不触发 HO。
- HO 失败后不会卡住旧 candidate。
- NTN location mobility 开启时，传统 RSRP measurement 不直接触发 HO。

主要风险：

- UE location 来源当前是假设已解码输入，真实 RRC 字段确认放到 Phase 6。
- 跨 DU / 跨星失败恢复还需要比当前单元测试更长的集成链路。

## 8. Phase 4: NGAP 位置上报与 AMF 控制

目标：把 accepted UE NTN location report 与核心网标准位置上报打通，同时不暴露 CU-CP 内部经纬度决策模型。

关键接口：

- `ngap_location_report`
- `ngap_location_reporting_control`
- NGAP `LocationReport`
- NGAP `UserLocationInformationNR`
- NGAP `NRNTNTAIInformation`

任务：

1. 只有 `ntn_location_report_result::accepted` 进入核心网上报逻辑。
2. UE location 可作为 CU-CP mobility/cache 输入被 accepted，但进入核心网上报前必须确认 serving NCI 当前属于 active 或 draining beam；candidate/inactive 不发送本地 LocationReport，AMF direct 返回 failure。
3. 支持本地配置触发 LocationReport。
4. 支持 AMF `LocationReportingControl` direct/deactivate/failure。
5. 对 `change_of_serving_cell` 建立 UE serving cell 基线，只在后续 accepted location report 的 serving NCI 变化时上报。
6. 支持 `stop_change_of_serving_cell` 清理 active reporting state，避免停用后继续上报。
7. LocationReport 使用标准 NR CGI、TAI、timestamp、request type。
8. 可选填 `NRNTNTAIInformation`，不新增私有经纬度 IE。
9. AMF 请求 unknown UE、重复 reference id、direct 但尚无 accepted UE location、不可服务状态时返回标准 failure indication。

验收标准：

- direct LocationReport 单测通过。
- AMF LocationReportingControl 正常建立/关闭 reporting state。
- change-of-serving-cell 只在 serving cell 变化时发送，重复位置报告不产生冗余上报。
- duplicate reference id、unknown UE、direct 无可用位置返回预期 failure。
- NGAP ASN.1 编码保持标准字段。

主要风险：

- 当前仍缺真实 AMF 互通抓包。
- AMF 若需要精确 UE 经纬度，应走标准定位流程；本阶段 NGAP report 只表达标准 NR location 语义。

## 9. Phase 5: CU-CP 集成链路与回归保护

目标：把 Phase 1-4 串成 CU-CP 层可验证闭环，并保护非 NTN 路径不变。

集成链路：

```text
satellite update
  -> visible beams
  -> placement plan
  -> active served beams
  -> UE location report
  -> candidate beam
  -> handover trigger
  -> handover result cleanup
```

并行链路：

```text
accepted UE location report
  -> CU-CP reporting policy
  -> NGAP LocationReport
```

任务：

1. 补齐 `satellite update -> served beams -> UE location -> handover trigger` 的 CU-CP 集成测试。
2. 补齐 `accepted UE location -> NGAP LocationReport` 的 CU-CP 层端到端测试。
3. 覆盖 inter-DU handover setup failure 后的结果清理与后续 UE location 重新触发。
4. 保留 `ntn_location_mobility.enabled == true` 时 RSRP measurement 不直接触发 HO 的回归测试。
5. 保留 `ntn_location_mobility.enabled == false` 时传统 measurement-based mobility 行为不变。
6. 提供 `configs/leo_500km_cucp_ntn.yml` 和 `configs/leo_500km_beam_table.json`，用于人工联调。
7. 使用 `utils/ntn/generate_leo_beam_table.py --center-lat-deg 31.2304 --center-lon-deg 121.4737 --region-radius-km 30 --beam-radius-m 15000 --output /tmp/leo_test_beams.json` 可生成区域级 15 km beam table；不传 `--region-radius-km` 时按 `--satellite-height-m` 和 `--min-elevation-deg` 估算 LEO 足迹半径。

验收标准：

- CU-CP NTN mobility 单元测试通过。
- cell measurement NTN location tests 通过。
- NGAP location reporting tests 通过。
- `git diff --check` 通过。
- LEO 500 km 示例配置可被人工检查并作为启动配置基础。

主要风险：

- 当前构建目录可能未开启 `BUILD_TESTING`；若测试目标不存在，需要重新配置测试 build。
- Windows/WSL 混合构建环境可能导致工具链路径和增量构建行为不一致。

## 10. Phase 6: 后续增强

这些任务不进入 CU-CP v1 完成标准，但必须记录，避免第一阶段设计把接口堵死。

后续任务：

1. 用 SGP4 替换轻量 TLE propagator，保持上层接口不变。
2. 实现真实 RRC UEAssistanceInformation / LocationMeasurementIndication 位置解码。
3. 接入动态邻星/Xn/ISL 状态，而不是只依赖静态 JSON。
4. 将 active beam plan 对接 DU beam activation、SIB19 更新和 RU beamforming。
5. 加入真实 AMF、DU、RU 长链路验证和抓包。
6. 对 Doppler、TA、Koffset、HARQ、channel emulator 做 DU/MAC/PHY 侧专项计划。

## 11. 功能归属矩阵

| NTN 功能 | 归属 | 当前处理 |
|---|---|---|
| O&M 星历/轨道参数 | CU-CP 依赖外部输入 | config/manual/circular/TLE source |
| satellite state 周期更新 | CU-CP负责 | Phase 1 |
| visible beams 计算 | CU-CP负责 | Phase 1 |
| beam placement active/candidate/draining | CU-CP负责 | Phase 2 |
| DU capacity / DU connected state | CU-CP依赖DU | Phase 2 使用 connected DU 和 served-cell snapshot，真实 beam capacity 仍待 DU/RU 对接 |
| UE 经纬度输入 | CU-CP依赖RRC/定位源 | Phase 3 只消费 decoded report |
| UE 位置过滤与候选稳定性 | CU-CP负责 | Phase 3 |
| handover trigger/result | CU-CP负责 | Phase 3 |
| NGAP LocationReport | CU-CP负责 | Phase 4 |
| SIB19 / NTN-Config 编码增强 | CU-CP依赖DU/RRC | Phase 6 |
| DU scheduler / HARQ / Koffset / TA | CU-CP依赖DU/MAC | Phase 6 |
| RU beamforming / precoder | CU-CP依赖RU/PHY | Phase 6 |
| PHY Doppler / channel emulator | CU-CP依赖PHY/测试环境 | Phase 6 |

## 12. 当前代码能力矩阵

| 能力 | 关键模块 | 状态 | 缺口 |
|---|---|---|---|
| LEO 500 km circular orbit | `orbit_propagator` | 已有 | 真实 TLE/SGP4 |
| ECEF -> visible beams | `ntn_served_beam_selector` | 已有 | 长时间过境验证 |
| served beam update 去重 | `ntn_served_beam_scheduler` | 已有 | 更完整 CU-CP 集成测试 |
| beam placement | `ntn_beam_placement_planner` | 已有 | 真实 DU capacity 输入 |
| active beam repository | `ntn_beam_assignment_repository` | 已有 | DU activation 联动 |
| UE location report | `ntn_location.h` | 已有 | 真实 RRC 解码 |
| location mobility | `ntn_location_mobility_controller` | 已有 | 跨 DU/跨星恢复测试 |
| NGAP location reporting | `ngap_location_reporting` / `ngap_impl` | 已有 | 真实 AMF 互通 |
| O-CU-CP config | `apps/units/o_cu_cp/cu_cp` | 已有 | LEO 示例配置已补充 |

## 13. P0/P1/P2 缺口表

| 优先级 | 任务 | 验收标准 |
|---|---|---|
| P0 | 固化完整分阶段计划 | 本文档完成 |
| P0 | LEO 500 km 最小配置和 beam table | `configs/leo_500km_*` 存在 |
| P0 | circular orbit 500 km 半径测试 | updater 测试覆盖 |
| P0 | served beam update 去重 | scheduler 测试覆盖 |
| P0 | DU full -> candidate | placement 测试覆盖 |
| P0 | UE location 过滤和 retry | cell_meas_manager 测试覆盖 |
| P1 | CU-CP placement 重试集成测试 | DU 后上线后相同 satellite state 可从 candidate 变 active |
| P1 | active served beam 查询一致性 | DU 不可用时 current served beam set 清空，不返回旧 scheduler cache |
| P1 | DU supported beam 运行时映射 | DU 不承载 beam 对应 NCI 时，该 beam 保持 candidate |
| P1 | 多波束/跳波束 CU-CP 集成 | `max_nof_served_beams > 1` 时 active/candidate/draining 与 UE HO target 选择均正确 |
| P1 | CU-CP 端到端 mobility 集成测试 | satellite update 到 HO trigger |
| P1 | CU-CP 到 NGAP report 集成测试 | accepted location 到 LocationReport，且使用标准 NRNTNTAIInformation |
| P1 | NGAP reporting 配置暴露 | CU-CP app YAML/CLI 可配置 local forwarding、AMF control、节流 |
| P1 | AMF LocationReportingControl 互通 | direct/deactivate/failure 覆盖，真实 AMF 抓包仍待做 |
| P1 | RRC 位置协议确认 | 明确真实解码边界 |
| P2 | TLE SGP4 替换 | 与外部参考数据对齐 |
| P2 | 动态邻星/Xn 状态 | 运行时 satellite/link state |
| P2 | DU/RU 联动 | active beam plan 对接真实 beam activation |

## 14. 测试计划

单元测试：

- circular orbit 500 km ECEF radius。
- served beam update duplicate suppression。
- multi-beam served beam hopping window selection。
- beam placement active/candidate/draining/no DU/DU full/overflow。
- UE location invalid/stale/inaccurate/duplicate/out-of-order/gap/retry。
- HO success/failure candidate cleanup。
- NGAP LocationReport / LocationReportingControl / failure indication。

集成测试：

- satellite update 到 active served beam set。
- satellite update 到 multi-beam active served beam subset。
- active served beam set 到 UE location handover trigger。
- accepted UE location 到 NGAP LocationReport。
- AMF direct/deactivate/failure reporting control。
- NTN enabled 时 RSRP measurement 不直接触发 HO。

人工验收：

- 使用 `configs/leo_500km_cucp_ntn.yml` 作为 CU-CP 配置基础。
- 使用 `configs/leo_500km_beam_table.json` 加载静态 beam table。
- 启动后先确认 DU F1 setup 承载 5 个示例 NCI：`0x66c000` 到 `0x66c004`。
- 使用 `ntn_sat_geo 31.2304 121.4737` 注入 500 km 高度、星下点靠近示例 beam pool 的卫星状态；也可使用 `ntn_sat <ecef_x_m> <ecef_y_m> <ecef_z_m>` 注入精确 ECEF。
- 连续执行相同 `ntn_sat` 命令并随后执行 `ntn_beams`，在 `served_beam_hopping_enabled=true` 且 visible pool 大于 3 时，应看到 active/candidate/draining/inactive 计数变化，并可用 `ntn_beams active 16` 验证 active window 按 `served_beam_hopping_dwell_updates` 在 5 个 beam 中轮转。
- `ntn_beams active` 输出中 `state=active` 且 `window=yes` 的 beam 才能进入 UE location mobility；`candidate/draining` 或 `window=no` 不作为 handover target。
- 注入 UE location 后能稳定触发或不触发 HO，符合 Phase 3 场景。
- NGAP 上报包中无私有经纬度扩展；若 UE serving NCI 不属于当前 active/draining beam，本地 forwarding 不发送 LocationReport，AMF direct 返回 failure。

## 15. 协议和开源资料索引

| 资料 | 本计划使用方式 |
|---|---|
| 3GPP NTN Overview | Release 17 NTN 背景、透明载荷、LEO、UE 位置能力、O&M 边界 |
| TS 38.300 | NTN overall architecture、NGSO/LEO、beam 类型、O&M ephemeris |
| TS 38.331 | SIB19、NTN-Config、referenceLocation、distanceThresh、time/location CHO trigger |
| TS 38.304 | idle/inactive location-based measurement initiation/reselection |
| TS 38.413 | LocationReportingControl、LocationReport、UserLocationInformationNR、NRNTNTAIInformation |
| TR 38.821 | LEO moving/earth-fixed beams、TA/mobility 问题、TLE/SGP4/ECEF 背景 |
| srsRAN NTN Tutorial | 当前公开 NTN 教程偏 GEO/SIB19/TA/Koffset/channel emulator |
| OAI NTN MR !2722/!2723 | 主要覆盖 gNB/UE 空口定时能力，不等价 CU-CP location mobility |

## 16. 第一阶段完成定义

第一阶段完成时必须同时满足：

- 文档层：完整分阶段计划、功能归属矩阵、缺口表、测试计划、示例配置齐备。
- 控制面链路：CU-CP 能根据 LEO 500 km satellite state 得到 active served beam set。
- 移动性链路：UE 位置报告能驱动 candidate beam 和 handover trigger/result cleanup。
- 核心网链路：accepted UE location 在 active/draining serving beam 上能进入标准 NGAP LocationReport 路径。
- 边界约束：不修改 RU/PHY/SIB19，不添加私有 NGAP 经纬度扩展；DU 仅保留可选 SR/SRS offset hook，非 NTN 路径回退原策略。
- 回归约束：非 NTN 配置路径行为不变。

## 17. SR/SRS 时隙落点补充

在“真实调度时隙”推进中，当前实现已经把 CU-CP beam placement plan 的 SR/SRS offset 设计落到 DU 初始 UE 资源分配入口：

- `cell_group_config` 新增 `ntn_ul_slot_request`，用于携带 `sr_slot_offset/sr_slot_period` 和 `srs_slot_offset/srs_slot_period`。
- `du_ran_resource_manager_impl::create_ue_resource_configurator()` 接收可选 NTN slot request，并在 PCell 资源分配前写入 CellGroupConfig。
- `du_pucch_resource_manager` 会严格尝试请求的 SR slot offset，仍保留 PUCCH grants-per-slot 与 CSI 兼容检查；若 offset 不可用，本次 NTN PUCCH 分配失败，不再静默回退到其他 SR slot。
- `du_srs_resource_manager` 会严格尝试请求的 SRS slot offset；若 offset 不可用，本次 NTN SRS 分配失败，不再回退到原 max-UL-rate 策略。
- 新增单元测试覆盖 SR、SRS、以及初始 UE allocation 同时采用请求 offset 的路径。

CU-CP 到 DU 的 wire-level 携带方式已经补上内部实现：srsRAN 使用 `SRSNTN02` vendor container 复用空闲的 F1AP `res_coordination_transfer_container`，用于在 UE Context Setup/Modification 中携带 CU-CP 计算出的 SR/SRS offset 与 period；`SRSNTN02 flags=0` 表示清除已有 NTN slot request，解码侧保留 `SRSNTN01` 旧 offset-only payload 兼容。这不是标准化跨厂商 F1AP 扩展，只是当前工程内的 CU-CP/DU 协同接口。

## 18. F1AP 承载补充：CU-CP slot plan 到 DU UE creation

当前实现已经补上 CU-CP 到 DU 的首段承载路径：

- CU-CP 在 Initial Context Setup 前读取当前 `ntn_beam_placement_plan`，只对 UE 所在 NCI/DU 匹配且状态为 `active` 或 `draining`、并已经由用户负载触发 SR/SRS slot offset 的波位生成 `f1ap_ntn_ul_slot_resource_request`。
- F1AP-CU 在 UE Context Setup Request 中，如果标准 `res_coordination_transfer_container` 为空，则用 srsRAN 内部 `SRSNTN02` vendor 容器携带 SR/SRS offset 与 period。
- F1AP-DU 在无 `gNB-DU-UE-F1AP-ID` 的 UE Context Setup 中识别该容器，并把 NTN slot request 放进 `f1ap_ue_context_creation_request`。
- DU UE manager 将 F1AP request 转换为 DU 内部 `ntn_ul_slot_resource_request`，传给 `ue_creation_procedure`，最终进入 `du_ran_resource_manager_impl::create_ue_resource_configurator()`，由 PUCCH/SRS resource manager 生成真实 SR/SRS RRC 资源。

## 19. F1AP Modification 与在线 UE SR/SRS slot 重配置

本轮把“只有新建 UE 能拿到 slot request”的路径继续推进到了在线 UE：

- F1AP-CU `UE Context Modification Request` 现在也支持可选 `f1ap_ntn_ul_slot_resource_request`，并在标准 container 为空时写入 `SRSNTN02` vendor payload；空 request 会编码成 `flags=0`，作为撤销旧 NTN SR/SRS slot request 的 clear 信号。
- F1AP-DU 在 UE Context Setup 与 UE Context Modification 中都会解码该 payload，并把它放进 `f1ap_ue_context_update_request`。
- `ue_configuration_procedure` 将 NTN slot request 视为真实配置变化，即使没有新增/删除 DRB，也会触发 DU RAN resource update 和 MAC UE reconfiguration。
- `du_ran_resource_manager_impl::update_context()` 在 PCell 不变时会重新分配 PUCCH SR 与 SRS offset；收到 clear 信号时会撤销 `cell_group_config.ntn_ul_slot_request` 并按非 NTN 默认策略重新分配 PUCCH/SRS；若新分配失败，会尝试恢复旧 SR/SRS 资源，避免在线 UE 丢失原有 UL 控制/探测资源。
- CU-CP 在 beam placement plan 更新后，会对已经 attach、已有 CU-UP/DRB、且所在 active/draining beam 拥有 SR/SRS slot plan 的 UE 发起 UE Context Modification。DU 返回 CellGroupConfig 后，CU-CP 再触发 RRC Reconfiguration，把新的 SR/SRS RRC 配置交给 UE。
- CU-CP 现在还会在 PDU Session setup/modify/release 过程结束后，基于最近一次卫星状态得到的 visible beam candidates 主动刷新 placement plan；因此用户接入建立 DRB 后无需等待下一次星历 tick 即可触发在线 SR/SRS slot update，最后一个 DRB 释放后也会立即触发 clear。
- CU-CP 会按 UE 缓存最近一次已下发/待完成的 NTN SR/SRS slot request；星历刷新只改变仰角或 beam 状态快照、但 SR/SRS offset/period 未变时，不重复发起 UE Context Modification。若此前已下发 slot request、但当前 UE 已无 CU-UP/DRB 负载或当前 beam 不再产生 slot plan，CU-CP 会先发送 clear request，让 DU/UE 撤销旧 NTN 时隙意图；clear 成功后清理缓存，失败时恢复旧缓存以便后续重试。
- DU 侧收到 period 后会严格校验它与当前 PUCCH SR/SRS 周期配置一致；若 CU-CP 的周期意图与 DU 本地可用周期不匹配，本次 NTN 资源分配失败，避免只满足 offset、却丢失 CU-CP 时序周期语义。

该路径仍然是 srsRAN 内部 vendor 承载，不改变 3GPP ASN.1 标准字段定义；如果后续需要与异厂家 DU 互通，应把该容器升级为明确协商的 private IE 或标准化扩展。
