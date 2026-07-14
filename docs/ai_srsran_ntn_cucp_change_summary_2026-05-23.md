# AI 辅助 srsRAN NTN / CU-CP 改动总览与设计理解

记录日期：2026-05-23
仓库：`d:\code\srsRAN_Project-main`
当前分支：`main`
整理范围：从本轮 AI 辅助评估开始到当前功能整理点，包括已提交的 NTN 功能增强、文档/PPT，以及已经拆分入 git 历史的 CU-CP NTN 移动性、邻区、接纳控制和核心网位置上报改动。

## 1. 总体结论

这段工作已经从“评估 AI 是否能辅助通信软件编程”推进到一个较完整的 NTN/CU-CP 功能原型：先在 O-DU / NTN 公共库侧补强 beam hopping、多波束 dwell、feeder link Doppler 和远程配置校验；随后把重心转到 CU-CP，围绕“NTN 下完全基于 UE 位置进行移动性判断”重新设计了测量管理、服务波位、目标波位、邻星邻区、切换触发、DU 波位分配、用户接纳和核心网位置上报。

我对当前方案的理解是：NTN 移动性不应再把传统小区测量事件作为唯一决策依据。卫星过境时，地面波位与服务波位集合会随轨道动态变化，UE 的经纬度、波位覆盖几何、当前服务波位列表和邻星/Xn 关系，才是 CU-CP 判断“该不该切换、切到哪里、由哪个 DU 调度”的核心输入。RSRP/测量报告仍然可以作为位置来源或辅助信号，但不应主导 NTN 场景下的移动性决策。

## 2. 整理后的 Git 记录

截至功能整理提交 `94973bc`，初始化提交 `19a9aa4` 之后已有 17 个提交。前 12 个提交主要覆盖 O-DU/NTN 公共能力和评估材料，后 5 个提交是本次把工作区改动按功能边界拆分后的 CU-CP/NTN 主体改动。

### 2.1 早期 NTN 与评估材料提交

| 提交 | 主题 | 主要意义 |
|---|---|---|
| `6a97eb7` | `fix: harden NTN beam hopping scheduling` | 强化 beam hopping 调度，避免空闲 tick、重复 dwell 等边界问题。 |
| `19ffb83` | `fix: refresh NTN beam hopping on ephemeris update` | 星历变化时刷新 beam hopping，避免同一时间点但星历版本变化时不更新。 |
| `9521125` / `d38bfd8` / `a1468d5` / `6538098` / `7e5cdf4` / `efe0a9c` | docs / PPT 更新 | 持续把 AI 编程评估、NTN 迭代结果、多波束方案和 CU-CP 机会点写入文档/PPT。 |
| `e8aac66` | `test: cover NTN remote command validation` | 增加远程 JSON 命令校验测试，覆盖正向、停止和异常输入。 |
| `e797e1f` | `feat: apply NTN feeder link Doppler compensation` | 将 feeder link Doppler 补偿接入配置管理和 RU CFO 下发路径。 |
| `cc6e2af` | `feat: support NTN multi-beam dwell sets` | 将单 beam dwell 扩展成多 beam dwell set，支持同色非相邻波位并发调度思路。 |
| `5ba5992` | `feat: harden NTN multi-beam aggregation` | 优化多波束聚合，处理球面中心、经度环绕和跨 180 度经线场景。 |

这些早期提交相对初始化提交的增量约为 `14 files changed, 1750 insertions, 136 deletions`。集中在：

- `lib/ntn/beam_hopping_controller.cpp`
- `lib/ntn/beam_hopping_table.cpp`
- `include/srsran/ntn/beam_hopping_controller.h`
- `include/srsran/ntn/beam_hopping_table.h`
- `apps/units/flexible_o_du/split_helpers/...`
- `tests/unittests/ntn/...`
- `docs/ai_comm_*`

### 2.2 本次拆分出的 CU-CP/NTN 功能提交

