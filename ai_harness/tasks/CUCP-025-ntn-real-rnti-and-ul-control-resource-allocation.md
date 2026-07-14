# CUCP-025 NTN Real RNTI And UL Control Resource Allocation

## Goal

Make CU-CP authoritative for NTN C-RNTI lease intent and digital-service SR/SRS slot-resource allocation, then let DU-side code consume and validate the CU-CP allocation in NTN paths while keeping terrestrial defaults unchanged.

## Read First

- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_interface_contracts.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/audit/path_review_notes.md`
- `ai_harness/tasks/CUCP-024-ntn-beam-service-resource-manager.md`

## Accepted local contracts

- CU-CP owns NTN resource decisions.
- DU/MAC/Scheduler execute, validate, and report failure.
- Initial access RNTI authority is modeled as a pre-access lease pool; CU-CP does not invent the C-RNTI after Initial UL.
- Digital-service SR/SRS assignments use `f1ap_ntn_ul_slot_resource_request` in the existing F1AP resource coordination container.
- Empty `SRSNTN02` request means clear the previous NTN digital-service SR/SRS assignment.

## In scope

- CU-CP authoritative resource-manager state for RNTI leases and SR/SRS assignment status.
- F1AP-DU decode of the CU-CP NTN UL slot resource container.
- DU RAN resource manager applying CU-CP requested SR/SRS period and offset to UE-dedicated PUCCH/SRS resources.
- MAC RACH lease-consumption support for NTN RNTI lease pools.
- Focused tests covering CU-CP manager, F1AP-DU, DU resource allocation, and MAC RACH/RNTI behavior.

## Out of scope

- Multi-satellite behavior.
- Real RF analog beam control.
- O-DU and flexible_o_du ownership changes.
- Broad DU behavior outside the exact F1AP-DU, RAN resource manager, and NTN lease-consumption exceptions below.
- Broad MAC behavior outside the exact NTN RACH/RNTI lease-consumption exceptions below.
- PRACH/HARQ/TA scheduler redesign.
- PHY and lower PHY behavior.
- RU/RF/ZMQ transport or radio changes.
- GIS/site code.
- Generated ASN.1 changes.
- Removing terrestrial DU/MAC automatic RNTI, SR, or SRS allocation.

## CU-CP Only

This task is not CU-CP-only. It explicitly grants exact non-CU-CP exceptions for F1AP-DU, DU RAN resource management, and MAC RACH/RNTI code needed to consume CU-CP NTN allocations.

## Forbidden Areas

- PHY and lower PHY.
- RU/RF/ZMQ.
- GIS/site code.
- Generic scheduler behavior unrelated to SR/SRS validation.
- Terrestrial RACH, RNTI, PUCCH, or SRS behavior.
- Generated ASN.1.

## Allowed edit paths

- `ai_harness/**`
- `include/srsran/cu_cp/**`
- `lib/cu_cp/**`
- `tests/unittests/cu_cp/**`
- `include/srsran/f1ap/ntn_ul_slot_resource_request.h`
- `include/srsran/f1ap/du/f1ap_du_ue_context_update.h`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_setup_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_modification_procedure.cpp`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_setup_procedure_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_modification_test.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_ue_resource_config.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_pucch_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_pucch_resource_manager.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_srs_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_srs_resource_manager.cpp`
- `lib/du/du_high/du_manager/procedures/ue_creation_procedure.h`
- `lib/du/du_high/du_manager/procedures/ue_creation_procedure.cpp`
- `lib/du/du_high/du_manager/procedures/ue_configuration_procedure.cpp`
- `lib/du/du_high/du_manager/du_ue/du_ue_manager.cpp`
- `tests/unittests/du_manager/du_manager_test_helpers.h`
- `tests/unittests/du_manager/du_manager_test_helpers.cpp`
- `tests/unittests/du_manager/du_ran_resource_manager_test.cpp`
- `lib/mac/rnti_manager.h`
- `lib/mac/mac_sched/mac_rach_handler.h`
- `lib/mac/mac_sched/mac_rach_handler.cpp`
- `tests/unittests/mac/rnti_manager_test.cpp`
- `tests/unittests/mac/mac_rach_handler_test.cpp`

## Allowed task exception paths

- `include/srsran/f1ap/ntn_ul_slot_resource_request.h`
- `include/srsran/f1ap/du/f1ap_du_ue_context_update.h`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_setup_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_modification_procedure.cpp`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_setup_procedure_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_modification_test.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_ue_resource_config.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_pucch_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_pucch_resource_manager.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_srs_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_srs_resource_manager.cpp`
- `lib/du/du_high/du_manager/procedures/ue_creation_procedure.h`
- `lib/du/du_high/du_manager/procedures/ue_creation_procedure.cpp`
- `lib/du/du_high/du_manager/procedures/ue_configuration_procedure.cpp`
- `lib/du/du_high/du_manager/du_ue/du_ue_manager.cpp`
- `tests/unittests/du_manager/du_manager_test_helpers.h`
- `tests/unittests/du_manager/du_manager_test_helpers.cpp`
- `tests/unittests/du_manager/du_ran_resource_manager_test.cpp`
- `lib/mac/rnti_manager.h`
- `lib/mac/mac_sched/mac_rach_handler.h`
- `lib/mac/mac_sched/mac_rach_handler.cpp`
- `tests/unittests/mac/rnti_manager_test.cpp`
- `tests/unittests/mac/mac_rach_handler_test.cpp`

## Required behavior

- CU-CP maintains authoritative NTN RNTI lease state and can reject unexpected access RNTIs when strict validation is enabled.
- CU-CP digital service SR/SRS assignment is carried through the F1AP resource coordination container and decoded by F1AP-DU.
- DU RAN resource management applies requested SR/SRS period and offset to real UE-dedicated PUCCH/SRS resources.
- MAC RACH NTN lease mode consumes only CU-CP-provided leases and does not fall back to local allocation when the lease pool is empty.
- Terrestrial/default RNTI, PUCCH, and SRS allocation behavior remains available outside NTN lease/assignment paths.

## Required tests

- `ntn_beam_service_resource_manager_test`
- `f1ap_du_test`
- `du_ran_resource_manager_test`
- `mac_test` focused RNTI/RACH subsets when MAC lease consumption is changed.

## Required validation

- `ctest --test-dir build/ai-clean -R "ntn_rnti|ntn_ul_control|ntn_beam_service_resource|mac_rach|du_ran_resource|f1ap_du" --output-on-failure`
- `BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh`
- `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-025-ntn-real-rnti-and-ul-control-resource-allocation.md`
- `python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-025-ntn-real-rnti-and-ul-control-resource-allocation.md`
- `python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-025-ntn-real-rnti-and-ul-control-resource-allocation.md`

## Validation

Focused build/test may be run before the full CU-CP validation. The required validation includes the path guard, task-aware rejected overlap check, and metadata check. If full build or test execution is blocked by unrelated existing workspace changes, record the failing command and the boundary.

## Done means

- CU-CP manager exposes authoritative NTN RNTI lease and SR/SRS assignment state.
- DU receives and applies CU-CP SR/SRS assignment to real UE-dedicated PUCCH/SRS resources.
- NTN RNTI lease behavior is represented by tests and does not regress terrestrial RNTI allocation.
- Path guard passes with this task file or any boundary failures are explicitly reported.
