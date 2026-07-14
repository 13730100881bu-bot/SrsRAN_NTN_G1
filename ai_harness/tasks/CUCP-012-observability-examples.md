# CUCP-012: Observability and examples

## Goal

Expose CU-CP NTN runtime state through commands, logs, and example
configuration that reflect the final candidate-inventory and loaded-calendar
semantics.

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
- Commands and examples must describe control-plane state, not physical execution.
- The LEO example must not imply a three-beam coverage cap.

## In scope

- Add or update `ntn_state`, `ntn_assistance`, `ntn_beams`, and `ntn_ues` style command output.
- Update example configuration and docs to show full candidate inventory and loaded service calendar semantics.
- Add tests for command snapshots where existing seams allow it.

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

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_yaml_writer.cpp`
- `configs/CMakeLists.txt`
- `configs/leo_500km_beam_table.json`
- `configs/leo_500km_cucp_ntn.yml`

## Required behavior

1. Operators can inspect satellite state, assistance validity, beam state, and UE NTN context.
2. Beam output includes state reason, elevation, DU, UE/DRB load, slot request intent, and drain status.
3. Example config uses `50 deg` admission and full candidate inventory semantics.

## Required tests

1. Command snapshot tests for beam and satellite state.
2. Config example validation test.
3. Negative test proving output does not label empty candidates as active loaded.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-012 -TaskFile ai_harness/tasks/CUCP-012-observability-examples.md -CTestRegex "cu_cp_ntn_mobility_test|cu_cp_app_unit"
```

## Done means

- Observability reflects CU-CP runtime semantics.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
