# CUCP-016: NTN service-area paging observability

## Goal

Expose beam-derived TAC service-area state and paging recommendation eligibility
through CU-CP NTN runtime status and O-CU-CP command output, while keeping the
CUCP-015 single-satellite paging-assistance behavior inside CU-CP scope.

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
- The task is single-satellite only; do not introduce a multi-satellite catalog.
- Beam-derived TAC is parsed from the final contiguous decimal digit run in
  `ntn_beam_position::beam_id`.
- TAC values must be valid 24-bit TAC values in `[0, 16777215]`.
- Paging assistance is a CU-CP hint and does not imply DU paging scheduling.
- Release paging recommendation requires a valid beam-derived TAC; core location
  reporting keeps the existing DU/NGAP TAC fallback.

## In scope

- Extend `cu_cp_ntn_beam_status` with derived TAC state and paging
  recommendation reason.
- Extend `cu_cp_ntn_runtime_status` with service-area and paging eligibility
  counters.
- Compute service-area status in `cu_cp_impl::get_current_ntn_beam_status()`.
- Tighten UE release paging recommendation so invalid beam-derived TAC does not
  create NTN recommended-cell hints.
- Add `ntn_state` and `ntn_beams all` observability output for TAC and paging
  eligibility.
- Add focused CU-CP and O-CU-CP command tests.

## Out of scope

- Multi-satellite catalog or RF gateway switching.
- UE idle reselection.
- DU paging scheduler behavior.
- O-DU and flexible_o_du.
- DU and DU scheduler behavior.
- MAC scheduler behavior.
- HARQ timing execution.
- TA scheduler behavior.
- PRACH behavior.
- PHY, lower PHY, RU, RF, radio drivers.
- ZMQ channel behavior.
- GIS-site behavior.
- NGAP ASN.1 helper changes.
- Generated ASN.1 files.

## Allowed edit paths

- `ai_harness/**`
- `include/srsran/cu_cp/**`
- `lib/cu_cp/**`
- `tests/unittests/cu_cp/**`

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

## Required behavior

1. Beam status exposes `derived_tac`, `derived_tac_reason`, `paging_recommendable`,
   and `paging_recommendation_reason`.
2. `derived_tac_reason` uses deterministic values: `valid`,
   `missing_decimal_suffix`, or `out_of_range`.
3. `paging_recommendation_reason` uses deterministic values: `eligible`,
   `stale_assistance`, `inactive`, `draining`, or `invalid_tac`.
4. Fresh `candidate` and `active_loaded` beams with valid TAC are paging
   recommendable; stale, draining, inactive, or invalid-TAC beams are not.
5. Runtime status counts valid service-area beams, invalid service-area beams,
   and paging-recommendable beams.
6. `ntn_state` prints service-area counters and `ntn_beams all` prints `tac`
   and `paging` columns.
7. UE release complete does not include NTN recommended cells when the serving
   beam ID cannot produce a valid beam-derived TAC.

## Required tests

1. `cu_cp_ntn_mobility_test` proves a fresh `CN-BEAM-0001` candidate or
   active-loaded beam exposes TAC `1` and `paging_recommendable=true`.
2. `cu_cp_ntn_mobility_test` proves an invalid beam ID exposes an invalid TAC
   reason and does not generate UE-release paging recommendations.
3. `cu_cp_ntn_mobility_test` proves stale assistance and draining beams expose
   the correct non-recommendation reasons.
4. `cu_cp_unit_config_test` proves `ntn_state` prints service-area counters.
5. `cu_cp_unit_config_test` proves `ntn_beams all` prints TAC and paging
   eligibility fields.

## Validation

```powershell
ctest --test-dir build/ai -R "ntn_beam_tac|cu_cp_ntn_mobility_test|cu_cp_paging_test|cu_cp_unit_config" --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-016 -TaskFile ai_harness/tasks/CUCP-016-ntn-service-area-paging-observability.md -CTestRegex "ntn_beam_tac|cu_cp_ntn_mobility_test|cu_cp_paging_test|cu_cp_unit_config"
```

Required final validation:

```bash
bash ai_harness/scripts/configure_build.sh
JOBS=1 bash ai_harness/scripts/build.sh
bash ai_harness/scripts/run_cucp_tests.sh
python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-016-ntn-service-area-paging-observability.md
python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base
python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-016-ntn-service-area-paging-observability.md
```

## Done means

- Service-area and paging eligibility observability is exposed by CU-CP.
- UE release paging recommendation requires valid beam-derived TAC.
- No DU/MAC/PHY/RU/RF/GIS or NGAP ASN.1 helper changes are introduced.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
