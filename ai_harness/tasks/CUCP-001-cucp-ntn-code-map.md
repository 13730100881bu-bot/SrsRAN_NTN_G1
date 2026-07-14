# CUCP-001: Build CU-CP NTN code map

## Goal

Analyze this repository and write a CU-CP-focused code map for future NTN control-plane tasks.

This is a read-and-document task. Do not modify production code.

## Read first

- AGENTS.md
- ai_harness/context/cucp_scope.md
- ai_harness/context/ntn_cucp_spec_matrix.md
- ai_harness/context/allowed_paths.md
- ai_harness/audit/goal_mode_import_report.md
- ai_harness/audit/path_review_notes.md
- ai_harness/audit/accepted_paths.txt
- ai_harness/audit/rejected_or_quarantined_paths.txt

## Allowed writes

You may write only:

- ai_harness/context/cucp_code_map.md
- ai_harness/results/CUCP-001-codex-final.md

Do not modify production code.

## What to find

Identify likely paths for:

1. CU-CP source files
2. CU-CP public headers
3. CU-CP unit tests
4. CU-CP integration/component tests
5. CU-CP configuration parsing and validation
6. o_cu_cp configuration glue
7. RRC control-plane logic
8. SIB / system information control-plane logic
9. F1AP-CU / F1-C logic
10. NGAP / NG-C logic
11. E1 control-plane logic
12. UE manager
13. Measurement manager
14. Mobility manager
15. CU-CP satellite updater
16. orbit_propagator / beam_hopping_table dependency paths, only if they are used by the CU-CP satellite updater path
17. Logs and metrics relevant to CU-CP NTN work

Also identify paths that look NTN-relevant but are forbidden for this harness because they belong to:

- O-DU
- flexible_o_du
- DU
- DU-high
- MAC
- scheduler
- PHY
- lower PHY
- RU / RF / radio
- ZMQ
- GIS site behavior
- PRACH
- HARQ timing
- TA scheduler

## Final file format

Write ai_harness/context/cucp_code_map.md with these sections:

1. Summary
2. Confirmed CU-CP paths
3. Approved control-plane exception paths
4. Candidate paths needing human review
5. CU-CP test paths
6. RRC / SIB / system information paths
7. F1AP-CU paths
8. NGAP paths
9. E1 paths
10. UE context / mobility / measurement paths
11. CU-CP satellite updater paths
12. orbit_propagator / beam_hopping_table dependency notes
13. o_cu_cp and config glue paths
14. Forbidden paths for this harness
15. Recommended updates to allowed_paths.md
16. Open questions

## Important rules

- Be conservative.
- Do not mark broad directories as allowed unless they are clearly CU-CP-only.
- If a path is mixed CU/DU or mixed control/user plane, mark it as candidate needing human review.
- Do not modify production code.
- Do not move rejected/quarantined files back into the active baseline.

## Validation

After writing the file, run:

git diff --name-only

Then run the PowerShell guard:

powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/guard_changed_paths.ps1 -Base ai/cucp-harness-base

The only intended changed files are ai_harness task/result/context files.
