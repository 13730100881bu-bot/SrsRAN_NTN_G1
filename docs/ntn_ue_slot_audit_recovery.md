# NTN 终端上行资源核对与断线恢复

更新时间：2026-08-09

任务编号：`CUCP-050`

实现结论：CUCP-050 已完成资源代次、DU 实际应用清单、断线重连核对、
有界修复和只读状态。专项构建与测试结果见第 10 节。

## 1. 解决的问题

终端接入后，CU-CP 会为其安排 SR/SRS 上行资源，DU 则负责把这些安排落实
到终端的实际配置中。正常响应可以告诉 CU-CP 单次设置是否成功，但在响应
丢失、DU 断线或 CU-CP 与 DU 状态不一致时，仅依靠单次响应无法确定 DU
当前真正使用的配置。

CUCP-050 建立一套完整清单核对流程：CU-CP 向当前连接的 DU 查询每个终端
实际生效的 SR/SRS 配置，用当前 F1 连接中的终端身份进行逐项匹配，再对
确定安全的差异做一次保守修复。该流程不会因为清单差异直接释放终端。

## 2. 与 C-RNTI 回收的关系

CUCP-049 已完成 C-RNTI 的安全回收和重新使用。只有在 CU-CP 确认终端已经
离开、当前 DU 完整清单报告同一代号码已经过期，并且 MAC 再次原子检查
通过后，号码才会进入可复用状态。DU 重连后必须先重新核对，旧响应和旧代次
不能删除已经重新使用的号码。

CUCP-050 处理另一类资源：终端用于上行控制的 SR/SRS 配置。C-RNTI 清单和
SR/SRS 清单在同一次私有核对流程中传递，但两者独立判定。SR/SRS 清单不完整
不会中断已经满足条件的 C-RNTI 回收。

## 3. 核心数据

### 3.1 资源安排代次

每次新版 SR/SRS 操作携带非零 `assignment_generation` 和明确操作：

- `set`：设置或更新终端的 SR/SRS 资源；
- `clear`：清除该终端的活动 SR/SRS 资源。

同一内容的重试沿用原代次。资源内容变化、清除后重新分配时使用更大的
代次。同一代次却携带不同内容会返回
`assignment_generation_conflict`；更小代次会返回
`stale_assignment_generation`。代次达到 `UINT32_MAX` 后停止为该终端创建
新安排，并返回 `slot_assignment_generation_exhausted`。

### 3.2 DU 实际应用清单

DU 先暂存资源更新，只有资源分配和 MAC/scheduler 配置事务都成功后，才发布
新的活动项。MAC/scheduler 配置失败时保留原活动项，并将该小区清单标记为
不完整。DU 内部活动项包含：

- DU 内部终端标识和小区；
- `assignment_generation`；
- DU 实际应用的 SR/SRS 周期和偏移。

生成 F1 核对响应时，DU 再从当前终端上下文取得 C-RNTI，并从当前 F1 连接
取得两侧 F1 UE ID。这样，清单不会长期保存可能随终端上下文变化的连接身份。

清除成功或终端删除后，活动项从清单中移除；代次高水位保留到相应终端
上下文销毁。分配失败或回滚时，原有活动项保持不变。若内部状态无法形成
完整清单，DU 返回 `ue_slot_snapshot_complete=false`，不会把部分清单当作
完整结果。

### 3.3 当前连接中的终端身份

跨 F1 传递的完整清单不使用 CU-CP 内部 `ue_index_t`。每项使用当前 F1
连接中的：

- `gnb_cu_ue_f1ap_id`；
- `gnb_du_ue_f1ap_id`；
- NCGI、PCI 和 C-RNTI；
- `assignment_generation`；
- DU 实际应用的 SR/SRS 参数。

CU-CP 只有在两个 F1 UE ID 同时精确解析到当前连接的同一终端，并且小区和
C-RNTI 一致时才使用该项。无法解析的项进入隔离区，不按序号或 C-RNTI
猜测归属。

## 4. 核对流程

DU 每次连接后经历以下状态：

```text
awaiting_capability
  -> awaiting_complete_snapshot
  -> reconciled
```

