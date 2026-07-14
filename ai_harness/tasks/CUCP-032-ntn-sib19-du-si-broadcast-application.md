# CUCP-032 NTN SIB19 DU SI Broadcast Application

## Goal

Advance the CU-CP NTN SIB19 assistance contract into a DU-applied dynamic SI PDU update loop for active/candidate
beams, with clear/reject feedback for draining or stale assistance.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_interface_contracts.md`
- `ai_harness/tasks/CUCP-013-ntn-rrc-sib19-assistance-contract.md`
- `ai_harness/tasks/CUCP-026-ntn-rnti-lease-pool-distribution.md`
- `ai_harness/tasks/CUCP-030-ntn-resource-consistency-auditor-and-repair-loop.md`

## Accepted local contracts

- CU-CP generates the SIB19 payload from `ntn_sib19_assistance_snapshot`.
- Only `active_loaded` and `candidate` beams are update-eligible.
- `draining`, stale satellite state, and invalid assistance produce clear or stale-blocked state.
- DU cells must already have SIB19 SI scheduling configured; CU-CP only updates or clears the dynamic SIB19 PDU.
- F1AP uses the private resource-coordination OCTET STRING container; generated ASN.1 files are not modified.
- Terrestrial SI/SIB behavior remains unchanged.

## In scope

- CU-CP SIB19 broadcast controller and status counters.
- Private F1AP SIB19 update/clear payload and result payload.
- DU resource coordination handler that validates cell/PCI/NCI/SIB19 SI slot and forwards dynamic SI PDU updates.
- MAC dynamic SI PDU clear support.
- O-CU-CP `ntn_state` and `ntn_beams all` observability.

## Out of scope

- Creating DU SI scheduling slots.
- O-DU or flexible_o_du behavior.
- MAC scheduling or MAC policy changes beyond the exact dynamic SI PDU clear exception.
- PHY, lower PHY, PRACH physical procedure, HARQ timing, TA scheduler, RU/RF, ZMQ, GIS.
- Generated ASN.1 changes.
- UE idle reselection behavior.

## CU-CP only

This is a CU-CP-led task with exact F1AP/DU/MAC exceptions needed to apply and clear dynamic SIB19 SI payloads.

## Forbidden areas

- O-DU and flexible_o_du.
- PHY/lower PHY.
- PRACH physical procedure internals.
- HARQ and TA scheduler internals.
- RU/RF/ZMQ.
- GIS/site code.
- Generated ASN.1.
- Terrestrial SI/SIB scheduling behavior.

## Allowed edit paths

- `ai_harness/**`
- `include/srsran/cu_cp/**`
- `lib/cu_cp/**`
- `tests/unittests/cu_cp/**`

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`
- `include/srsran/f1ap/ntn_rnti_lease_pool.h`
- `include/srsran/f1ap/cu_cp/f1ap_cu_resource_coordination.h`
- `include/srsran/f1ap/du/f1ap_du_connection_manager.h`
- `include/srsran/du/du_high/du_manager/du_configurator.h`
- `include/srsran/du/du_high/du_manager/du_manager.h`
- `include/srsran/mac/mac_cell_manager.h`
- `lib/f1ap/cu_cp/procedures/gnb_du_resource_coordination_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.cpp`
- `lib/f1ap/du/procedures/f1ap_du_gnbdu_resource_coordination_procedure.h`
- `lib/du/du_high/adapters/f1ap_adapters.h`
- `lib/du/du_high/du_manager/du_manager_impl.cpp`
- `lib/du/du_high/du_manager/du_manager_impl.h`
- `lib/du/du_high/du_manager/procedures/du_mac_si_pdu_update_procedure.cpp`
- `lib/mac/mac_dl/sib_pdu_assembler.cpp`
- `tests/unittests/f1ap/cu_cp/CMakeLists.txt`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/CMakeLists.txt`
- `tests/unittests/f1ap/du/f1ap_du_gnbdu_resource_coordination_test.cpp`
- `tests/unittests/f1ap/du/f1ap_du_test_helpers.h`
- `tests/unittests/mac/sib_pdu_assembler_test.cpp`

## Required behavior

1. Valid active/candidate assistance generates packed SIB19 update actions.
2. Draining assistance generates clear actions.
3. Stale or invalid assistance does not advertise update eligibility and clears previously sent/applied dynamic SIB19 state.
4. DU rejects malformed requests, wrong cell/PCI/NCI, stale generation, or missing SIB19 SI slot without crashing.
5. MAC dynamic SI clear removes the dynamic override and falls back to static SI behavior.
6. CU-CP observability distinguishes assistance packaging from DU-applied SIB19 broadcast state.

## Required tests

1. CU-CP SIB19 broadcast controller tests for active/candidate update, draining clear, and stale blocked state.
2. F1AP CU/DU resource coordination tests for SIB19 update/result containers.
3. MAC SI PDU assembler test for dynamic SIB19 clear.
4. O-CU-CP command tests for SIB19 counters and per-beam state.

## Validation

```bash
ctest --test-dir build/ai-clean -R "ntn_sib19|gnbdu_resource_coordination|sib_pdu|cu_cp_ntn_mobility|f1ap_cu|f1ap_du|du_manager|cu_cp_unit_config" --output-on-failure
BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh
python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-032-ntn-sib19-du-si-broadcast-application.md
python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-032-ntn-sib19-du-si-broadcast-application.md
python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-032-ntn-sib19-du-si-broadcast-application.md
```

## Done means

- CU-CP produces SIB19 DU update/clear actions from assistance and beam runtime state.
- F1AP/DU/MAC can apply or clear dynamic SIB19 payloads and return results.
- `ntn_state` and `ntn_beams all` show SIB19 broadcast state and reasons.
- Required focused tests pass, or failures are explained with logs.
- Path guard and rejected-overlap results are reported.
