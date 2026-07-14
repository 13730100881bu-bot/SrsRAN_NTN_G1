# srsRAN/OCUDU 干净代码基线对比报告

日期：2026-05-07
目的：下载一套干净代码作为基线，对比当前工程中的 NTN / beam hopping 改动，提升 AI 辅助编程测评的证据强度。

## 1. 下载的基线代码

| 基线 | 来源 | 分支 | commit | 本地路径 |
|---|---|---|---|---|
| srsRAN Project 归档主线 | `https://github.com/srsran/srsRAN_Project.git` | `main` | `4bf1543` | `.analysis_baselines/srsran_project_main_4bf1543` |
| OCUDU 当前主线 | `https://gitlab.com/ocudu/ocudu.git` | `main` | `0e4a114` | `.analysis_baselines/ocudu_main_0e4a114` |
| OCUDU NTN参考分支 | `https://gitlab.com/ocudu/ocudu.git` | `pr_add_ntn_4` | `df8cfef` | `.analysis_baselines/ocudu_pr_add_ntn_4_df8cfef` |

说明：
- 当前 srsRAN Project README 显示项目已迁移至 OCUDU。
- GitHub `srsran/srsRAN_Project` 已归档，因此本轮同时使用 srsRAN 归档主线和 OCUDU 主线作为基线。
- OCUDU `pr_add_ntn_4` 不是主线，只作为 NTN 方向参考。

## 2. NTN 核心目录对比

当前工程：

```text
include/srsran/ntn/*.h + lib/ntn/*.cpp + lib/ntn/CMakeLists.txt + tests/unittests/ntn/*
合计 1,416 行
```

srsRAN Project 归档主线：

```text
include/srsran/ntn/ntn_configuration_manager.h
include/srsran/ntn/ntn_configuration_manager_config.h
合计 115 行
```

OCUDU 当前主线：

```text
include/ocudu/ntn/ntn_cell_params.h
include/ocudu/ntn/ntn_configuration_manager.h
include/ocudu/ntn/ntn_configuration_manager_config.h
合计 181 行
```

OCUDU `pr_add_ntn_4`：

```text
NTN 相关选择路径源码和测试合计 4,548 行，其中 `tests/unittests/ntn` 为 1,842 行
方向集中在 orbital/ephemeris/assistance-info/converter/propagator
未发现 beam_hopping、beam_position、dwell_frames、n_active 等多波束 hopping 关键词
```

结论：
- 当前工程相对 srsRAN 归档主线新增了完整的 beam position、TA calculator、beam hopping table/controller 和 NTN 单测。
- 当前工程相对 OCUDU 主线也包含主线没有的 beam hopping 能力。
- 当前工程与 OCUDU NTN 参考分支方向不同：当前偏多波束 hopping，参考分支偏轨道传播和 assistance info 生成。

## 3. flexible O-DU 集成对比

对比文件：
- `flexible_o_du_ntn_configuration_manager_factory.*`
- `commands/ntn_config_update_remote_command_factory.*`
- `split_helpers/CMakeLists.txt`

| 代码集 | 行数 | 说明 |
|---|---:|---|
| 当前工程 | 1,077 | 远程命令解析、beam hopping start/stop、SIB19 update、周期 tick |
| srsRAN 归档主线 | 210 | 基础配置管理骨架 |
| OCUDU 当前主线 | 122 | OCUDU 命名空间下的基础骨架 |

差异摘要：
- 当前工程相对 srsRAN 归档主线，在 flexible O-DU NTN glue code 中 `git diff --no-index --shortstat` 结果为 `3 files changed, 868 insertions(+), 1 deletion(-)`，净增 867 行。
- 新增重点包括：
  - `beam_hopping` JSON 对象解析
  - `stop_beam_hopping` 命令
  - `n_active / dwell_frames / grid` 参数校验
  - ECEF 星历要求
  - hopping session 管理
  - 周期 tick 驱动 SIB19 更新

## 4. 测试对比

当前工程：
- `tests/unittests/ntn/ntn_multibeam_test.cpp`
- `tests/unittests/ntn/CMakeLists.txt`
- `ntn_test` 已接入 CTest
- gtest 覆盖 4 个用例：
  - beam grid 邻接颜色约束
  - hopping table 三相位循环
  - TA calculator nadir beam
  - controller dwell 边界调度

srsRAN 归档主线：
- 未发现 `tests/unittests/ntn`。

OCUDU 当前主线：
- 未发现 `tests/unittests/ntn`。

OCUDU `pr_add_ntn_4`：
- 有较多 NTN 测试，覆盖 coordinate converter、ephemeris converter、assistance info generator、math helpers、reference frame converter、RK4 propagator。
- 未覆盖 beam hopping 多波束表和 controller。

## 5. 对 AI 测评结论的影响

对比前的结论：
- AI 对代码理解、测试补齐和验证路径定位帮助明显。
- 风险集中在实时生命周期、协议语义和系统级验证。

对比后的增强结论：
- 本轮样本不是“局部小修”，而是在上游 NTN 骨架基础上扩展出一条新的 beam hopping 功能链路。
- AI 的价值更体现在跨模块串联：远程命令、O-DU 配置管理、核心算法、SIB19 更新、单测接入。
- 上游基线对比也暴露了测试缺口：当前 beam hopping 测试是新增的，但还缺 parser 负向测试、start/stop 集成测试、周期 tick 生命周期测试和系统级验证。

## 6. 建议

P0：
- 修复或明确 stop 后周期 tick 生命周期策略。
- 补 `ntn_config_update` parser 负向测试。

P1：
- 增加 start/stop 的 O-DU mock 集成测试。
- 增加 missed boundary / late tick 测试。

P2：
- 参考 OCUDU `pr_add_ntn_4` 的 NTN 测试组织方式，补充 converter/assistance-info/SIB19 编码一致性测试。
- 未来如迁移到 OCUDU 主线，需要先处理命名空间、许可、接口和 NTN 模型差异。
