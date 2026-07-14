# CUCP-015: NTN paging and idle assistance

## Goal

Use single-satellite CU-CP NTN runtime state to derive beam-based TAC values,
populate paging recommendation hints on UE release, and narrow incoming paging
messages when the AMF provides recommended cells.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
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

## In scope

- Add a CU-CP helper for beam-id-derived TAC parsing.
- Use beam-derived TAC in NTN core location reporting when the serving beam can
  be resolved.
- Generate `info_on_recommended_cells_and_ran_nodes_for_paging` on UE release
  when the UE has fresh active or candidate NTN beam context.
- Respect valid AMF recommended cells in CU-CP paging distribution while keeping
  the current TAC-wide fallback for empty or unusable recommendations.
- Add focused CU-CP unit tests.

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
- Generated ASN.1 files.

## Allowed edit paths

- `ai_harness/**`
- `include/srsran/cu_cp/**`
- `lib/cu_cp/**`
- `tests/unittests/cu_cp/**`

## Allowed task exception paths

- `lib/ngap/ngap_asn1_helpers.h`

## Required behavior

1. `CN-BEAM-0007` derives TAC `7`, `beam-42` derives TAC `42`, beam IDs without
   trailing digits or with values above `16777215` are invalid without fatal
   errors.
2. Accepted NTN location reports prefer beam-derived TAC for `tai.tac` and
   `ntn_derived_tac`; existing DU/NGAP TAC fallback remains for unresolved
   beams.
3. UE release complete includes recommended cells for paging only when the last
   NTN UE context maps to a fresh active or candidate beam.
4. Draining, inactive, stale, unknown, or invalid beam context does not create
   NTN paging recommendations.
5. Incoming NGAP Paging with valid recommended cells is forwarded only to
   matching DU/cell targets.
6. Incoming NGAP Paging with empty or unusable recommended cells keeps the
   existing TAC-wide fallback behavior.

## Required tests

1. Beam-derived TAC parser positive and negative tests.
2. NTN location report test proving NR NTN TAI info carries the beam-derived
   TAC.
3. UE release test proving NTN recommended cells are populated for valid beam
   context and skipped for stale or draining context.
4. Paging distribution test proving recommended cells narrow F1AP Paging.
5. Paging distribution fallback test proving unusable recommendations do not
   drop a paging message that can be served by TAC.

## Validation

```powershell
ctest --test-dir build/ai-clean -R "cu_cp_paging_test|cu_cp_ntn_mobility_test|ntn_paging" --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-015 -TaskFile ai_harness/tasks/CUCP-015-ntn-paging-idle-assistance.md -CTestRegex "cu_cp_paging_test|cu_cp_ntn_mobility_test|ntn_paging"
```

Required final validation:

```bash
bash ai_harness/scripts/configure_build.sh
bash ai_harness/scripts/build.sh
bash ai_harness/scripts/run_cucp_tests.sh
python ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-015-ntn-paging-idle-assistance.md
python ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base
python ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-015-ntn-paging-idle-assistance.md
```

## Done means

- Beam-derived TAC is used by CU-CP NTN location and paging assistance.
- Paging narrowing preserves the current safe fallback.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
