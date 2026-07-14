# CUCP-028 NTN SR/SRS Application Feedback

## Goal

Close the NTN digital-service SR/SRS control loop: CU-CP sends a real SR/SRS slot assignment, DU applies or rejects it, CU-CP records the result, and scheduler/resource tests verify the applied UE configuration uses the requested period and offset.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_interface_contracts.md`
- `ai_harness/tasks/CUCP-027-ntn-rnti-lease-lifecycle-and-access-readiness.md`

## Accepted local contracts

- CU-CP remains authoritative for NTN digital-service SR/SRS assignment.
- DU applies the CU-CP assignment through existing PUCCH/SRS resource managers and reports the result.
- F1AP uses an srsRAN-private resource coordination container; generated ASN.1 files are not modified.
- Scheduler verification means scheduler-visible UE configuration follows the applied assignment, not PHY/RF confirmation.
- Terrestrial/default SR/SRS behavior remains unchanged.

## Required behavior

- CU-CP must not mark an NTN digital service UE as SR/SRS-ready until DU returns an applied result.
- DU rejection, malformed response, missing response container, or timeout must leave CU-CP in rejected/rollback state and must not leave stale pending slot intent.
- Clear requests must be reported back as `cleared_by_du` before CU-CP treats the applied SR/SRS assignment as gone.
- Test doubles may simulate DU applied results only when the matching UE Context Setup/Modification request carried an NTN SR/SRS request.
- Existing CUCP-025/026 exact non-CU-CP paths remain task exceptions only for the current accepted NTN RNTI/F1AP resource-coordination baseline.

## In scope

- Private F1AP SR/SRS result payload encode/decode.
- UE Context Setup/Modification response plumbing for DU applied/rejected result.
- CU-CP beam service resource manager states: `desired`, `sent_to_du`, `applied_by_du`, `rejected_by_du`, `clear_sent`, `cleared_by_du`, `rollback_restored`.
- DU resource-manager apply/reject result generation for NTN SR/SRS slot requests.
- O-CU-CP observability counters for sent/applied/rejected/cleared/rollback slot intents.
- Focused scheduler/resource tests that prove requested SR/SRS period and offset reach the UE configuration.

## Out of scope

- PHY, lower PHY, PRACH physical procedure, HARQ timing, TA scheduler, RU/RF, ZMQ, GIS, O-DU, and flexible_o_du.
- Generated ASN.1 changes.
- New SR/SRS configuration schema.
- New MAC scheduling policy.
- Terrestrial SR/SRS behavior changes.

## CU-CP only

This task is not CU-CP-only. It grants exact non-CU-CP exceptions for F1AP CU/DU private containers, DU PUCCH/SRS resource application, and focused scheduler/resource tests. It must not broaden into PHY, PRACH, HARQ, TA, RU/RF, ZMQ, GIS, O-DU, or flexible_o_du.

## Forbidden areas

- O-DU and flexible_o_du.
- PHY/lower PHY.
- PRACH physical procedure internals.
- HARQ and TA scheduler internals.
- RU/RF/ZMQ.
- GIS/site code.
- Generated ASN.1.
- Terrestrial SR/SRS allocation policy.

## Allowed edit paths

- `ai_harness/**`
- `include/srsran/cu_cp/**`
- `lib/cu_cp/**`
- `tests/unittests/cu_cp/**`
- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`
- `include/srsran/f1ap/ntn_ul_slot_resource_request.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu_ue_context_update.h`
- `include/srsran/f1ap/du/f1ap_du_ue_context_update.h`
- `lib/f1ap/cu_cp/procedures/ue_context_setup_procedure.cpp`
- `lib/f1ap/cu_cp/procedures/ue_context_modification_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_setup_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_modification_procedure.cpp`
- `lib/du/du_high/du_manager/procedures/ue_configuration_procedure.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_pucch_resource_manager.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_srs_resource_manager.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_srs_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ue_resource_config.h`
- `lib/du/du_high/test_mode/mac_test_mode_adapter.h`
- `lib/du/du_high/test_mode/mac_test_mode_adapter.cpp`
- `tests/test_doubles/f1ap/f1ap_test_messages.cpp`
- `tests/test_doubles/f1ap/f1ap_test_messages.h`
- `tests/unittests/f1ap/cu_cp/CMakeLists.txt`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_modification_procedure_test.cpp`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_setup_procedure_test.cpp`
- `tests/unittests/f1ap/du/CMakeLists.txt`
- `tests/unittests/f1ap/du/f1ap_du_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_test_helpers.h`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_modification_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_setup_procedure_test.cpp`
- `tests/unittests/du_manager/du_manager_test_helpers.cpp`
- `tests/unittests/du_manager/du_manager_test_helpers.h`
- `tests/unittests/du_manager/du_ran_resource_manager_test.cpp`
- `tests/unittests/mac/mac_rach_handler_test.cpp`
- `tests/unittests/mac/rnti_manager_test.cpp`
- `tests/unittests/scheduler/uci_and_pucch/`
- `tests/unittests/scheduler/srs_scheduling/srs_scheduler_test.cpp`

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`
- `include/srsran/f1ap/ntn_ul_slot_resource_request.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu_ue_context_update.h`
- `include/srsran/f1ap/du/f1ap_du_ue_context_update.h`
- `lib/f1ap/cu_cp/procedures/ue_context_setup_procedure.cpp`
- `lib/f1ap/cu_cp/procedures/ue_context_modification_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_setup_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_modification_procedure.cpp`
- `lib/du/du_high/du_manager/procedures/ue_configuration_procedure.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_pucch_resource_manager.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_srs_resource_manager.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_srs_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ue_resource_config.h`
- `tests/test_doubles/f1ap/f1ap_test_messages.cpp`
- `tests/test_doubles/f1ap/f1ap_test_messages.h`
- `tests/unittests/f1ap/cu_cp/CMakeLists.txt`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_modification_procedure_test.cpp`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_setup_procedure_test.cpp`
- `tests/unittests/f1ap/du/CMakeLists.txt`
- `tests/unittests/f1ap/du/f1ap_du_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_test_helpers.h`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_modification_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_setup_procedure_test.cpp`
- `tests/unittests/du_manager/du_manager_test_helpers.cpp`
- `tests/unittests/du_manager/du_manager_test_helpers.h`
- `tests/unittests/du_manager/du_ran_resource_manager_test.cpp`
- `tests/unittests/mac/mac_rach_handler_test.cpp`
- `tests/unittests/mac/rnti_manager_test.cpp`
- `tests/unittests/scheduler/uci_and_pucch/`
- `tests/unittests/scheduler/srs_scheduling/srs_scheduler_test.cpp`

## Required tests

- F1AP result payload encode/decode and malformed payload handling.
- F1AP-CU forwards UE Context Setup/Modification response result to CU-CP.
- F1AP-DU encodes DU manager apply/reject result into UE Context Setup/Modification response.
- CU-CP resource manager transitions slot intent through sent, applied, rejected, cleared, and rollback states.
- DU resource manager applies requested SR/SRS period and offset, rejects unsupported requests, and restores previous resources on failure.
- Scheduler/resource tests show applied SR/SRS configuration uses the CU-CP-requested period and offset.

## Required validation

- `ctest --test-dir build/ai-clean -R "ntn_ul_slot|ntn_sr_srs|du_ran_resource_manager|du_pucch_resource_manager|du_srs_resource_manager|f1ap_cu_ue_context|f1ap_du_ue_context|srs_scheduler|uci|cu_cp_ntn_mobility|cu_cp_unit_config" --output-on-failure`
- `BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh`
- `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-028-ntn-sr-srs-application-feedback.md`
- `python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-028-ntn-sr-srs-application-feedback.md`
- `python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-028-ntn-sr-srs-application-feedback.md`

## Validation

Focused tests may be run before the full validation set. Final validation must use the task-aware path guard because this task intentionally grants exact F1AP-DU, DU resource-manager, and scheduler-test exceptions.

## Done means

- DU returns applied/rejected SR/SRS slot assignment results to CU-CP.
- CU-CP records applied/rejected/cleared/rollback state instead of assuming success from F1AP procedure success alone.
- Rejected DU assignment does not leave stale pending CU-CP digital load or stale DU periodic SR/SRS resources.
- Scheduler/resource tests verify the requested SR/SRS period and offset reach the applied UE configuration.
- Path guard, rejected-overlap check, metadata validation, and focused tests pass, or remaining failures are clearly explained.
