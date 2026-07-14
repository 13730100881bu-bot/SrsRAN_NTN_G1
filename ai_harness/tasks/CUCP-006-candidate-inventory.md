# CUCP-006: Candidate inventory

## Goal

Compute full NTN candidate beam inventory from satellite/service-area state
without using loaded-service resource limits to cap coverage.

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
- Admission elevation default: `50 deg`.
- Release elevation is separate and uses hysteresis.
- `candidate_inventory` is not capped by `max_nof_loaded_service_beams`.

## In scope

- Update CU-CP NTN selection so all admissible beams are retained as candidates.
- Add explicit reason state for excluded or inactive beams.
- Preserve old config compatibility while introducing loaded-beam limit semantics.

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

- `docs/superpowers/specs/2026-06-05-ntn-cucp-inventory-service-calendar-design.md`
- `docs/superpowers/plans/2026-06-05-ntn-cucp-inventory-service-calendar-implementation.md`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config.h`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_cli11_schema.cpp`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_validator.cpp`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`
- `include/srsran/ntn/beam_hopping_table.h`
- `include/srsran/ntn/orbit_propagator.h`
- `lib/ntn/beam_hopping_table.cpp`
- `lib/ntn/orbit_propagator.cpp`

## Required behavior

1. More visible beams than the loaded-beam limit remain present as candidates.
2. Candidate admission uses elevation, service window, PLMN/TAC/slice, enabled flag, and DU support.
3. Stale satellite state blocks new candidates from becoming admission-eligible.

## Required tests

1. Selector test with 1000+ candidate beams and no loaded-beam truncation.
2. Test for `50 deg` admission and lower release hysteresis.
3. Negative test for disabled, stale, unsupported, and out-of-service-window beams.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-006 -TaskFile ai_harness/tasks/CUCP-006-candidate-inventory.md -CTestRegex "cu_cp_ntn_mobility_test|ntn_mobility_test"
```

## Done means

- Candidate inventory is not capped by loaded service limits.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
