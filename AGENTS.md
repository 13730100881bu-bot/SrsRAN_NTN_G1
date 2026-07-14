# AGENTS.md

## Project role

This is a fork of srsRAN Project.

Current work focuses on NTN features for the CU-CP side of srsRAN, with
task-scoped DU/F1AP/MAC changes only when a requested feature truly requires
them.

Before editing, read:

- docs/ntn_cucp_agent_memory.md
- The user's active task/request

## Communication

默认用中文回复用户。代码标识、命令、日志、协议字段和文件路径保持原文，
不要为了翻译而改写技术名称。

## Hard boundary

Codex must not modify O-DU, flexible_o_du, DU, MAC scheduler, PHY, lower PHY, RU, RF, ZMQ, PRACH, HARQ timing, TA scheduler, or GIS-site code unless the task explicitly grants a non-CU-CP exception.

If a requested behavior cannot be implemented within CU-CP, stop and report the limitation instead of modifying non-CU-CP code.

## Existing worktree

This worktree may contain previous NTN changes. Do not revert unrelated changes.
If unrelated modified files appear while working, leave them alone unless the
user explicitly asks for cleanup.

## Git discipline

Keep future work reviewable. Start each task with `git status --short`, identify
the files you intend to touch, and avoid staging unrelated dirty files. Prefer
one focused commit per completed CUCP stage or coherent feature slice. Use
explicit `git add <paths>` rather than `git add .`, and mention focused
validation results in the final handoff.

## Validation efficiency

Validation should be evidence-driven, but it should not waste time by repeating
large builds after every small edit.

Use a staged validation ladder:

1. For ordinary edit/compile/test loops, run the smallest target that proves the
   changed code compiles, for example `cmake --build build/ai-clean --target
   <target> -j1`, followed by a focused `ctest -R` for the changed behavior.
2. Reconfigure only when CMake files, build options, generated build inputs, or
   the build directory changed, or when a previous build failure indicates
   configuration drift.
3. Run broader builds/tests at closeout only for high-risk tasks or broad
   interface changes. For documentation, command-output, or tightly scoped
   unit-test-only changes, focused build/test evidence is acceptable if the
   final answer clearly states what was skipped and why.
4. If a required broad command is known to be very slow or times out in the
   current environment, prefer targeted build targets and focused `ctest`
   evidence, then report the skipped or timed-out command honestly with the
   reason and residual risk.

Avoid duplicate validation:

- Do not rerun an unchanged full build just because another small source edit was
  made; rerun the affected target and focused tests first.
- Reuse fresh results from the current turn when the relevant files have not
  changed since that command completed.
- Prefer exact CMake targets over broad builds. Examples: `srsran_cu_cp`,
  `ntn_mobility_test`, `f1ap_cu_test`, `f1ap_du_test`, `mac_test`.
- Prefer focused test filters over repository-wide `ctest`.

Long-running compilation and test work should be delegated or isolated when
possible:

- If subagents are available, the main agent should keep code design and edits in
  the primary thread and delegate slow compile/test monitoring to subagents.
- Independent validations may run in parallel when they do not write the same
  build tree or otherwise contend for generated files.
- Do not leave build, test, or helper processes running when ending the turn.

## Build and test time budget

后续 agent 不应在编译和测试上无控制地消耗时间。默认采用“先小后大”的
验证策略，并把完整编译视为阶段性验证，而不是每次小改动后的默认动作。

默认预算：

- focused build 优先控制在 5-10 分钟以内。
- focused `ctest` 优先控制在 3-5 分钟以内。
- full build、full CU-CP tests 或大范围 `ctest` 只有在确实需要时才运行。

默认禁止的低效行为：

- 不要在每个小改动后运行完整 build。
- 不要在每个小改动后运行完整 `ctest`。
- 不要反复 reconfigure，除非修改了 CMake、构建选项、生成文件输入，
  或已有构建目录明显失效。
- 不要为了“更保险”重复运行刚刚已经证明过、且相关文件未再变化的命令。

优先选择：

- 编译最小相关 target，例如 `srsran_cu_cp`、`ntn_mobility_test`、
  `cu_cp_unit_config_test`、`f1ap_cu_test`、`f1ap_du_test`、`mac_test`。
- 运行 focused `ctest -R "<regex>"`。
- 对系统联调，优先运行 split demo 并用 `tshark` 检查 pcap。

只有以下情况才考虑完整 build 或大范围测试：

- 修改了跨模块公共接口。
- 修改了 CU-CP 与 F1AP/DU/MAC 的交互边界。
- 修改了 CMake 或构建结构。
- 准备阶段性提交/交付，且 focused 验证不足以证明风险可控。

如果确实需要跑大命令，先在工作说明中写明原因。若命令明显超时或卡住，
应停止并改用更小 target 或 focused test；最终回复必须说明跳过或失败的
验证项、原因和残余风险。

## NTN integration validation

后续 NTN 功能开发不能只依赖单元测试。凡是改动会影响运行态信令、UE
能力门控、RNTI 号码池、SR/SRS 资源下发、SIB19 广播、paging、handover、
F1AP/DU/MAC 交互或 CU-CP 观测状态时，都应考虑增加轻量联调验证。

默认联调基线是 split 运行流程：

- 运行脚本：`run_artifacts/srsran_runtime_capture/split/run_split_demo.ps1`
- 输出抓包：`run_artifacts/srsran_runtime_capture/split/split_live_sctp_latest.pcap`
- 优先用 `tshark` 自动检查 pcap，不要只依赖人工 Wireshark 观察。

基础联调至少应能看到这些信令阶段：

- NGAP setup
- E1 setup
- F1 setup
- RRC setup
- UE capability transfer
- Initial Context Setup
- PDU Session setup
- E1 bearer setup/modification
- F1 UE Context Modification
- RRC Reconfiguration

后续 NTN 功能若适合做系统级验证，应在这个 split 流程上追加 focused
检查点，例如：

- NTN UE capability gate。
- SIB19 assistance / DU broadcast application result。
- RNTI lease pool distribution and access validation。
- SR/SRS assignment application feedback。
- Analog access / digital service ownership transition。
- Beam-derived TAC and paging recommendation behavior。
- Connected handover target resource readiness。

不要恢复旧的 harness 流程作为主工作方式。联调验证应保持轻量、脚本化，
重点验证可观察信令和运行态状态。

运行联调时：

- 不要打开多个 GUI 窗口。
- 默认使用保存的 pcap 和 `tshark` 断言。
- 结束前必须清理 `srsue`、`srsdu`、`srscuup`、`srscucp` 和 `tcpdump`。

## Done means

A task is complete only when:

1. The change is inside CU-CP scope.
2. Relevant tests are added or updated when behavior changes.
3. Focused validation passes, or failures are clearly explained.
4. The final answer lists files changed, tests run, results, and remaining risks.
