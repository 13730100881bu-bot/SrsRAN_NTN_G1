# AI辅助NTN功能迭代统计

记录时间：2026-05-07 23:58:34 - 2026-05-09 22:12:00 CST
项目：`d:\code\srsRAN_Project-main`
主题：srsRAN NTN beam hopping、多波束与 feeder link Doppler 功能增强

## 1. 连续六轮代码改动

| 轮次 | Git提交 | 改动主题 | 文件 | 插入 | 删除 | 单测变化 |
|---|---|---|---:|---:|---:|---|
| 第1轮 | `6a97eb7` | 强化beam hopping调度：按`valid_from slot`去重；停止空闲周期tick | 4 | 91 | 7 | 4 -> 6 |
| 第2轮 | `19ffb83` | 星历更新刷新待发送dwell：同一`valid_from`但星历版本变化时重新生成SIB19 | 3 | 52 | 5 | 6 -> 7 |
| 第3轮 | `e8aac66` | 补齐`ntn_config_update`远程JSON命令正向、停止和负向校验单测 | 2 | 228 | 2 | 7 -> 11 |
| 第4轮 | `e797e1f` | 实现`feeder_link_info`解析与RU CFO Doppler补偿下发 | 5 | 517 | 3 | 11 -> 15 |
| 第5轮 | `cc6e2af` | 实现多波束dwell set：同色非相邻波束并发调度，聚合SIB19覆盖中心和半径 | 9 | 302 | 54 | 15 -> 18 |
| 第6轮 | `5ba5992` | 优化多波束聚合：球面参考点、经度环绕距离和跨±180°经线回归测试 | 3 | 220 | 41 | 18 -> 19 |
| 合计（提交口径） | - | 最近六轮NTN功能增强和测试闭环 | 11个文件触达 | 1410 | 112 | 4 -> 19 |

补充口径：相对仓库初始化提交，当前代码净态为11个文件、`+1399/-101`。

## 2. 本轮功能结果

- `beam_hopping`远程JSON支持`nof_beams_per_dwell`，默认保持1，兼容原单波束配置。
- hopping table支持每个dwell激活一组同色波束；同色组在当前六边形网格下互不相邻，降低同时照射的邻区干扰风险。
- controller从单个`target_beam`扩展为`active_beam_set_t`，为每个dwell生成多波束聚合SIB19：参考点取活动波束中心的球面向量均值，`distanceThresh`覆盖所有活动波束足迹。
- 本轮修正了跨±180°经线场景：参考点不会被经纬度算术平均错误拉到0°经线，地面距离计算也会按最短经度差处理。
- 保留原SIB19单`ref_location`限制：当前实现是控制面多波束簇方案，真实RU/precoder多波束发射仍是下一阶段。
- 新增表生成、聚合SIB19、跨经线聚合、远程解析和负向校验单测。

## 3. 验证结果

| 验证项 | 命令 | 结果 | 用时 |
|---|---|---|---:|
| NTN核心库增量构建 | `gmake -f lib/ntn/CMakeFiles/srsran_ntn.dir/build.make lib/ntn/libsrsran_ntn.a -j2` | 通过 | 5.57 s |
| 完整手工NTN测试二进制 | 手工编译缺失测试对象并链接`ntn_test_manual` | 通过 | 24.23 s |
| 完整gtest运行 | `/tmp/ntn_manual_v5/ntn_test_manual` | `19 tests from 6 test suites`，`19 PASSED` | 47 ms |
| 旧生成规则目标构建 | `gmake -f tests/unittests/ntn/CMakeFiles/ntn_test.dir/build.make tests/unittests/ntn/ntn_test -j2` | 通过 | 84.63 s |
| 旧生成规则gtest | `./tests/unittests/ntn/ntn_test` | `16 tests from 5 test suites`，`16 PASSED` | 44 ms |
| CTest复跑 | `ctest -R ntn_test --output-on-failure` | `1/1 Passed`，CTest报告`0.06 sec` | 14.07 s wall |

说明：`cmake -S . -B build`在当前WSL挂载盘上仍会卡住，因此本轮继续采用手工完整二进制验证新增CMake目标内容，并用旧生成规则复跑已有CTest入口。

## 4. 效率口径

- 最近六轮均为小范围、可复跑、可提交的增量闭环，代码提交和文档提交分离。
- 测试数量从4个扩展到19个，新增6个controller/table行为测试、6个远程命令解析测试和3个O-DU/RU manager测试。
- 六轮提交口径新增净代码`+1298`行，关闭了七类实际风险：空闲tick生命周期、重复beam dwell去重、同一dwell星历刷新、远程JSON异常输入拦截、feeder link Doppler未下发到RU、多波束只能串行单beam调度、跨±180°经线聚合错误。
- 没有人工对照组，因此不写“AI提升百分比”；当前采用“闭环耗时、测试通过率、缺陷闭环数、改动集中度”作为效率证据。

## 5. 当前剩余风险

- O-DU start/stop 长时间运行链路仍未覆盖，只验证了配置管理器和RU CFO mock路径。
- SIB19字段语义和真实gNB/O-DU运行链路仍需协议专家和系统级验证。
- 当前Doppler模型采用网关-卫星径向速度的一阶补偿和恒速近似，后续需要结合真实星历传播、RU符号时钟和空口链路做系统级校准。
- 当前多波束方案受SIB19单`ref_location`限制，采用聚合覆盖表达；真实多波束发射还需要接入RU beamforming/precoder控制链路。