| 提交 | 主题 | 改动规模 | 主要意义 |
|---|---|---:|---|
| `313a1aa` | `feat(ntn): add orbit-driven beam visibility` | `5 files, +798/-0` | 增加圆轨道/TLE 轻量传播、仰角可见性判断和轨道传播单测，给服务波位调度提供真实轨道输入。 |
| `4b884b9` | `feat(cu-cp): add NTN beam planning modules` | `17 files, +2000/-1` | 增加静态波位表加载、服务波位选择、调度、卫星状态更新、CU-CP 波位到 DU 的规划与仓储。 |
| `a82d1d2` | `feat(cu-cp): add NTN location mobility managers` | `22 files, +2787/-27` | 增加 UE 位置报告类型、位置驱动移动性控制器、邻区/邻星管理、位置报告过滤与单测。 |
| `5b6f2cd` | `feat(ngap): support NTN location reporting` | `10 files, +524/-2` | 增加 NGAP LocationReportingControl / LocationReport 类型、AMF 控制入口、RRC 位置入口和 NGAP 单测。 |
| `94973bc` | `feat(cu-cp): integrate NTN control and admission` | `44 files, +2591/-74` | 把 NTN 控制面能力接入 CU-CP 配置、命令处理、移动性、接纳控制、批量释放、切换流程和应用层配置测试。 |

### 2.3 当前累计规模

| 统计口径 | 文件/行数 | 说明 |
|---|---:|---|
| 早期已提交历史，相对初始化提交 | `14 files changed, 1750 insertions, 136 deletions` | NTN beam hopping、多波束 dwell、Doppler、远程命令测试和评估文档/PPT。 |
| 本次整理出的 5 个 CU-CP/NTN 功能提交 | `98 files changed, 8700 insertions, 104 deletions` | 从原工作区拆分出的轨道、波位规划、位置移动性、NGAP 位置上报、接纳控制和批量释放改动。 |
| 截至 `94973bc`，相对初始化提交 `19a9aa4..HEAD` | `111 files changed, 10450 insertions, 240 deletions` | 这是已经进入 git 历史的总体功能与文档增量，不再依赖未提交 diff 统计。 |
| 功能提交后的文档整理 | 本文档独立提交 | 代码改动已按模块提交；本文档作为单独 docs 提交追加，便于区分功能实现与总结材料。 |

## 3. 新增 CU-CP/NTN 功能内容

下面几节对应上面 5 个新功能提交，是后续 NTN CU-CP 方案的主体。

### 3.1 位置驱动的 NTN 移动性

核心文件：

- `include/srsran/cu_cp/ntn_location.h`
- `lib/cu_cp/cell_meas_manager/ntn_location_mobility_controller.h`
- `lib/cu_cp/cell_meas_manager/ntn_location_mobility_controller.cpp`
- `lib/cu_cp/cell_meas_manager/cell_meas_manager_impl.cpp`
- `lib/cu_cp/cell_meas_manager/cell_meas_manager_impl.h`
- `include/srsran/cu_cp/cell_meas_manager_config.h`

主要变化：

- 新增 `ntn_ue_location_report` 和 `ntn_location_report_result`，把 UE 位置报告的处理结果区分为 `accepted`、`invalid`、`unknown_ue`、`stale`、`inaccurate`、`duplicate` 等。
- 将 `cell_meas_manager::report_ue_location(...)` 从“只触发内部移动性”改成“先校验并缓存，再返回明确结果”。
- 将候选目标波位状态放到 UE 维度，避免把 `consecutive_reports` 这类稳定性状态误当成小区级状态。
- 通过 `ntn_candidate_beam_state` 做时间触发、连续报告、最大报告间隔、handover retry 等控制，降低边界抖动和 ping-pong。
- 增加最近一次有效 UE NTN 位置查询接口，供 AMF direct LocationReportingControl 立即上报使用。

设计理解：

位置驱动移动性里，`candidate beam` 不是“已经切换的目标”，而是“当前几何上更合适、但还需要稳定性确认的目标波位”。它的存在是为了把瞬时位置抖动、定位误差和波位边界效应过滤掉。只有候选持续满足条件，才进入后续切换流程。

### 3.2 静态波位表与运行时服务波位

核心文件：

- `lib/cu_cp/cell_meas_manager/ntn_beam_table_json.cpp`
- `include/srsran/cu_cp/cell_meas_manager_config.h`
- `configs/geo_ntn.yml`

