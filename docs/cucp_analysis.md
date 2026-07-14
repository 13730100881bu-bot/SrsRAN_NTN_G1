# srsRAN CU-CP（L3）功能分析

## 整体架构

CU-CP 是整个系统的控制核心，代码位于 `lib/cu_cp/`，通过以下接口协调各节点：

```
CU-CP
├── NG 接口   ←→ AMF（核心网）
├── F1-C 接口 ←→ DU（1~6个）
├── E1 接口   ←→ CU-UP（1~6个）
└── NRPPA    ←→ 定位功能（可选）
```

---

## 一、RRC 过程（`lib/rrc/`）

| 过程 | 说明 |
|------|------|
| **RRC Setup** | 初始接入、SRB1 建立、RRC Setup Complete |
| **RRC Reestablishment** | 链路失败重建立、上下文恢复、回退处理 |
| **RRC Reconfiguration** | SRB/DRB 重配、测量配置更新、切换重配 |
| **UE Capability Transfer** | UE 能力询问与存储（按 PLMN 缓存） |

---

## 二、NGAP 过程（`lib/ngap/procedures/`）

**连接管理：**
- NG Setup / Reset
- Initial UE Message / UE Context Release
- DL/UL NAS 传输

**上下文管理：**
- Initial Context Setup（AMF 下发 UE 初始上下文）
- DL/UL RAN Status Transfer（切换时 PDCP/RLC 状态同步）

**PDU 会话管理：**
- PDU Session Resource Setup / Modify / Release

**切换：**
- Handover Required / Handover Command（Xn/N2 切换协调）
- Handover Resource Allocation（目标侧资源分配）

**寻呼：**
- Paging 消息接收与多 DU 广播分发

---

## 三、切换与移动性（`lib/cu_cp/mobility_manager/`、`routines/`）

| 场景 | 说明 |
|------|------|
| **Intra-CU 切换**（同 CU 不同 DU）| 源侧触发、目标侧准备、执行三阶段 |
| **Inter-CU 切换**（不同 gNB）| 源/目标两侧各自的 routine |
| **测量事件触发** | A1/A2/A3/A4/A5 事件，邻区优于服务区自动触发切换 |
| **测量配置** | SSB 频率、服务小区配置、邻区跟踪 |

支持切换指标统计：成功/失败/乒乓次数。

---

## 四、安全（`lib/cu_cp/ue_security_manager/`）

- K_gNB 推导（来自 5G AKA 上下文）
- 水平密钥推导（切换时密钥刷新）
- 完整性保护算法选择（NIA0/NIA1/NIA2/NIA3）
- 加密算法选择（NEA0/NEA1/NEA2/NEA3），128-bit 变体
- RRC 和 UP 各自独立的 AS 安全上下文

---

## 五、QoS 与承载管理（`up_resource_manager/`）

- 支持 **27+ 种 5QI 标准 profile**（GBR：5QI 1~4；Non-GBR：5QI 5~9 等）
- 每个 5QI 映射：RLC 模式（UM/AM）、丢弃定时器、SN 长度、重排序定时器
- DRB ↔ 5QI 映射管理，每 UE 最多 8 个 DRB
- PDU 会话到 GTP-U 隧道（TEID）管理

---

## 六、寻呼（`paging/`）

- 接收 AMF Paging 请求
- 向所有服务 DU 广播寻呼消息

---

## 七、容量与配置参数

```
最大 DU 数:         6
最大 CU-UP 数:      6
最大 UE 数:         8192
每 UE 最大 DRB:     8
RRC 流程保护定时器:  1000ms
AMF 重连间隔:        1000ms
```

支持无核心网运行（`no_core = true`，调试/测试用）。

---

## 八、可观测性

- NGAP/RRC/切换 各类指标统计
- 可配置的指标上报周期
- WebSocket 指标接口（25.10 版新增）

---

## 九、未实现或受限的功能

| 功能 | 状态 |
|------|------|
| RRC Inactive 状态 | 已有 MVP（见下文十），完整态仍受限 |
| 双连接（EN-DC / NR-DC）| 不支持 |
| Rel-18 高级移动性特性 | 不支持 |
| NRPPA 定位 | 接口已定义，功能有限 |

---

## 十、RRC Inactive MVP 实现状态

本仓库在主干基础上新增了一个 **RRC Inactive 最小可行实现（MVP）**，覆盖挂起（Suspend）全流程以及恢复（Resume）的入口与回退路径。目标是打通 3GPP TS 38.331 §5.3.13 的主要状态转换骨架，并为后续完整实现预留扩展点。

