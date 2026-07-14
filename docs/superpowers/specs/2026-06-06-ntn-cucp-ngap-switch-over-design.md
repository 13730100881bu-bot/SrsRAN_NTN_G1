# NTN CU-CP NGAP 收口与切换韧性设计

## 摘要

本阶段承接 `CUCP-008` 和 `CUCP-009`，把已建立的 NTN assistance、admission、mobility、draining 和 loaded service calendar 能力继续向两个方向推进：

- `CUCP-010`：收口 NGAP/core mapping，确保核心网看到的 UE 位置、TAI、derived TAC、LocationReportingControl 和 area-of-interest 语义完整一致。
- `CUCP-011`：实现 CU-CP 侧 NTN service switch-over/resilience，支持 soft/hard switch-over、manual override、source priority 和安全降级。

本阶段仍严格限制在 CU-CP 控制面。它不实现 DU、MAC scheduler、PHY、PRACH、HARQ、TA scheduler、RU、RF、ZMQ、GIS 或物理 feeder gateway 行为。

## 目标

- 将现有 NGAP location reporting 行为做一次系统性收口，避免“测试已有但语义分散”。
- 让 CU-CP 能用 NTN runtime state 生成稳定的核心网位置视图，包括 NR CGI、TAI、derived TAC、timestamp 和 Mapped Cell 语义。
- 增加 service switch-over 事件模型，使 CU-CP 能区分 soft preparation 和 hard outage。
- 在 hard switch-over、stale ephemeris、manual override 等场景下，确定性地阻止新接入、重建目标、handover target 和 PDU session demand。
- 保留已有 UE/DRB 的 draining 控制面合约，直到 handover、release 或 demand cleanup 完成。
- 为后续 `CUCP-012` observability/examples 留出稳定的运行时状态和原因码。

## 非目标

- 不实现 SIB19 广播调度；CU-CP 只持有 assistance/contract 输入。
- 不修改 DU-owned SI scheduling、MAC scheduler、PHY、PRACH、HARQ timing、TA scheduler。
- 不实现 RF gateway 或 feeder link 的物理切换。
- 不引入新的 `cu_cp.ntn` 配置根；继续使用 `mobility_config.ntn_location_mobility`。
- 不把 observability/config example 作为本阶段主体；它们应在 switch-over 语义稳定后进入 `CUCP-012`。

## 当前基线

当前代码已经具备这些能力：

- `candidate_inventory` 不再由 loaded-service limit 裁剪。
- `active_loaded`、`candidate`、`draining`、`inactive` 已用于 beam placement。
- loaded service calendar 只由 UE/DRB/handover demand 驱动。
- CU-CP 可以生成 `ntn_assistance_snapshot`，并暴露给 command handler。
- stale assistance 会阻止新 RRC setup、reestablishment、PDU session setup 和 location handover demand。
- stale assistance 可把已有 loaded beam 转入 draining。
- UE removal 完成后会刷新 beam placement，避免 rejected/released UE 残留在 beam load accounting。
- NGAP location reporting 已经有 direct、change-of-serving-cell、draining baseline 等测试覆盖，但还需要按 `CUCP-010` 做语义收口和缺口审计。

## 阶段拆分

### CUCP-010 NGAP Core Mapping Closeout

`CUCP-010` 的目标是“收口”，不是重写。实现前先审计当前 NGAP/CU-CP location reporting 已有行为，保留通过测试证明有效的部分，只补齐缺口。

#### 行为要求

1. UE location report 必须能映射为核心网位置：
   - NR CGI；
   - TAI；
   - optional derived TAC；
   - timestamp；
   - Mapped Cell 语义，如果当前 ASN.1/NGAP contract 已具备承载点。
2. `LocationReportingControl` 必须确定性处理：
   - direct；
   - change-of-serving-cell；
   - area-of-interest；
   - stop/cancel；
   - duplicate or overlapping area-of-interest ref-id。
3. draining beam 上已有 UE 的 location reporting 应继续可用，因为 draining 代表“保留已有控制面合约”，不是新 admission。
4. stale assistance 下不得创建新的 mobility/location demand；但已有 last accepted location 可以用于 release/drain 诊断和 AMF direct request 的受控响应。
5. throttle、accuracy gate、AMF-control-enabled 和 local-forwarding-disabled 默认值必须被测试覆盖。

