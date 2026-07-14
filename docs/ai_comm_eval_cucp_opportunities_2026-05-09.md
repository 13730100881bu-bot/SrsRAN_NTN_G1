# CU-CP方向分析：AI辅助编程可以继续做什么

记录时间：2026-05-09 22:12 CST
代码范围：`include/srsran/cu_cp`、`lib/cu_cp`、`tests/unittests/cu_cp`

## 1. 总体优化结论

当前NTN样本已经完成从“功能实现”到“测试验证”和“PPT呈现”的闭环，下一步不宜只继续堆NTN算法点，而应把样本扩展到CU-CP控制面。原因是CU-CP同时连接F1AP、E1AP、NGAP、RRC、UE上下文、测量与移动性，能更好测出AI在跨模块通信软件中的真实价值。

本轮总体优化后的测评主线建议调整为：

- NTN/O-DU样本：证明AI能做跨模块功能实现、边界修复和验证闭环。
- CU-CP样本：验证AI能否理解控制面状态机、UE上下文、测量触发和移动性流程。
- 测评指标：继续使用代码差异、测试通过率、风险闭环数、构建/运行耗时，不写没有人工对照组支撑的效率提升百分比。

## 2. CU-CP现状摘录

- `lib/cu_cp`主库聚合了`cell_meas_manager`、`mobility_manager`、`du_processor`、`ue_manager`、`paging`、`up_resource_manager`等模块，相关源码与单测约218个文件。
- CU-CP配置中已经有`mobility_configuration`，包含`cell_meas_manager_cfg`和`mobility_manager_cfg`，适合作为新功能配置入口。
- `cell_meas_manager`负责生成RRC测量配置、接收MeasurementReport、找最强邻区，并通过`on_neighbor_better_than_spcell()`通知移动性管理器。
- `mobility_manager`可以由测量触发handover，并在本CU内/跨CU之间选择不同流程。
- `du_configuration_manager`保存DU上报的小区、MIB/SIB1、PCI、TAC、band等信息，但`validate_du_config_update()`仍是TODO，适合做低风险的配置校验增强。

## 3. 可做方向

| 方向 | 切入模块 | 价值 | 风险 | 建议优先级 |
|---|---|---|---|---|
| CU-CP测量配置增强 | `cell_meas_manager` | 把测量对象、report config、UE capability过滤做成更可控策略 | 中 | P0 |
| NTN/多波束感知移动性 | `cell_meas_manager` + `mobility_manager` | 对MeasurementReport触发handover增加beam dwell、SIB19有效期、TA风险等约束 | 中高 | P1 |
| DU小区配置校验补强 | `du_configuration_manager` | 补齐DU config update校验，减少F1配置状态错误 | 低 | P0 |
| CU-CP指标与测评埋点 | `metrics_handler` + `mobility_manager` | 统计测量触发、handover尝试/成功/失败和reconfiguration_disabled原因 | 中 | P1 |
| CU-CP系统级场景测试 | `tests/unittests/cu_cp/cu_cp_*` | 用现有mock AMF/DU/CU-UP框架扩展端到端控制面路径 | 中 | P1 |
| 真实NTN控制面联动 | CU-CP + O-DU NTN接口 | 将O-DU多波束结果传递到CU-CP策略层 | 高 | P2 |

## 4. 建议先做的两个任务

### 任务A：补齐DU配置更新校验

目标：实现`du_configuration_manager::validate_du_config_update()`，覆盖重复CGI、超出最大cell数、删除不存在cell、添加cell的PLMN/NCI合法性等情况。

为什么适合先做：

- 不碰协议过程状态机，风险低。
- 单测入口已经存在：`tests/unittests/cu_cp/du_processor/du_configuration_manager_test.cpp`。
- 能形成清晰的AI辅助闭环：读现有校验逻辑、补实现、补负向测试、跑单测。

### 任务B：CU-CP测量触发防抖/门限策略

目标：在`cell_meas_manager`或`mobility_manager`增加一个小范围、可配置的handover触发保护，例如最小RSRP差、连续N次报告、或measurement-trigger cooldown。

为什么适合第二步：

- 当前`find_strongest_neighbor()`只根据单次报告选择最强邻区，真实移动性容易抖动。
- 这能自然引出NTN/多波束场景：beam dwell边界、SIB19有效期和TA变化都可以作为后续门控条件。
- 单测可在`tests/unittests/cu_cp/cell_meas_manager`和`tests/unittests/cu_cp/mobility`分层补齐。

## 5. NTN与CU-CP的结合方式

推荐不要让CU-CP直接承担波束几何计算，而是接收O-DU侧已经抽象好的状态：

- cell级能力：该cell是否启用NTN、是否启用beam hopping、多波束dwell大小。
- 时间有效性：当前SIB19/beam set的`valid_from`、dwell周期、UL sync validity。
- 移动性提示：是否接近dwell边界、TA变化是否超过阈值、是否建议延迟handover。

这样CU-CP只做控制面策略，不和O-DU/RU的实时波束控制耦合。

## 6. PPT更新口径

PPT中应把CU-CP定位为“下一阶段测评方向”，不是已经完成的功能结果。建议用两页表达：

- 一页讲CU-CP可做的四类能力：DU配置校验、测量配置增强、移动性策略、指标与系统测试。
- 一页讲路线图：P0先做低风险配置校验，P1做测量/移动性策略，P2再接NTN状态与真实链路。
