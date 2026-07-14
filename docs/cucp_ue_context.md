# CU-CP UE 上下文分析

> 基于 srsRAN Project 25.10.0 源码分析

---

## 目录

1. [整体架构](#1-整体架构)
2. [主上下文结构](#2-主上下文结构)
3. [子上下文详解](#3-子上下文详解)
   - [UP 资源上下文](#31-up-资源上下文)
   - [安全上下文](#32-安全上下文)
   - [测量上下文](#33-测量上下文)
   - [切换上下文](#34-切换上下文)
   - [RRC 迁移上下文](#35-rrc-迁移上下文)
4. [UE 容器与仓库](#4-ue-容器与仓库)
5. [协议适配器](#5-协议适配器)
6. [生命周期管理](#6-生命周期管理)
7. [索引类型](#7-索引类型)
8. [层次结构总览](#8-层次结构总览)

---

## 1. 整体架构

CU-CP 中的 UE 上下文是一个多层次的数据结构，核心类为 `cu_cp_ue`，其内部聚合了无线、安全、承载、测量等多个子上下文，并通过适配器将 RRC、NGAP、E1AP、F1AP 等协议栈解耦。

```
ue_manager (全局 UE 管理器)
└── std::unordered_map<ue_index_t, cu_cp_ue>
    └── cu_cp_ue (单个 UE 完整上下文)
        ├── cu_cp_ue_context        基础标识
        ├── up_resource_manager     用户面承载/QoS
        ├── ue_security_manager     安全密钥/算法
        ├── cell_meas_manager_ue_context  无线测量
        ├── rrc_ue_interface*       RRC 状态机
        ├── cu_cp_ue_handover_context (可选)  切换状态
        └── 协议适配器 ×6           事件解耦
```

---

## 2. 主上下文结构

### 2.1 `cu_cp_ue_context` — 基础标识上下文

**文件:** [lib/cu_cp/ue_manager/cu_cp_ue_impl.h](lib/cu_cp/ue_manager/cu_cp_ue_impl.h#L43)

| 字段 | 类型 | 说明 |
|------|------|------|
| `ue_index` | `ue_index_t` | CU-CP 全局唯一 UE 索引 |
| `du_idx` | `du_index_t` | 所连接的 DU 索引 |
| `cu_up_idx` | `cu_up_index_t` | 所使用的 CU-UP 索引 |
| `du_id` | `gnb_du_id_t` | gNB-DU ID |
| `plmn` | `plmn_identity` | PLMN 标识 |
| `crnti` | `rnti_t` | 小区无线网络临时标识 |
| `reconfiguration_disabled` | `bool` | 切换期间禁止重配标志 |

### 2.2 `cu_cp_ue` — UE 主类

**文件:** [lib/cu_cp/ue_manager/cu_cp_ue_impl.h](lib/cu_cp/ue_manager/cu_cp_ue_impl.h#L60)

| 成员 | 类型 | 说明 |
|------|------|------|
| `ue_index` | `ue_index_t` | UE 标识 |
| `pcell_index` | `du_cell_index_t` | 主小区索引 |
| `pci` | `pci_t` | 物理小区标识 |
| `ue_ctxt` | `cu_cp_ue_context` | 基础上下文 |
| `task_sched` | `ue_task_scheduler_impl` | 每 UE 任务调度器 |
| `up_mng` | `up_resource_manager` | UP 资源管理器 |
| `sec_mng` | `ue_security_manager` | 安全管理器 |
| `rrc_ue` | `rrc_ue_interface*` | RRC UE 接口指针 |
| `meas_context` | `cell_meas_manager_ue_context` | 测量上下文 |
| `ho_context` | `std::optional<cu_cp_ue_handover_context>` | 切换上下文（可选） |
| `handover_ue_release_timer` | `unique_timer` | 切换释放定时器 |

---

## 3. 子上下文详解

### 3.1 UP 资源上下文

**文件:** [include/srsran/cu_cp/up_context.h](include/srsran/cu_cp/up_context.h#L34)

#### `up_context` — 用户面总上下文

| 字段 | 类型 | 说明 |
|------|------|------|
| `pdu_sessions` | `map<pdu_session_id_t, up_pdu_session_context>` | 所有 PDU 会话 |
| `drb_map` | `map<drb_id_t, pdu_session_id_t>` | DRB → 会话映射 |
| `qos_flow_map` | `map<qos_flow_id_t, drb_id_t>` | QoS 流 → DRB 映射 |
| `drb_dirty` | `array<bool, MAX_NOF_DRBS>` | DRB 脏标记 |

#### `up_pdu_session_context` — PDU 会话上下文

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | `pdu_session_id_t` | PDU 会话 ID |
| `drbs` | `map<drb_id_t, up_drb_context>` | 会话内所有 DRB |

#### `up_drb_context` — DRB 上下文

| 字段 | 类型 | 说明 |
|------|------|------|
| `drb_id` | `drb_id_t` | DRB 标识 |
| `pdu_session_id` | `pdu_session_id_t` | 所属 PDU 会话 |
| `s_nssai` | `s_nssai_t` | 切片/服务标识 |
| `default_drb` | `bool` | 是否为默认承载 |
| `rlc_mod` | `rlc_mode` | RLC 模式 (AM/UM/TM) |
| `qos_params` | `qos_flow_level_qos_parameters` | QoS 参数 |
| `qos_flows` | `map<qos_flow_id_t, up_qos_flow_context>` | QoS 流映射 |
| `ul_up_tnl_info_to_be_setup_list` | `vector<up_transport_layer_info>` | 上行隧道端点信息 |
| `pdcp_cfg` | `pdcp_config` | PDCP 配置 |
| `sdap_cfg` | `sdap_config_t` | SDAP 配置 |

#### `up_qos_flow_context` — QoS 流上下文

| 字段 | 类型 | 说明 |
|------|------|------|
| `qfi` | `qos_flow_id_t` | QoS 流标识 |
| `qos_params` | `qos_flow_level_qos_parameters` | 流级 QoS 参数 |

---

### 3.2 安全上下文

**文件:** [include/srsran/security/security.h](include/srsran/security/security.h#L200)

#### `security_context` — 安全总上下文

| 字段 | 类型 | 说明 |
|------|------|------|
| `k` | `sec_key` | 主密钥 K（256 bit） |
| `ncc` | `uint8_t` | 下一跳链计数器 |
| `supported_int_algos` | `supported_algorithms` | UE 支持的完整性算法 |
| `supported_enc_algos` | `supported_algorithms` | UE 支持的加密算法 |
| `sel_algos` | `sec_selected_algos` | 已选择的 NIA/NEA 算法 |
| `as_keys` | `sec_as_keys` | AS 层密钥集 |

#### `sec_as_keys` — AS 层密钥

| 密钥 | 用途 |
|------|------|
| `k_rrc_int` | RRC 完整性密钥 |
| `k_rrc_enc` | RRC 加密密钥 |
| `k_up_int` | UP 完整性密钥 |
| `k_up_enc` | UP 加密密钥 |

#### `sec_128_as_config` — 128 位 AS 配置（下发给 PDCP）

| 字段 | 类型 | 说明 |
|------|------|------|
| `domain` | `sec_domain` | RRC 或 UP |
| `k_128_int` | `optional<sec_128_key>` | 128 位完整性密钥 |
| `k_128_enc` | `sec_128_key` | 128 位加密密钥 |
| `integ_algo` | `optional<integrity_algorithm>` | 完整性算法 |
| `cipher_algo` | `ciphering_algorithm` | 加密算法 |

---

### 3.3 测量上下文

**文件:** [lib/cu_cp/cell_meas_manager/measurement_context.h](lib/cu_cp/cell_meas_manager/measurement_context.h#L37)

#### `cell_meas_manager_ue_context`

| 字段 | 类型 | 说明 |
|------|------|------|
| `meas_ids` | `slotted_array<meas_id_t, MAX_NOF_MEAS>` | 已分配的测量 ID |
| `meas_obj_ids` | `slotted_array<meas_obj_id_t, MAX_NOF_MEAS_OBJ>` | 测量对象 ID |
| `meas_id_to_meas_context` | `map<meas_id_t, meas_context_t>` | 测量 ID → 上下文 |
| `nci_to_meas_obj_id` | `map<nr_cell_identity, meas_obj_id_t>` | NCI → 测量对象 |
| `meas_results` | `optional<cell_measurement_positioning_info>` | 测量结果（定位用） |

#### `meas_context_t` — 单个测量项上下文

| 字段 | 类型 | 说明 |
|------|------|------|
| `meas_obj_id` | `meas_obj_id_t` | 测量对象 ID |
| `report_cfg_id` | `report_cfg_id_t` | 上报配置 ID |
| `gnb_id_bit_length` | `unsigned` | gNB ID 比特长度 |
| `nci` | `nr_cell_identity` | NR 小区标识 |
| `pci` | `pci_t` | 物理小区 ID |

---

### 3.4 切换上下文

**文件:** [lib/cu_cp/ue_manager/cu_cp_ue_impl.h](lib/cu_cp/ue_manager/cu_cp_ue_impl.h#L55)

#### `cu_cp_ue_handover_context`（可选，切换时存在）

| 字段 | 类型 | 说明 |
|------|------|------|
| `target_ue_index` | `ue_index_t` | 目标侧 UE 索引 |
| `rrc_reconfig_transaction_id` | `uint8_t` | RRC 重配事务 ID |

---

### 3.5 RRC 迁移上下文

**文件:** [include/srsran/cu_cp/cu_cp_ue_messages.h](include/srsran/cu_cp/cu_cp_ue_messages.h#L36)

> 切换/重建时用于在 UE 之间传递完整状态

#### `rrc_ue_transfer_context`

| 字段 | 类型 | 说明 |
|------|------|------|
| `sec_context` | `security::security_context` | 安全上下文 |
| `meas_cfg` | `optional<rrc_meas_cfg>` | 测量配置 |
| `up_ctx` | `up_context` | UP 承载上下文 |
| `srbs` | `static_vector<srb_id_t, MAX_NOF_SRBS>` | 活跃 SRB 列表 |
| `handover_preparation_info` | `byte_buffer` | 切换准备信息 |
| `ue_cap_rat_container_list` | `byte_buffer` | UE 能力容器列表 |
| `is_inter_cu_handover` | `bool` | 是否为 inter-CU 切换 |

---

## 4. UE 容器与仓库

### 4.1 `ue_manager` — UE 全局管理器

**文件:** [lib/cu_cp/ue_manager/ue_manager_impl.h](lib/cu_cp/ue_manager/ue_manager_impl.h#L43)

| 存储结构 | 类型 | 说明 |
|----------|------|------|
| `ues` | `unordered_map<ue_index_t, cu_cp_ue>` | 所有 UE 上下文 |
| `pci_rnti_to_ue_index` | `map<tuple<pci_t, rnti_t>, ue_index_t>` | PCI+RNTI 快速查找 |
| `blocked_plmns` | `set<plmn_identity>` | 黑名单 PLMN |

**查找接口:**

| 方法 | 说明 |
|------|------|
| `find_ue(ue_index_t)` | 按 UE 索引查找 |
| `get_ue_index(pci_t, rnti_t)` | 按 PCI/RNTI 查找 |
| `find_du_ue(ue_index_t)` | 查找 DU 侧 UE 上下文 |
| `find_ues(plmn_identity)` | 查找指定 PLMN 的所有 UE |

### 4.2 `ngap_repository` — NGAP/AMF 仓库

**文件:** [lib/cu_cp/ngap_repository.h](lib/cu_cp/ngap_repository.h#L92)

| 字段 | 说明 |
|------|------|
| `plmn_to_amf_index` | PLMN → AMF 索引映射 |
| `ngap_db` | AMF 索引 → NGAP 上下文 |

### 4.3 `du_processor_repository` — DU 仓库

**文件:** [lib/cu_cp/du_processor/du_processor_repository.h](lib/cu_cp/du_processor/du_processor_repository.h#L100)

| 字段 | 说明 |
|------|------|
| `du_db` | DU 索引 → DU 上下文映射 |

### 4.4 `cu_up_processor_repository` — CU-UP 仓库

**文件:** [lib/cu_cp/cu_up_processor/cu_up_processor_repository.h](lib/cu_cp/cu_up_processor/cu_up_processor_repository.h#L71)

| 字段 | 说明 |
|------|------|
| `cu_up_db` | CU-UP 索引 → CU-UP 上下文映射 |

---

## 5. 协议适配器

**目录:** [lib/cu_cp/adapters/](lib/cu_cp/adapters/)

每个 UE 内部持有以下适配器实例，用于跨协议栈事件解耦：

| 适配器 | 文件 | 连接方向 |
|--------|------|----------|
| `rrc_ue_ngap_adapter` | rrc_ue_adapters.h | RRC → NGAP |
| `rrc_ue_cu_cp_ue_adapter` | rrc_ue_adapters.h | RRC → CU-CP UE |
| `rrc_ue_cu_cp_adapter` | rrc_ue_adapters.h | RRC → CU-CP |
| `ngap_cu_cp_ue_adapter` | ngap_adapters.h | NGAP → CU-CP |
| `ngap_rrc_ue_adapter` | ngap_adapters.h | NGAP → RRC |
| `nrppa_cu_cp_ue_adapter` | nrppa_adapters.h | NRPPA → CU-CP |

---

## 6. 生命周期管理

### 6.1 UE 创建

**文件:** [lib/cu_cp/ue_manager/ue_manager_impl.h](lib/cu_cp/ue_manager/ue_manager_impl.h#L104)

```cpp
ue_index_t add_ue(du_index_t du_index,
                  optional<gnb_du_id_t> du_id,
                  optional<pci_t> pci,
                  optional<rnti_t> rnti,
                  optional<du_cell_index_t> pcell_index);
```

触发时机：F1AP 收到 Initial UL RRC Message Transfer

### 6.2 UE 删除

```cpp
void remove_ue(ue_index_t ue_index);
```

### 6.3 生命周期异步流程

**目录:** [lib/cu_cp/routines/](lib/cu_cp/routines/)

| 流程文件 | 说明 |
|----------|------|
| `initial_context_setup_routine.h` | NGAP Initial Context Setup（连接建立） |
| `ue_context_release_routine.h` | UE 上下文释放 |
| `ue_removal_routine.h` | UE 资源清理 |
| `ue_amf_context_release_request_routine.h` | AMF 发起的上下文释放 |
| `reestablishment_context_modification_routine.h` | RRC 重建后上下文修改 |
| `ue_transaction_info_release_routine.h` | 事务信息清理 |

### 6.4 RRC 状态机

**文件:** [include/srsran/rrc/rrc_ue.h](include/srsran/rrc/rrc_ue.h#L52)

```
idle  →  connected  →  connected_inactive
  ↑____________↓______________↓
```

---

## 7. 索引类型

**文件:** [include/srsran/cu_cp/cu_cp_types.h](include/srsran/cu_cp/cu_cp_types.h)

| 类型 | 范围 | 说明 |
|------|------|------|
| `ue_index_t` | `[0, 2^32-1)` | CU-CP 全局 UE 标识 |
| `du_index_t` | `[0, MAX_NOF_DUS)` | CU-CP 内 DU 标识 |
| `du_cell_index_t` | `[0, MAX_NOF_CELLS_PER_DU)` | DU 内小区标识 |
| `cu_up_index_t` | `[0, MAX_NOF_CU_UPS)` | CU-CP 内 CU-UP 标识 |
| `amf_index_t` | `[0, MAX_NOF_AMFS)` | NGAP AMF 标识 |

---

## 8. 层次结构总览

```
cu_cp_ue
├── cu_cp_ue_context
│   ├── ue_index_t
│   ├── du_index_t  ──────────────→  du_processor_repository
│   ├── cu_up_index_t  ───────────→  cu_up_processor_repository
│   ├── rnti_t
│   └── plmn_identity  ───────────→  ngap_repository
│
├── up_resource_manager
│   └── up_context
│       └── map<pdu_session_id, up_pdu_session_context>
│           └── map<drb_id, up_drb_context>
│               ├── rlc_mode / pdcp_cfg / sdap_cfg
│               └── map<qos_flow_id, up_qos_flow_context>
│
├── ue_security_manager
│   └── security_context
│       ├── sec_key K (256-bit)
│       ├── NIA / NEA 算法选择
│       └── sec_as_keys
│           ├── K_RRC_int / K_RRC_enc
│           └── K_UP_int  / K_UP_enc
│
├── cell_meas_manager_ue_context
│   ├── slotted_array<meas_id>
│   ├── map<meas_id, meas_context_t>
│   └── optional<positioning_info>
│
├── rrc_ue_interface*  ────────────→  RRC 状态机 (idle/connected/inactive)
│
├── [optional] cu_cp_ue_handover_context
│   ├── target_ue_index
│   └── rrc_reconfig_transaction_id
│
└── 适配器层
    ├── rrc_ue_ngap_adapter
    ├── rrc_ue_cu_cp_ue_adapter
    ├── ngap_cu_cp_ue_adapter
    ├── ngap_rrc_ue_adapter
    ├── nrppa_cu_cp_ue_adapter
    └── rrc_ue_cu_cp_adapter
```

---

*生成时间: 2026-04-20 | srsRAN Project v25.10.0*
