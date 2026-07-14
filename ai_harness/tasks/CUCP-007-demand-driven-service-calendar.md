# CUCP-007: Demand-driven service calendar

## Goal

Generate CU-CP slot request intent only for beams with UE, DRB,
reestablishment, or handover demand.

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
- Empty candidate beams consume no antenna slot, SR request, or SRS request.
- F1AP-CU slot requests are control-plane intent only.

## In scope

- Update beam placement/calendar logic to allocate service only to loaded beams.
- Keep loaded draining beams until UE/DRB demand clears or policy releases them.
- Update CU-CP and F1AP-CU contract tests for set, update, and clear behavior.

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
- `include/srsran/f1ap/cu_cp/f1ap_cu_ue_context_update.h`
- `include/srsran/f1ap/ntn_ul_slot_resource_request.h`
- `lib/f1ap/cu_cp/procedures/ue_context_modification_procedure.cpp`
- `lib/f1ap/cu_cp/procedures/ue_context_setup_procedure.cpp`
- `tests/unittests/f1ap/common/f1ap_asn1_helpers_test.cpp`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_modification_procedure_test.cpp`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_setup_procedure_test.cpp`

## Required behavior

1. Empty candidate beams have slot period zero and no SR/SRS request.
2. UE/DRB/HO demand promotes a beam into the loaded service calendar.
3. Unchanged slot requests are not retransmitted.
4. Demand removal clears the CU-CP slot request contract.

## Required tests

1. Unit test for loaded calendar ordering by UE, DRB, priority, elevation, and age.
2. CU-CP test for initial access generating a slot request.
3. F1AP-CU contract tests for setup, modification, unchanged, and clear cases.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-007 -TaskFile ai_harness/tasks/CUCP-007-demand-driven-service-calendar.md -CTestRegex "cu_cp_ntn_mobility_test|f1ap_cu"
```

## Done means

- Slot request intent is demand-driven.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
