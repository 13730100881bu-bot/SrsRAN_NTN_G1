# AI辅助srsRAN开发测评执行报告

日期：2026-05-07
样本：srsRAN NTN 多波束 / beam hopping
执行结论：本轮样本满足“AI辅助开发可行性样本”标准，但尚不足以形成统计型效率结论。

## 1. 样本证据

本轮观察到的主要代码范围：

| 文件 | 当前行数 | 作用 |
|---|---:|---|
| `include/srsran/ntn/beam_hopping_controller.h` | 150 | controller 接口、配置、notifier |
| `include/srsran/ntn/beam_hopping_table.h` | 93 | hopping table 数据结构 |
| `lib/ntn/beam_hopping_controller.cpp` | 169 | slot tick、SIB19 生成、TA 计算 |
| `lib/ntn/beam_hopping_table.cpp` | 115 | 三相位颜色交织 hopping table |
| `apps/units/flexible_o_du/split_helpers/flexible_o_du_ntn_configuration_manager_factory.cpp` | 499 | O-DU NTN 配置管理与 hopping session |
| `apps/units/flexible_o_du/split_helpers/commands/ntn_config_update_remote_command_factory.cpp` | 441 | 远程 JSON 命令解析 |
| `tests/unittests/ntn/ntn_multibeam_test.cpp` | 166 | NTN 多波束单测 |
| `tests/unittests/ntn/CMakeLists.txt` | 23 | `ntn_test` 目标 |

本轮用于测评的核心选择路径：
- NTN核心/测试路径：1,416 行。
- O-DU NTN集成路径：1,077 行。
- 两组合计：2,493 行。

## 2. 实际执行命令

构建与测试：

```bash
cd /mnt/d/code/srsRAN_Project-main/build
cmake --build . --target ntn_test -j2
ctest -R ntn_test --output-on-failure
```

测试清单：

```bash
cd /mnt/d/code/srsRAN_Project-main
./build/tests/unittests/ntn/ntn_test --gtest_list_tests
```

质量扫描：

```bash
rg -n "TODO|FIXME|XXX|HACK|srsran_assert|static_cast<uint16_t>|schedule_periodic_tick|hopping_tick_started" ...
```

环境检查：
- WSL 中存在 `/usr/bin/cmake` 和 `/usr/bin/ctest`
- 未发现 `clang-tidy`
- 未发现 `cppcheck`
- 未发现 `compile_commands.json`

## 3. 构建与测试结果

构建结果：
- `srsran_ntn` target 构建通过
- `ntn_test` target 构建通过

CTest 结果：
- `ntn_test`: Passed
- `1/1` tests passed
- CTest 报告测试时间：`0.02 sec`

gtest 结果：
- `4 tests from 4 test suites` 全部通过，gtest 总运行时间 `13 ms`

已覆盖用例：
- `ntn_beam_position_grid.assigns_hex_neighbours_with_different_colours`
- `ntn_beam_hopping_table.supports_three_phase_cycles_that_do_not_divide_sfn_wrap`
- `ntn_ta_calculator.computes_service_link_ta_for_nadir_beam`
- `ntn_beam_hopping_controller.schedules_next_dwell_when_tick_arrives_after_boundary`

## 4. 测试覆盖评价

已覆盖：
- hex beam grid 的邻接颜色约束。
- hopping table 的三相位颜色交织。
- `cycle_frames` 不整除 `NOF_SFNS` 的循环场景。
- nadir beam 的 service-link TA 计算。
- controller 在 tick 触发下生成后续 dwell 的 SIB19。

未覆盖：
- `ntn_config_update` 远程 JSON 解析的负向测试。
- beam hopping start/stop 的 O-DU 集成级测试。
- stop 后周期 tick 是否停止或降频。
- 多 cell、多 session 并发场景。
- 星历持续更新、slot 映射缺失和 DU 配置失败路径。
- SIB19 编码后的协议字段一致性检查。
- 空口/系统级运行验证。

## 5. 风险审查发现

### 发现 1：stop 后 1ms 周期 tick 仍可能持续自调度

