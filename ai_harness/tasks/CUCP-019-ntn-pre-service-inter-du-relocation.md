# CUCP-019: NTN pre-service inter-DU relocation

## Goal

Implement CU-CP-only NTN pre-service inter-DU relocation so a UE that initially accesses from a non-selected analog access DU can be temporarily admitted and moved to the selected access/service DU before first service demand.

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
- Analog access beams provide CU-CP access eligibility grouping.
- Digital service beams remain the real NCI, TAC, loaded service calendar, SR/SRS, QoS, and service DU grain.
- Existing intra-CU inter-DU handover routine is reused for relocation.
- Do not modify DU, MAC, PHY, PRACH, HARQ, TA scheduler, RU/RF, ZMQ, GIS, O-DU, or flexible_o_du.

## In scope

- Allow temporary signaling-only RRC setup when the UE DU differs from the selected analog access DU and a deterministic target DU exists.
- Record per-UE NTN pre-service relocation state.
- Trigger relocation after initial context/security/capability completion and before first service PDU session demand.
- Block new PDU session demand while relocation is pending, preparing, or retryable.
- Expose relocation state through CU-CP NTN UE/runtime status and O-CU-CP `ntn_state` / `ntn_ues`.

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
- Multi-satellite policy.
- PDU session queuing while relocation is pending.
- Automatic selection of another digital child beam when the selected access DU cannot serve the current digital beam.

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

1. RRC setup from the selected access DU follows CUCP-018 behavior.
2. RRC setup from a different DU is temporarily accepted only when the current digital beam has a valid selected access DU that also supports the same digital beam.
3. After successful initial context setup, CU-CP triggers an existing intra-CU inter-DU handover to the selected access/service DU before first PDU session setup.
4. PDU session setup while relocation is pending, preparing, or retryable is rejected with `radio_res_not_available`.
5. Relocation state is cleared on successful handover or UE removal.
6. Invalid target, stale assistance, draining beam, or unsupported service DU keeps CUCP-018 rejection behavior.

## Required tests

1. `cu_cp_ntn_mobility_test` covers temporary RRC setup from a non-selected access DU and pending relocation status.
2. `cu_cp_ntn_mobility_test` covers PDU session rejection while relocation is pending or preparing.
3. `cu_cp_ntn_mobility_test` covers existing CUCP-018 rejection when no valid relocation target exists.
4. `cu_cp_unit_config_test` covers `ntn_state` and `ntn_ues` relocation observability.
5. Path guard, rejected overlap, and task metadata validation must pass with this task file.

## Validation

- `ctest --test-dir build/ai-clean -R "ntn_relocation|cu_cp_ntn_mobility|ntn_beam_placement|cu_cp_unit_config" --output-on-failure`
- `BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh`
- `python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-019-ntn-pre-service-inter-du-relocation.md`
- `python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base`
- `python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-019-ntn-pre-service-inter-du-relocation.md`

## Done means

- Non-selected access DU setup is admitted only when pre-service relocation can be deterministic.
- Relocation is triggered before first service demand.
- PDU setup is blocked while relocation is unresolved.
- O-CU-CP observability shows relocation counters and UE targets.
- Path guard and required focused validation pass, or failures are documented.
