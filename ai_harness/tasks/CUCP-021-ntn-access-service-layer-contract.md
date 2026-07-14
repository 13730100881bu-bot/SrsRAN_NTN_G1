# CUCP-021: NTN analog access / digital service layer contract

## Goal

Implement an explicit CU-CP-only NTN UE runtime contract that separates analog access context from digital service
context. A UE may be admitted on an analog access beam without immediately loading a digital service beam; the digital
service beam is bound when the first PDU/DRB service demand is admitted.

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

## Accepted local contracts

- CU-CP only.
- Single-satellite baseline.
- Analog access beams provide CU-CP access, reestablishment, paging-coarse, and pre-service relocation context.
- Digital service beams remain the NCI, beam-derived TAC, loaded service calendar, SR/SRS, QoS, and service DU grain.
- Digital service binding is location-first, then access-cell fallback.
- Do not modify DU, MAC, PHY, PRACH, HARQ, TA scheduler, RU/RF, ZMQ, GIS, O-DU, or flexible_o_du.

## In scope

- Add explicit per-UE NTN analog access context and optional digital service context.
- Keep RRC setup/reestablishment in `access_only` until PDU/DRB service demand is admitted.
- Bind digital service context during first PDU session setup using fresh UE location first and access cell/NCI fallback second.
- Reject new PDU demand when digital binding cannot produce an active-loaded, DU-policy-eligible digital service beam.
- Clear service context after PDU release while keeping access context; clear both on UE release.
- Expose access/service split state through CU-CP NTN UE/runtime status and O-CU-CP `ntn_state` / `ntn_ues`.

## Out of scope

- Connected beam-to-beam switching changes.
- Multi-satellite policy.
- Real analog beamforming.
- DU paging, PRACH, SSB, RF, or scheduler changes.
- O-DU and flexible_o_du.
- DU and DU scheduler behavior.
- MAC scheduler behavior.
- HARQ timing execution.
- TA scheduler behavior.
- PHY, lower PHY, RU, RF, radio drivers.
- ZMQ channel behavior.
- GIS-site behavior.

## Allowed edit paths

- `ai_harness/`
- `include/srsran/cu_cp/`
- `lib/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

## Required behavior

1. NTN UE setup creates or refreshes analog access context but does not by itself create digital service load.
2. First PDU session setup binds digital service context by latest valid UE location within the parent analog children.
3. If no valid location exists, access cell/NCI fallback may bind a child digital beam of the parent analog access beam.
4. Digital binding must require an active-loaded target and same-DU access/service policy.
5. Binding failure rejects new PDU demand with `radio_res_not_available` and leaves no pending service load.
6. PDU release clears digital service context while retaining analog access context; UE release clears both.
7. Observability shows access-only, service-binding pending/blocked, service-bound, location-bound, and fallback-bound counts.

## Required tests

1. `cu_cp_ntn_mobility_test` covers access-only UE setup without loaded digital service or SR/SRS slot request.
2. `cu_cp_ntn_mobility_test` covers first PDU setup binding by fresh UE location and loading the matching child digital beam.
3. `cu_cp_ntn_mobility_test` covers access-cell fallback binding when no fresh UE location exists.
4. `cu_cp_ntn_mobility_test` covers binding rejection when location is outside the parent analog, target is ineligible, or DU policy fails.
5. `cu_cp_ntn_mobility_test` covers PDU release clearing service context while retaining access context.
6. `cu_cp_unit_config_test` covers `ntn_state` and `ntn_ues` access/service split observability.
7. Path guard, rejected overlap, and task metadata validation must pass with this task file.

## Validation

- `ctest --test-dir build/ai-clean -R "cu_cp_ntn_mobility|ntn_beam_placement|cu_cp_unit_config|cu_cp_pdu_session_resource_setup" --output-on-failure`
- `BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh`
- `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-021-ntn-access-service-layer-contract.md`
- `python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base`
- `python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-021-ntn-access-service-layer-contract.md`

## Done means

- Analog access context and digital service context are explicit in CU-CP runtime.
- Access-only UEs do not consume digital loaded service resources.
- First PDU/DRB service demand deterministically binds digital service by location-first, fallback-second policy.
- O-CU-CP observability shows access/service split state.
- Path guard and required focused validation pass, or failures are documented.
