# CUCP-018: NTN access DU assignment policy

## Goal

Implement a CU-CP-only mixed analog/digital DU assignment policy for NTN access-time decisions.

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
- Analog access beams provide CU-CP access and coarse eligibility grouping.
- Digital service beams remain the real NCI, TAC, loaded service calendar, SR/SRS, and QoS grain.
- Access DU and service DU use same-DU-first policy.
- Do not implement automatic inter-DU migration for mismatched access/service DU.
- Do not modify DU, MAC, PHY, PRACH, HARQ, TA scheduler, RU/RF, ZMQ, GIS, O-DU, or flexible_o_du.

## In scope

- Add analog-level access DU assignments to NTN beam placement output.
- Add per-digital-beam access DU status and deterministic DU-policy reasons.
- Select analog access DU by supported child count, assigned analog count, current UE/DRB load, and DU index.
- Prefer parent analog access DU when loading a digital service beam.
- Reject new access/PDU demand when current UE DU cannot satisfy access/service DU policy.
- Expose access/service DU policy in `ntn_state` and `ntn_beams all`.

## Out of scope

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
- Automatic inter-DU handover when access DU and service DU differ.
- New configuration knobs.

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

1. Planner emits analog access DU assignments for active analog access beams.
2. Planner prefers the parent analog access DU when loading a child digital service beam.
3. CU-CP admission gates reject new access or PDU demand when the current UE DU cannot satisfy the selected access/service DU contract.
4. Observability exposes access DU, service DU, DU policy reason, assigned/unassigned analog counters, and same/split DU service counters.
5. Digital service beams remain the NCI/TAC/loaded service calendar/SR/SRS/QoS grain; no DU/MAC/PHY behavior is changed.

## Required tests

1. `ntn_beam_placement_planner_test` covers analog access DU support-count selection, tie-breaks, same-DU service preference, and split-DU candidate behavior.
2. `cu_cp_ntn_mobility_test` remains green for NTN admission, handover, loaded service calendar, stale/draining, QoS, and paging paths.
3. `cu_cp_unit_config_test` covers `ntn_state` and `ntn_beams all` access/service DU observability.
4. Path guard, rejected overlap, and task metadata validation must pass with this task file.

## Validation

- `ctest --test-dir build/ai-clean -R "ntn_beam_placement|cu_cp_ntn_mobility|cu_cp_unit_config" --output-on-failure`
- `BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh`
- `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-018-ntn-access-du-assignment-policy.md`
- `python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base`
- `python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-018-ntn-access-du-assignment-policy.md`

## Done means

- Planner selects analog access DUs deterministically.
- Digital service beams prefer same DU as parent analog access beam.
- New access/PDU demand is gated when access/service DU policy cannot be satisfied.
- O-CU-CP observability shows access DU, service DU, and DU-policy counters.
- Path guard and required focused validation pass, or failures are documented.
