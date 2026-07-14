# CUCP-029 NTN Connected Handover Target Resource Reservation

## Goal

Make NTN connected beam-to-beam handover wait for target resources before sending the source-side RRC handover
reconfiguration. A stable location-driven target must reserve a target C-RNTI from the CU-CP NTN lease pool and receive a
DU-applied target SR/SRS result before the existing intra-CU handover routine proceeds.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_interface_contracts.md`
- `ai_harness/tasks/CUCP-020-ntn-beam-to-beam-connected-mobility.md`
- `ai_harness/tasks/CUCP-027-ntn-rnti-lease-lifecycle-and-access-readiness.md`
- `ai_harness/tasks/CUCP-028-ntn-sr-srs-application-feedback.md`

## Accepted local contracts

- Single-satellite baseline.
- Location reports are the only NTN connected handover trigger in this task.
- CU-CP is authoritative for NTN handover target C-RNTI reservation and digital SR/SRS assignment.
- DU/MAC execute and report the requested NTN target resources.
- Terrestrial handover, terrestrial RNTI allocation, and terrestrial SR/SRS behavior remain unchanged.
- Generated ASN.1 files are not modified.

## In scope

- Extend CU-CP connected handover state with target-resource preparation and applied states.
- Add CU-CP manager APIs to reserve, commit, release, and rollback target handover C-RNTI leases.
- Preload the target digital beam as handover demand before handover, then require target beam `active_loaded`.
- Require target C-RNTI reservation and DU-applied target SR/SRS result before source RRC handover reconfiguration.
- Extend internal F1AP/DU UE creation/update contracts with an optional CU-CP-requested target C-RNTI for NTN handover.
- Expose target resource readiness through `ntn_state`, `ntn_ues`, and `ntn_beams all`.

## Out of scope

- RSRP/measurement-driven NTN handover.
- Multi-satellite policy.
- MAC scheduling policy, PHY, lower PHY, PRACH physical procedure, HARQ timing, TA scheduler, RU/RF, ZMQ, GIS, O-DU,
  and flexible_o_du.
- Generated ASN.1 changes.
- Terrestrial handover or terrestrial resource allocation behavior changes.

## CU-CP only

This task is not CU-CP-only. It grants exact non-CU-CP exceptions for the internal F1AP/DU target UE setup path needed to
apply CU-CP-provided NTN handover target C-RNTI and SR/SRS resources. It must not broaden into PHY, PRACH, HARQ, TA,
RU/RF, ZMQ, GIS, O-DU, or flexible_o_du.

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
- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`
- `include/srsran/f1ap/cu_cp/f1ap_cu_ue_context_update.h`
- `include/srsran/f1ap/du/f1ap_du_ue_context_update.h`
- `include/srsran/f1ap/ntn_ul_slot_resource_request.h`
- `lib/f1ap/cu_cp/procedures/ue_context_setup_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_setup_procedure.h`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_setup_procedure.cpp`
- `lib/du/du_high/du_manager/du_ue/du_ue_manager.cpp`
- `lib/du/du_high/du_manager/procedures/ue_creation_procedure.h`
- `lib/du/du_high/du_manager/procedures/ue_creation_procedure.cpp`
- `lib/du/du_high/du_manager/procedures/ue_configuration_procedure.cpp`
- `tests/test_doubles/f1ap/f1ap_test_messages.cpp`
- `tests/test_doubles/f1ap/f1ap_test_messages.h`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_setup_procedure_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_setup_procedure_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_test_helpers.h`
- `tests/unittests/du_manager/du_manager_test_helpers.cpp`
- `tests/unittests/du_manager/du_manager_test_helpers.h`

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`
- `include/srsran/f1ap/cu_cp/f1ap_cu_ue_context_update.h`
- `include/srsran/f1ap/du/f1ap_du_ue_context_update.h`
- `include/srsran/f1ap/ntn_ul_slot_resource_request.h`
- `lib/f1ap/cu_cp/procedures/ue_context_setup_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_setup_procedure.h`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_setup_procedure.cpp`
- `lib/du/du_high/du_manager/du_ue/du_ue_manager.cpp`
- `lib/du/du_high/du_manager/procedures/ue_creation_procedure.h`
- `lib/du/du_high/du_manager/procedures/ue_creation_procedure.cpp`
- `lib/du/du_high/du_manager/procedures/ue_configuration_procedure.cpp`
- `tests/test_doubles/f1ap/f1ap_test_messages.cpp`
- `tests/test_doubles/f1ap/f1ap_test_messages.h`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_setup_procedure_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_setup_procedure_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_test_helpers.h`
- `tests/unittests/du_manager/du_manager_test_helpers.cpp`
- `tests/unittests/du_manager/du_manager_test_helpers.h`

## Required behavior

1. Stable location candidates enter `target_resource_preparing` and preload target beam demand.
2. Target handover beam must become `active_loaded` and satisfy analog, DU, resource-domain, and QoS gates.
3. Target C-RNTI must be reserved from an applied CU-CP NTN RNTI lease for the target DU/cell/analog beam.
4. Target UE Context Setup must request the reserved C-RNTI and carry target SR/SRS assignment when NTN handover context exists.
5. Source RRC handover reconfiguration is sent only after target UE Context Setup returns the matching C-RNTI and DU-applied SR/SRS result.
6. DU reject, missing result, mismatched target C-RNTI, setup failure, or timeout rolls back target reservation and pending slot intent.
7. Handover success commits the target C-RNTI and target service context; source resources are cleaned through the existing release path.

## Required tests

1. `cu_cp_ntn_mobility_test` covers target resource pending blocking source handover reconfiguration.
2. `cu_cp_ntn_mobility_test` covers target C-RNTI unavailable blocking connected handover.
3. `cu_cp_ntn_mobility_test` covers target C-RNTI plus SR/SRS applied allowing handover.
4. Manager tests cover target handover lease reserve, commit, rollback, and mismatch.
5. F1AP/DU tests cover optional requested target C-RNTI propagation for NTN handover target UE creation.
6. O-CU-CP command tests cover target resource counters and per-UE target state.

## Required validation

- `ctest --test-dir build/ai-clean -R "ntn_connected_handover|cu_cp_ntn_mobility|ntn_beam_placement|ntn_beam_service_resource|f1ap_cu_ue_context|f1ap_du_ue_context|cu_cp_unit_config" --output-on-failure`
- `BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh`
- `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-029-ntn-connected-handover-target-resource-reservation.md`
- `python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-029-ntn-connected-handover-target-resource-reservation.md`
- `python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-029-ntn-connected-handover-target-resource-reservation.md`

## Validation

Focused build/test may be run before the full CU-CP validation. Final validation must use the task-aware path guard
because this task intentionally grants exact F1AP/DU target UE setup exceptions.

## Done means

- NTN connected handover waits for target C-RNTI and target SR/SRS DU-applied result before source RRC handover command.
- Failure paths roll back target reservations without dropping source service resources.
- Terrestrial handover and resource allocation behavior remain unchanged.
- Path guard, rejected overlap, metadata validation, and focused tests pass, or remaining failures are clearly explained.
