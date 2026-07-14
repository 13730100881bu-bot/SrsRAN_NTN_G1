# CUCP-002: CU-CP NTN configuration validation

## Goal

Add, complete, or validate CU-CP-level NTN configuration handling.

This task must stay within CU-CP control-plane scope.

The repository already contains reviewed goal-mode changes. First inspect what
already exists. Do not create a parallel NTN configuration model if one already
exists.

## Human decision

For phase 1, CU-CP may support an NTN control-plane profile.

Suggested logical fields, if not already modeled differently by the existing
code:

```yaml
cu_cp:
  ntn:
    enabled: true
    orbit_type: geo
    sib19_enabled: true
```

If the current repository already uses a different approved schema, reuse the
existing schema and document it in the final answer.

## Accepted CU-CP NTN schema after CUCP-002

CUCP-002 confirmed that this repository does not use a new `cu_cp.ntn.orbit_type`
or `cu_cp.ntn.sib19_enabled` schema.

The accepted CU-CP-facing NTN configuration path is:

- `mobility_config.ntn_location_mobility`

The accepted satellite state source values are:

- `manual`
- `circular_orbit`
- `tle`

GEO-like behavior is represented through the `circular_orbit` source rather than
a dedicated `orbit_type: geo` field.

Future Codex tasks must reuse this schema and must not introduce a parallel
`cu_cp.ntn` schema unless a human-approved task explicitly requests a schema
migration.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/audit/path_review_notes.md`
- `ai_harness/audit/accepted_paths.txt`
- `ai_harness/audit/rejected_or_quarantined_paths.txt`
- `ai_harness/audit/goal_mode_import_report.md`

## First step before editing

Before modifying production code, inspect the current CU-CP NTN config path and
determine:

1. Whether NTN config structs already exist.
2. Whether o_cu_cp config glue already exists.
3. Whether validation already exists.
4. Whether unit tests already exist.
5. Which exact paths must be edited.

If a required path is not already allowed by
`ai_harness/context/allowed_paths.md` or clearly approved by
`ai_harness/audit/path_review_notes.md`, stop and report the needed exception
instead of editing it.

## Allowed production areas

Allowed only if already confirmed by `ai_harness/context/cucp_code_map.md` or
`path_review_notes.md`:

- CU-CP config structs
- CU-CP config validation
- CU-CP startup validation
- CU-CP tests
- Narrow `o_cu_cp` config glue required to pass config into CU-CP
- Narrow RRC / F1AP-CU / NGAP control-plane paths only if already approved and
  directly required

## Forbidden areas

Do not modify:

- O-DU
- `flexible_o_du`
- DU
- DU-high
- DU manager
- MAC scheduler
- HARQ timing
- TA scheduler
- PRACH
- PHY
- lower PHY
- RU / RF / radio drivers
- ZMQ channel behavior
- GIS site behavior
- broad app-wide or lib-wide refactors

Do not reintroduce files listed in:

- `ai_harness/audit/rejected_or_quarantined_paths.txt`

## Required behavior

Implement or verify these behaviors:

1. NTN disabled preserves existing terrestrial CU-CP behavior.
2. A valid GEO CU-CP NTN config is accepted.
3. Unknown orbit type is rejected, if `orbit_type` is part of the approved
   schema.
4. `sib19_enabled=true` without required CU-CP NTN context is rejected, unless
   the existing approved model has a different valid dependency rule.
5. Error messages are clear enough to diagnose invalid config.
6. Existing terrestrial CU-CP config remains valid.
7. Validation must not depend on DU/MAC/PHY behavior.

## Required tests

Add or update tests for:

1. NTN disabled config.
2. Valid GEO NTN config.
3. Invalid orbit type, if `orbit_type` exists.
4. Missing required NTN context, if applicable to the approved schema.
5. Existing terrestrial config remains valid.
6. No rejected/quarantined path is required for this behavior.

If tests already exist, strengthen or extend them instead of duplicating them.

## Boundary rule

If this repository version does not route this configuration through CU-CP, do
not force a broad refactor.

Instead:

1. Document the actual config path.
2. Add only the smallest already-approved config glue, if safe.
3. Report any new non-CU-CP exception needed for human review.
4. Do not modify forbidden paths.

## Validation commands

Always run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/guard_changed_paths.ps1 -Base ai/cucp-harness-base
```

If bash / CMake build environment is available, also run:

```bash
bash ai_harness/scripts/configure_build.sh
bash ai_harness/scripts/build.sh
bash ai_harness/scripts/run_cucp_tests.sh
```

If bash / CMake cannot run in this environment, report the exact limitation and
still run the PowerShell path guard.

## Done means

- The implementation stays inside CU-CP scope or an already-approved narrow
  control-plane exception.
- Tests are added or updated.
- Required validation commands pass, or failures are explained with logs.
- The PowerShell path guard passes.
- No rejected/quarantined paths are reintroduced.
- The final response lists:
  - files changed
  - behavior changed
  - tests added or updated
  - commands run
  - results
  - known risks
  - any new exception requested for human review
