# NTN CU-CP Task Change Index

Last updated: 2026-07-19

This file maps the CUCP task chain to the main feature changes and
representative code areas. It is a compact lookup table for future agents.

Git provenance note: the maintained change series is rooted at the official
`srsran/srsRAN_Project` commit
`4bf1543936d062686d64c10724d2f27a9854f065`. This file reconstructs the
task-to-feature relationship; it is not a claim that every historical task maps
one-to-one to a commit. Use it to find the right feature area, then use
`git log -- <path>` and inspect the listed code/tests for the exact implementation.

## How To Use

- Do not read archived task cards by default.
- Use this file when you need to answer "which task introduced this concept?" or
  "where should I look for this feature?"
- Open the source/test files listed under a task before modifying behavior.
- CUCP-000 to CUCP-003 are bootstrap/history, not the practical start of the
  current feature sequence.

## Current High-Level Code Groups

CU-CP contracts and status:

- `include/srsran/cu_cp/cu_cp_command_handler.h`
- `include/srsran/cu_cp/ntn_location.h`
- `include/srsran/cu_cp/ntn_beam_service_resources.h`
- `include/srsran/cu_cp/ntn_qos_policy.h`
- `include/srsran/cu_cp/ntn_service_switch_over.h`
- `include/srsran/cu_cp/ntn_ue_capability.h`

CU-CP implementation:

- `lib/cu_cp/cu_cp_impl.cpp`
- `lib/cu_cp/cu_cp_impl.h`
- `lib/cu_cp/ntn_mobility/`
- `lib/cu_cp/cell_meas_manager/`
- `lib/cu_cp/paging/`
- `lib/cu_cp/routines/mobility/`
- `lib/cu_cp/routines/ue_context_release_routine.*`

O-CU-CP observability:

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

Representative CU-CP tests:

