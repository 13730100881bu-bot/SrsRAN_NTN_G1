# NTN 接入临时号码安全回收与重连恢复

## 1. 功能目的

CUCP-049 为 NTN 接入号码池增加安全回收和再次使用能力。

C-RNTI 是终端接入小区时使用的临时号码。CU-CP 会提前向 DU/MAC 下发一组
可用于随机接入的 C-RNTI，DU 在 RAR 过程中取出其中一个号码。终端离开后，
这个号码不能立即重新发给其他终端，因为 CU-CP、DU 和 MAC 可能仍在处理旧的
释放、超时或重试消息。

本功能采用以下原则：

- CU-CP 已确认终端不再使用该号码；
- DU 的完整号码清单确认同一代号码已经 `expired`；
- MAC 在真正删除记录前再次检查该号码没有被终端、其他 NTN 小区或 terrestrial
  临时接入占用；
- DU 明确确认回收成功，或者后续完整清单确认此前请求的整批号码已经全部移除后，
  CU-CP 才允许再次分配该号码。

因此，号码回收不是单纯删除一条本地记录，而是 CU-CP、DU 和 MAC 共同完成的
一次确认操作。

## 2. 号码状态

现有接入流程继续使用 `reserved`、`offered_in_rar`、`initial_ul_seen`、
`committed`、`released` 和 `expired` 等状态。CUCP-049 在可回收的终止状态后增加：

```text
released / expired
        |
        | 当前 DU 连接上的完整检查确认同代号码已过期
        v
retire_pending
        |
        | 私有 F1AP 回收请求已发送
        v
retire_sent
        |
        | DU/MAC 原子检查并确认删除，或等待后续完整清单核对
        v
retire_waiting_audit
        |
        | 精确整批号码均已从完整清单消失
        v
retired
```

CU-CP 已跟踪的号码只有在本地已经 `released` 或 `expired`，并且 DU 报告相同
C-RNTI、相同 `generation` 已经 `expired` 时，才可进入回收流程。CU-CP 重启后从
DU 完整清单发现的未知号码先进入隔离区；其中只有已由 DU 标记为 `expired`、
且未被当前 UE 占用的号码可进入回收流程，`pending` 和 `consumed_by_mac` 继续隔离。

以下记录不会自动回收：

- 仍与活动 UE 关联的 `committed` 记录；
- 已在 RAR 中使用或已经收到 Initial UL 的记录；
- handover 目标预留；
- DU 尚未报告过期的记录；
- 身份、代次或清单存在冲突的记录。

一批回收采用全有或全无规则。MAC 对整批号码完成检查后再统一删除；只要其中
一个号码仍不安全，整批记录均保持不变。回收响应丢失时，只有后续完整清单
确认精确整批全部消失，才会释放这一批号码；只消失一部分时不会释放其中任何一个。

## 3. generation 防止旧消息影响新号码

同一个 C-RNTI 可以在不同时间分配给不同终端，因此仅比较号码数值并不充分。
每次号码池更新都携带代次号（`generation`），它表示该号码记录所属的更新批次。

MAC 在号码回收后保留一条精简的 generation 记录：

- 相同 generation 的重复回收按幂等成功处理；
- 更早 generation 的 `add` 或 `retire` 请求被拒绝；
- 再次使用同一号码时必须采用更大的 generation；
- 延迟到达的旧回收请求不能删除较新 generation 的号码；
- generation 达到 `UINT32_MAX` 时停止继续分配并报告
  `rnti_generation_exhausted`。

这些精简记录不会恢复号码的接入资格，只用于识别过期消息。它们的数量受
C-RNTI 有限号码空间约束。

例如，`0x4601/gen12` 在 UE 离开并经 DU 确认过期后完成回收，CU-CP 可在后续
号码池中把同一个数值作为 `0x4601/gen13` 再次分配。此时迟到的 `gen12 retire`
会被 MAC 识别为旧消息，无法删除 `gen13` 的新记录。

## 4. CU-CP 与 DU 的私有协调

CUCP-049 扩展标准 `GNBDUResourceCoordination` 消息中已有的私有数据区，增加号码
`retire` 操作以及能力信息：

- `retire_supported`：DU 是否支持安全回收；
- `rnti_generation_high_water`：DU 当前识别的最高号码 generation。

原有 `replace`、`add` 和 `clear` 操作保持原格式。标准 F1AP 消息和 generated
ASN.1 类型不变。

CU-CP 在当前 DU 连接上先取得完整号码清单和能力结果，再发送回收请求。旧 DU
明确返回无法识别新格式时，CU-CP 将其标为 `retire_unsupported`。响应缺失时能力
保持未知；清单不完整但带有能力信息时，可以显示 DU 是否支持回收，但该小区仍
处于等待完整清单状态，期间不授权回收或新号码池下发。旧格式完整核对成功后，
现有号码池下发和普通接入流程可继续，待回收或未知号码仍保持隔离。

## 5. 重启与 DU 重连

号码回收状态不增加新的持久化文件。CU-CP 重启或 DU 重连后，以当前连接上的
DU 完整查询结果重建可回收判断。

处理顺序如下：

1. DU 断开时，使该连接上尚未完成的检查和回收响应失效；
2. 新连接建立后，查询配置中的 NTN 小区，即使 CU-CP 当前没有本地号码记录；
3. 将 DU 中存在、但 CU-CP 本地不知道的记录放入隔离区；
4. `pending` 或已经被 MAC 使用的隔离记录继续保留；
5. 只有 DU 报告为 `expired` 的隔离记录才可进入回收流程；
6. DU 成功响应回收，或后续完整清单确认此前精确整批已全部移除后，号码才重新
   具备分配资格。

