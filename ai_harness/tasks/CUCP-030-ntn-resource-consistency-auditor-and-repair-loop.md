# CUCP-030 NTN Resource Consistency Auditor And Repair Loop

## Goal

Add a reliability layer for NTN resources that lets CU-CP actively audit DU state and generate conservative repair
actions for RNTI lease pools, digital SR/SRS assignments, and stale handover target reservations.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_interface_contracts.md`
- `ai_harness/tasks/CUCP-026-ntn-rnti-lease-pool-distribution.md`
- `ai_harness/tasks/CUCP-027-ntn-rnti-lease-lifecycle-and-access-readiness.md`
- `ai_harness/tasks/CUCP-028-ntn-sr-srs-application-feedback.md`
- `ai_harness/tasks/CUCP-029-ntn-connected-handover-target-resource-reservation.md`

## Accepted local contracts

- CU-CP remains authoritative for NTN RNTI lease pools and digital SR/SRS assignment state.
- DU/MAC/Scheduler state is queried only for NTN audit and repair.
- Repair actions are conservative: resend, clear, or roll back control-plane state; committed UEs are not forcibly
  released solely due to an audit mismatch.
- Terrestrial RNTI, SR/SRS, RACH, and handover behavior remain unchanged.
- Generated ASN.1 files are not modified.

## In scope

- Extend CU-CP resource-manager contracts with audit reports and repair decisions.
- Add a private F1AP resource-coordination audit request/result payload alongside the existing RNTI lease payload.
- Let F1AP-CU send audit requests and parse accepted/rejected audit results.
- Let F1AP-DU decode audit requests, forward them to DU resource snapshots, and encode audit results.
- Add DU manager and test-helper hooks for read-only NTN resource audit snapshots.
- Add focused tests for missing applied RNTI pools, unknown DU SR/SRS assignments, and F1AP audit payload transport.

## Out of scope

- PHY, lower PHY, PRACH physical procedure, HARQ timing, TA scheduler, RU/RF, ZMQ, GIS, O-DU, and flexible_o_du.
- MAC scheduler policy and terrestrial MAC resource allocation behavior.
- Generated ASN.1 changes.
- Replacing terrestrial resource allocation, RACH, or SR/SRS behavior.
- Forcibly releasing committed UEs on audit mismatch.

## CU-CP only

This task is not CU-CP-only. It grants exact non-CU-CP exceptions for private F1AP resource coordination, DU audit
snapshot hooks, and focused tests. It must not expand into PHY, PRACH, HARQ, TA, RU/RF, ZMQ, GIS, O-DU, or
flexible_o_du.

## Forbidden areas

- O-DU and flexible_o_du.
- PHY/lower PHY.
- PRACH physical procedure internals.
- HARQ and TA scheduler internals.
- RU/RF/ZMQ.
- GIS/site code.
- Generated ASN.1.
- Terrestrial RNTI or SR/SRS allocation policy.

## Allowed edit paths

- `ai_harness/**`
- `include/srsran/cu_cp/**`
- `lib/cu_cp/**`
- `tests/unittests/cu_cp/**`
- `include/srsran/f1ap/ntn_rnti_lease_pool.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu_resource_coordination.h`
- `include/srsran/f1ap/du/f1ap_du_connection_manager.h`
- `include/srsran/du/du_high/du_manager/du_manager.h`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.h`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.cpp`
- `lib/du/du_high/adapters/f1ap_adapters.h`
- `lib/du/du_high/du_manager/du_manager_impl.h`
- `lib/du/du_high/du_manager/du_manager_impl.cpp`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_test_helpers.h`

## Allowed task exception paths

- `include/srsran/f1ap/ntn_rnti_lease_pool.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu_resource_coordination.h`
- `include/srsran/f1ap/du/f1ap_du_connection_manager.h`
- `include/srsran/du/du_high/du_manager/du_manager.h`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.h`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.cpp`
- `lib/du/du_high/adapters/f1ap_adapters.h`
- `lib/du/du_high/du_manager/du_manager_impl.h`
- `lib/du/du_high/du_manager/du_manager_impl.cpp`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_test_helpers.h`

## Required behavior

1. CU-CP can ingest a DU audit report and produce deterministic repair actions.
2. Missing DU-applied RNTI leases that CU-CP believes are applied produce a resend lease-pool repair.
3. Unknown DU SR/SRS assignments that CU-CP does not recognize produce a clear repair.
4. Malformed or rejected audit results are recorded as conflicts without crashing.
5. F1AP resource coordination can carry audit request/result payloads without modifying generated ASN.1.
6. DU-side resource coordination forwards audit requests to DU manager snapshot hooks and returns accepted/rejected results.
7. Terrestrial behavior and existing RNTI lease update payload compatibility are preserved.

## Required tests

1. Resource-manager tests cover missing applied RNTI pool and unknown DU SR/SRS assignment repair decisions.
2. F1AP-CU tests cover audit request/result container round-trip and successful procedure completion.
3. F1AP-DU tests cover audit request forwarding and result response encoding.
4. Existing NTN mobility/resource tests remain green.

## Required validation

- `ctest --test-dir build/ai-clean -R "ntn_resource_audit|ntn_beam_service_resource|gnbdu_resource_coordination|cu_cp_ntn_mobility|f1ap_cu|f1ap_du|du_ran_resource_manager|rnti_manager|cu_cp_unit_config" --output-on-failure`
- `BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh`
- `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-030-ntn-resource-consistency-auditor-and-repair-loop.md`
- `python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-030-ntn-resource-consistency-auditor-and-repair-loop.md`
- `python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-030-ntn-resource-consistency-auditor-and-repair-loop.md`

## Validation

Focused build/test may run before full CU-CP validation. Final validation must include the required validation commands
listed above and must use this task file because CUCP-030 intentionally grants exact F1AP/DU audit exceptions.
The final path guard command is `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base
--task-file ai_harness/tasks/CUCP-030-ntn-resource-consistency-auditor-and-repair-loop.md`.

## Done means

- CU-CP can compare authoritative NTN resource state against a DU audit result and generate repair actions.
- F1AP-CU and F1AP-DU carry the private audit request/result payload safely.
- DU has a read-only audit hook for NTN resource snapshots.
- Terrestrial resource behavior remains unchanged.
- Path guard, rejected overlap, metadata validation, and focused tests pass, or remaining failures are clearly explained.
