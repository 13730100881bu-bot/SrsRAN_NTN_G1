# CUCP-026 NTN RNTI Lease Pool Distribution

## Goal

Deliver CU-CP-authoritative NTN RNTI lease pools to DU/MAC before PRACH/RAR by using an F1AP non-UE resource coordination procedure, and record DU apply results in CU-CP state.

## Read first

- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/context/ntn_cucp_interface_contracts.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/tasks/CUCP-025-ntn-real-rnti-and-ul-control-resource-allocation.md`

## Accepted local contracts

- CU-CP remains authoritative for NTN RNTI leases.
- DU/MAC consumes lease pools before NTN RAR and reports apply result.
- `GNBDUResourceCoordinationRequest/Response` carries a private srsRAN NTN RNTI lease OCTET STRING.
- Generated ASN.1 files are not modified.
- Terrestrial/default RNTI allocation remains unchanged.

## In scope

- Private F1AP NTN RNTI lease pool update/ack container.
- F1AP-CU resource coordination sender and response parsing.
- F1AP-DU resource coordination receiver and DU configurator forwarding.
- DU/MAC lease pool apply API and per-cell lease consumption.
- CU-CP resource-manager apply-state counters and focused tests.

## Out of scope

- O-DU and flexible_o_du ownership changes.
- PHY and lower PHY changes.
- RU/RF/ZMQ transport or radio changes.
- GIS/site code.
- PRACH/HARQ/TA scheduler redesign.
- Broad DU behavior outside the exact F1AP-DU and lease apply exceptions below.
- Broad MAC behavior outside the exact NTN RNTI lease exceptions below.
- Removing terrestrial DU/MAC automatic RNTI allocation.
- SR/SRS slot assignment changes beyond existing CUCP-025 behavior.
- Generated ASN.1 changes.

## CU-CP only

This task is not CU-CP-only. It grants exact non-CU-CP exceptions for F1AP-CU/DU resource coordination, DU configurator lease apply, and MAC NTN lease-pool consumption.

## Forbidden areas

- O-DU and flexible_o_du.
- PHY/lower PHY.
- RU/RF/ZMQ.
- GIS/site code.
- PRACH/HARQ/TA scheduler internals.
- Terrestrial RACH/RNTI behavior.
- Generated ASN.1.

## Allowed edit paths

- `ai_harness/**`
- `include/srsran/cu_cp/**`
- `lib/cu_cp/**`
- `tests/unittests/cu_cp/**`
- `include/srsran/f1ap/ntn_rnti_lease_pool.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu_resource_coordination.h`
- `include/srsran/f1ap/du/f1ap_du.h`
- `include/srsran/f1ap/du/f1ap_du_connection_manager.h`
- `lib/f1ap/f1ap_asn1_utils.h`
- `lib/f1ap/cu_cp/f1ap_cu_impl.h`
- `lib/f1ap/cu_cp/f1ap_cu_impl.cpp`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.h`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.cpp`
- `lib/f1ap/cu_cp/CMakeLists.txt`
- `lib/f1ap/du/f1ap_du_impl.h`
- `lib/f1ap/du/f1ap_du_impl.cpp`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.h`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.cpp`
- `lib/f1ap/du/procedures/CMakeLists.txt`
- `lib/du/du_high/du_manager/du_configurator.h`
- `lib/du/du_high/du_manager/du_manager_impl.h`
- `lib/du/du_high/du_manager/du_manager_impl.cpp`
- `include/srsran/du/du_high/du_manager/du_manager.h`
- `lib/du/du_high/adapters/f1ap_adapters.h`
- `include/srsran/mac/mac_manager.h`
- `lib/mac/mac_impl.h`
- `lib/mac/rnti_manager.h`
- `lib/mac/mac_sched/mac_rach_handler.h`
- `lib/mac/mac_sched/mac_rach_handler.cpp`
- `tests/unittests/f1ap/cu_cp/CMakeLists.txt`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/CMakeLists.txt`
- `tests/unittests/f1ap/du/f1ap_du_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_test_helpers.h`
- `tests/unittests/du_manager/du_manager_test_helpers.h`
- `tests/unittests/mac/rnti_manager_test.cpp`
- `tests/unittests/mac/mac_rach_handler_test.cpp`
- `tests/unittests/cu_cp/ntn_mobility/ntn_beam_service_resource_manager_test.cpp`

## Allowed task exception paths

- `include/srsran/f1ap/ntn_rnti_lease_pool.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu_resource_coordination.h`
- `include/srsran/f1ap/du/f1ap_du.h`
- `include/srsran/f1ap/du/f1ap_du_connection_manager.h`
- `lib/f1ap/f1ap_asn1_utils.h`
- `lib/f1ap/cu_cp/f1ap_cu_impl.h`
- `lib/f1ap/cu_cp/f1ap_cu_impl.cpp`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.h`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.cpp`
- `lib/f1ap/cu_cp/CMakeLists.txt`
- `lib/f1ap/du/f1ap_du_impl.h`
- `lib/f1ap/du/f1ap_du_impl.cpp`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.h`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.cpp`
- `lib/f1ap/du/procedures/CMakeLists.txt`
- `lib/du/du_high/du_manager/du_configurator.h`
- `lib/du/du_high/du_manager/du_manager_impl.h`
- `lib/du/du_high/du_manager/du_manager_impl.cpp`
- `include/srsran/du/du_high/du_manager/du_manager.h`
- `lib/du/du_high/adapters/f1ap_adapters.h`
- `include/srsran/mac/mac_manager.h`
- `lib/mac/mac_impl.h`
- `lib/mac/rnti_manager.h`
- `lib/mac/mac_sched/mac_rach_handler.h`
- `lib/mac/mac_sched/mac_rach_handler.cpp`
- `tests/unittests/f1ap/cu_cp/CMakeLists.txt`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/CMakeLists.txt`
- `tests/unittests/f1ap/du/f1ap_du_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_test_helpers.h`
- `tests/unittests/du_manager/du_manager_test_helpers.h`
- `tests/unittests/mac/rnti_manager_test.cpp`
- `tests/unittests/mac/mac_rach_handler_test.cpp`

## Required behavior

- CU-CP can encode and send one lease-pool update per DU/cell/analog beam before UE context exists.
- DU decodes the resource coordination request, validates the private payload, applies leases to MAC, and returns an ack container.
- MAC NTN lease mode is per cell; empty NTN lease pool rejects CBRA instead of falling back to local allocation.
- CU-CP records `sent_to_du`, `applied_by_du`, and `rejected_by_du` lease distribution states.
- Terrestrial RACH keeps using existing local allocation.

## Required tests

- `f1ap_cu_gnbdu_resource_coordination_test`
- `f1ap_du_gnbdu_resource_coordination_test`
- `ntn_beam_service_resource_manager_test`
- `rnti_manager_test`
- `mac_rach_handler_test`

## Required validation

- `ctest --test-dir build/ai-clean -R "ntn_rnti_lease|gnbdu_resource_coordination|mac_rach|rnti_manager|cu_cp_ntn_mobility" --output-on-failure`
- `BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh`
- `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-026-ntn-rnti-lease-pool-distribution.md`
- `python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-026-ntn-rnti-lease-pool-distribution.md`
- `python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-026-ntn-rnti-lease-pool-distribution.md`

## Validation

Focused build/test may be run before the full CU-CP validation. The required validation includes path guard, task-aware rejected overlap check, and metadata check.

## Done means

- CU-CP can push an NTN RNTI lease pool to DU before UE access.
- DU/MAC can apply the pool and use it for NTN CBRA RAR.
- CU-CP records DU apply result.
- Focused tests and harness path validation pass, or remaining failures are explicitly reported.