- `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
- `tests/unittests/cu_cp/cu_cp_paging_test.cpp`
- `tests/unittests/cu_cp/ntn_mobility/`
- `tests/unittests/cu_cp/cu_cp_test_environment.*`

Task-scoped CU/DU coordination areas used by later resource tasks:

- `include/srsran/f1ap/**`
- `lib/f1ap/**`
- `include/srsran/du/**`
- `lib/du/**`
- `include/srsran/mac/**`
- `lib/mac/**`
- `tests/unittests/f1ap/**`
- `tests/unittests/du_manager/**`
- `tests/unittests/mac/**`

Treat those non-CU-CP areas as exact, feature-driven surfaces. Do not expand
them casually.

## Task-To-Change Map

| Task | Feature / design change | Representative implementation and tests |
|---|---|---|
| CUCP-000 | Imported older goal-mode diff for review. Historical only. | Archived task card only. |
| CUCP-001 | CU-CP NTN code map and scope inventory. Historical only. | `docs/cucp_analysis.md`, archived code-map notes. |
| CUCP-002 | Early CU-CP NTN config validation baseline. Historical bootstrap. | O-CU-CP config translator/validator tests; older run summaries. |
| CUCP-003 | Early NTN mobility runtime contract. Historical bootstrap. | Early runtime contract notes and CU-CP config/runtime files. |
| CUCP-004 | Established project baseline and task sequence. Practical start of the current chain. | Mostly documentation and guard scripts; do not continue expanding this area for normal feature work. |
| CUCP-005 | Runtime taxonomy: candidate, active, loaded, draining, assistance, UE runtime context. | `include/srsran/cu_cp/ntn_location.h`, `include/srsran/cu_cp/cu_cp_command_handler.h`, `lib/cu_cp/cu_cp_impl.*`. |
| CUCP-006 | Full candidate inventory. Candidate beams are never capped by loaded/served limits. | `lib/cu_cp/cell_meas_manager/`, `lib/cu_cp/ntn_mobility/ntn_beam_assignment_repository.cpp`, `tests/unittests/cu_cp/cell_meas_manager/`. |
| CUCP-007 | Demand-driven loaded service calendar and SR/SRS intent. Empty candidate beams do not consume service resources. | `lib/cu_cp/ntn_mobility/ntn_served_beam_scheduler.*`, `ntn_served_beam_selector.*`, `cu_cp_impl.cpp`, scheduler-focused NTN tests. |
| CUCP-008 | CU-CP NTN assistance snapshot: ephemeris, TA, Koffset, Kmac, UL sync, t-Service. | `lib/cu_cp/ntn_mobility/ntn_assistance_snapshot_generator.*`, `tests/unittests/cu_cp/ntn_mobility/ntn_assistance_snapshot_test.cpp`. |
| CUCP-009 | Admission and mobility gates using NTN runtime state. Stale/draining/candidate policy starts driving UE setup, reestablishment, HO, PDU demand. | `lib/cu_cp/cu_cp_impl.cpp`, `lib/cu_cp/mobility_manager/`, `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`. |
| CUCP-010 | NGAP/core mapping: mapped cell, derived TAC, TAI, location reporting. | `include/srsran/cu_cp/ntn_location.h`, `lib/ngap/ngap_asn1_helpers.h`, `lib/cu_cp/cu_cp_impl.cpp`, NGAP/CU-CP tests. |
| CUCP-011 | Service/feeder switch-over resilience: soft/hard switch-over, draining, safe degradation. | `include/srsran/cu_cp/ntn_service_switch_over.h`, `lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.*`, related tests. |
| CUCP-012 | NTN observability and examples for state/assistance/beams/UEs. | `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`, config/example files, O-CU-CP command tests. |
| CUCP-013 | RRC/SIB19 assistance packaging contract, ASN.1 pack/unpack verified, no real DU SI scheduling yet. | `lib/cu_cp/ntn_mobility/ntn_sib19_assistance_builder.*`, `tests/unittests/cu_cp/ntn_mobility/ntn_sib19_assistance_test.cpp`. |
| CUCP-014 | QoS-aware service policy: ARP/5QI/GBR/slice prioritization for placement, service admission, and observability. | `include/srsran/cu_cp/ntn_qos_policy.h`, `lib/cu_cp/ntn_mobility/ntn_qos_policy.*`, `ntn_beam_placement_planner.*`, QoS tests. |
| CUCP-015 | Beam-derived TAC and paging/idle assistance. Beam ID suffix maps to 24-bit TAC. | `lib/cu_cp/ntn_mobility/ntn_beam_tac.*`, `lib/cu_cp/paging/`, `tests/unittests/cu_cp/cu_cp_paging_test.cpp`, `ntn_beam_tac_test.cpp`. |
| CUCP-016 | Service-area and paging observability: TAC validity, paging recommendation eligibility/reasons. | `include/srsran/cu_cp/cu_cp_command_handler.h`, `lib/cu_cp/cu_cp_impl.cpp`, O-CU-CP `ntn_state` / `ntn_beams` tests. |
| CUCP-017 | Two-level analog/digital hex beam model. Default LEO profile: 843 digital beams, 137 analog beams. | `utils/ntn/generate_leo_beam_table.py`, `configs/leo_500km_beam_table.json`, `configs/leo_500km_cucp_ntn.yml`, beam table parser/generator tests. |
| CUCP-018 | Access DU assignment policy. Analog access DU selected first; same-DU digital service preferred. | `lib/cu_cp/ntn_mobility/ntn_beam_placement_planner.*`, `lib/cu_cp/cu_cp_impl.cpp`, placement planner and O-CU-CP tests. |
| CUCP-019 | Pre-service inter-DU relocation for signaling-only UEs before first service demand. | `lib/cu_cp/cu_cp_impl.cpp`, `lib/cu_cp/routines/mobility/`, UE runtime/status fields, CU-CP mobility tests. |
| CUCP-020 | Connected beam-to-beam mobility policy: location-driven target preload before existing HO routine. | `lib/cu_cp/cu_cp_impl.cpp`, `lib/cu_cp/routines/mobility/intra_cu_handover_routine.cpp`, `cell_meas_manager` NTN helpers, mobility tests. |
| CUCP-021 | Access/service layer contract. RRC setup creates analog access only; first PDU/DRB binds digital service. | `include/srsran/cu_cp/cu_cp_command_handler.h`, `lib/cu_cp/cu_cp_impl.cpp`, `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`. |
| CUCP-022 | Resource-domain guard policy: analog/digital caps, reuse groups, explicit conflicts, blocked reasons. | `include/srsran/cu_cp/ntn_location.h`, `lib/cu_cp/cell_meas_manager/ntn_beam_table_json.cpp`, `ntn_beam_placement_planner.*`, resource tests. |
| CUCP-023 | Analog access release after ICS and digital service ownership. Control-only UE state. | `include/srsran/cu_cp/ntn_beam_service_resources.h`, `lib/cu_cp/ntn_mobility/ntn_beam_service_resource_manager.*`, `cu_cp_impl.cpp`, PDU/session tests. |
| CUCP-024 | Beam service resource manager: analog C-RNTI ownership contract and digital SR/SRS intent snapshot. | `lib/cu_cp/ntn_mobility/ntn_beam_service_resource_manager.*`, `tests/unittests/cu_cp/ntn_mobility/ntn_beam_service_resource_manager_test.cpp`. |
| CUCP-025 | CU-CP-authoritative real RNTI and digital SR/SRS architecture. DU/MAC execute; with NTN inactive, terrestrial selection/default outcomes remain while synchronization overhead is outside the compatibility claim. | `include/srsran/f1ap/ntn_ul_slot_resource_request.h`, F1AP/DU/MAC resource interfaces, resource manager tests. |
| CUCP-026 | RNTI lease pool distribution through private F1AP resource coordination before access. | `include/srsran/f1ap/ntn_rnti_lease_pool.h`, `include/srsran/f1ap/cu_cp/f1ap_cu_resource_coordination.h`, `lib/f1ap/*/gnb_du_resource_coordination*`, F1AP CU/DU tests. |
| CUCP-027 | RNTI lease lifecycle and access readiness: reserved, sent, applied, offered, initial UL, committed, released, expired, conflict. | `lib/cu_cp/ntn_mobility/ntn_beam_service_resource_manager.*`, `lib/mac/rnti_manager.h`, `lib/mac/mac_sched/mac_rach_handler.cpp`, MAC/RNTI tests. |
| CUCP-028 | SR/SRS application feedback. DU applied/rejected response gates service-bound state; scheduler consistency tested. | `include/srsran/f1ap/ntn_ul_slot_resource_request.h`, F1AP UE context setup/modification, DU PUCCH/SRS managers, scheduler SR/SRS tests. |
| CUCP-029 | Connected HO target resource reservation. Target C-RNTI and target SR/SRS must be ready before RRC HO command. | `lib/cu_cp/cu_cp_impl.cpp`, resource manager handover APIs, F1AP/DU UE context target paths, CU-CP/F1AP tests. |
| CUCP-030 | Resource consistency auditor. CU-CP queries DU NTN resource snapshots and generates repair actions. | `lib/cu_cp/ntn_mobility/ntn_beam_service_resource_manager.*`, F1AP private audit payloads, DU/MAC resource snapshot APIs, audit tests. |
| CUCP-031 | Repair executor and guarded recovery: resend, apply, clear, rollback, conflict block. | `lib/cu_cp/cu_cp_impl.cpp`, resource manager repair state, F1AP feedback paths, repair tests. |
| CUCP-032 | SIB19 DU SI broadcast application. CU-CP sends active/candidate dynamic SIB19 updates and clears draining/stale updates. | `lib/cu_cp/ntn_mobility/ntn_sib19_broadcast_controller.*`, F1AP resource coordination, DU SI update, MAC SIB PDU assembler tests. |
| CUCP-033 | UE NTN capability gate. `nonTerrestrialNetwork-r17` required for UE-specific NTN service/HO/release hints. | `include/srsran/cu_cp/ntn_ue_capability.h`, `lib/cu_cp/ntn_mobility/ntn_ue_capability_gate.*`, CU-CP capability tests. |
| CUCP-034 | UE capability profile policy for `leo_ngso`: NGSO/both/implicit-both match, GSO-only is supported but profile-blocked. | `include/srsran/cu_cp/ntn_ue_capability.h`, `lib/cu_cp/ntn_mobility/ntn_ue_capability_gate.cpp`, `lib/cu_cp/cu_cp_impl.cpp`, O-CU-CP status tests. |
| CUCP-035 | Versioned management-center onboard L1 plan: complete inventory validation, explicit two-cell NCI/PCI profile, deterministic partition, configurable access-calendar dry-run, timer-safe atomic activation and synchronized read-only OAM. Independent opt-in profile; no RF application. | `lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.*`, `lib/cu_cp/cu_cp_impl.*`, `include/srsran/cu_cp/cu_cp_configuration.h`, `include/srsran/cu_cp/cu_cp_command_handler.h`, O-CU-CP config/`ntn_state`, `tests/unittests/cu_cp/ntn_mobility/ntn_*position_plan*`, `ntn_access_calendar_audit_test.cpp`. |
| CUCP-036 | Versioned access-calendar cross-layer deployment: private F1AP prepare/query/clear, same-DU two-cell validation, both-slot-thread armed barrier, configurable prepare/apply deadlines, persistent rollback cleanup, extended validity, `mu=4`-exact wall-clock mapping, execution-envelope startup validation, opt-in scheduler SSB/PRACH software gate, applied/clear feedback and Web candidate plan exporter backed by a versioned explicit identity registry. Default off. Evidence explicitly stops before position/port beam steering and RF application. | `include/srsran/f1ap/ntn_access_calendar.h`, F1AP resource-coordination procedures, `du_manager_impl.*`, `include/srsran/mac/mac_manager.h`, `lib/mac/mac_impl.h`, `lib/mac/mac_ntn_access_calendar_manager.h`, `lib/mac/mac_ntn_access_calendar_compiler.h`, `lib/mac/mac_dl/mac_cell_time_mapper_impl.*`, `include/srsran/scheduler/ntn_access_calendar.h`, `lib/scheduler/ntn_access_calendar_gate.h`, `cell_scheduler.*`, `lib/cu_cp/cu_cp_impl.*`, `web_replicas/ntn_beam_planner/app/position-plan-model.ts`, `app/onboard-cell-identity-registry.json`, `docs/ntn_access_calendar_cross_layer_execution.md`, focused F1AP/DU/MAC/scheduler/CU-CP/Web tests. |
| CUCP-037 | Initial UL active-plan audit contract and activation-integrity hardening. RNTI lease ownership is cell-scoped even when onboard cells reuse PCI, and duplicate entries in one batch fail atomically; deployment feedback cannot regress; prepare/query feedback must preserve the complete intent count. A private pure auditor matches proposed satellite/version/hash, stable NCI/PCI, L1 owner, PRACH occasion and UL port against the current active plan. No production Initial UL transport or RF evidence is claimed. | `lib/cu_cp/ntn_mobility/ntn_beam_service_resource_manager.*`, `lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.*`, `lib/cu_cp/cu_cp_impl.cpp`, CU-CP test environment, `ntn_beam_service_resource_manager_test.cpp`, `ntn_onboard_position_plan_test.cpp`, `cu_cp_ntn_mobility_test.cpp`, roadmap/catalog/runtime-contract and access-plan documentation. |
| CUCP-038 | Fail-safe DU resource-audit completeness and RNTI lifecycle reconciliation. Private codec v2 independently qualifies RNTI and UE-slot snapshots; v1 is incomplete by default. MAC retains pending/consumed/expired leases with enforced expiry; exact-generation/full-set ACK validation, `ack_unknown` same-generation recovery, unresolved-pool generation gating, recoverable audit conflicts and indexed snapshot lookup close the CU/DU software-state loop. Same-DU C-RNTI values remain unique, SR/SRS repair records the DU-applied request, and SIB19 feedback is generation/state guarded. UE-slot mapping and durable terminal GC/reuse remain incomplete. This is not endurance or RF evidence. | Implementation commits `465e8c6` and `2ad0b1b`. Representative areas: `include/srsran/f1ap/ntn_rnti_lease_pool.h`, `include/srsran/mac/mac_manager.h`, `lib/mac/rnti_manager.h`, `lib/du/du_high/du_manager/du_manager_impl.cpp`, `lib/cu_cp/ntn_mobility/ntn_beam_service_resource_manager.*`, `lib/cu_cp/cu_cp_impl.*`, focused F1AP/DU/MAC/CU-CP tests and these NTN documents. |
| CUCP-039 | Durable onboard-plan restart recovery. Execution mode requires a private `state_file` that is atomically replaced with version high-water marks, active/pending plans, stable two-cell partition, hashes, activation state, lower-layer software deployment state and exact cleanup obligations. Restart revalidates the saved plan and queries DU before exposing `active/applied`; partial or mismatched feedback fails closed. Expired deployments retain cleanup work without clearing a different valid fallback. Read-only OAM reports storage and recovery health. Default-off terrestrial behavior is unchanged, and `applied` remains software-gate evidence rather than RF evidence. A trusted monotonic anchor for whole-file rollback/deletion and DU connection-generation binding remain follow-up work. | `lib/cu_cp/ntn_mobility/ntn_onboard_position_plan_state.*`, `ntn_onboard_position_plan.*`, `lib/cu_cp/cu_cp_impl.*`, private CU-CP configuration and `ntn_state`, focused state/controller/CU-CP/config tests, runtime-contract and memory documentation. |
| CUCP-040 | DU reconnect-safe onboard-plan recovery. CU-CP binds prepare completions to the DU connection generation and exact plan, while query/clear also carry a request-instance guard. It hides application evidence immediately on disconnect, persists the recovery state, and accepts it again only after a matching response from the live connection. A future prepared plan cannot clear the current plan before `activation_epoch`; early confirmation keeps the new plan hidden as pending, and the normal path switches and cleans up only after the epoch. If that pending plan expires before a delayed timer runs, the historical fallback returns to live-DU reconciliation. Read-only `ntn_state` exposes the active calendar hash, cleanup queue head and state-storage health. Expired `not_sent` plans do not generate clears. A confirmed clear is removed from the live queue only after a successful durable state write; write failure leaves the obligation unchanged in fail-closed memory, and a durably removed clear is not repeated after restart. State schema v2 independently retains the complete latest parsed candidate inventory, including a rejected 257-position input, across cleanup and repeated restart without promoting it or changing accepted version high-water; schema v1 remains readable. Default-off terrestrial behavior is unchanged; this is CU/DU software-state evidence, not RF evidence. | `lib/cu_cp/du_processor/du_processor_repository.cpp`, `lib/cu_cp/cu_cp_impl*`, `lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.*`, `lib/cu_cp/ntn_mobility/ntn_onboard_position_plan_state.*`, `include/srsran/cu_cp/cu_cp_command_handler.h`, O-CU-CP `ntn_state`, focused controller/state/CU-CP/config tests. |
| CUCP-041 | Historical-plan fallback lifecycle convergence. When a future plan was confirmed early, then superseded by a newer checked plan, failure of that newer deployment now returns the historical plan to live-DU verification instead of leaving it hidden until process restart. The recovery target, accepted-version high-water and the complete most recently received candidate inventory survive a state save/load round trip. A rejected lower-version replay may update the read-only “last received input” observation, but it cannot change the accepted high-water or active plan. No public protocol, F1 payload, DU/MAC/PHY/RU/RF, Web/GIS or generated ASN.1 change is involved. | `lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.cpp`, `tests/unittests/cu_cp/ntn_mobility/ntn_onboard_position_plan_test.cpp`, this index and agent memory. |

## Current Useful Validation Notes

CUCP-035 focused evidence (WSL build tree):

```bash
cmake --build build/ai-clean --target ntn_mobility_test -j1
ctest --test-dir build/ai-clean -R '^(ntn_onboard_position_plan|ntn_access_calendar_audit)\.' --output-on-failure
```

The position-plan/calendar suites passed 21/21. The current `cu_cp_impl.cpp.o`, four
O-CU-CP config objects and `cu_cp_unit_config_test.cpp.o` compiled successfully;
after relinking the current archives/test binary, the focused terrestrial-default,
independent-profile, two-identity validation and `ntn_state` tests passed 4/4.
An earlier full `-L ntn_mobility` run passed 147/153; its six failures were pre-existing
placement/rebalance policy expectations outside CUCP-035. Broad `srsran_cu_cp` and
dependency-heavy config-target builds exceeded the staged time budget, so closeout uses
the current object/archive plus focused-test evidence.

CUCP-034 focused evidence:

```bash
cmake --build build/ai-clean --target ntn_mobility_test -j 1
cmake --build build/ai-clean --target cu_cp_unit_config_test -j 1
cmake --build build/ai-clean --target srsran_cu_cp -j 1
bash -lc "cd /mnt/d/code/srsRAN_Project-main && ctest --test-dir build/ai-clean -R 'ntn_ue_capability_gate|cu_cp_ntn_mobility_test.unsupported_ntn_ue_capability_blocks_first_digital_service_binding|cu_cp_unit_config.ntn_(state|ues)' --output-on-failure"
```

The focused CTest run passed 12/12.

Direct Windows `ctest --test-dir build/ai-clean` may fail if CTest generated
include files refer to `/mnt/d/...`; run ctest through WSL in that case.

CUCP-036 closeout evidence (2026-07-14):

```bash
ctest --test-dir build/ai-clean -R '^(ntn_onboard_position_plan|ntn_access_calendar_audit)\.' --output-on-failure
cmake --build build/ai-clean --target mac_ntn_access_calendar_compiler_test -j1
ctest --test-dir build/ai-clean -R '^mac_ntn_access_calendar_compiler_test$' --output-on-failure
cd web_replicas/ntn_beam_planner
node --import tsx --test tests/satellite-cell.test.mjs tests/position-plan-export.test.mjs
npm run build
```

The current position-plan and calendar-audit suites passed 26/26, and the official
MAC calendar compiler target passed 7/7. Standalone focused binaries using the current
sources passed the scheduler gate 10/10, MAC two-cell manager 3/3, nanosecond slot mapper
51/51, F1 CU resource-coordination 12/12 and F1 DU resource-coordination 6/6. The Web
identity/plan suites passed 14/14; focused ESLint, targeted TypeScript and the production
build also passed. The registry contains 3528 satellites and 7056 unique opaque 36-bit
NCIs, with SHA-256
`7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a`.

`du_manager_impl.cpp`, the expanded DU procedure test source, `cell_scheduler.cpp`,
`scheduler_impl.cpp`, `mac_impl.cpp`, the O-CU-CP validator and the O-CU-CP config test
source all passed focused `-fsyntax-only -Werror`/object compilation. The dependency-heavy
`du_manager_procedure_test`, `common_scheduler_test`, `sched_no_ue_test` and current
`cu_cp_unit_config_test` binaries did not finish linking within their bounded build
windows. In the config attempt, the final `libsrsran_cu_cp_app_unit.a` was rebuilt before
the 604.2 s run reached unrelated NGAP dependencies; therefore no stale config binary is
reported as current runtime evidence. `srsran_cu_cp` was not rerun because it would repeat
the same broad dependency rebuild without adding focused evidence.

`applied` in CUCP-036 means the SSB/PRACH scheduler software gate consumed the matching snapshot. It is not position/port beam steering, PHY/OFH command evidence, or RU/RF telemetry.

CUCP-037 closeout evidence (2026-07-15):

```bash
cmake --build build/ai-clean --target ntn_mobility_test -j2
ctest --test-dir build/ai-clean -R '^(ntn_onboard_position_plan|ntn_beam_service_resource_manager|ntn_access_calendar_audit)\.' --output-on-failure
cmake --build build/ai-clean --target cu_cp_test -j4
build/ai-clean/tests/unittests/cu_cp/cu_cp_test --gtest_filter='<seven focused CU-CP NTN orchestration tests>'
cmake --build build/ai-clean --target srsran_cu_cp -j1
cmake --build build/ai-clean --target cu_cp_unit_config_test -j1
build/ai-clean/tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test --gtest_filter='<four focused NTN/default-terrestrial configuration tests>'
```

The NTN mobility target built successfully. The position-plan, cell-scoped RNTI
resource-manager and access-calendar suites passed 59/59. The seven CU-CP orchestration
tests passed 7/7, including incomplete DU intent feedback, non-regressing late prepare
feedback, prepare-guard ordering, apply-deadline rollback/clear and default-disabled
behavior. `srsran_cu_cp` and `cu_cp_unit_config_test` built successfully; the four focused
configuration/default-terrestrial tests passed 4/4. The first config-target attempt hit a
transient missing metrics-helper archive during WSL/NTFS relinking; an immediate serial
retry rebuilt that same target successfully.

A direct full `ntn_mobility_test` run passed 163/169. Its six failures are the same
pre-existing placement/rebalance expectations outside CUCP-037: one
`ntn_beam_placement_plan_helpers` case and five `ntn_beam_rebalance_policy` cases. Cold
single-job builds exceeded bounded validation windows before the successful incremental
builds and are not reported as test failures.

No split demo was run for the private Initial UL auditor because no production Initial UL
transport supplies its complete sideband metadata. The CU-CP mock integration proves
control-plane ordering and rollback only. It does not prove sender authentication,
freshness/anti-replay, position/port beam steering, PRACH detection, PHY/OFH execution or
RU/RF telemetry.

CUCP-038 closeout evidence (2026-07-15):

```bash
cmake --build build/ai-clean --target ntn_mobility_test -j1
build/ai-clean/tests/unittests/cu_cp/ntn_mobility/ntn_mobility_test \
  --gtest_filter='ntn_beam_service_resource_manager.*'