主要变化：

- 将静态地面波位设计为外部配置数据，支持 JSON 加载。
- 波位几何参数包括 `beam_id`、`nci`、中心经纬度、覆盖半径、启用状态。
- 明确“正六边形可以用于表达/规划，但天线真实覆盖近似圆形”，因此判决逻辑使用圆形覆盖半径。
- 服务波位不写死在静态 JSON 中。静态表描述“可用波位全集”，运行时由卫星轨道、仰角、调度策略选出“当前服务波位列表”。

设计理解：

静态波位表是地图，运行时服务波位是卫星过境期间的当前调度结果。一个卫星过境期间可以同时服务多个波位，DU/RU 通过跳波束或时分方式承载这些服务波位。CU-CP 不应每次都从文件做低效查询，而是加载后构建内存索引，运行时只查缓存。

### 3.3 轨道计算与服务波位调度

核心文件：

- `include/srsran/ntn/orbit_propagator.h`
- `lib/ntn/orbit_propagator.cpp`
- `lib/cu_cp/ntn_mobility/ntn_served_beam_selector.h`
- `lib/cu_cp/ntn_mobility/ntn_served_beam_selector.cpp`
- `lib/cu_cp/ntn_mobility/ntn_served_beam_scheduler.h`
- `lib/cu_cp/ntn_mobility/ntn_served_beam_scheduler.cpp`
- `lib/cu_cp/ntn_mobility/ntn_satellite_state_updater.h`
- `lib/cu_cp/ntn_mobility/ntn_satellite_state_updater.cpp`

主要变化：

- 增加圆轨道传播模型，默认高度可按 500 km 配置。
- 增加 TLE 解析与轻量 TLE 传播接口，保留后续替换为完整 SGP4 的空间。
- 根据卫星 ECEF 坐标、地面波位中心、最小仰角阈值筛选可见波位。
- 支持将可见波位转换成 CU-CP 侧服务波位候选，并周期更新服务波位集合。

设计理解：

“真实轨道计算”是 NTN 多波束方案的关键输入。没有轨道状态，服务波位只能靠人工配置，无法体现卫星过境、仰角变化和波位进入/离开可服务窗口。当前实现是工程原型：圆轨道和简化 TLE 足够支撑 CU-CP 调度链路验证，后续可替换传播内核而不改上层接口。

### 3.4 CU-CP 多波束规划与 DU 分配

核心文件：

- `lib/cu_cp/ntn_mobility/ntn_beam_placement_planner.h`
- `lib/cu_cp/ntn_mobility/ntn_beam_placement_planner.cpp`
- `lib/cu_cp/ntn_mobility/ntn_beam_assignment_repository.h`
- `lib/cu_cp/ntn_mobility/ntn_beam_assignment_repository.cpp`
- `lib/cu_cp/cu_cp_impl.cpp`
- `lib/cu_cp/cu_cp_impl.h`

主要变化：

- 新增 `ntn_beam_placement_planner`，由 CU-CP 决定哪些地面波位分配给哪些 DU 调度。
- 引入波位状态：`inactive`、`candidate`、`active`、`draining`。
- 输入包括静态波位、可见波位、DU capacity、当前波位负载、上一次 assignment。
- 保留 DU ownership 稳定性，避免每次轨道刷新都频繁改 DU 归属。
- `get_active_ntn_beam_ids(...)` 从 placement plan 中提取当前 active beam set，再下发到测量/移动性模块。

设计理解：

CU-CP 负责策略和拓扑，不负责物理波束赋形。也就是说，CU-CP 可以决定“波位 A/B/C 当前由 DU-0 服务，波位 D 进入 draining”，但真正的时频资源、beamforming、precoder 和 RU 发射控制仍应由 DU/RU 链路完成。这符合 CU-CP 的控制面定位，也让后续接入真实硬件时边界更清晰。

### 3.5 NTN 切换流程增强

核心文件：

