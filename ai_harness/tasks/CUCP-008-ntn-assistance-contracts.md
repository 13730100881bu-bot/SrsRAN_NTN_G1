# CUCP-008: NTN assistance contracts

## Goal

Create CU-CP-side NTN assistance snapshots for RRC/SIB19 contracts without
implementing DU-owned system information scheduling.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_interface_contracts.md`
- `ai_harness/audit/path_review_notes.md`
- `ai_harness/audit/rejected_or_quarantined_paths.txt`

## Accepted local contracts

- CU-CP only.
- Assistance snapshot may include ephemeris, Common TA, Koffset, Kmac, UL sync validity, reference location, t-Service, and neighbour assistance.
- Actual SI scheduling and broadcast are out of scope.

## In scope

- Add CU-CP assistance snapshot types and generation from satellite runtime state.
- Add RRC/SIB19 packaging or callback tests only in approved paths.
- Expose assistance snapshot through NTN observability.

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
- SI scheduling and SIB broadcast execution.

## Allowed edit paths

- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- `include/srsran/rrc/rrc_ue.h`
- `lib/rrc/ue/rrc_ue_impl.h`
- `lib/rrc/ue/rrc_ue_message_handlers.cpp`
- `include/srsran/ntn/orbit_propagator.h`
- `lib/ntn/orbit_propagator.cpp`
- `docs/superpowers/specs/2026-06-05-ntn-cucp-inventory-service-calendar-design.md`
- `docs/superpowers/plans/2026-06-05-ntn-cucp-inventory-service-calendar-implementation.md`

## Required behavior

1. Valid satellite state generates an assistance snapshot.
2. Stale satellite state marks assistance invalid.
3. Serving and neighbour assistance can be represented without DU scheduling.

## Required tests

1. Assistance generation tests for manual, circular, and TLE-backed state.
2. Invalid/stale snapshot tests.
3. RRC/SIB19 contract test proving CU-CP packaging input is present and bounded.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-008 -TaskFile ai_harness/tasks/CUCP-008-ntn-assistance-contracts.md -CTestRegex "cu_cp_ntn_mobility_test|ntn_satellite_state_updater|rrc"
```

## Done means

- Assistance contracts are CU-CP-side only.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