1. CU-CP 先询问当前 DU 是否支持带 F1 终端身份的完整 SR/SRS 清单。
2. 新版请求绑定 gNB-DU ID、NCGI、DU cell、PCI、非零
   `connection_token` 和本次 audit generation。
3. DU 回送相同目标身份和 token，并返回最多 1,024 条活动项。
4. CU-CP 先校验响应属于当前连接，再把 F1 UE ID 解析为本地终端。
5. 清单完整且全部一致时，该 DU 进入 `reconciled`。
6. 修复过差异后再次查询完整清单；第二次全部一致后才恢复
   `reconciled`。

旧 DU 无法识别新版请求时，CU-CP 退回原核对格式。旧版 SR/SRS applied
feedback 和 C-RNTI 核对仍可继续，SR/SRS 完整清单自动修复保持关闭。

## 5. 差异处理规则

| CU-CP 期望状态 | DU 清单状态 | 处理 |
|---|---|---|
| 内容、身份和代次完全一致 | 存在 | 标记 `matched`，不发送更新 |
| 有期望安排 | 缺失 | 使用原 `assignment_generation` 重发一次 |
| 已无期望安排 | 能解析到当前终端的活动项 | 发送 `generation + 1` 的 `clear` |
| 同代次但内容不同 | 存在 | 标记 `conflict`，不自动覆盖 |
| DU 代次高于 CU-CP | 存在 | 标记 `conflict`，不自动覆盖 |
| 无法解析到当前 F1 终端 | 存在 | 标记 `quarantined`，不发送清除 |
| 小区、连接 token 或目标身份不符 | 任意 | 丢弃响应，等待当前连接的新清单 |

同一终端、同一操作、同一 `assignment_generation` 和同一资源内容构成的精确
差异只自动修复一次。再次核对仍不一致时，保留冲突状态并停止自动改写，
方便运维定位，而不会直接释放终端。资源内容或代次变化后按新的差异处理。

## 6. 断线与重启

- DU 断开后，当前连接的 capability、token、在途响应和 `reconciled`
  状态立即失效。
- CU-CP 在本进程中保留期望的 SR/SRS 安排，但隐藏旧连接的 DU applied
  状态。
- DU 重连后必须取得当前连接的完整清单；旧连接的延迟响应不能改变新连接
  状态。
- CU-CP 重启后不从 DU 清单重建本地 UE 身份。不能解析到当前 F1 UE
  context 的遗留项进入隔离区。
- 本任务不增加状态文件，恢复信息来自重连后的 DU 查询。

## 7. 私有接口与兼容性

本任务更新现有私有 F1 resource-coordination container，并保持标准 F1AP
消息和 generated ASN.1 不变：

- SR/SRS request/result 增加 `set/clear` 和 assignment generation；
- audit request/result 增加连接目标、能力标志、完整 UE 清单和代次高水位；
- 新 CU 对旧 DU 自动降级，不凭空假定旧 DU 支持完整清单；
- 新 DU 收到旧格式请求时继续返回旧格式结果；
- NTN 默认关闭时，不发起这项能力探测，也不建立 SR/SRS 清单；
- terrestrial 默认分配流程保持原有行为。

本轮授权范围包括 CU-CP、私有 F1 容器、F1AP CU/DU、DU manager 和 DU RAN
resource manager。MAC scheduler、PRACH detector、PHY、RU/RF、Web/GIS 和
generated ASN.1 保持不变。

## 8. 只读状态

现有 `ntn_state` 增加以下信息，不新增命令：

- `ue_slot_audit_stage`；
- 每个 DU 是否支持完整 UE 清单；
- 当前清单是否完整；
- `matched`、`missing`、`conflict`、`quarantined`、`repaired` 数量；
- assignment generation 高水位；
- 最近一次失败原因。

`matched`、`missing`、`conflict` 和 `quarantined` 是各目标最近一次完整清单的
结果；`repaired` 是当前 DU 连接和当前目标集合内，经 DU 确认成功的修复累计
数。DU 重连或目标集合变化后重新统计。总体 assignment generation 高水位取
各 DU 当前值中的最大值。

这些字段描述 CU-CP 与 DU 软件配置的一致性。无线发射、接收和硬件波束状态
由设备执行接口负责。

