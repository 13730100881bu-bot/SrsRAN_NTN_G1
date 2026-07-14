# CUCP-017: NTN analog/digital hex beam model

## Goal

Implement a CU-CP-only two-level NTN beam inventory where analog access beams are deterministic 7-cell hex clusters over digital service beams.

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
- Digital service beams keep NCI, beam-derived TAC, loaded service calendar, SR/SRS request and QoS policy semantics.
- Analog access beams are CU-CP control-plane eligibility groups only; they do not imply DU/MAC/PHY/RU beamforming control.
- Do not cap `candidate_inventory` with loaded-beam resource limits.

## In scope

- Add analog access beam inventory metadata to CU-CP beam-table parsing and runtime snapshots.
- Generate a deterministic 500 km / 50 deg / 15 km hex profile with 843 digital beams and 137 analog access clusters.
- Use ring-1 axial hex clusters with child offsets `(0,0)`, `(1,0)`, `(1,-1)`, `(0,-1)`, `(-1,0)`, `(-1,1)`, `(0,1)`.
- Use analog access beam eligibility to mark child digital beams as access-eligible for CU-CP admission/mobility decisions.
- Keep loaded service calendar demand-driven and digital-beam scoped.
- Expose analog/digital inventory and eligibility in O-CU-CP `ntn_state` and `ntn_beams` observability.

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
- Real analog beamforming or SSB/PRACH scheduling.

## Allowed edit paths

- `ai_harness/`
- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `apps/units/o_cu_cp/cu_cp/cu_cp_config_translators.cpp`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config.h`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_cli11_schema.cpp`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_validator.cpp`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_yaml_writer.cpp`
- `configs/leo_500km_beam_table.json`
- `configs/leo_500km_cucp_ntn.yml`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`
- `utils/ntn/generate_leo_beam_table.py`

## Required behavior

1. A hierarchical beam table with `analog_beams` and digital `beams` parses into CU-CP config.
2. Every digital beam may reference exactly one parent analog beam; every analog child must refer to a configured digital beam.
3. Full analog clusters have exactly seven child digital beams; edge partial clusters may have fewer than seven only when `is_edge_partial=true`.
4. Analog access eligibility gates new access/mobility eligibility at CU-CP control-plane level; digital loaded service remains demand-driven.
5. Existing single-level beam tables remain valid and keep previous behavior.

## Required tests

1. Parser tests for valid hierarchical table, duplicate child rejection, unknown parent rejection and invalid non-edge partial cluster rejection.
2. Scheduler/runtime tests showing analog access window selection keeps full digital candidate inventory while only child beams of selected analog clusters are in the access window.
3. O-CU-CP config/command tests showing LEO example counts and analog/digital observability.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-017 -TaskFile ai_harness/tasks/CUCP-017-ntn-analog-digital-hex-beam-model.md -CTestRegex "cell_meas_manager|ntn_served_beam_scheduler|ntn_beam_placement|cu_cp_ntn_mobility|cu_cp_unit_config"
```

If bash is available:

```bash
TASK_ID=CUCP-017 TASK_FILE=ai_harness/tasks/CUCP-017-ntn-analog-digital-hex-beam-model.md CTEST_REGEX="cell_meas_manager|ntn_served_beam_scheduler|ntn_beam_placement|cu_cp_ntn_mobility|cu_cp_unit_config" bash ai_harness/scripts/run_task_validation.sh
```

## Done means

- The change is inside CU-CP scope.
- Path guard passes with the CUCP-017 task file.
- Rejected/quarantined overlap check passes.
- Required tests are added or updated.
- Required validation passes, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation, risks, and requested exceptions.
