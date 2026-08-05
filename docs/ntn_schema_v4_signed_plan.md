# NTN schema v4 管理计划签名与版本恢复

本文说明管理中心位置计划从 schema v3 升级到 schema v4 后的签名、验签、版本保护和重启恢复流程。该功能属于默认关闭的星载位置计划配置；未启用时，terrestrial 和原有 NTN 路径保持原有行为。

## 1. 解决的问题

schema v3 已经能够在同一份计划中携带完整可见一级波位和本星实际负责的一级波位，并完成容量检查、双小区划分、软件日历安装和重启恢复。schema v4 在此基础上增加两项保护：

1. 管理中心使用私钥为计划签名，CU-CP 只信任本地配置的公钥；
2. CU-CP 使用独立版本文件记录已经接受的最高版本，重启时同时核对计划状态和版本记录。

计划签名保护计划内容和签名密钥标识。版本记录用于发现状态文件单独被替换为较旧副本的情况。

## 2. schema v4 输入

schema v4 保留 schema v3 的规划批次、catalog、身份 registry、access profile、卫星标识、两个稳定 NCI/PCI、有效期、启用时刻、完整可见清单和实际负责子集，并增加：

| 字段 | 含义 |
|---|---|
| `authentication.algorithm` | 固定为 `ecdsa-p256-sha256` |
| `authentication.key_id` | 选择 CU-CP 本地配置中的可信公钥 |
| `authentication.signature_base64` | ASN.1 DER 格式的 ECDSA 签名，再以规范 Base64 编码 |

根对象和 `authentication` 都采用 exact-key 解析。缺少字段、出现未知字段、重复字段、签名编码错误、未知 `key_id` 或算法不匹配都会拒绝计划。参与签名的规划标识只接受不会与换行和分隔符混淆的安全字符；版本和毫秒时间也限定在 Node 与 C++ 都能精确处理的共同范围内。坐标中的负零也会在生成和读取阶段被拒绝，保证同一份计划在文件写入前后具有完全一致的签名内容。

`content_hash` 继续对规范化后的计划业务内容计算 SHA-256，不包含签名字节。签名输入包含独立的用途标识、完整规范化计划、`content_hash`、算法和 `key_id`，从而把签名明确限定在 srsRAN NTN schema v4 位置计划用途内。

管理中心工具：

```bash
node utils/ntn/versioned_position_plan_v4.mjs sign \
  position-plan-v3.json ntn-management-key-v1 private-key.pem position-plan-v4.json

node utils/ntn/versioned_position_plan_v4.mjs validate \
  position-plan-v4.json public-key.pem
```

输出文件仍使用同目录临时文件和原子替换，计划文件上限仍为 4 MiB。私钥由管理中心保存，不写入计划文件；仓库中的跨语言样例只包含公钥和已签名计划。

## 3. CU-CP 配置

在原有位置计划配置上启用签名，并配置一个或多个可信公钥：

```yaml
ntn_onboard_position_plan:
  enabled: true
  du_execution_enabled: true
  require_signed_plan: true
  satellite_id: P01-S01
  plan_json_file: /var/lib/srsran/ntn/position-plan.json
  state_file: /var/lib/srsran/ntn/position-plan-state.json
  version_anchor_file: /var/lib/srsran/ntn/position-plan-version.json
  trusted_signing_keys:
    - key_id: ntn-management-key-v1
      public_key_file: /etc/srsran/ntn-management-key-v1-public.pem
```

签名模式还要求完整的 catalog、identity registry、access profile 上下文以及两个长期星载小区身份。计划文件、状态文件、版本文件和各公钥文件必须使用互不相同的路径，`key_id` 必须唯一。

CU-CP 以有界方式读取本地 PEM 公钥，确认其为 ECDSA P-256 公钥，并对规范化的 DER SPKI 公钥计算 SHA-256 指纹。运行状态显示 `key_id` 和指纹，不显示公钥路径。

## 4. 接收与启用顺序

签名执行模式采用以下固定顺序：

1. 读取并解析 schema v4，检查规划上下文、卫星身份、两个 NCI/PCI、版本、有效期、波位清单和容量；
2. 根据 `key_id` 选择本地公钥，重新计算 `content_hash` 并验证 ECDSA 签名；
3. 检查 catalog/schedule 版本没有低于已经接受的最高版本，同版本不同 hash 也按回放拒绝；
4. 在独立版本文件中预留新版本；
5. 原子保存 schema v4 恢复状态，包括最高版本对应的完整已签名原始计划；
6. 将预留版本提交为最高已接受版本；
7. 完成上述持久化后，才允许进入原有 DU 软件日历 `prepare → applied → activation` 流程。

任一步失败都会停止新计划部署，旧的有效计划和既有清理责任保持原状。常用机器可读原因包括 `signature_required`、`unsupported_signature_algorithm`、`unknown_signing_key`、`invalid_public_key`、`invalid_signature`、`version_replay` 和 `version_anchor_unavailable`。

## 5. 状态文件、版本文件与重启

签名执行模式写入状态 schema v4。它在 schema v3 状态内容之外保存：

