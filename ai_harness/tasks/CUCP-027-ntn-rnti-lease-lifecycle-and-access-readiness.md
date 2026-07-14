# CUCP-027 NTN RNTI Lease Lifecycle And Access Readiness

## Goal

Make the CU-CP-authoritative NTN RNTI lease pool a real access-readiness gate: active analog access beams receive applied DU/cell lease pools before access, DU-consumed RNTIs are validated by CU-CP during Initial UL/RRC setup, and successful Initial Context Setup commits the lease as the UE C-RNTI.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_interface_contracts.md`
- `ai_harness/audit/path_review_notes.md`
- `ai_harness/audit/rejected_or_quarantined_paths.txt`
- `ai_harness/tasks/CUCP-026-ntn-rnti-lease-pool-distribution.md`

## Accepted local contracts

- CU-CP remains authoritative for NTN RNTI leases.
- Analog access beams drive access readiness; digital service beams do not allocate access RNTIs.
- DU/MAC only consumes CU-CP lease pools in NTN lease mode and reports apply/consume state.
- `GNBDUResourceCoordinationRequest/Response` carries the private srsRAN NTN RNTI lease pool payload introduced by CUCP-026.
- Terrestrial/default RNTI allocation remains unchanged.
- Released NTN RNTIs are not immediately reused in this task.

## In scope

- CU-CP RNTI lease lifecycle states: `reserved`, `sent_to_du`, `applied_by_du`, `offered_in_rar`, `initial_ul_seen`, `committed`, `released`, `expired`, `conflict`.
- Analog-beam access readiness based on DU/cell lease pool apply state and available lease depth.
- Strict Initial UL/RRC setup validation that the observed RNTI belongs to the applied CU-CP lease pool for the UE DU/cell/analog beam.
- Lease commit on Initial Context Setup success and deterministic cleanup on setup failure, UE release, reestablishment failure, handover failure, analog-window expiry, and low-watermark refill.
- Minimal DU/MAC reporting hooks or test hooks for `offered_in_rar`.
- O-CU-CP observability for lease pool readiness and per-UE RNTI source.

## Out of scope

- O-DU and flexible_o_du.
- PHY and lower PHY.
- RU/RF/ZMQ transport or radio drivers.
- GIS-site code.
- PRACH physical procedure changes.
- HARQ timing execution.
- TA scheduler behavior.
- Generated ASN.1 changes.
- SR/SRS real execution feedback; this is reserved for CUCP-028.
- Removing terrestrial DU/MAC RNTI allocation.

## CU-CP only

This task is not CU-CP-only. It grants exact non-CU-CP exceptions for the F1AP lease-pool transport created in CUCP-026, DU lease-pool apply/consume plumbing, and MAC NTN lease-mode RACH/RNTI tests. It must not broaden into DU scheduler, PHY, PRACH physical timing, HARQ, TA, RU/RF, ZMQ, GIS, O-DU, or flexible_o_du.

## Forbidden areas

- O-DU and flexible_o_du.
- PHY/lower PHY.
- RU/RF/ZMQ.
- GIS/site code.
- PRACH physical procedure internals.
- HARQ and TA scheduler internals.
- Terrestrial RACH/RNTI behavior.
- Generated ASN.1.

## Allowed edit paths

- `ai_harness/**`
- `include/srsran/cu_cp/**`
- `lib/cu_cp/**`
- `tests/unittests/cu_cp/**`
- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `include/srsran/f1ap/ntn_rnti_lease_pool.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu_resource_coordination.h`
- `include/srsran/f1ap/du/f1ap_du.h`
- `include/srsran/f1ap/du/f1ap_du_connection_manager.h`
- `lib/f1ap/f1ap_asn1_utils.h`
- `lib/f1ap/cu_cp/CMakeLists.txt`
- `lib/f1ap/cu_cp/f1ap_cu_impl.h`
- `lib/f1ap/cu_cp/f1ap_cu_impl.cpp`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.h`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.cpp`
- `lib/f1ap/du/CMakeLists.txt`
- `lib/f1ap/du/f1ap_du_impl.h`
- `lib/f1ap/du/f1ap_du_impl.cpp`
- `lib/f1ap/du/procedures/CMakeLists.txt`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.h`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.cpp`
- `lib/du/du_high/adapters/f1ap_adapters.h`
- `include/srsran/du/du_high/du_manager/du_manager.h`
- `lib/du/du_high/du_manager/du_configurator.h`
- `lib/du/du_high/du_manager/du_manager_impl.h`
- `lib/du/du_high/du_manager/du_manager_impl.cpp`
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

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `include/srsran/f1ap/ntn_rnti_lease_pool.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu_resource_coordination.h`
- `include/srsran/f1ap/du/f1ap_du.h`
- `include/srsran/f1ap/du/f1ap_du_connection_manager.h`
- `include/srsran/f1ap/du/f1ap_du_ue_context_update.h`
- `lib/f1ap/f1ap_asn1_utils.h`
- `lib/f1ap/cu_cp/CMakeLists.txt`
- `lib/f1ap/cu_cp/f1ap_cu_impl.h`
- `lib/f1ap/cu_cp/f1ap_cu_impl.cpp`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.h`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.cpp`
- `lib/f1ap/du/CMakeLists.txt`
- `lib/f1ap/du/f1ap_du_impl.h`
- `lib/f1ap/du/f1ap_du_impl.cpp`
- `lib/f1ap/du/procedures/CMakeLists.txt`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.h`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_modification_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_ue_context_setup_procedure.cpp`
- `lib/du/du_high/adapters/f1ap_adapters.h`
- `include/srsran/du/du_high/du_manager/du_manager.h`
- `lib/du/du_high/du_manager/du_configurator.h`
- `lib/du/du_high/du_manager/du_manager_impl.h`
- `lib/du/du_high/du_manager/du_manager_impl.cpp`
- `lib/du/du_high/du_manager/procedures/ue_configuration_procedure.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_pucch_resource_manager.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_srs_resource_manager.cpp`
- `lib/du/du_high/du_manager/ran_resource_management/du_srs_resource_manager.h`
- `lib/du/du_high/du_manager/ran_resource_management/du_ue_resource_config.h`
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
- `tests/unittests/f1ap/du/f1ap_du_ue_context_modification_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_ue_context_setup_procedure_test.cpp`
- `tests/unittests/du_manager/du_manager_test_helpers.cpp`
- `tests/unittests/du_manager/du_manager_test_helpers.h`
- `tests/unittests/du_manager/du_ran_resource_manager_test.cpp`
- `tests/unittests/mac/rnti_manager_test.cpp`
- `tests/unittests/mac/mac_rach_handler_test.cpp`

## Required behavior

1. An active, eligible analog access beam is access-ready only after its selected DU/cell has at least one non-expired CU-CP lease in `applied_by_du` state.
2. DU rejection, DU timeout, empty pool, expired pool, or low applied depth makes the analog beam non-ready with reason `rnti_pool_unavailable` until refill succeeds.
3. NTN Initial UL/RRC setup validates `(DU, cell, PCI, analog beam, RNTI)` against the CU-CP lease pool.
4. A DU-offered lease moves through `offered_in_rar` and `initial_ul_seen`, then `committed` after Initial Context Setup success.
5. RRC setup failure, reestablishment failure, handover failure, UE release, and analog-window expiry clear or expire uncommitted leases deterministically.
6. Terrestrial access still uses the existing local allocation path and is not rejected by NTN lease readiness checks.

## Required tests

1. Manager tests for ready-after-ack, DU reject, timeout/expiry, low-watermark refill, RAR offer, Initial UL validation, commit, release, duplicate, wrong cell/beam, and expired RNTI.
2. CU-CP integration tests for no-pool NTN access rejection, applied-pool NTN access acceptance, unexpected RNTI rejection, analog-window expiry, and terrestrial compatibility.
3. F1AP/DU/MAC tests for pool application to the target cell, offered-in-RAR reporting, per-cell isolation, empty-pool NTN rejection, and terrestrial RACH compatibility.
4. O-CU-CP observability tests for `ntn_state`, `ntn_beams all`, and `ntn_ues` lease lifecycle output.

## Required validation

- `ctest --test-dir build/ai-clean -R "ntn_rnti_lease|ntn_beam_service_resource|gnbdu_resource_coordination|mac_rach|rnti_manager|cu_cp_ntn_mobility|cu_cp_unit_config" --output-on-failure`
- `BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh`
- `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-027-ntn-rnti-lease-lifecycle-and-access-readiness.md`
- `python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-027-ntn-rnti-lease-lifecycle-and-access-readiness.md`
- `python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-027-ntn-rnti-lease-lifecycle-and-access-readiness.md`

## Validation

Focused build/test may be run before full CU-CP validation. The final run must use the task-aware path guard and rejected-overlap checker because this task intentionally has exact non-CU-CP exceptions.

## Done means

- CU-CP can decide whether an analog access beam is ready for NTN access based on applied RNTI lease availability.
- NTN Initial UL/RRC setup rejects RNTIs that do not originate from the correct CU-CP lease pool.
- Initial Context Setup success commits the lease; failures and release clear the lifecycle deterministically.
- DU/MAC NTN lease-mode behavior and terrestrial fallback are covered by focused tests.
- Path guard, rejected/quarantined overlap, metadata validation, and focused tests pass, or failures are clearly explained.
