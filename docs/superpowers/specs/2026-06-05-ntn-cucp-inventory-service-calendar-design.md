# NTN CU-CP 候选清单与服务日历设计

## 摘要

本阶段把 `CUCP-006` 和 `CUCP-007` 作为一个连续开发阶段推进。
`CUCP-006` 先把 CU-CP 侧的 NTN 候选波位清单做完整，确保它不再被
loaded service 资源上限裁剪。`CUCP-007` 再基于这份候选清单生成
按真实用户需求驱动的 loaded service calendar。

本阶段仍严格限定在 CU-CP 控制面。它不实现 DU、MAC、PHY、PRACH、
HARQ、TA、RU、RF 或调度器行为。

## 目标

- 保留所有满足准入条件的 NTN 波位到 `candidate_inventory`。
- 停止用 loaded-service 资源上限裁剪候选覆盖范围。
- 将 `mobility_eligible_beams` 与真正消耗服务资源的波位区分开。
- 只为存在 UE、DRB、重建或切换需求的波位生成 `loaded_service_calendar`。
- 保持旧字段 `max_nof_served_beams` 的兼容性，同时把实现语义逐步迁移到
  `max_nof_loaded_service_beams`。
- F1AP-CU 侧 SR/SRS slot request 仍然只是 CU-CP 发出的控制面意图，
  不是 DU/MAC 调度实现。

## 非目标

- 不实现 DU 或 MAC scheduler 资源分配。
- 不实现 PHY、PRACH、HARQ、TA scheduler、RU、RF、ZMQ 或 GIS 行为。
- 本阶段不实现 SIB19/RRC assistance。
- 本阶段不扩展 NGAP Mapped Cell 或 derived TAC。
- 除了维持新 inventory/calendar 合约所必需的最小逻辑外，不重写 admission 策略。

## 当前状态

`CUCP-005` 已经建立了 runtime taxonomy：

- `candidate`
- `active_loaded`
- `draining`
- `inactive`

当前 beam placement planner 已经能把空的可见波位保留为 `candidate`，
并且只把有负载的波位提升为 `active_loaded`。但是 candidate selector
仍然保留着以 `served` beam 为中心的旧命名和旧 API，部分路径仍可能按
`max_nof_served_beams` 对候选向量做 resize。

所以下一阶段应先消除这层语义混淆，再强化 F1AP slot request 合约。

## 架构

### Candidate Inventory

`candidate_inventory` 是 CU-CP 侧完整的 NTN 候选波位列表。一个波位进入该列表，
需要通过静态和运行时检查：

- beam 已启用；
- 卫星状态对新接入仍然新鲜有效；
- 仰角达到 admission 阈值，默认 50 度；
- 如果配置了服务窗口，beam 位于服务窗口内；
- 如果存在 PLMN/TAC/slice 策略，beam 与策略兼容；
- 至少有一个 CU-CP 可见 DU capability 支持该 beam。

候选清单的顺序必须确定。主排序为仰角降序，beam id 作为 tie-breaker。

loaded-beam limit 不得裁剪该列表。loaded-beam limit 为 0 表示 CU-CP
不限制 loaded service beam 数量，而不是生成空的 candidate inventory。

### Hopping Window

hopping window 是 candidate inventory 的 CU-CP 控制面子集。它可以限制某个
控制面周期内哪些 candidate 立即具备移动性或服务焦点，但它不是 candidate
inventory 本身。

本阶段要明确区分这些概念：

- `candidate_inventory`：完整可准入覆盖范围。
- `hopping_window`：当前 CU-CP 服务/移动性关注窗口。
- `active_loaded`：当前服务窗口内有真实负载的波位。
- `draining`：正在退出服务、但还承载已有 UE/DRB 合约的旧 loaded 波位。

### Loaded Service Calendar

loaded service calendar 在 candidate inventory 和 beam load 都已知后生成。
它包含：

- 有 UE、DRB、重建或切换需求的 `active_loaded` 波位；
- 仍承载已有 UE/DRB 控制面合约的 `draining` 波位。

它排除空的 candidate。空 candidate 不得获得：

- antenna slot index；
- 非零 antenna slot period；
- SR slot offset 或 period；
- SRS slot offset 或 period；
- F1AP-CU slot request set/update payload。

### F1AP-CU Contract

如果 `CUCP-007` 需要修改 F1AP-CU，只能使用任务卡中批准的 CU-side exact path。
payload 仍然表示 CU-CP 发给 DU 的意图。DU 可以根据自己的实现忽略、转换或拒绝
该意图；本阶段不实现 DU 行为。

CU-CP 应避免重复发送未变化的 slot request。当 beam 离开 `active_loaded` 且不再属于
loaded service calendar 时，CU-CP 应发送 clear/update。

## 数据流