ctest --test-dir build/ai-clean -R 'ntn_mobility' --output-on-failure
```

All affected MAC/RACH, resource-manager, CU-CP implementation/test, F1 CU and
O-CU-CP CLI test objects were forced to rebuild with `gmake -B -j1` and compiled
without diagnostics. The `ntn_mobility_test` target linked successfully; the
resource-manager suite passed 39/39. A temporary focused executable linked from
the latest `rnti_manager_test.cpp.o` passed 23/23 and was then deleted.

A direct full `ntn_mobility_test` run passed 174/180. Its six failures are the
same pre-existing placement/rebalance expectation mismatches recorded for
CUCP-035 and CUCP-037: one `ntn_beam_placement_plan_helpers` case and five
`ntn_beam_rebalance_policy` cases; none of their implementation or test files is
part of CUCP-038. The requested broad `ctest -R ntn_mobility` unexpectedly
matched 159 CU integration cases and was stopped after 304 seconds while running
test 106, without a complete summary, so it is not reported as passing.

`f1ap_cu_test` (two attempts), `cu_cp_unit_config_test` and `cu_cp_test` each
spent their 314-second bounded window rebuilding unrelated dependencies and did
not link; their directly affected objects did compile, but their gtests were not
run. Passing tests prove the CU resource-manager and MAC RNTI-manager state
machines; affected DU/F1/CU integration paths have compile-only evidence. No
split demo was run. This is not proof of RAR transmission, raw PRACH detection,
authenticated Initial UL metadata, position/port steering, PHY/OFH execution,
RU/RF telemetry or long-duration endurance. Terminal-history GC and durable
C-RNTI reuse remain required before claiming namespace stability. With NTN
inactive, terrestrial selection/default outcomes are preserved; added
synchronization overhead is not a performance-equivalence claim.

CUCP-039 closeout evidence (2026-07-18):

```bash
cmake --build build/ai-clean --target srsran_cu_cp -j1
cmake --build build/ai-clean --target cu_cp_test -j1
build/ai-clean/tests/unittests/cu_cp/cu_cp_test \
  --gtest_filter='cu_cp_ntn_mobility_test.restart_*:cu_cp_ntn_mobility_test.default_cu_cp_rejects_ntn_satellite_state_updates'
