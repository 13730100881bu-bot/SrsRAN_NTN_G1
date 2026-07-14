# CUCP-014: NTN QoS-aware service policy

## Goal

Implement CU-CP only NTN QoS-aware service policy so loaded service calendar decisions, beam placement, SR/SRS slot
intent ordering, and new PDU-session demand admission can account for QoS, ARP, GBR, and slice metadata.

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
- `max_nof_loaded_service_beams=0` means no CU-CP cap.
- Do not cap `candidate_inventory` with loaded-beam resource limits.
- ARP priority level `1` is highest priority; `15` is no priority.

## In scope

- CU-CP QoS demand summary types and helpers.
- CU-CP NTN beam load and assignment QoS summaries.
- QoS-aware beam placement ordering and CU-CP SR/SRS slot intent ordering.
- New PDU-session demand rejection when a lower-priority NTN demand would displace higher-priority loaded service.
- CU-CP observability fields for beam and UE QoS summaries.
- CU-CP unit tests and harness metadata updates.

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
- Real bearer preemption or release of existing lower-priority UE/DRB state.
- RRC/SIB19, NGAP core mapping, and E1AP wire-contract changes.

## Allowed edit paths

- `ai_harness/`
- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- None

## Required behavior

1. CU-CP derives an NTN QoS demand summary from PDU session setup requests and committed UE UP context.
2. Beam placement uses QoS priority before UE/DRB count when active-loaded capacity is limited.
3. SR/SRS slot intent keeps the existing UE/DRB-based slot count, but orders service beams by QoS priority first.
4. A new lower-priority PDU-session demand is rejected when accepting it would displace an already loaded higher-priority
   NTN beam.
5. Candidate inventory remains complete and is not cropped by QoS policy.
6. Existing stale assistance, draining, and service switch-over gates keep their current behavior.

## Required tests

1. Focused QoS policy tests for setup-request and committed-UP-context aggregation.
2. Planner tests for capacity-limited QoS ordering and slot-order behavior.
3. CU-CP integration tests for PDU setup demand, observability, and low-priority rejection.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-014 -TaskFile ai_harness/tasks/CUCP-014-ntn-qos-aware-service-policy.md -CTestRegex "ntn_qos|ntn_beam_placement|cu_cp_ntn_mobility|cu_cp_pdu_session_resource_setup"
```

If bash is available:

```bash
TASK_ID=CUCP-014 TASK_FILE=ai_harness/tasks/CUCP-014-ntn-qos-aware-service-policy.md CTEST_REGEX="ntn_qos|ntn_beam_placement|cu_cp_ntn_mobility|cu_cp_pdu_session_resource_setup" bash ai_harness/scripts/run_task_validation.sh
```

## Done means

- The change is inside CU-CP scope.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests are added or updated.
- Required validation passes, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