位置：
- `apps/units/flexible_o_du/split_helpers/flexible_o_du_ntn_configuration_manager_factory.cpp`
- `schedule_periodic_tick()` 在 timer 回调中无条件再次调用自身。
- `on_periodic_tick()` 在没有 active session 时直接返回，但外层回调仍会继续 reschedule。
- `hopping_tick_started` 只置为 true，stop session 后未恢复 false。

影响：
- 不影响当前 `ntn_test` 通过。
- 长期运行时可能造成不必要的 1ms timer 唤醒。
- 属于通信软件中典型的实时性/资源风险。

建议：
- stop 最后一个 session 后取消 timer 或允许 `hopping_tick_started` 恢复。
- 增加单测或集成测试验证 stop 后不再周期 tick。

### 发现 2：远程命令解析缺少专项单测

位置：
- `apps/units/flexible_o_du/split_helpers/commands/ntn_config_update_remote_command_factory.cpp`

影响：
- 代码中已经有较完整的参数校验，但没有独立测试覆盖异常输入。
- AI 生成的参数校验容易出现“看起来全、实际缺边界”的问题。

建议：
- 增加 JSON parser 单测，覆盖：
  - `n_active = 0`
  - `n_active % 3 != 0`
  - `dwell_frames = 0`
  - `n_active * dwell_frames > 1024`
  - 经纬度反向、越界、极区附近
  - beam hopping 搭配 orbital ephemeris 时应失败

### 发现 3：controller 的 late tick 行为需要更明确的设计断言

位置：
- `lib/ntn/beam_hopping_controller.cpp`

影响：
- 当前测试覆盖了提前调度下一 dwell 的路径。
- 如果周期 tick 丢失或首次后续 tick 已经越过边界，当前逻辑可能直接调度再下一个 dwell。
- 这可能是设计选择，也可能是 missed boundary 风险，需要在设计说明和测试里明确。

建议：
- 增加“错过边界后的第一次 tick”用例。
- 明确策略：补发当前 dwell、调度下一 dwell，还是上报 warning。

## 6. AI作用评分

| 维度 | 分数 | 评价 |
|---|---:|---|
| 代码理解 | 18/20 | 能快速定位 NTN、O-DU、远程命令、CMake 和单测入口之间的关系 |
| 实现辅助 | 15/20 | 适合生成表结构、配置 glue code、参数校验和 controller 骨架，但实时生命周期仍需人工审查 |
| 测试补齐 | 14/20 | 已覆盖核心算法路径，但 parser、stop、并发和集成路径不足 |
| 调试验证 | 14/15 | 能找到 WSL 构建环境并复跑 target/CTest |
| 风险识别 | 12/15 | 识别出 timer 生命周期、parser 单测缺口和 late tick 行为风险 |
| 落地可控性 | 8/10 | 已能形成 CI + 单测 + 风险清单闭环，仍需系统级验证和专家复核 |

综合评分：81/100

## 7. 结论

本轮实际测评说明：
- AI 对代码理解、任务拆解、测试点枚举和验证路径定位帮助明显。
- AI 能把通信软件里的部分工程约束转成可执行单测。
- AI 容易漏掉长期运行生命周期、实时调度和协议语义层面的深层风险。
- 当前样本可作为“AI辅助开发可行性样本”，不能作为“AI效率提升百分比”的最终统计依据。

建议采用的落地方式：
- AI 用于探索、草拟、补测和审查清单。
- CI/单测用于防止基础回归。
- 通信专家负责协议语义、实时性和系统场景最终把关。

## 8. 下一步实际任务卡

优先级 P0：
- 修复或明确 stop 后周期 tick 的生命周期策略。
- 为 `ntn_config_update` parser 增加负向单测。

优先级 P1：
- 增加 late tick / missed boundary controller 单测。
- 增加 beam hopping start/stop 的 O-DU mock 集成测试。

优先级 P2：
- 补充 SIB19 编码字段一致性检查。
- 增加多 cell、多 session 和星历连续更新场景。