build/ai-clean/tests/unittests/cu_cp/ntn_mobility/ntn_mobility_test \
  --gtest_filter='ntn_onboard_position_plan.*:ntn_onboard_position_plan_state.*'
build/ai-clean/tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test \
  --gtest_filter='cu_cp_unit_config.ntn_state_command_*:cu_cp_unit_config.onboard_position_plan_*:cu_cp_unit_config.default_terrestrial_config_keeps_ntn_disabled'
```

`srsran_cu_cp` and `cu_cp_test` built successfully. The restart/default-disabled
CU-CP group passed 7/7, the plan/controller/state group passed 45/45, and the
configuration/read-only-status group passed 7/7. `git diff --check` also passed.
The first bounded `cu_cp_test` attempt spent 604 seconds rebuilding the large
CU-CP archive and was stopped without a compiler error; later incremental runs
finished the target and found two test-construction errors, which were corrected
before the final passing run.

No split demo was run because this slice changes private restart storage and
reuses the already tested calendar query/clear path; the mock-DU integration
proves ordering and fail-closed behavior, not a live device restart. No PHY,
RU/RF, Web/GIS or generated ASN.1 files changed. A trusted monotonic anchor for
whole-file rollback/deletion and DU connection-generation binding remain open.

CUCP-040 closeout evidence (2026-07-19):

```bash
cmake --build build/ai-clean --target ntn_mobility_test -j1
build/ai-clean/tests/unittests/cu_cp/ntn_mobility/ntn_mobility_test \
  --gtest_filter='ntn_onboard_position_plan.*:ntn_onboard_position_plan_state.*'
