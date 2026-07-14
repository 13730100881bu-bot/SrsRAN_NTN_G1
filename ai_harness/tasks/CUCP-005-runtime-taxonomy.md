# CUCP-005: Runtime taxonomy

## Goal

Separate CU-CP NTN runtime terms so future code no longer mixes candidate,
active, served, loaded, and draining beam semantics.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/audit/path_review_notes.md`
- `ai_harness/audit/rejected_or_quarantined_paths.txt`

## Accepted local contracts

- CU-CP only.
- Configuration root: `mobility_config.ntn_location_mobility`.
- `candidate_inventory` is never capped by loaded-beam limits.
- `max_nof_loaded_service_beams=0` means no CU-CP cap.

## In scope

- Add or rename CU-CP runtime types for candidate inventory, mobility eligible beams,
  active loaded beams, draining beams, loaded service calendar, assistance snapshot,
  and UE NTN context.
- Update CU-CP tests to assert the new taxonomy.

## Out of scope

- O-DU and flexible_o_du.
- DU and DU scheduler behavior.
- MAC scheduler behavior.
- HARQ timing execution.
- TA scheduler behavior.
- PRACH behavior.
- PHY, lower PHY, RU, RF, radio drivers.
- ZMQ channel behavior.
- GIS-site behavior.

## Allowed edit paths

- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- None

## Required behavior

1. CU-CP exposes separate runtime snapshots for candidate, mobility eligible,
   active loaded, draining, and inactive beams.
2. Empty candidate beams do not appear as active loaded beams.
3. Existing NTN disabled and terrestrial behavior remains unchanged.

## Required tests

1. Unit test for taxonomy conversion and state naming.
2. CU-CP test showing visible empty beams are candidates, not active loaded.
3. Negative test proving draining beams are excluded from mobility target eligibility.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-005 -TaskFile ai_harness/tasks/CUCP-005-runtime-taxonomy.md -CTestRegex "cu_cp_ntn_mobility_test|ntn_mobility_test"
```

## Done means

- CU-CP-only paths were edited.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