#### 主要接口

- `include/srsran/cu_cp/cu_cp_types.h`
  - 检查 `cu_cp_user_location_info_nr` 是否足够表达 derived TAC、TAI、timestamp 和 Mapped Cell。
- `lib/cu_cp/cu_cp_impl.cpp`
  - 收口 `build_ntn_core_user_location_info`、`handle_location_reporting_control` 和 `report_ntn_location_to_core_if_required`。
- NGAP exception paths
  - 仅在必须补齐 ASN.1 converter 或 NGAP public contract 时使用任务卡中列出的 exact paths。

#### 测试要求

- NGAP converter focused tests：
  - derived TAC present/absent；
  - TAI 和 timestamp；
  - Mapped Cell，如果 contract 存在。
- CU-CP NTN mobility tests：
  - direct reporting；
  - change-of-serving-cell；
  - area-of-interest accept/reject；
  - stop/cancel；
  - draining beam 可继续上报已有 UE；
  - stale assistance 不产生新 demand。

### CUCP-011 Switch-over and Resilience

`CUCP-011` 是本阶段主体。它把“卫星/服务状态变化”从简单 stale gate 扩展为可表达的 switch-over runtime event。

#### 新运行时概念

建议新增 CU-CP 侧类型：

- `ntn_service_switch_over_event`
  - event id；
  - event type：`soft`、`hard`；
  - affected beam ids 或 NCIs；
  - optional service area id；
  - start time；
  - optional expected end time；
  - source：manual、tle、circular fallback、operator；
  - reason；
  - priority；
  - policy：prepare、drain、handover-preferred、release-allowed。

- `ntn_service_source_priority`
  - manual override；
  - fresh TLE；
  - circular fallback；
  - stale/no source。

- `ntn_manual_override_state`
  - none；
  - freeze current state；
  - replace satellite/service state；
  - restore automatic source。

#### Soft switch-over

Soft switch-over 表示服务即将切换，但当前链路仍可保持已有 UE：

- 不立即释放 UE。
- affected loaded beam 进入 preparation/draining-ready 状态，或在现有 taxonomy 内以 reason 标注为 draining candidate。
- 不接受新的低优先级 demand。
- 如果存在 candidate target，允许或触发 handover preparation demand。
- loaded service calendar 仍可保留已有 UE/DRB 的 SR/SRS slot request intent。

#### Hard switch-over

Hard switch-over 表示服务不可继续承载新 demand：

- affected beam 不接受新 RRC setup。
- affected beam 不作为 reestablishment target。
- affected beam 不作为新的 handover target。
- 新 PDU session demand 被拒绝或按策略降级。
- 已有 loaded beam 进入 draining；如果策略允许 release，则触发 UE release。
- pending handover demand、candidate state 和 slot request intent 必须确定性清理。

#### Manual override

Manual override 是最高优先级控制源：

- freeze：冻结当前 satellite/service runtime state，停止自动 updater 覆盖。
- replace：用 operator 提供的 satellite/service state 替换当前状态。
- restore：恢复自动源优先级。

优先级建议为：

1. manual override；
2. fresh TLE；
3. circular fallback；
4. stale/no source。

#### 与现有 stale assistance 的关系

stale assistance 是 hard degradation 的一种输入，但不等于所有 switch-over：

- stale assistance：来源是时间新鲜度失败。
- hard switch-over：来源可以是 operator、service window、feeder outage、satellite source change。
- soft switch-over：来源是提前通知，目标是让 CU-CP 先准备 HO/drain，而不是立刻拒绝全部已有服务。

实现时应尽量复用现有 `block_new_ntn_demand_if_assistance_is_stale` 这一类 gate，但不把所有 switch-over 都塞进 stale 命名里。

## 数据流

1. CU-CP 接收 satellite update、manual override 或 switch-over event。
2. CU-CP 计算当前 source priority 和 service validity。
3. CU-CP 刷新 candidate inventory、mobility eligible beams 和 beam placement。
4. 如果 event 是 soft：
   - 标记 affected beams；
   - 保留已有 loaded service calendar；
   - 可生成 handover preparation demand。
