# CUCP-031 NTN Resource Repair Executor And Guarded Recovery

## Goal

Turn CUCP-030 repair decisions into executed CU-CP recovery actions for NTN RNTI lease pools, digital SR/SRS
assignments, and handover target reservations.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_interface_contracts.md`
- `ai_harness/tasks/CUCP-030-ntn-resource-consistency-auditor-and-repair-loop.md`

## Accepted local contracts

- CU-CP remains authoritative for NTN RNTI lease pools and digital SR/SRS assignment state.
- Repair is conservative: resend, clear, roll back, or block new NTN demand; do not release committed UEs solely due to
  one audit mismatch.
- Retry limit is fixed at one retry in this task.
- Terrestrial RNTI, RACH, SR/SRS, and handover behavior remain unchanged.
- Generated ASN.1 files are not modified.

## In scope

- Track repair action lifecycle as `queued`, `sent`, `applied`, `failed`, `retry_exhausted`, or `blocked_conflict`.
- Execute `resend_rnti_lease_pool` through the existing private F1AP resource coordination path.
- Execute SR/SRS apply and clear repair through the existing UE Context Modification NTN SR/SRS container.
- Roll back stale handover target RNTI reservations without touching source service resources.
- Expose repair counters through CU-CP and O-CU-CP NTN status.

## Out of scope

- New F1AP wire formats.
- New DU/MAC/Scheduler policy.
- PHY, lower PHY, PRACH physical procedure, HARQ timing, TA scheduler, RU/RF, ZMQ, GIS, O-DU, and flexible_o_du.
- Forcibly releasing committed UEs on audit mismatch.

## CU-CP only

This task is CU-CP-led. It reuses existing task-scoped F1AP/DU contracts from CUCP-026 through CUCP-030 and only adds
O-CU-CP status output as a narrow exception.

## Forbidden areas

- O-DU and flexible_o_du.
- PHY/lower PHY.
- PRACH physical procedure internals.
- HARQ and TA scheduler internals.
- RU/RF/ZMQ.
- GIS/site code.
- Generated ASN.1.
- Terrestrial RNTI or SR/SRS allocation policy.

## Allowed edit paths

- `ai_harness/**`
- `include/srsran/cu_cp/**`
- `lib/cu_cp/**`
- `tests/unittests/cu_cp/**`

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

## Required behavior

1. CU-CP queues each repair decision and executes only queued actions.
2. RNTI pool resend repairs are sent to the DU/cell that failed audit.
3. SR/SRS apply or clear repairs are sent through UE Context Modification and update repair state from DU response.
4. Repeated repair failure exhausts after one retry and blocks further automatic execution for that mismatch.
5. Resource conflict repairs are observable and block new NTN demand without releasing committed UEs.

## Required tests

1. Resource-manager tests cover repair lifecycle and one-retry exhaustion.
2. Resource-manager audit tests cover missing DU SR/SRS generating `apply_sr_srs_assignment`.
3. O-CU-CP command tests cover repair counters in `ntn_state`.

## Validation

```bash
ctest --test-dir build/ai-clean -R "ntn_resource_repair|ntn_resource_audit|ntn_beam_service_resource|gnbdu_resource_coordination|cu_cp_ntn_mobility|f1ap_cu|f1ap_du|cu_cp_unit_config" --output-on-failure
BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh
python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-031-ntn-resource-repair-executor-and-guarded-recovery.md
python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-031-ntn-resource-repair-executor-and-guarded-recovery.md
python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-031-ntn-resource-repair-executor-and-guarded-recovery.md
```

## Done means

- Repair decisions are converted into executed CU-CP recovery actions.
- Repair lifecycle is observable in snapshots and `ntn_state`.
- Required tests pass, or failures are explained with logs.
- Path guard and rejected-overlap results are reported.
