# CUCP-011: Switch-over and resilience

## Goal

Represent soft and hard NTN service/feeder switch-over events in CU-CP and
apply safe admission, draining, handover, release, and manual override policy.

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
- Switch-over is represented as a control-plane service event.
- Physical feeder/gateway/RF behavior is out of scope.

## In scope

- Add soft and hard service-window events.
- Stop or reduce new admission for affected beams.
- Move affected loaded beams into draining, handover, or release policy.
- Add manual override and source priority handling.

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
- Physical feeder link switch-over.

## Allowed edit paths

- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- `docs/superpowers/plans/2026-06-05-ntn-cucp-inventory-service-calendar-implementation.md`
- `docs/superpowers/plans/2026-06-06-ntn-cucp-ngap-switch-over.md`
- `docs/superpowers/specs/2026-06-05-ntn-cucp-inventory-service-calendar-design.md`
- `docs/superpowers/specs/2026-06-06-ntn-cucp-ngap-switch-over-design.md`

## Required behavior

1. Soft switch-over starts a preparation window without immediate UE release.
2. Hard switch-over blocks new access and drains or releases affected beams.
3. Manual override can freeze, restore, or replace satellite/service state.
4. Source priority is manual, fresh TLE, circular fallback.

## Required tests

1. Soft event test for pre-handover/drain readiness.
2. Hard event test for admission stop and drain/release policy.
3. Manual override and source priority tests.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-011 -TaskFile ai_harness/tasks/CUCP-011-switch-over-resilience.md -CTestRegex "cu_cp_ntn_mobility_test|ntn_mobility_test"
```

## Done means

- Switch-over behavior is CU-CP control-plane only.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