ctest --test-dir build/ai-clean -R 'ntn_mobility' --output-on-failure
cmake --build build/ai-clean --target srsran_cu_cp -j1
cmake --build build/ai-clean --target cu_cp_test -j1
build/ai-clean/tests/unittests/cu_cp/cu_cp_test \
  --gtest_filter='cu_cp_ntn_mobility_test.restart_*:cu_cp_ntn_mobility_test.du_disconnect_*:cu_cp_ntn_mobility_test.disconnect_during_recovery_query_*:cu_cp_ntn_mobility_test.future_recovered_update_*:cu_cp_ntn_mobility_test.live_active_expiry_*:cu_cp_ntn_mobility_test.default_cu_cp_rejects_ntn_satellite_state_updates'
cmake --build build/ai-clean --target cu_cp_unit_config_test -j1
build/ai-clean/tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test \
  --gtest_filter='cu_cp_unit_config.ntn_state_command_*:cu_cp_unit_config.default_terrestrial_config_keeps_ntn_disabled'
cmake --build build/ai-clean --target du_processor_test_helpers -j1
```

`ntn_mobility_test` built successfully; its complete position-plan/state group
passed 53/53. `srsran_cu_cp` and `cu_cp_test` built successfully. The final
disconnect/reconnect, future-epoch, live/restart expiry, cleanup (including
forced state-write failure), state-schema migration, 257-position observation
and default-disabled group passed 14/14; the four most direct reconnect/expiry
cases also passed 4/4 separately. `cu_cp_unit_config_test` built successfully
and its read-only status/default-disabled group passed 5/5. The DU processor test helper also
compiled and linked after its mocks were brought up to the current CU-CP private
interfaces. `git diff --check` passed.

The broad `ctest -R ntn_mobility` expression matched 177 old and new tests,
so it was bounded rather than treated as a focused gate. The run was stopped at
about 290 seconds after 25/177 had passed and test 26 had started; no assertion
failure was reported. It is recorded as incomplete, not as a full pass.

No split demo was run because this slice does not change the existing private F1
calendar payload or scheduler execution path. The mock-DU tests exercise a real
CU-CP disconnect/reconnect lifecycle and response ordering, but do not prove
SCTP transport endurance, authenticated replay protection, position/port beam
steering, PHY/OFH execution or RU/RF telemetry. Whole-state-file rollback or
deletion still requires a separate trusted monotonic anchor. F1AP common
transaction cancellation during DU teardown remains a follow-up lifecycle
hardening item; CU-CP generation/request guards prevent those stale completions
from restoring NTN application evidence.

CUCP-041 closeout evidence (2026-07-19):

```bash
cmake --build build/ai-clean --target ntn_mobility_test -j1
build/ai-clean/tests/unittests/cu_cp/ntn_mobility/ntn_mobility_test \
  --gtest_filter='ntn_onboard_position_plan.*:ntn_onboard_position_plan_state.*'