5. 如果 event 是 hard：
   - affected beams 拒绝新 demand；
   - loaded beams 转 draining 或 release；
   - 清理 pending demand；
   - 刷新 SR/SRS slot request intent。
6. CU-CP 根据最新 UE location/reporting state 向 NGAP 输出受控 location report。
7. 后续 `CUCP-012` 读取稳定后的 runtime state 暴露 `ntn_state`、`ntn_assistance`、`ntn_beams`、`ntn_ues`。

## 错误处理

- 未知 beam id：记录 warning，event 对该 beam 无效，但不影响其他 beam。
- 无 satellite state：按 hard degradation 处理新 demand，已有 UE draining。
- stale TLE：降低 source priority；如果没有 manual/fallback，则进入 stale policy。
- override replace 输入无效：拒绝 override，不覆盖当前状态。
- switch-over 与 handover 并发：handover routine 失败时必须报告 NTN result，并清理 pending demand。
- duplicate area-of-interest ref-id：拒绝新的 overlapping control request。

## 测试策略

### CUCP-010

- Converter test：derived TAC、TAI、timestamp、Mapped Cell。
- CU-CP test：AMF direct request 返回 last accepted NTN location。
- CU-CP test：draining beam 上已有 UE 仍可被 report。
- CU-CP test：area-of-interest ref-id 重叠被拒绝。
- CU-CP test：stale assistance 不产生新的 location handover demand。

### CUCP-011

- Soft switch-over：
  - existing UE 不释放；
  - affected beam 不接收新低优先级 access；
  - candidate target 可形成 handover preparation demand。
- Hard switch-over：
  - 新 access、reestablishment target、handover target 被拒绝；
  - affected loaded beam 进入 draining；
  - slot request intent 被更新或清理。
- Manual override：
  - freeze 阻止自动 updater 覆盖；
  - replace 刷新 satellite/service state；
  - restore 回到 TLE/circular source priority。
- Cleanup：
  - handover success/failure；
  - UE release；
  - reestablishment fallback；
  - event clear 后 beam load 不残留。

## 验证

优先使用 focused tests：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-010 -TaskFile ai_harness/tasks/CUCP-010-ngap-core-mapping.md -CTestRegex "cu_cp_ntn_mobility_test|ngap"
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-011 -TaskFile ai_harness/tasks/CUCP-011-switch-over-resilience.md -CTestRegex "cu_cp_ntn_mobility_test|ntn_mobility_test"
```

并按 AGENTS 要求运行：

```bash
bash ai_harness/scripts/configure_build.sh
bash ai_harness/scripts/build.sh
bash ai_harness/scripts/run_cucp_tests.sh
python ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-010-ngap-core-mapping.md
python ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-011-switch-over-resilience.md
python ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base
python ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-010-ngap-core-mapping.md ai_harness/tasks/CUCP-011-switch-over-resilience.md
```

如果 repo-local WSL build 因 `/mnt/d` 性能或 `_NOT_BUILT` 占位测试失败，应保留日志并说明实际通过的 focused build/test 范围。

## 风险

- `CUCP-010` 可能已经部分实现。风险不是缺代码，而是重复修改已稳定行为；因此第一步必须做测试和代码审计。
- NGAP exception paths 必须 exact，不允许扩大到 broad prefix。
- switch-over 容易被误解为物理 feeder/gateway 切换；实现必须保持 CU-CP 控制面事件模型。
- manual override 如果设计过宽，会变成新的配置系统。本阶段只做 runtime override，不引入持久化 OAM schema。
- soft switch-over 的状态命名需要谨慎，避免破坏现有 `candidate/active_loaded/draining/inactive` taxonomy。

## 完成标准

- `CUCP-010` 明确哪些 NGAP mapping 行为已存在、哪些被补齐，并有测试证明。
- `CUCP-011` 支持 soft/hard switch-over、manual override、source priority 和 cleanup。
- 新 demand gate 与已有 stale assistance gate 语义清楚，不混用命名。
- 已有 UE/DRB draining 合约不被破坏。
- Path guard 和 rejected overlap check 通过。
- focused tests 和必要 CU-CP validation 有明确结果。