- `include/srsran/cu_cp/cu_cp_types.h`
- `lib/cu_cp/mobility_manager/mobility_manager_impl.cpp`
- `lib/cu_cp/mobility_manager/mobility_manager_impl.h`
- `lib/cu_cp/routines/mobility/intra_cu_handover_routine.cpp`
- `lib/cu_cp/routines/mobility/intra_cu_handover_target_routine.cpp`
- `lib/cu_cp/routines/mobility/handover_reconfiguration_routine.cpp`

主要变化：

- 为 NTN handover 增加上下文：`ntn_handover_context`、`ntn_handover_result`、失败原因等。
- 将位置候选波位、连续报告数、candidate age 等信息带入切换链路。
- 同时考虑源侧和目标侧：源侧负责触发和上下文携带，目标侧负责资源/UE 上下文建立后的结果回传。
- 增加 `handle_ntn_handover_result` 一类结果处理入口，用于让测量/移动性逻辑知道切换成功或失败，并据此进入 retry 或恢复策略。

设计理解：

NTN 切换不能只看“邻区更好”。位置驱动后，切换链路需要知道目标波位为什么被选中、稳定了多久、是不是因为当前服务波位离开覆盖，以及失败后能否恢复源侧服务。否则会出现候选状态和切换实际结果脱节。

### 3.6 邻区 / 邻星 / Xn 管理

核心文件：

- `include/srsran/cu_cp/neighbor_cell_manager_config.h`
- `lib/cu_cp/neighbor_cell_manager/neighbor_cell_manager.cpp`
- `configs/cu_cp_neighbor_cells_example.json`
- `tests/unittests/cu_cp/neighbor_cell_manager/neighbor_cell_manager_test.cpp`

主要变化：

- 增加邻区信息管理模块和 JSON 配置入口。
- 邻区不仅是普通 NR 小区关系，还带有 `satellite_id`。
- 增加邻星 Xn 关系：`source_satellite_id`、`target_satellite_id`、是否双向、是否启用、是否允许 handover。
- `merge_into(cell_meas_manager_cfg&)` 可将允许切换的邻区关系合并进测量管理配置。

设计理解：

NTN 下邻区更准确地说是“邻星管理的小区/波位”。是否允许切换，不只取决于两个 NCI 是否相邻，还取决于邻星之间是否有 Xn 关系、是否启用、是否允许 handover。这个模块把几何邻接和控制面可达性分开，是后续多星协同的基础。

### 3.7 用户批量释放

核心文件：

- `include/srsran/cu_cp/cu_cp_types.h`
- `lib/cu_cp/routines/ue_batch_release_routine.h`
- `lib/cu_cp/routines/ue_batch_release_routine.cpp`
- `lib/cu_cp/routines/cell_deactivation_routine.cpp`
- `tests/unittests/cu_cp/cu_cp_ue_context_release_test.cpp`

主要变化：

- 增加 `cu_cp_ue_context_release_batch_command` 和 response。
- 增加批量释放 routine，记录 requested、released、not found、duplicate、failed to schedule 等结果。
- 将 cell deactivation 场景接入批量释放思路，便于 NTN 波位/小区离开服务窗口时释放一批 UE。

设计理解：

NTN 场景里，波位服务窗口变化可能导致一组 UE 同时需要迁移或释放。批量释放不是简单循环调用单 UE release，而是要有结果汇总、重复 UE 去重和失败项可追踪，方便运维和后续 retry。

### 3.8 接纳控制

核心文件：

- `include/srsran/cu_cp/cu_cp_configuration.h`
- `lib/cu_cp/cu_cp_controller/cu_cp_ue_admission_controller.h`
- `lib/cu_cp/cu_cp_controller/cu_cp_controller.cpp`
- `lib/cu_cp/ue_manager/ue_manager_impl.cpp`
- `tests/unittests/cu_cp/cu_cp_connectivity_test.cpp`

主要变化：

- 增加按请求类型区分的接纳控制：初始接入、重建、切换入。
- 增加 UE 数、DRB 数、最大容量和水位线。
- `ue_manager` 增加 DRB 统计能力。
- CU-CP 状态接口增加接纳控制状态输出。

设计理解：

接纳控制不是简单的“是否已满”。初始接入、重建、切换入对业务连续性的影响不同，应有不同水位线。典型策略是：切换入和重建可以允许更高水位，初始接入更保守，以保护已有业务和移动性成功率。

