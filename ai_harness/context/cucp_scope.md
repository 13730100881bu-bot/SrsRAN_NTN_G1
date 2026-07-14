# CU-CP Scope for NTN Codex Harness

## Goal

This harness restricts Codex implementation work to CU-CP NTN control-plane functionality in srsRAN.

The human owner decides:

- NTN standard interpretation.
- Whether a task may touch CU-CP-adjacent code.
- Whether a non-CU-CP change is accepted.
- Final merge decisions.

Codex may implement:

- CU-CP NTN configuration parsing and validation.
- CU-CP NTN cell context.
- CU-CP-side RRC/SIB19 generation or packaging, if the implementation path belongs to CU-CP control-plane code.
- F1AP-CU / F1-C control-plane delivery or contract tests.
- NGAP / NG-C control-plane logging, validation, or UE context behavior related to NTN.
- CU-CP measurement and mobility policy for NTN cells.
- Narrow o_cu_cp configuration glue when required to reach CU-CP.
- CU-CP satellite updater control-plane support when explicitly approved.
- CU-CP unit tests, component tests, log parsers, and harness scripts.

Codex must not implement:

- O-DU or flexible_o_du behavior.
- DU-high scheduler behavior.
- MAC scheduler behavior.
- HARQ timing implementation.
- TA scheduler implementation.
- PRACH behavior.
- PHY or lower PHY behavior.
- RU / RF / radio driver behavior.
- ZMQ channel behavior.
- GIS site behavior.
- Broad architecture refactors.

## Important boundary

CU-CP-only NTN support is not full NTN support.

A task is successful if CU-CP control-plane behavior is correct and validated.
It does not need to prove full satellite-link attach unless lower layers already support the required behavior.

## Non-CU-CP exception rule

Codex may touch non-CU-CP files only when the task explicitly lists them under:

Allowed non-CU-CP exception paths

If a requested behavior cannot be implemented within CU-CP, Codex must stop and report the boundary instead of modifying DU/MAC/PHY/RU/RF code.
