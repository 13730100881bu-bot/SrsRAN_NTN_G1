# CUCP-034 NTN UE Capability Profile Policy

## Goal

Extend CUCP-033 capability gating with Rel-17 NTN scenario matching so a UE that only supports a mismatched NTN
scenario cannot enter UE-specific NTN service, connected handover, or release paging recommendation.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/ntn_cucp_feature_catalog.md`
- `ai_harness/tasks/CUCP-033-ntn-ue-capability-gating.md`

## Accepted local contracts

- CUCP-033 `nonTerrestrialNetwork-r17` remains the base hard gate.
- CUCP-034 v1 deployment profile is fixed to `leo_ngso`.
- `ntn-ScenarioSupport-r17=ngso` or `both` matches `leo_ngso`.
- If `ntn-ScenarioSupport-r17` is absent while `nonTerrestrialNetwork-r17` is present, treat it as `implicit_both`.
- `ntn-Parameters-r17` is observability metadata only in this task.

## In scope

- CU-CP NTN UE capability profile summary, parser/helper, runtime state and O-CU-CP observability.
- CU-CP gates for digital service binding, NTN connected handover and release recommended cells.
- CU-CP unit and focused integration tests.

## Out of scope

- RRC UE capability transfer redesign.
- Generated ASN.1 changes.
- DU, MAC, PHY, PRACH, HARQ, TA scheduler, RU/RF, ZMQ, GIS, O-DU or flexible_o_du behavior.
- New configuration schema.
- Rel-18 or Rel-19 NTN/NES feature gating.

## CU-CP only

This task is CU-CP-only. It consumes existing RRC UE capability bytes through existing CU-CP/RRC interfaces.

## Forbidden areas

- Generated ASN.1.
- DU/MAC/PHY/PRACH/HARQ/TA/RU/RF/ZMQ/GIS/O-DU/flexible_o_du.
- Terrestrial admission, PDU or handover behavior.

## Required behavior

- RRC setup and reestablishment may keep a UE in signaling-only NTN state while capability/profile is unknown.
- First NTN digital service binding must require `nonTerrestrialNetwork-r17` and a matching `leo_ngso` profile.
- NTN connected handover and NTN release paging recommendations must be suppressed for profile-mismatched UEs.
- GSO-only UEs must be observable as supported but profile-blocked in the current `leo_ngso` deployment.
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

- Helper tests for NGSO, GSO, both and implicit-both scenario matching.
- Helper tests proving missing NTN flag, no NR container and malformed bytes keep CUCP-033 behavior.
- CU-CP tests for GSO-only PDU rejection, connected handover suppression and release recommendation suppression.
- O-CU-CP command tests for runtime and per-UE profile output.

## Validation

```bash
cmake --build build/ai-clean --target srsran_cu_cp ntn_mobility_test cu_cp_unit_config_test -j1
ctest --test-dir build/ai-clean -R "ntn_ue_capability_gate|cu_cp_ntn_mobility.*capability|cu_cp_unit_config.ntn_(state|ues)" --output-on-failure
python3 ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-034-ntn-ue-capability-profile-policy.md
python3 ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-034-ntn-ue-capability-profile-policy.md
python3 ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base
```

## Done means

- UE NTN scenario profile is parsed, matched to `leo_ngso`, and visible in runtime status.
- GSO-only UEs cannot create NTN digital service, NTN connected HO or NTN release recommended cells.
- `implicit_both` UEs remain service-compatible.
- Focused validation passes or failures are explained.
