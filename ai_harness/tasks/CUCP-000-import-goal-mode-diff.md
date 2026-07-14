# CUCP-000: Import and classify existing Codex goal-mode changes

## Goal

Classify the existing Codex goal-mode changes before continuing CU-CP-limited NTN development.

## Inputs

Read:

- ai_harness/audit/baseline_commit.txt
- ai_harness/audit/goal_snapshot_commit.txt
- ai_harness/audit/goal_mode_changed_files.txt
- ai_harness/audit/goal_mode_full.diff

## Current boundary

The future harness is restricted to CU-CP NTN control-plane work.

Treat the following as likely allowed areas if they are present in this repository:

- CU-CP source and headers
- CU-CP tests
- RRC control-plane logic
- F1-C control-plane logic
- NG-C / NGAP control-plane logic
- E1 control-plane glue
- CU-CP configuration parsing and validation
- ai_harness files

Treat the following as likely out of CU-CP scope:

- DU-high scheduler
- MAC scheduler
- HARQ timing implementation
- TA scheduler implementation
- PRACH
- PHY
- lower PHY
- RU / RF / radio drivers
- ZMQ channel behavior
- broad architecture refactors

## Rules

Do not modify production code.

Your final answer must be a Markdown report with these sections:

1. Summary
2. Baseline and snapshot commits
3. File-by-file classification table
4. CU-CP allowed changes
5. CU-CP-adjacent exception candidates
6. Harness/test/docs only changes
7. Out-of-CU-CP-scope changes
8. Unknown / human-review-required changes
9. Recommended accepted_paths.txt content
10. Recommended rejected_or_quarantined_paths.txt content
11. Risks if we continue without cleanup
12. Suggested next steps

For every changed file, classify it as exactly one of:

- CU-CP allowed
- CU-CP-adjacent exception candidate
- Harness/test/docs only
- Out of CU-CP scope
- Unknown / human review required

Be conservative. If unsure, classify as Unknown / human review required.
