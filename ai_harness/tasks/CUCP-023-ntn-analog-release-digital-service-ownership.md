# CUCP-023: NTN Analog Release and Digital Service Ownership

## Goal

Implement CU-CP-only UE ownership separation for NTN analog access and digital service beams. Analog access beams own a
UE only during access, reestablishment, paging coarse narrowing, and pre-service relocation. Once Initial Context Setup
succeeds, the UE releases analog access ownership. A UE with no PDU/DRB demand remains `control_only` and does not occupy
a digital service beam. First service demand binds a digital service beam and from that point digital context owns loaded
calendar, SR/SRS intent, QoS, resource-domain gating, and CU-CP antenna intent.

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
- `ai_harness/tasks/CUCP-022-ntn-resource-domain-guard-policy.md`

## Accepted local contracts

- CU-CP only.
- Single-satellite baseline.
- Configuration root: `mobility_config.ntn_location_mobility`.
- Analog access beam is a CU-CP access control contract, not RF/PRACH/SSB behavior.
- Digital service beam is the only business-resource grain for NCI, TAC, loaded calendar, SR/SRS intent, QoS, DRB, and
  digital resource-domain ownership.
- Initial Context Setup success releases per-UE analog ownership when no service demand is being bound.
- `control_only` means ICS succeeded, analog ownership is released, and no digital service beam is bound.
- CU-CP antenna intent is read-only contract/observability. It does not imply DU, MAC, PHY, RF, or antenna execution.

## In scope

- Add explicit analog access and digital service lifecycle states in CU-CP UE runtime status.
- Release analog access ownership after successful Initial Context Setup.
- Keep control-only UEs from occupying digital service beams, loaded service calendar, SR/SRS intent, or digital resource
  caps.
- Bind digital service on first PDU/DRB demand using the existing location-first and access-cell fallback logic.
- Keep analog access-only cap accounting limited to UEs still in active analog access.
- Clear digital service ownership on service release while keeping the UE control-only.
- Expose CU-CP analog access intent and digital service intent snapshots for commands and tests.
- Extend O-CU-CP `ntn_state` and `ntn_ues` output for analog release and digital ownership.

## Out of scope

- Connected beam-to-beam handover.
- Multi-satellite behavior.
- Real analog RF beamforming or antenna control.
- DU, O-DU, flexible_o_du, MAC scheduler, PHY, lower PHY, PRACH, HARQ timing, TA scheduler, RU/RF, radio drivers, ZMQ,
  GIS, or generated ASN.1.
- DU-side consumption of CU-CP antenna intent.

## Allowed edit paths

- `ai_harness/`
- `include/srsran/cu_cp/`
- `lib/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

## Required behavior

1. RRC setup and reestablishment create an `access_active` analog access context.
2. Initial Context Setup success without PDU/DRB changes the UE to `released_after_ics` analog state and `control_only`
   runtime semantics.
3. Control-only UEs do not load digital service beams and do not generate SR/SRS slot request intent.
4. First PDU/DRB setup binds a digital service beam by fresh UE location first, then access-cell fallback.
5. Successful digital binding moves only the digital service state to `service_bound`; analog ownership remains released.
6. Failed digital binding leaves the UE control-only, does not reacquire analog ownership, and leaves no pending digital
   load.
7. PDU release clears digital service ownership but preserves control-only UE context.
8. Analog access-only caps count only active access UEs, not released/control-only/service-bound UEs.
9. Digital UE/DRB caps apply only to digital service binding or service-bound demand.
10. Observability distinguishes analog access intent from digital service intent.

## Forbidden areas

- Do not modify DU, MAC, PHY, PRACH, HARQ, TA scheduler, RU/RF, ZMQ, GIS, O-DU, or flexible_o_du code.
- Do not modify generated ASN.1.
- Do not add broad `apps/**` exceptions.

## Required validation

- Focused CU-CP NTN mobility lifecycle tests.
- PDU-session setup/release tests affected by digital service binding.
- O-CU-CP command rendering tests for analog release, control-only, and antenna-intent counters.
- Path guard, rejected/quarantined overlap check, and task metadata validation.

## Required tests

1. RRC setup before ICS is `access_active` and counts analog access ownership.
2. ICS success without PDU/DRB releases analog ownership, enters control-only, and generates no loaded digital service.
3. A second UE can access the same analog beam after the first UE releases analog ownership.
4. First PDU setup from control-only binds a digital beam and generates loaded calendar / SR/SRS intent.
5. PDU setup failure returns to control-only without restoring analog ownership or leaving pending digital load.
6. PDU release clears service context but keeps control-only state.
7. `ntn_state` and `ntn_ues` show access-active, control-only, analog-released, and digital service-bound state.
8. CU-CP antenna intent snapshot separates analog access intent and digital service intent.

## Validation

```bash
ctest --test-dir build/ai-clean -R "cu_cp_ntn_mobility|ntn_beam_placement|cu_cp_unit_config|cu_cp_pdu_session_resource_setup" --output-on-failure
BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh
python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-023-ntn-analog-release-digital-service-ownership.md
python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base
python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-023-ntn-analog-release-digital-service-ownership.md
```

## Done means

- The change is inside CU-CP scope.
- Analog and digital UE ownership are observably separate.
- Path guard passes with this task file.
- Rejected/quarantined overlap check passes.
- Required tests are added or updated.
- Required validation passes, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation, risks, and requested exceptions.