## 9. 代码位置

| 功能 | 主要路径 |
|---|---|
| SR/SRS 私有请求与结果 | `include/srsran/f1ap/ntn_ul_slot_resource_request.h` |
| 完整核对清单编码 | `include/srsran/f1ap/ntn_rnti_lease_pool.h` |
| 当前 F1 UE 身份解析 | `include/srsran/f1ap/cu_cp/f1ap_cu.h`、`lib/f1ap/cu_cp/f1ap_cu_impl.*` |
| CU-CP 期望状态、比较与修复 | `lib/cu_cp/ntn_mobility/ntn_beam_service_resource_manager.*` |
| 断线重连和查询编排 | `lib/cu_cp/cu_cp_impl.*` |
| DU 实际应用清单 | `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager*` |
| DU 清单组装 | `lib/du/du_high/du_manager/du_manager_impl.cpp` |
| 只读状态 | `include/srsran/cu_cp/cu_cp_command_handler.h`、`apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h` |

## 10. 验证记录

专项检查覆盖的关键场景包括：

- `set`、`clear`、同内容重试、旧代次、同代次不同内容和代次耗尽；
- DU 应用成功、应用失败回滚、清理成功和终端删除；
- 0、1,024 和 1,025 条边界，以及重复身份、错误小区、错误 token、
  截断或尾随数据；
- 完全匹配、期望项缺失、可解析的多余项和无法解析的隔离项；
- applied feedback 丢失后用同代次完整清单恢复；
- DU 断线使旧响应失效，重连后取得完整清单并再次核对；
- 新 CU/旧 DU 和旧 CU/新 DU 的降级兼容；
- SR/SRS 清单失败时 C-RNTI 回收继续独立处理；
- NTN 默认关闭时不探测、不建立清单，terrestrial 行为保持原有路径。

| 验证项 | 命令或过滤器 | 结果 |
|---|---|---|
| CU-CP 资源管理 | `ntn_mobility_test` 中 `ntn_beam_service_resource_manager.*` | 69/69 通过 |
| 私有 F1 CU 编解码与连接身份 | `f1ap_cu_test` 专项过滤器 | 14/14 通过 |
| 私有 F1 DU 编解码与请求转发 | `f1ap_du_test` 专项过滤器 | 6/6 通过 |
| DU 活动资源清单 | `du_ran_resource_manager_ntn_slot_registry_test.*` | 10/10 通过 |
| DU 清单查询 | `du_manager_ntn_rnti_lease_test` 的 UE 资源清单用例 | 4/4 通过 |
| DU 配置事务 | `ue_config_tester.when_mac_rejects_versioned_ntn_slot_update_then_the_authoritative_snapshot_is_not_completed` | 1/1 通过 |
| CU-CP 断线重连与修复 | `cu_cp_test` 中 `ue_slot_audit_*` 与 `ue_slot_repair_*` | 8/8 通过 |
| CUCP-049 回收流程回归 | `cu_cp_test` 中 C-RNTI retirement 用例 | 11/11 通过 |
| 只读状态输出 | `cu_cp_unit_config.ntn_state_command_prints_service_area_paging_counters` | 1/1 通过 |
| CU-CP 库构建 | `srsran_cu_cp` | 构建通过 |
| 脚本化软件流程仿真 | `ntn_ue_slot_recovery_sim` | 3 组、18/18 通过 |
| 文本检查 | `git diff --check` | 通过 |
| 残留进程检查 | CU、DU、UE、CTest 和抓包进程 | 0 个残留进程 |

`ntn_ue_slot_recovery_sim` 将三组非空 GTest 按以下软件流程组织：

```text
分配 -> DU applied -> 断线 -> 重连 -> 完整清单
     -> 缺失项重发/多余项清理 -> 再次核对 -> 清理资源
```

## 11. 后续工作

后续开发顺序确定为：

1. DU/MAC 提供真实首次接入波位记录，并绑定 C-RNTI generation；
2. 建立二级波位的数字业务绑定；
3. 接入设备波束端口和真实无线执行状态。

当前 RACH 数据中的时隙、频域位置和 preamble 不用于推导一级波位。
