# CUCP-010: NGAP core mapping

## Goal

Map NTN UE, beam, and service-area state into NGAP-facing user location,
Mapped Cell, derived TAC, TAI, and AMF reporting behavior.

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
- NGAP changes must be control-plane location/reporting behavior.
- AMF control is enabled by default; local forwarding is disabled by default.

## In scope

- Generate NR CGI, Mapped Cell, derived TAC, TAI, timestamp, and area-of-interest presence.
- Handle direct, change-of-serving-cell, area-of-interest, stop, and cancel reporting.
- Apply report throttling, accuracy gates, and privacy gates.

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

- `include/srsran/ngap/ngap.h`
- `include/srsran/ngap/ngap_location_reporting.h`
- `lib/ngap/ngap_asn1_converters.h`
- `lib/ngap/ngap_impl.cpp`
- `lib/ngap/ngap_impl.h`
- `tests/unittests/ngap/ngap_ue_context_management_procedure_test.cpp`
- `tests/unittests/ngap/test_helpers.h`
- `docs/superpowers/plans/2026-06-05-ntn-cucp-inventory-service-calendar-implementation.md`
- `docs/superpowers/plans/2026-06-06-ntn-cucp-ngap-switch-over.md`
- `docs/superpowers/specs/2026-06-05-ntn-cucp-inventory-service-calendar-design.md`
- `docs/superpowers/specs/2026-06-06-ntn-cucp-ngap-switch-over-design.md`

## Required behavior

1. UE location maps to NGAP user location with NR CGI, TAI, optional derived TAC, and timestamp.
2. AMF LocationReportingControl events are accepted, stopped, or cancelled deterministically.
3. Area-of-interest reports are generated only when configured policy matches.
4. Reports are throttled by configured minimum interval.

## Required tests

1. NGAP converter tests for Mapped Cell, derived TAC, TAI, and timestamp.
2. CU-CP tests for AMF direct and area-of-interest control.
3. Negative tests for stale, inaccurate, throttled, and cancelled reports.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-010 -TaskFile ai_harness/tasks/CUCP-010-ngap-core-mapping.md -CTestRegex "cu_cp_ntn_mobility_test|ngap"
```

## Done means

- NGAP changes remain control-plane only.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