cmake --build build/ai-clean --target srsran_cu_cp -j1
cmake --build build/ai-clean --target cu_cp_test -j1
build/ai-clean/tests/unittests/cu_cp/cu_cp_test \
  --gtest_filter='cu_cp_ntn_mobility_test.future_recovered_update_*:cu_cp_ntn_mobility_test.disconnect_during_recovery_query_*:cu_cp_ntn_mobility_test.du_disconnect_*:cu_cp_ntn_mobility_test.restart_*:cu_cp_ntn_mobility_test.live_active_expiry_*:cu_cp_ntn_mobility_test.default_cu_cp_rejects_ntn_satellite_state_updates'
```

`ntn_mobility_test` rebuilt successfully and the complete position-plan/state
group passed 54/54, including the new nested-replacement failure and state
round-trip case. `srsran_cu_cp` and `cu_cp_test` both rebuilt successfully; the
focused disconnect, restart, future activation, active expiry and default-off
group passed 14/14. `git diff --check` passed, and no build/test processes were
left running.

No broad `ctest` or split demo was repeated: this slice changes only the private
CU-CP controller fallback decision and its focused test. It does not change the
existing F1 payload, DU/MAC scheduler gate, PHY, RU/RF, Web/GIS or generated
ASN.1. The evidence remains software-control evidence, not proof that a radio
beam was transmitted. Whole-state rollback/deletion, authenticated management
input, F1 common transaction cancellation and RF/device execution remain later
work.
