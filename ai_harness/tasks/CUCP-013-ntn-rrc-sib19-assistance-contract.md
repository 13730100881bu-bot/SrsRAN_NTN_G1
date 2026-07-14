# CUCP-013: NTN RRC/SIB19 assistance contract

## Goal

Convert CU-CP NTN assistance snapshots into bounded RRC/SIB19 packaging
contracts that can be ASN.1 packed and decoded as SIB19-r17 inputs.

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
- RRC/SIB19 output is packaging input only; it does not imply DU SI scheduling
  or SIB19 broadcast.

## In scope

- Add CU-CP SIB19 assistance contract types.
- Build a per-beam SIB19 assistance contract from `ntn_assistance_snapshot`.
- Convert contract entries into `asn1::rrc_nr::sib19_r17_s`.
- Expose the current contract through the CU-CP NTN command handler.
- Add focused CU-CP tests for valid, invalid, bounded, and ASN.1 conversion behavior.

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
- Generated ASN.1 code changes.
- Real SIB19 scheduling or broadcast.
- Re-enabling quarantined NTN configuration-manager or beam-hopping-controller paths.

## Allowed edit paths

- `ai_harness/`
- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

## Allowed task exception paths

- None

## Required behavior

1. Valid NTN assistance snapshots produce bounded per-beam SIB19 contract entries.
2. Invalid, stale, disabled, or empty satellite assistance produces an invalid
   SIB19 contract and no broadcastable entries.
3. Entry bounds limit only the SIB19 contract output and never alter
   `candidate_inventory`.
4. Epoch wall-clock metadata is preserved, but ASN.1 `epochTime-r17` is set only
   when an SFN/subframe epoch is explicitly provided.
5. The command handler exposes CU-CP contract state without promising DU
   broadcast execution.

## Required tests

1. Builder test for active, candidate, and draining contract entries.
2. Builder negative tests for invalid assistance and output bounding.
3. ASN.1 converter tests for reference location, ECEF position/velocity, TA,
   Koffset, Kmac, UL sync duration, t-Service, and epoch-time absence/presence.
4. Negative converter tests for unsupported UL sync duration and out-of-range
   Koffset/Kmac/ephemeris values.
5. CU-CP integration test proving the command handler returns the current SIB19
   assistance contract after a satellite update and returns invalid after stale
   assistance.

## Validation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-013 -TaskFile ai_harness/tasks/CUCP-013-ntn-rrc-sib19-assistance-contract.md -CTestRegex "ntn_sib19|ntn_assistance|cu_cp_ntn_mobility"
```

If bash is available:

```bash
TASK_ID=CUCP-013 TASK_FILE=ai_harness/tasks/CUCP-013-ntn-rrc-sib19-assistance-contract.md CTEST_REGEX="ntn_sib19|ntn_assistance|cu_cp_ntn_mobility" bash ai_harness/scripts/run_task_validation.sh
```

## Done means

- The change is inside CU-CP scope.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Required tests are added or updated.
- Required validation passes, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests, validation,
  risks, and requested exceptions.