### 已实现

| 方向 | 模块 | 说明 |
|------|------|------|
| Suspend 路径 | `rrc_ue_impl::get_rrc_ue_inactive_release_context()` | 分配 40/24-bit I-RNTI，构建 `RRCRelease` + `SuspendConfig`（含 `ran_paging_cycle=rf128`、NCC 占位），通过 PDCP 打包 SRB1 PDU，并将 UE 置为 `connected_inactive`。 |
| 上下文仓库 | `lib/rrc/ue/rrc_inactive_context_repository.h` | 进程级线程安全单例（`mutex` + `unordered_map`），按 Full / Short I-RNTI 双向索引 `rrc_inactive_ue_context`，快照字段包括 `ue_index`、`old_c_rnti`、`cell`、NCC 以及完整的 `rrc_ue_transfer_context`（安全、UP、SRB、UE 能力）。`store()` 采用 `emplace + move` 规避 `byte_buffer` 删除的拷贝赋值。 |
| Resume 入口 | `rrc_ue_impl::handle_rrc_resume_request()` | 在 UL-CCCH 分支中识别 `rrc_resume_request`，按 Short I-RNTI 查库，区分 **RNAU** 与普通 Resume，命中后通过 `emplace-move` 将快照转移到新 UE 的 `transfer_context`，随后走标准 Release 回退到 RRCSetup（TS 38.331 §5.3.13.5 显式允许）。 |
| NGAP 辅助 | `lib/ngap/procedures/ngap_ue_context_suspend_resume_helper.h` | 静态封装 `UE_CONTEXT_SUSPEND`（proc id 59）、`UE_CONTEXT_RESUME`（proc id 58）的 `Initiating Message` 构建与发送，供后续 async_task 串接。 |
| CU-CP 控制器 | `lib/cu_cp/cu_cp_inactive_controller.h` | 无状态静态入口 `trigger_suspend()`，调用 RRC UE 生成 PDU 并返回 `cu_cp_inactive_suspend_result`；已在注释中给出向完整 async_task 演进的 5 步编排计划。 |

### 仍为 Stub / 待完成

- **Full Resume**：未做 ResumeMAC-I 校验、未做 K_gNB 水平衍生、未做 PDCP 重建与 SRB/DRB 恢复；所有 Resume 请求一律回退 RRCSetup。
- **NGAP 响应**：Suspend/Resume Response 分支尚未接入 `ngap_impl` 的事件路由；AMF 侧的 UE 上下文状态目前不会被刷新。
- **F1AP**：缺 RAN-based Paging、缺 UE Context Release（保留 gNB-CU UE ID）这一步；CU-CP UE Manager 的"软释放"暂未引入。
- **RNAU 响应**：检测已实现，但响应仍是 RRCSetup，而非保持 INACTIVE 的 `RRCRelease(suspendConfig)`。
- **计时器 / NCC**：T380、`ran_notif_area_info` 字段未下发；NCC 固定为 0，需要接入 `ue_security_manager` 真实的水平密钥派生。
- **单元测试**：新类仅通过现有 `handover_reconfiguration_routine_test` 的 stub 覆盖接口契约，尚无 Suspend/Resume 专项测试。

### 下一步工作（按优先级）

1. 在 `ue_security_manager` 中实现 `derive_horizontal_keys()`，给 NCC 与新 K_gNB 提供真实来源；
2. 把 `cu_cp_inactive_controller::trigger_suspend()` 改为 async_task，串入 F1AP DL RRC Transfer → NGAP Suspend → F1AP UE Context Release → CU-CP UE Manager 软释放；
3. 在 `rrc_ue_impl` 中实现真正的 RRCResume：恢复 SRB1/SRB2、通过 PDCP 重新派生密钥、恢复 DRB 与 QoS；
4. 为 `ngap_impl` 路由 Suspend/Resume Response，落实 AMF 侧状态机；
5. 补齐 RNAU → `RRCRelease(suspendConfig)` 的响应分支与 F1AP RAN Paging；
6. 引入 Suspend/Resume 单元测试与端到端（gNB + AMF + UE 仿真）回归。

---

## 总结

srsRAN CU-CP 是一个生产级 5G L3 实现，覆盖了 3GPP Rel-17 核心的 RRC、NGAP、安全、切换、QoS 等完整流程，采用 C++20 协程异步架构，代码量约 **5 万行**，适合科研、测试和小规模商用部署。
