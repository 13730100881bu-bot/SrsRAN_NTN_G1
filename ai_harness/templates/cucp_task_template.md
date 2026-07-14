# CUCP-XXX: <task title>

## Goal

Describe the single CU-CP NTN behavior this task implements or verifies.

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
- Configuration root: `mobility_config.ntn_location_mobility`.
- Admission elevation default: `50 deg`.
- `max_nof_loaded_service_beams=0` means no CU-CP cap.
- Do not cap `candidate_inventory` with loaded-beam resource limits.

## In scope

- List the specific CU-CP-owned behavior for this task.

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

- `ai_harness/`
- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- None

## Required behavior

1. Describe required behavior.
2. Describe required compatibility behavior.
3. Describe required boundary behavior.

## Required tests

1. Describe the focused unit test.
2. Describe the CU-CP integration or contract test.
3. Describe the negative/boundary test.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-XXX -TaskFile ai_harness/tasks/CUCP-XXX-<slug>.md
```

If bash is available:

```bash
TASK_ID=CUCP-XXX TASK_FILE=ai_harness/tasks/CUCP-XXX-<slug>.md bash ai_harness/scripts/run_task_validation.sh
```

## Done means

- The change is inside CU-CP scope.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests are added or updated.
- Required validation passes, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
