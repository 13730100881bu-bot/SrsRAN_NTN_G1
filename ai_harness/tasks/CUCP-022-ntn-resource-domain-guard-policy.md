# CUCP-022: NTN Resource Domain Guard Policy

## Goal

Implement CU-CP-only analog access / digital service resource-domain guards for NTN. The guards model configured caps,
reuse groups, explicit conflict groups, and deterministic blocking reasons used by admission, digital service binding,
loaded service placement, and observability.

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
- `ai_harness/tasks/CUCP-021-ntn-access-service-layer-contract.md`

## Accepted local contracts

- CU-CP only.
- Configuration root: `mobility_config.ntn_location_mobility`.
- Admission elevation default: `50 deg`.
- `0` in a resource cap means unlimited.
- `max_nof_loaded_service_beams=0` means no CU-CP loaded-service beam cap.
- Candidate inventory is never capped by resource-domain policy.
- Explicit conflict groups are hard guards; adjacent beams without explicit conflicts can still be active-loaded.
- Analog access beam remains a CU-CP control-plane contract and does not imply real RF/PRACH/SSB control.
- Digital service beam remains the NCI, beam-derived TAC, loaded calendar, SR/SRS intent, QoS, and DRB service
  granularity.

## In scope

- Add an NTN resource-domain policy contract under CU-CP NTN mobility configuration.
- Support global resource defaults plus optional analog/digital beam-table overrides.
- Parse and validate resource-domain policy from NTN beam table JSON while preserving legacy compatibility.
- Gate analog access, first digital service binding, loaded service calendar eligibility, and connected HO preload at
  CU-CP policy level.
- Expose resource eligibility, cap/load summary, reuse/conflict groups, and blocked counters through existing NTN
  command snapshots and O-CU-CP commands.

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
- Real RF frequency reuse, power control, or physical analog beam conflict enforcement.
- Multi-satellite behavior.
- Bearer preemption or release of existing low-priority UE/DRB service.

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

1. Legacy beam tables and configs that omit resource-domain policy behave as before.
2. Effective resource policy merges global defaults with analog/digital overrides; `0` remains unlimited.
3. RRC setup and reestablishment check parent analog access-only resource caps when an analog access context is created.
4. First PDU-session digital binding checks parent analog service caps, target digital caps, and explicit conflict groups.
5. Loaded service calendar remains demand-driven; resource-blocked beams stay candidate/ineligible and do not generate
   SR/SRS intent.
6. Explicit conflict groups prevent two digital service beams in the same conflict domain from being active-loaded
   together.
7. Without explicit conflict groups, neighboring or sibling digital beams can be active-loaded together.
8. QoS/ARP still orders competing pending demand, but existing service-bound UE/DRB is not preempted by this task.
9. Observability reports resource-domain eligibility, reasons, caps, loads, reuse groups, conflict groups, and blocked
   counters.

## Forbidden areas

- Do not modify DU, MAC, PHY, PRACH, HARQ, TA scheduler, RU/RF, ZMQ, GIS, O-DU, or flexible_o_du code.
- Do not modify generated ASN.1.
- Do not add broad `apps/**` exceptions.

## Required validation

- Focused resource-domain tests.
- CU-CP NTN mobility and PDU session setup tests affected by service binding and loaded placement.
- O-CU-CP command rendering tests for resource-domain observability.
- Path guard and rejected/quarantined overlap checks.

## Required tests

1. Parser tests: legacy policy absence, global/analog/digital merge, negative cap, unknown child, duplicate/invalid conflict
   group and reuse id rejection.
2. Planner tests: analog loaded child cap, digital UE/DRB cap, explicit conflict hard block, no-conflict sibling active
   loading, QoS ordering without existing service preemption.
3. CU-CP tests: analog access cap rejection, PDU binding rejection on analog/digital cap or explicit conflict, release frees
   resource, connected HO preload is resource-gated.
4. Observability tests: `ntn_state` resource counters and `ntn_beams all` reuse/conflict/resource reason columns.

## Validation

```bash
ctest --test-dir build/ai-clean -R "ntn_resource|ntn_beam_placement|cu_cp_ntn_mobility|cu_cp_unit_config|cu_cp_pdu_session_resource_setup" --output-on-failure
BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh
python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-022-ntn-resource-domain-guard-policy.md
python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base
python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-022-ntn-resource-domain-guard-policy.md
```

## Done means

- The change is inside CU-CP scope.
- Path guard passes with this task file.
- Rejected/quarantined overlap check passes.
- Required tests are added or updated.
- Required validation passes, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation, risks, and requested exceptions.