### 3.9 UE NTN 位置上报到核心网

核心文件：

- `include/srsran/ngap/ngap_location_reporting.h`
- `include/srsran/ngap/ngap.h`
- `lib/ngap/ngap_impl.cpp`
- `lib/ngap/ngap_impl.h`
- `lib/ngap/ngap_asn1_converters.h`
- `lib/cu_cp/cu_cp_impl.cpp`
- `lib/cu_cp/adapters/ngap_adapters.h`
- `lib/rrc/ue/rrc_ue_message_handlers.cpp`

主要变化：

- 新增公共类型：`ngap_location_report`、`ngap_location_reporting_control`、`ngap_location_reporting_control_response`。
- NGAP 支持 AMF 下发 `LocationReportingControl`，并返回 `LocationReportingFailureIndication`。
- CU-CP 维护每 UE Location Reporting 状态，按 `locationReportReferenceID` 保存 AMF 控制请求。
- 本地配置和 AMF 控制两种触发都支持，默认本地转发关闭、AMF 控制开启。
- 只有 `ntn_location_report_result::accepted` 的 UE 位置报告才会触发核心网上报。
- `change_of_serving_cell` 建立 serving cell 基线，只在 serving NCI 变化时发送 LocationReport；`stop_change_of_serving_cell` 后停止上报。
- direct `LocationReportingControl` 若尚无 accepted UE location，则明确返回 failure，而不是静默接受。
- `LocationReport` 填标准字段：AMF/RAN UE NGAP ID、`UserLocationInformationNR`、`NR CGI`、`TAI`、`timeStamp`、`LocationReportingRequestType`。
- 可选填 `NRNTNTAIInformation` 扩展，但不把 UE 经纬度作为私有 NGAP 扩展上报。
- RRC 侧补充 `LocationMeasurementIndication` 和 `UEAssistanceInformation` 入口；对无法解析为经纬度的 opaque 字段只记录，不伪造位置。

设计理解：

核心网位置上报和 CU-CP 内部移动性位置判断是两条不同边界。CU-CP 内部可以使用经纬度做 NTN mobility，但对 AMF 上报应保持标准 NGAP 语义，避免引入私有扩展破坏互通性。AMF 如果要精确定位，应走标准定位流程；这里实现的是 NGAP `LocationReport`。

## 4. 配置与应用层入口

当前改动还扩展了 O-CU-CP unit 配置链路：

- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config.h`
- `apps/units/o_cu_cp/cu_cp/cu_cp_config_translators.cpp`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_cli11_schema.cpp`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_validator.cpp`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_yaml_writer.cpp`

配置方向包括：

- NTN location mobility 开关。
- 静态 beam table / neighbor cell JSON。
- 服务波位数量、最小仰角、轨道更新周期。
- 接纳控制水位线。
- 核心网位置上报策略。

## 5. 当前测试与验证结果

### 5.1 最新 CU-CP 多波束跳波束增量

当前工作区在既有 CU-CP NTN 原型上继续补齐了控制面跳波束语义：

