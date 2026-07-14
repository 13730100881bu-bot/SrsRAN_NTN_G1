# CUCP-020: NTN beam-to-beam connected mobility

## Goal

Implement CU-CP-only NTN connected UE beam-to-beam mobility. A stable NTN location candidate must preload the target
digital service beam into the loaded service calendar before CU-CP triggers the existing intra-CU handover path.

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
- Digital service beams are the NCI, TAC, loaded service calendar, SR/SRS, QoS, and service DU grain.
- Analog access beams gate access and cross-analog handover eligibility.
- Existing intra-CU handover routines are reused.
- Do not modify DU, MAC, PHY, PRACH, HARQ, TA scheduler, RU/RF, ZMQ, GIS, O-DU, or flexible_o_du.

## In scope

- Route NTN location handover triggers through a CU-CP connected handover coordinator.
- Preload target digital beams as handover demand before triggering handover.
- Support same-analog digital beam handover and cross-analog handover when the target analog access beam is eligible.
- Block handover when assistance is stale, target beam is not candidate/mobility eligible, target analog is ineligible, target
  DU is unavailable, or capacity is reserved for higher-priority service.
- Expose connected handover state through CU-CP NTN UE/runtime status and O-CU-CP `ntn_state` / `ntn_ues`.

## Out of scope

- Measurement-driven NTN handover from RSRP reports.
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

1. Stable NTN location candidates are evaluated by CU-CP before mobility manager handover execution.
2. Target digital service beam is preloaded as handover demand before handover is triggered.
3. Target must be active loaded, mobility eligible, and DU-policy eligible before handover is accepted.
4. Handover success clears connected handover demand and UE state.
5. Handover failure clears preload demand and leaves retryable state.
6. Pre-service relocation suppresses connected beam handover for the same UE.
7. Observability shows connected handover counters and per-UE source/target beam state.

## Required tests

1. `cu_cp_ntn_mobility_test` covers same-analog target preload before handover.
2. `cu_cp_ntn_mobility_test` covers blocked target when assistance is stale or target is not eligible.
3. `cu_cp_ntn_mobility_test` covers handover failure cleanup and retryable state.
4. `cu_cp_unit_config_test` covers `ntn_state` and `ntn_ues` connected handover observability.
5. Path guard, rejected overlap, and task metadata validation must pass with this task file.

## Validation

- `ctest --test-dir build/ai-clean -R "ntn_connected_handover|cu_cp_ntn_mobility|cell_meas_manager|ntn_beam_placement|cu_cp_unit_config" --output-on-failure`
- `BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh`
- `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-020-ntn-beam-to-beam-connected-mobility.md`
- `python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base`
- `python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-020-ntn-beam-to-beam-connected-mobility.md`

## Done means

- Connected UE beam-to-beam handover preloads target service demand before handover.
- Existing NTN TTT, consecutive report, hysteresis, and retry behavior remains intact.
- O-CU-CP observability shows connected handover counters and UE targets.
- Path guard and required focused validation pass, or failures are documented.