1. CU-CP 接收卫星状态更新，或由 CU-CP updater 计算出新的卫星状态。
2. CU-CP 基于 beam table 和卫星状态计算完整 candidate inventory。
3. CU-CP 基于 candidate inventory 计算或推进 hopping window。
4. CU-CP 汇总每个 beam 当前的 UE、DRB、重建和切换需求。
5. Beam placement 根据 candidate、loaded beam 和 CU-CP 可见 DU capability
   生成 DU placement。
6. 空的、受支持的 beam 保持为 `candidate`。
7. 有负载的 beam 变为 `active_loaded`；正在退出服务但仍有需求的 beam 保持
   `draining`。
8. loaded service calendar 只为 `active_loaded` 和仍有负载的 `draining`
   beam 生成 slot request intent。
9. 只有 slot request 合约发生变化时，CU-CP 才发送 F1AP-CU setup/modification
   更新。

## 接口与命名

实现时优先使用与 runtime contract 一致的新命名：

- `ntn_candidate_inventory`
- `ntn_candidate_beam`
- `ntn_hopping_window`
- `ntn_loaded_service_calendar`
- `max_nof_loaded_service_beams`

如果公共接口或配置 schema 仍需要旧的 `served` 命名，可以作为兼容别名保留，
特别是当重命名会越过当前任务允许路径时。所有保留的兼容别名都应在注释和测试中说明。

## 错误处理

条件允许时，candidate exclusion 应携带原因。初始原因至少覆盖：

- beam disabled；
- 仰角低于 admission 阈值；
- 卫星状态 stale；
- 不在 service window；
- 没有 DU capability 支持；
- PLMN/TAC/slice policy mismatch；
- beam 数据未知或无效。

卫星状态 stale 时，CU-CP 应阻止新的 candidate admission；对于已有 loaded beam，
在现有 CU-CP 状态允许的情况下进入 draining。

## 测试策略

### CUCP-006 测试

- 即使 loaded-service capacity 很小，1000+ 个可见 beam 仍保留在 candidate inventory。
- 新 candidate admission 使用 50 度仰角阈值。
- release/elevation hysteresis 与 admission 阈值分离。
- disabled、stale、unsupported、out-of-service-window 的 beam 被排除或标记为 inactive，
  并带有原因。
- candidate inventory 排序确定。

### CUCP-007 测试

- 空 candidate 没有 antenna slot、SR 或 SRS request。
- UE/DRB demand 会把 candidate 提升为 `active_loaded`。
- handover preparation demand 可以把 candidate target 加入 loaded service calendar。
- demand 移除后会清除 slot request 合约。
- 未变化的 slot request 不重复发送。
- calendar 排序确定；在已有输入支持时，按 demand priority、UE/DRB load、elevation
  和 age 排序。

### 验证

通过 harness 运行任务验证：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-006 -TaskFile ai_harness/tasks/CUCP-006-candidate-inventory.md -CTestRegex "cu_cp_ntn_mobility_test|ntn_mobility_test"
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-007 -TaskFile ai_harness/tasks/CUCP-007-demand-driven-service-calendar.md -CTestRegex "cu_cp_ntn_mobility_test|f1ap_cu"
```

同时运行 AGENTS 要求的验证：

```bash
bash ai_harness/scripts/configure_build.sh
bash ai_harness/scripts/build.sh
bash ai_harness/scripts/run_cucp_tests.sh
```

## 执行顺序

1. 为 `CUCP-006` 编写红灯测试：完整 candidate inventory，不被 loaded limit 裁剪。
2. 实现 `CUCP-006`：调整 selector、scheduler、placement 的命名与状态语义。
3. 运行 `CUCP-006` 验证。
4. 为 `CUCP-007` 编写红灯测试：loaded service calendar 与 F1AP-CU slot request 行为。
5. 实现 `CUCP-007`：调整 placement/calendar 以及 CU-CP/F1AP-CU 合约。
6. 运行 `CUCP-007` 验证。
7. 输出合并报告，列出行为变化、测试结果、风险和使用到的 task exception。

## 风险

- 仓库中仍有旧的 `served` 命名。一次性重命名所有公共命名很可能越过 app/config 路径，
  所以本阶段优先拆清语义，公共 schema 重命名留给后续获批任务。
- F1AP-CU slot request 测试可能需要使用 `CUCP-007` 中列出的 exact non-CU-CP exception path。
  实现不得扩大到 F1AP-DU。
- calendar 排序必须确定，但不要提前过度绑定未来 QoS 策略。QoS/ARP 排序钩子应保持最小，
  等 `CUCP-009` 或后续任务再扩展。
- 当前工作区已有 `CUCP-004` 和 `CUCP-005` 的未提交变更。本阶段不得回退或扩大这些无关变更。

## 完成标准

- candidate inventory 完整，且不被 loaded-service limit 裁剪。
- loaded service calendar 由真实需求驱动。
- 空 candidate 不消耗 SR/SRS/antenna slot intent。
- 每张任务卡对应的 CU-CP path guard 通过。
- rejected/quarantined overlap check 通过。
- focused validation 和必需的 CU-CP validation 通过；如果失败，必须附日志并说明边界。
