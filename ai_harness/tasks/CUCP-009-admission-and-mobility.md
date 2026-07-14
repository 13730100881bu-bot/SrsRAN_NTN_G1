# CUCP-009: Admission and mobility

## Goal

Apply NTN state to CU-CP admission, reestablishment, handover target selection,
draining, and stale-ephemeris policy.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/audit/path_review_notes.md`
- `ai_harness/audit/rejected_or_quarantined_paths.txt`

## Accepted local contracts

- CU-CP only.
- Candidate beams may be handover targets.
- Draining beams must not admit new UEs.
- Stale ephemeris stops new access and keeps existing UEs draining when possible.

## In scope

- Gate UE setup, reestablishment, handover, and PDU-session-driven demand using NTN state.
- Promote candidate target beams to loaded service demand during handover preparation.
- Clean up demand after handover success, failure, release, or stale service window.

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
- UE-side idle reselection.

## Allowed edit paths

- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- `include/srsran/rrc/rrc_ue.h`
- `lib/rrc/ue/rrc_ue_impl.h`
- `lib/rrc/ue/rrc_ue_message_handlers.cpp`
- `docs/superpowers/specs/2026-06-05-ntn-cucp-inventory-service-calendar-design.md`
- `docs/superpowers/plans/2026-06-05-ntn-cucp-inventory-service-calendar-implementation.md`

## Required behavior

1. Candidate beams can trigger NTN handover.
2. Draining beams are rejected for new access and new handover target selection.
3. Stale ephemeris stops new access and starts draining for loaded beams.
4. Handover success and failure update UE NTN runtime context deterministically.

## Required tests

1. Admission tests for location accuracy, elevation, service window, and stale ephemeris.
2. Mobility tests for candidate target, draining target rejection, success, and failure.
3. Release/reestablishment tests proving demand cleanup.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-009 -TaskFile ai_harness/tasks/CUCP-009-admission-and-mobility.md -CTestRegex "cu_cp_ntn_mobility_test|cu_cp_mobility_test|mobility"
```

## Done means

- NTN admission and mobility policy remains CU-CP-only.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