- `ntn_served_beam_candidate` 增加 `in_hopping_window` 标记，使 scheduler 可以同时输出完整 visible pool 和当前 active window。
- `ntn_served_beam_scheduler` 增加 `served_beam_hopping_enabled` 和 `served_beam_hopping_dwell_updates`，当 visible pool 大于 `max_nof_served_beams` 时，同一卫星状态的周期 tick 会在 dwell 计数满足后轮转 active window。
- `ntn_beam_placement_planner` 只允许当前 hopping window 内 beam 进入 active；window 外 visible beam 保持 candidate，上一轮 active 但本轮出 window 的 beam 进入 draining。
- `ntn_beam_placement_planner` 会跨多次 placement 保留仍有 UE/DRB load 的 draining beam，避免 HO 尚未完成时旧 serving beam 过早退役。
- `ntn_beam_placement_planner` 收紧 hopping window contract：window 外有 UE/DRB load 的 visible beam 不再提升为 active；只有 previous active/draining 且仍有负载的 beam 保留为 draining，并预占原 DU 的 beam/UE/DRB 容量，避免跳窗超配。
- `ntn_served_beam_scheduler` 改为按 visible beam 成员集合判断是否重置 hopping 起点；同一批 beam 的仰角排序变化不会打断轮转，避免 LEO 小步移动时 15 km 大波位池反复回到最高仰角窗口。
- `ntn_served_beam_scheduler` 进一步把 dwell 约束应用到仰角排序导致的窗口变化：同一 visible pool 在 dwell 未满时保持当前 active window beam IDs，只更新 candidate elevation。
- `ntn_location_mobility_controller` 改为按 active served beam 成员集合精准处理 UE candidate；同一批 active beams 只是顺序变化时不会打断 UE location TTT/连续报告累计，成员变化时只清目标 beam 已离开的 candidate。
- 大规模 placement 查询做了多处收敛：active beam 提取使用哈希集合，DU supported-beam 判断使用排序后二分查找，mobility manager 的 handover trigger placement 校验使用 beam_id map，核心网上报 gating 使用 core-reportable NCI 集合。
- `configs/leo_500km_cucp_ntn.yml` 打开 `served_beam_hopping_enabled`，示例 beam table 扩为 5 个 beam，默认 active window 为 3 个 beam，并用 `served_beam_hopping_dwell_updates=3` 保护 UE 位置 TTT/连续报告累计。
- 按 500 km LEO、10 度最小仰角估算，地面足迹半径约 1564 km，若每个波位半径按 15 km 计算，完整 visible pool 粗略可达 1.08 万个波位；这个值是理想面积除法，真实规划考虑重叠/保护带后可能继续上浮。当前 JSON 仅是 5 个 15 km 波位的最小联调切片，单元测试增加了密集 15 km beam pool 的最高仰角窗口选择覆盖。
- 增加 `utils/ntn/generate_leo_beam_table.py` 离线生成器，可按中心点、15 km beam 半径、区域半径或 500 km/10 deg LEO 足迹估算生成静态 beam table JSON，作为 O&M/规划工具雏形。
- app 配置链路已补齐 `served_beam_hopping_enabled` 从 YAML/CLI 到 CU-CP config 的翻译与单测断言。
- `ntn_beams` 状态快照增加 window 标记，并改为默认输出状态汇总；需要明细时使用 `ntn_beams window 64`、`ntn_beams active 64`、`ntn_beams candidate 64`、`ntn_beams draining 16` 等过滤命令，单次明细输出上限 1024 行，避免 15 km 万级波位池把控制台刷屏。
- CU-CP placement 更新日志改为摘要输出，只在 plan 变化时打印状态计数和前 32 个非 inactive 条目；全量细节由 `get_current_ntn_beam_status()` / `ntn_beams` 提供。
- NGAP LocationReport 增加 core-reportable beam 保护：UE location 仍可被 CU-CP 缓存，但本地 forwarding / AMF direct 上报前必须确认 serving NCI 当前属于 active 或 draining beam，candidate/inactive 不上报。
- LEO 500 km 示例配置补齐 5 个 beam 对应的 mobility cells；app 启动配置校验会拒绝 beam table 中没有 cell 配置的 NCI。

这仍然是 CU-CP 控制面的 beam-set hopping，不替代 DU/RU/PHY 的 slot 级 beamforming、precoder 或真实 beam activation。

最近完成的阶段性验证：

| 测试/检查 | 命令要点 | 结果 |
|---|---|---|
| NGAP 位置上报相关测试 | `./ngap_test --gtest_filter='...location...'` | 6 个用例覆盖，待测试构建复跑 |
| cell measurement NTN 位置报告测试 | `./cell_meas_manager_test --gtest_filter='*ntn_location_report*'` | 4 个用例通过 |
| 空白/补丁检查 | `git diff --check` | 通过 |

NGAP 6 个用例覆盖：

- direct `LocationReport` 发送。
- `ntn_derived_tac` 存在时填充 `NRNTNTAIInformation`。
- AMF `LocationReportingControl` 正常转交 CU-CP。
- unknown UE 返回 `unknown-local-UE-NGAP-ID` failure。
- 重复 location report ref id 返回 `multiple-location-reporting-reference-ID-instances` failure。
- CU-CP 拒绝 LocationReportingControl 时，NGAP 返回 LocationReportingFailureIndication。

