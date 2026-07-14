# CUCP-024: NTN Beam Service Resource Manager

## Goal

Introduce a CU-CP-only NTN beam service resource manager that centralizes analog access ownership, digital SR/SRS slot
intent, and C-RNTI ownership validation for DU-reported C-RNTIs.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/context/ntn_cucp_runtime_contract.md`
- `ai_harness/context/ntn_cucp_interface_contracts.md`
- `ai_harness/tasks/CUCP-021-ntn-access-service-layer-contract.md`
- `ai_harness/tasks/CUCP-023-ntn-analog-release-digital-service-ownership.md`

## Accepted local contracts

- CU-CP only.
- Single-satellite baseline.
- Configuration root: `mobility_config.ntn_location_mobility`.
- Analog access beam owns UE resources only during access/reestablishment/pre-service relocation.
- Digital service beam owns loaded calendar, SR/SRS intent, QoS, DRB, and service resource state.
- C-RNTI is observed and validated by CU-CP after DU/access-side assignment; CU-CP does not allocate real C-RNTIs.

## In scope

- Add a CU-CP NTN beam service resource manager under CU-CP NTN mobility code.
- Record and release per-UE analog access RNTI ownership.
- Detect duplicate `(DU, PCI, C-RNTI)` ownership conflicts without replacing the existing owner.
- Centralize digital service SR/SRS slot intent generation and clear decisions.
- Expose read-only resource-manager state through CU-CP NTN command/status surfaces.
- Keep existing F1AP-CU NTN UL slot request contract as the DU-facing control-plane intent.

## Out of scope

- Real CU-CP allocation of C-RNTI.
- DU-side C-RNTI allocation changes.
- DU-side SR/SRS scheduling implementation.
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

- `ai_harness/`
- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

## Required behavior

1. RRC setup/reestablishment registers DU-reported C-RNTI ownership against the current analog access context.
2. Initial Context Setup success releases per-UE analog access ownership while retaining C-RNTI observability.
3. Duplicate `(DU, PCI, C-RNTI)` attempts are reported as conflicts and do not replace the existing owner.
4. Digital SR/SRS slot resource intent is produced only for binding-pending or service-bound digital service contexts.
5. Control-only or failed service binding clears stale slot resource intent.
6. UE release/reestablishment cleanup removes stale access ownership and digital slot intent.

## Forbidden areas

- Do not modify DU, MAC, PHY, PRACH, HARQ, TA scheduler, RU/RF, ZMQ, GIS, O-DU, or flexible_o_du code.
- Do not modify generated ASN.1.
- Do not add broad `apps/**` exceptions.

## Required validation

- Focused manager unit tests.
- CU-CP NTN mobility tests that cover access-only, control-only, and service-bound slot intent behavior.
- O-CU-CP command rendering tests if command output is changed.
- Path guard, rejected/quarantined overlap check, and task metadata validation.

## Required tests

1. RRC setup records C-RNTI ownership, occupies analog access ownership, and creates no digital slot intent.
2. ICS success releases analog ownership while retaining C-RNTI observability.
3. Duplicate `(DU, PCI, C-RNTI)` is marked as conflict.
4. First successful PDU service binding creates digital SR/SRS slot intent.
5. Failed PDU service binding or service release clears stale digital slot intent.
6. UE release removes access ownership and digital slot intent.
7. `ntn_state` / `ntn_ues` show resource-manager ownership and slot-intent counters.

## Validation

```bash
ctest --test-dir build/ai-clean -R "ntn_resource_manager|ntn_beam_service_resource_manager|cu_cp_ntn_mobility|cu_cp_pdu_session_resource_setup|cu_cp_unit_config" --output-on-failure
BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh
python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-024-ntn-beam-service-resource-manager.md
python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base
python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-024-ntn-beam-service-resource-manager.md
```

## Done means

- The change is inside CU-CP scope.
- Path guard passes with this task file.
- Rejected/quarantined overlap check passes.
- Required tests are added or updated.
- Required validation passes, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation, risks, and requested exceptions.
