# CUCP-033 NTN UE Capability Gating

## Goal

Gate UE-specific NTN service, mobility and release/paging assistance on parsed Rel-17 UE NTN capability while still
allowing signaling-only access before capability is known.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/tasks/CUCP-032-ntn-sib19-du-si-broadcast-application.md`

## Accepted local contracts

- Capability unknown may keep a UE in signaling-only CU-CP context.
- UE-specific NTN digital service, connected handover and release paging recommendation require NTN capability support.
- v1 hard gate is `nonTerrestrialNetwork-r17` present in the NR UE capability container.
- `ntn-ScenarioSupport-r17` and `ntn-Parameters-r17` are observability metadata only.

## In scope

- CU-CP NTN UE capability summary, parser/helper, runtime state and O-CU-CP observability.
- CU-CP gates for digital service binding, NTN connected handover and release recommended cells.
- CU-CP unit and focused integration tests.

## Out of scope

- RRC UE capability transfer redesign.
- Generated ASN.1 changes.
- DU, MAC, PHY, PRACH, HARQ, TA scheduler, RU/RF, ZMQ, GIS, O-DU or flexible_o_du behavior.
- New configuration schema.

## CU-CP only

This task is CU-CP-only. It consumes existing RRC UE capability bytes through existing CU-CP/RRC interfaces.

## Forbidden areas

- Generated ASN.1.
- DU/MAC/PHY/PRACH/HARQ/TA/RU/RF/ZMQ/GIS/O-DU/flexible_o_du.
- Terrestrial admission, PDU or handover behavior.

## Required behavior

- RRC setup and reestablishment may keep a UE in signaling-only NTN state while capability is unknown.
- First NTN digital service binding must require `nonTerrestrialNetwork-r17` support.
- NTN connected handover and NTN release paging recommendations must be suppressed for unsupported or parse-failed UEs.
- Capability parsing failures must be observable and non-fatal.
- Terrestrial UE capability, PDU and handover behavior must remain unchanged.

## Allowed edit paths

- `ai_harness/**`
- `include/srsran/cu_cp/**`
- `lib/cu_cp/**`
- `tests/unittests/cu_cp/**`

## Allowed task exception paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

## Required tests

- Helper tests for supported, unsupported, no NR container and malformed capability bytes.
- CU-CP tests for unknown signaling-only access, unsupported PDU rejection, supported PDU success, unsupported HO block and
  unsupported release recommendation suppression.
- O-CU-CP command tests for runtime and per-UE capability output.

## Validation

```bash
cmake --build build/ai-clean --target srsran_cu_cp ntn_mobility_test -j1
ctest --test-dir build/ai-clean -R "ntn_capability|cu_cp_ntn_mobility|cu_cp_initial_context_setup|cu_cp_paging|cu_cp_unit_config" --output-on-failure
python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-033-ntn-ue-capability-gating.md
python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-033-ntn-ue-capability-gating.md
python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-033-ntn-ue-capability-gating.md
```

## Done means

- UE NTN capability is parsed and visible in runtime status.
- Unsupported or parse-failed UEs cannot create NTN digital service, NTN connected HO or NTN release recommended cells.
- Unknown capability remains signaling-only compatible.
- Focused validation passes or failures are explained.