cell measurement 4 个用例覆盖：

- 稳定的周期 NTN 位置报告触发 handover request。
- 报告间隔过大时重置候选波位。
- 定位精度过低时忽略。
- accepted report 返回明确结果，供核心网上报链路使用。

已知构建注意点：

- Windows + WSL 挂载目录下，部分 CMake 增量对象不会自动按头文件布局变化重建，曾导致测试 fixture 构造时 `bad_alloc` 或 ASN.1 optional 字段异常。处理方式是删除相关 `.o`、静态库或测试二进制后强制重编。
- 当前测试验证主要是单元级和局部集成级，尚未完成真实 AMF / DU / RU 长链路运行验证。

## 6. 我对整体架构的理解

### 6.1 CU-CP 应做的事

CU-CP 更适合做全局控制面判断：

- 管理静态波位表和邻星邻区关系。
- 根据轨道状态计算当前可服务波位。
- 根据 DU capacity、波位负载和历史 assignment 做 DU 分配。
- runtime placement 根据 DU F1 served-cell NCI 限制 `supported_beam_ids`，避免把波位分配给不承载该 cell 的 DU。
- 根据 UE 位置判断服务波位是否仍覆盖 UE，是否进入目标波位，是否需要切换。
- 在需要时发起 handover、批量释放或核心网 LocationReport。

### 6.2 CU-CP 不应直接做的事

CU-CP 不应承担物理层实时工作：

- 不直接做 RU beamforming。
- 不直接决定每个 slot 的 precoder。
- 不直接替代 DU scheduler。
- 不在 NGAP 中塞私有经纬度字段。

### 6.3 DU/RU 后续应承接的事

后续如果把原型推进到系统级，需要 DU/RU 继续补齐：

- 多波束真实时分调度。
- beam id 到 DU/RU beamforming/precoder 的映射。
- SIB19 / NTN assistance 信息与真实空口广播。
- feeder link / service link Doppler 在运行时的持续更新。
- UE 实测位置来源的真实解码和定位精度建模。

## 7. 风险与未完成项

当前仍然是工程原型，不是完整商用 NTN mobility：

- RRC `UEAssistanceInformation` 中可携带的位置相关字段仍需结合实际 ASN.1 编码确认，当前没有从 opaque 字节中强行解码经纬度。
- TLE 传播当前是轻量模型，接口已预留，后续最好替换或对齐完整 SGP4。
- NTN handover 结果处理已有框架，但跨 DU、跨卫星、失败恢复和源侧回滚还需要更系统的场景测试。
- 邻星/Xn 管理目前偏静态配置，后续可以接入动态星间链路状态。
- 接纳控制已有 UE/DRB 水位线，但还可以纳入 PRB、PDU session、CU-UP 负载、DU beam capacity 等指标。
- 核心网 LocationReport 已按标准字段发送，但还缺少真实 AMF 互通验证。
- 文档/PPT 记录了评估结果，但代码侧还需要一次整体 review，把原型中的命名、配置默认值和边界行为进一步产品化。

## 8. 建议的下一阶段

当前工作区拆分已经完成，下一阶段建议按这个顺序收敛：

1. 针对 CU-CP 位置驱动移动性做一次场景表：UE 在服务波位内、进入候选波位、离开所有服务波位、目标波位不可切、目标 DU 满载、handover 失败。
2. 补一层 CU-CP 集成测试，验证“位置报告 -> accepted -> LocationReport / handover trigger”的完整链路。
3. 将轨道驱动的服务波位更新和 DU assignment 接入更明确的运行时调度入口，减少 `cu_cp_impl.cpp` 的职责继续膨胀。
4. 对 RRC 位置输入做协议级确认，再决定是否实现真实 `LocationInfo` 解码。
5. 基于这 5 个功能提交做一次逐提交 review，把每个模块的边界、默认配置和失败恢复策略再收紧一轮。

## 9. 当前阶段一句话总结