- 最高 `catalog_version`、`schedule_version` 和 `content_hash`；
- 与最高版本对应的完整 schema v4 已签名计划，用于重启后重新验签；
- `software_only` 版本快照；
- active、pending、双小区划分、DU 软件部署阶段和待清理任务。

独立版本文件最大 64 KiB，绑定 `satellite_id`、规划上下文、两个 NCI/PCI，并记录已经提交或正在预留的计划身份。计划身份由 catalog 版本、schedule 版本、内容 hash 和 `key_id` 共同组成。文件通过同目录临时文件、同步写入和原子替换更新。

重启时按以下顺序核对：

1. 重新解析状态中的完整 schema v4 计划并重新验签；
2. 核对状态最高版本、签名计划和版本文件中的计划身份；
3. 处理崩溃前留下的精确预留：状态已经落盘时完成提交，状态仍是旧版本时取消尚未生效的预留；
4. 核对通过后继续查询 DU，只有匹配的软件日历结果才能恢复 `active/applied` 显示。

状态缺失但版本文件已经记录版本、版本文件缺失但状态存在、上下文不一致、版本倒退或同版本 hash 冲突都会进入 fail-closed 状态。此时 CU-CP 不重新授权旧日历，也不把未核对的计划显示为可用。

## 6. 只读运行状态

`ntn_state` 增加以下只读信息：

| 字段 | 含义 |
|---|---|
| `signature_required` | 当前是否要求 schema v4 签名 |
| `trusted_signing_key_count` | 已加载的可信公钥数量 |
| `signature_status` | 当前计划的验签状态 |
| `signing_key_id` | 当前最高版本使用的密钥标识 |
| `signing_key_fingerprint` | 对应公钥的 SHA-256 指纹 |
| `version_anchor_configured` | 是否配置独立版本文件 |
| `version_anchor_mode` | 当前实现固定显示 `software_only` |
| `version_anchor_status` / `version_anchor_error` | 版本文件当前状态和稳定错误码 |
| `version_anchor_generation` / `version_anchor_hash` | 版本文件的更新代次和内容 hash |
| `version_anchor_catalog_version` / `version_anchor_schedule_version` | 已提交的最高版本 |
| `version_anchor_reserved_version` | 尚在持久化事务中的预留 schedule 版本 |

文件路径和详细系统错误保留在日志中，便于排障，同时避免把本地路径作为稳定 OAM 内容。

## 7. 兼容性与工程边界

- `require_signed_plan=false` 时，schema v1/v2/v3 继续按原有规则工作，未签名执行状态继续使用 schema v3；schema v1/v2 仍按 `assigned=visible` 解释。
- `require_signed_plan=true` 时只接受经过可信公钥验证的 schema v4，较早 schema 返回 `signature_required`。
- 功能默认关闭；关闭时不读取计划、状态、版本或公钥文件，terrestrial 默认路径保持原有行为。
- 本轮复用现有 F1AP、DU 和 MAC 软件日历接口，没有增加公共协议字段，也没有修改 PHY、RU/RF、Web/GIS 或 generated ASN.1。
- `applied` 表示匹配版本的软件访问日历已经安装。天线指向、硬件波束切换、PHY/RU/RF 执行和空口发送由后续设备接口与无线测试确认。
- `software_only` 版本文件可以发现状态文件单独回滚、单独缺失和普通崩溃中断。它位于同一软件和文件系统信任域，不能替代 HSM、TPM 或可信单调存储；状态文件与版本文件被协调回滚或同时删除时，软件无法识别其历史版本。
- 签名确认计划由相应私钥生成。私钥托管、证书生命周期、传输通道认证、设备安全启动和主机入侵防护属于部署安全体系。

## 8. 主要实现与测试位置

- Node 签名与验证：`utils/ntn/versioned_position_plan_v4.mjs`
- CU-CP 解析、验签和版本检查：`lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.*`
- 状态 schema v4：`lib/cu_cp/ntn_mobility/ntn_onboard_position_plan_state.*`
- 软件版本文件：`lib/cu_cp/ntn_mobility/ntn_plan_version_anchor.*`
- CU-CP 持久化、重启核对和 OAM：`lib/cu_cp/cu_cp_impl.*`
- 跨 Node/C++ 固定样例：`utils/ntn/testdata/versioned-position-plan-v4-*`
- 聚焦测试：`utils/ntn/versioned_position_plan_v4.test.mjs`、`tests/unittests/cu_cp/ntn_mobility/ntn_onboard_position_plan_test.cpp`、`ntn_plan_version_anchor_test.cpp`、`tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`

## 9. 本轮验证结果

- Node schema v3/v4、导出与重放测试：33 项通过；
- CU-CP 计划控制器、状态恢复和版本文件测试：89 项通过；
- O-CU-CP 配置、转换和默认关闭测试：4 项通过；
- CU-CP 签名日历安装、重启重新验签、状态回退拒绝和默认关闭场景：3 项通过；
- `ntn_mobility_test`、`srsran_cu_cp`、`cu_cp_unit_config_test` 和 `cu_cp_test` 目标均完成构建。

本轮没有改变 F1AP、DU、MAC、PHY、RU/RF、Web/GIS 或 generated ASN.1 接口，因此没有重复运行这些模块的完整测试，也没有执行真实射频测试。