隔离记录不会授权接入，也不会被号码分配器选中。旧连接上的延迟响应与当前
DU connection generation 不匹配，因此不能完成新连接上的回收。

## 6. 有界号码分配

CU-CP 在每个 DU 内扫描 `0x4601..0xffef`，选择完整的 8 个安全号码。8 是当前软件
每次补充的固定池大小，并非 3GPP 规定的号码上限。

- 跳过当前 UE 正在使用的号码；
- 跳过未完成、冲突和隔离记录；
- 跳过还没有收到 DU 回收确认的号码；
- 同一 DU 的两个小区不能同时使用同一数值，这是当前实现为避免 DU 内号码冲突
  采用的更严格规则；
- 不同 DU 可以继续使用数值相同的 C-RNTI。

完整扫描后不足 8 个安全号码时，本轮不发送残缺号码池，并报告
`rnti_namespace_exhausted`。这避免部分补充导致各接入区域获得不一致的号码池。

## 7. 只读运行状态

现有 `ntn_state` 命令显示：

- `retire_pending`、`retire_sent`、`retire_waiting_audit` 和隔离记录数量；
- 本进程累计完成的回收、再次预留和明确拒绝的号码数量；
- 每个 DU 是否支持安全回收，以及是否完成当前连接上的完整核对；
- 当前 generation 高水位；
- `rnti_namespace_exhausted` 和 `rnti_generation_exhausted`；
- 最近一次回收或恢复失败原因。

这些字段用于查看软件中的号码管理状态，不包含本地文件路径或其他部署敏感
信息。

## 8. 兼容性与边界

- 功能沿用现有 NTN 号码池开关，默认关闭。关闭时不会启动回收查询或改变
  terrestrial C-RNTI 分配顺序。
- 新 CU-CP 连接不支持回收的旧 DU 时采用“宁可暂不复用，也不冒险重复分配”的
  处理方式：旧格式完整核对后原有号码池流程可继续，待回收和未知号码保持隔离。
- 本轮只扩展已有私有 F1AP coordination container、DU manager 和 MAC
  `rnti_manager`；不增加公开协议字段，也不修改 generated ASN.1。
- 本轮不修改 MAC scheduler、PRACH detector、PHY、lower PHY、RU/RF、天线控制
  或 Web/GIS。
- `retired` 统计已由 DU 成功响应或完整清单整批缺失确认的号码；`reused` 统计
  CU-CP 以更高 `generation` 再次预留的号码。计数范围止于软件号码管理，后续
  DU 安装、RAR 发送和无线设备执行分别由对应运行状态记录。

## 9. 主要代码位置

- CU-CP 号码生命周期和完整清单核对：
  `lib/cu_cp/ntn_mobility/ntn_beam_service_resource_manager.*`
- CU-CP 私有号码、回收批次和只读状态结构：
  `include/srsran/cu_cp/ntn_beam_service_resources.h`、
  `include/srsran/cu_cp/cu_cp_command_handler.h`
- CU-CP 调度、重连恢复和 `ntn_state`：`lib/cu_cp/cu_cp_impl.*`
- `ntn_state` 命令输出：`apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- 私有 F1AP 号码池格式：`include/srsran/f1ap/ntn_rnti_lease_pool.h`
- DU 转换和转发：`lib/du/du_high/du_manager/du_manager_impl.cpp`、
  `include/srsran/mac/mac_manager.h`
- MAC 原子回收和 generation 记录：`lib/mac/rnti_manager.h`
- 相关测试：`tests/unittests/cu_cp/ntn_mobility/`、
  `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`、
  `tests/unittests/f1ap/` 和 `tests/unittests/mac/`

## 10. 验证状态

2026-08-08 完成以下 focused validation：

- `ntn_beam_service_resource_manager.*`：52/52 通过，覆盖回收资格、批量原子性、
  隔离、回执丢失、DU 清单复核和更高 generation 再次使用；
- `f1ap_cu_test`：8/8 通过，覆盖私有号码池 container、能力查询和完整清单；
- `f1ap_du_test`：6/6 通过，覆盖清单 codec、回收请求和旧格式兼容；
- `rnti_manager_test.*(retire|generation|terrestrial)`：14/14 通过，覆盖 MAC
  原子回收、防止旧消息误删和 terrestrial 分配回归；
- `cu_cp_ntn_mobility_test` 的 CUCP-049 过滤组：11/11 通过，覆盖重启、重连、
  回执丢失恢复、号码空间与 generation 边界、旧 DU 和默认关闭路径；
- `cu_cp_unit_config_test` 中包含 NTN 的配置与只读状态测试：31/31 通过；
- `ntn_mobility_test`、`f1ap_cu_test`、`f1ap_du_test`、`mac_test`、`cu_cp_test`、
  `cu_cp_unit_config_test` 和 `srsran_cu_cp` 相关构建目标均已成功完成；
- `ntn_rnti_retirement_sim`：三组 focused CTest 共 37/37 通过，其中号码生命周期
  13 项、CU-CP 重连 10 项、MAC 号码管理 14 项，收尾检查未发现 CU、DU、UE
  或抓包进程；
- `git diff --check`：通过。

三目标 F1/MAC 组合构建设置了 1200 秒总时限；超时前两个 F1 目标已经完成，
`mac_test` 随后以单目标串行构建成功。软件流程演练使用 focused CTest，不启动
真实 CU、DU、UE 或无线设备。验证范围止于 CU-CP、F1 私有数据区、DU manager
和 MAC 号码管理。