这段工作已经把 NTN 从“DU 侧 beam hopping / Doppler 原型”推进到“CU-CP 侧基于位置的多波束移动性控制原型”。核心思想是：静态波位表提供几何地图，轨道计算产生运行时服务波位，CU-CP 根据 UE 位置、邻星/Xn 和 DU capacity 做移动性与资源控制，核心网只接收标准 NGAP 位置报告。

## 10. LEO 500 km CU-CP 专项方案

LEO 500 km、CU-CP-only 的完整分阶段方案已经单独整理到 `docs/ntn_cucp_leo_500km_plan.md`。该文档按 Phase 0 到 Phase 6 固定了目标、任务、交付物、验收门槛、风险、功能归属矩阵、代码能力矩阵、P0/P1/P2 缺口表和测试计划。

同时补充了第一阶段人工联调用的最小配置入口：

- `configs/leo_500km_cucp_ntn.yml`
- `configs/leo_500km_beam_table.json`

## 11. LEO 500 km 多波束/跳波束 CU-CP 收敛

本轮继续把 `max_nof_served_beams` 从单波束基线推进到多波束控制面：LEO 500 km 示例配置使用 3 个 beam 形成 CU-CP beam hopping window，并通过 `served_beam_hopping_dwell_updates=3` 让每个窗口保持多个卫星状态更新周期。satellite state 每次更新都会选择多个 visible candidates，placement planner 将可承载的 window 内 beam 标记为 active，将 DU 不支持、容量不足或 window 外的 visible beam 保持 candidate，将离开可见窗口/窗口外且仍有 UE/DRB load 的旧 active/draining beam 标记为 draining，并用 `max_nof_served_beams` 作为全局 active beam 上限。draining beam 会预占原 DU 的 beam/UE/DRB 容量，避免新窗口分配时超配。location mobility 只消费 active served beam 子集，因此 candidate/draining 不会成为 UE handover target。

同时补充了运行时状态查询接口：`get_current_ntn_served_beam_ids()` 继续返回当前 active subset，`get_current_ntn_beam_status()` 则返回完整 beam placement 快照，包含 active/candidate/draining/inactive 状态、DU、NCI、elevation 和 UE/DRB 水位，用于后续 CLI/northbound 观测和人工联调。命令行侧 `ntn_beams` 默认只输出计数汇总，支持按状态过滤和限制行数，适配 15 km 小波位带来的千级到万级 visible pool。

O-CU-CP 命令行增加了只读 `ntn_beams` 命令，打印 beam、state、DU、NCI、elevation、UE/DRB 水位，便于在 LEO 过境和 DU capacity 变化时观察 hopping window 的收敛过程。

同时增加 `ntn_sat <ecef_x_m> <ecef_y_m> <ecef_z_m>` 和 `ntn_sat_geo <lat_deg> <lon_deg> [alt_m]` 命令，用于手动注入卫星状态；`ntn_sat_geo` 按 WGS84 转 ECEF，默认高度为 500000 m。这样在 CU-CP-only 联调时可以直接执行 `ntn_sat_geo 31.2304 121.4737` 改变 visible candidates，再执行 `ntn_beams` 查看 active/candidate/draining plan。

静态 beam table 的 JSON 解析也收紧了输入校验：非法经纬度、非正覆盖半径、重复 beam id 或重复 NCI 会在加载阶段失败，避免错误波位表进入运行时 hopping window。

另一个收敛点是动态 DU 状态：即使 satellite state 得到的 visible candidates 没有变化，CU-CP 也会重新计算 beam placement。因此 DU 掉线、DU 恢复或容量/负载变化不会被 served-beam scheduler 的重复抑制挡住，active subset 会重新收敛，无法承载的 beam 会回到 candidate。

active served beam set 变化现在也会清理 UE 侧 pending NTN candidate，保证连续位置报告和 TTT 累计只在同一个 hopping window 内有效。目标 beam 离开窗口再回来时，UE 必须重新积累位置稳定性后才会触发 handover。

注意：这里的“跳波束”仍是 CU-CP 控制面的 beam-set hopping，不包含 PHY slot 级 beam hopping、RU beamforming 或 DU beam activation。
