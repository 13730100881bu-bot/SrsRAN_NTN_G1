# CUCP-004: Harness NTN control-plane suite

## Goal

Complete the CU-CP-only NTN harness before feature development continues.
This task creates planning context, templates, task cards, and validation scripts.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/audit/path_review_notes.md`
- `ai_harness/audit/rejected_or_quarantined_paths.txt`

## Accepted local contracts

- CU-CP only.
- Harness-only implementation task.
- No production code changes.
- Configuration root remains `mobility_config.ntn_location_mobility`.

## In scope

- Add NTN CU-CP context documents.
- Add CU-CP task and run-summary templates.
- Add CUCP-005 through CUCP-012 task cards.
- Add path guard task-file support.
- Add Python rejected-overlap and task-metadata checks.
- Add PowerShell and bash task validation runners.

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
- Any production source code.

## Allowed edit paths

- `ai_harness/`

## Allowed task exception paths

- None

## Required behavior

1. `ai_harness/context` explains NTN CU-CP requirements, boundaries, terms, and roadmap.
2. `ai_harness/tasks` contains CUCP-005 through CUCP-012 cards that can be executed independently.
3. `guard_changed_paths.py` and `guard_changed_paths.ps1` accept `--task-file` / `-TaskFile`.
4. Rejected/quarantined overlap has Python and PowerShell checks.
5. Task metadata validation checks required task-card sections.
6. Validation runners write logs under `ai_harness/results/<TASK>-<timestamp>/`.

## Required tests

1. Path guard passes for this harness-only diff.
2. Rejected overlap check passes.
3. Task metadata validation passes for at least `CUCP-005-runtime-taxonomy.md`.
4. Script syntax checks pass for new Python, PowerShell, and bash scripts where the host supports them.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/guard_changed_paths.ps1 -Base ai/cucp-harness-base -TaskFile ai_harness/tasks/CUCP-004-harness-ntn-control-plane-suite.md
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/check_rejected_overlap.ps1 -Base ai/cucp-harness-base
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-004 -TaskFile ai_harness/tasks/CUCP-004-harness-ntn-control-plane-suite.md -SkipBuild
```

If bash and Python 3 are available:

```bash
python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-004-harness-ntn-control-plane-suite.md
python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base
python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-005-runtime-taxonomy.md
TASK_ID=CUCP-004 TASK_FILE=ai_harness/tasks/CUCP-004-harness-ntn-control-plane-suite.md SKIP_BUILD=1 bash ai_harness/scripts/run_task_validation.sh
```

## Done means

- The change is inside `ai_harness/`.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Metadata validation passes for generated task cards.
- Required validation passes, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
