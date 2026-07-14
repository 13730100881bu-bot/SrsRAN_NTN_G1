# NTN CU-CP Agent Memory

Last updated: 2026-07-12

This file is the compact handoff memory for future conversations. It keeps the
durable NTN CU-CP design context without pulling in the old project-management
scaffolding.

## How To Resume

Read only:

1. `AGENTS.md`
2. `docs/ntn_cucp_agent_memory.md`
3. The user's current request

Then work on the feature directly. Open older notes only when a specific code or
design question requires them.

## Working Rules

- Focus on the requested NTN feature, not process infrastructure.
- Keep edits scoped to the smallest production/test surface that actually
  implements the task.
- Do not revert unrelated dirty worktree changes.
- Keep Git history usable: inspect `git status --short` before editing, stage
  only task-owned files, avoid `git add .`, and commit after each coherent CUCP
  stage when validation evidence is good enough.
- Prefer focused build targets and focused tests over broad rebuilds.
- Do not modify O-DU, flexible_o_du, PHY, lower PHY, RU, RF, ZMQ, GIS, PRACH,
  HARQ timing, or TA scheduler unless the user explicitly asks for that layer.
- DU/F1AP/MAC/Scheduler changes are allowed only when the requested feature
  truly requires them and the change is kept exact and minimal.

## Validation Style

Use a staged validation ladder:

- Compile the closest target first, for example:
  - `cmake --build build/ai-clean --target srsran_cu_cp -j1`
  - `cmake --build build/ai-clean --target ntn_mobility_test -j1`
  - `cmake --build build/ai-clean --target cu_cp_unit_config_test -j1`
- Run focused `ctest -R` filters for the changed behavior.
- Run broader builds/tests only for broad interface changes or when focused
  evidence is insufficient.
- If Windows `ctest --test-dir build/ai-clean` fails because generated CTest
  files contain `/mnt/d/...`, rerun through WSL:
  `bash -lc "cd /mnt/d/code/srsRAN_Project-main && ctest --test-dir build/ai-clean -R '<regex>' --output-on-failure"`
- If a broad validation step is skipped for speed, say so explicitly and state
  the residual risk.

## Core NTN Design Decisions

### Deployment Profile

- The product target is an onboard regenerative gNB: CU-CP is deployed on each
  satellite. The current working implementation remains a single-satellite
  LEO/NGSO control-plane prototype; do not describe the multi-satellite target
  as runtime-complete.
- The current runtime example remains 500 km, 50 degree minimum elevation, and
  15 km digital service radius. Do not confuse it with the global planning seed.
- Current deployment profile name: `leo_ngso`.
- The current prototype uses 843 digital service beams, 137 analog access
  beams, 16 active analog beams, and 256 loaded digital beams.
- The next-stage target covers global land from 57 degrees south to 57 degrees
  north. It starts the search from `Walker Delta 60°:3528/42/0` at 500 km, but
  this is a seed, not a selected or accepted constellation. `selectedScenario`
  remains empty until a real seven-day event-driven exact audit is reviewed.
- The old `53°:720/30/1`, mainland-China/Hainan 2,620-L1 catalog and 120-second
  sampling reports are historical comparison data only.

### Global Onboard Cell And Position Baseline

- The service mask is global land in latitude `[-57, +57]` degrees. Ocean and
  higher latitudes are outside this stage's continuous-service acceptance.
- The Web global catalog is implemented: 36,411 `G######` L1 positions and
  249,375 valid `<L1>-n` L2 positions, including 33,871 full and 2,540 edge L1.
  Ground positions keep IDs, geometry, adjacency and child masks only; they do
  not own permanent NCI or PCI values.
- The generator uses local Natural Earth 4.1.0 / `world-atlas@2.0.2` 1:50m land,
  244 equal-sin(latitude) bands and spherical equal-area quasi-hex centers.
  Target area is 3,117.691454 km2 and maximum area deviation is 0.1115%.
  Keep `exactRegularSphericalHexagons=false` and `exactCoastlineClipping=false`:
  this is center-point land inclusion, not exact polygon coastline clipping.
- Generator: `web_replicas/ntn_beam_planner/scripts/generate-global-land-catalog.mjs`.
  Asset: `public/data/global-land-l1-v1.json`; packaged source:
  `public/data/land-50m.json`. Cells SHA-256 is
  `b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a`.
  Generate/`--check` and focused catalog tests passed 6/6. This is Web
  implementation/test evidence, not CU-CP runtime or an operational GIS freeze.
- Each satellite has two long-lived onboard NR cells. NCI and PCI follow those
  cells rather than ground positions. An L1 can therefore be served by
  different NCI/PCI values over time.
- NCI is an opaque 36-bit planning ID, unique within the PLMN; NCGI is globally
  unique when PLMN identity and NCI are combined. At 3,528 satellites the seed
  needs 7,056 NCI values. The Web seed registry now allocates and tests all
  7,056 unique values, but its final size still follows the eventually selected
  constellation. Do not derive NCI semantics from satellite or beam IDs.
- PCI has only 1,008 values and is not globally unique. Build a conflict graph
  whose nodes are long-lived onboard cells and whose edges mean co-channel and
  simultaneously visible. Reuse PCI only across non-conflicting nodes; never
  change PCI on every beam-hopping visit.
- The current Web PCI evidence is deliberately a local Walker-lattice proxy:
  7,056 nodes, 59,976 edges, 8 colors and zero coloring conflicts in its own
  proxy graph. It is test evidence for the deterministic coloring mechanism,
  not a seven-day simultaneous-visibility, co-channel or RF conflict result.
- Do not add an NTN-specific `onboard_cell_index` or `SatelliteCellBinding`.
  Schedule entries directly contain `satellite_id`, `nci`, `pci`, time, port,
  and L1/L2 position.
- An offline planner produces ephemeris-based per-L1 candidate inventories and
  assignment proposals. A management center reviews and distributes those
  proposals with the NCI/PCI registry, versions, activation epoch, and checksums.
  The onboard gNB splits its authorized L1 set
  between its two long-lived cells and builds two calendars. Compactness and
  connectivity are best-effort optimization goals, not validated hard guarantees.
- A new candidate must place the individual L1 at or above 45 degrees; an
  existing owner may remain down to 42 degrees. The complete visible inventory
  is never capacity-, active-, or loaded-trimmed. Each L1 has at most one primary
  owner at an epoch.
- Each onboard cell has a fixed, non-borrowable pool of 16 analog and 64 digital
  resources, for whole-satellite limits of 32/128.
- Planning timing is 20 ms cell SSB cadence, 80 ms maximum per-L1 SSB revisit,
  and 640 ms maximum per-L1 PRACH revisit. Every active L1 transmits periodic
  SSB even with no UE; every advertised PRACH RO needs a receive beam.
- The calendar uses 10 ms access slots, three analog DL/UL phases
  `11/5, 11/5, 10/6`, and four sequential 2.5 ms visits per DL port per slot.
  The worst 80 ms alignment has 42 DL port-occasions or 168 visit opportunities.
  The configured payload limit is 128 L1 per cell and 256 per satellite, so
  128/256 is a hard guarantee of this discrete calendar model, not an average
  or best-phase ceiling. It is not a protocol/hardware guarantee; PHY/RU/RF,
  guard, power, bandwidth, common signaling and PRACH still need review.
- A position transfer does not migrate NCI. Only when source and target use
  different onboard NCI/PCI values may the target overlap for
  discovery/measurement. An offline assignment is a proposal, not an actual
  serving transition; target ready, DU applied feedback, and a 640 ms-aligned
  activation epoch are required. Connected UE use HO/CHO; idle UE use
  reselection.
- All values in this subsection are planning inputs, not current CU-CP runtime
  behavior. The Web catalog exists, but the orbit view displays a selection seed
  only. Do not use "passed", "recommended"
  or "selected" until a real seven-day event-driven exact report is available.
- The exact audit must capture 45/42-degree crossings, local capacity failures,
  ownership changes with hysteresis/freeze/switching cost, PCI conflict intervals,
  gateway changes, SSB/PRACH deadlines and N-1 scenarios.
- `scripts/audit-global-constellation.mjs` has focused CLI tests 2/2. Saved
  coarse reports are:
  - F=0 `app/global-constellation-snapshot.json`: at t=0, 23 L1 have no
    45-degree candidate, peak visible load is 206/256, and zero satellites
    exceed 256. This seed fails and is ineligible for selection.
  - F=1 `app/global-constellation-f1-snapshot.json`: at t=0, zero uncovered,
    minimum candidate count 1, peak 207/256, and zero overflow. This is only a
    single-epoch coarse observation.
  - F=1 `app/global-constellation-f1-day-coarse.json`: one day sampled every
    120 seconds has 720/720 discrete epochs with zero uncovered, minimum
    candidate count 1, peak 209/256, and zero overflow epochs. It remains
    `auditLevel=coarse`, `exact=false`; gaps may exist between samples.
- `app/global-constellation-audit.json` still records `selectedScenario=null`
  and exact audit `status=not_run`. Coarse reports cannot set selection.
- Historical China results such as 720/720 independent snapshots, 1,261,561
  previous-owner differences and one-hop PCI proxy conflicts are not global
  evidence and are not serving handovers or continuous-time RF proof.

### Beam Hierarchy

- In the current runtime prototype, digital service beams are the implemented
  service/indexing grain:
  - beam-derived TAC
  - loaded service calendar
  - SR/SRS resource assignment
  - QoS and DRB load
  - digital service ownership
- The prototype still contains beam-to-NCI indexing. Treat that as a refactor
  impact to audit, not as the target identity rule.
- CUCP-035 adds an independent, default-disabled onboard position-plan profile.
  It receives the complete management-center `G######` inventory, validates
  versions/hash/validity against two explicitly configured stable NCI/PCI cell
  identities, partitions L1 between them, audits configurable access-calendar
  limits (defaults 128/256 L1, 16/32 analog ports, 80/640 ms), and atomically
  activates at the aligned epoch. Same-PCI reuse is permitted; NCI must remain
  distinct. Long runtime deadlines are timer-sliced, and status/reload/activation
  share one synchronization boundary. It deliberately does not inject L1 into
  the legacy per-beam NCI table or claim DU/RF application.
- In the current runtime prototype, analog access beams are access groups over
  digital beams:
  - RRC setup/reestablishment gate
  - pre-service relocation
  - coarse paging/release hint eligibility
  - parent eligibility for child digital service beams
- The LEO hex model uses:
  - `843` digital service beams
  - `137` analog access beams
  - `109` full 7-cell clusters
  - `28` edge partial clusters
- Analog-to-digital grouping is one parent analog cluster to up to seven digital
  child beams. Edge partial clusters are valid because the footprint is circular.
- Do not carry the prototype beam-to-NCI identity into the next-stage target.
  L1 is an earth-fixed periodic signaling position, and L2 is its earth-fixed
  on-demand service child. The current onboard cell mapping supplies NCI/PCI at
  runtime; neither position permanently owns those identities.

### Access And Service Ownership

- Analog access and digital service ownership are separate.
- RRC setup/reestablishment creates analog access context only.
- After Initial Context Setup succeeds, per-UE analog ownership is released.
- A UE with no PDU/DRB after ICS is `control_only` and occupies no digital beam.
- First PDU/DRB demand binds a digital service beam:
  - fresh UE location inside parent analog child beams is preferred
  - access cell/NCI fallback is allowed when location is missing
- Digital service context drives loaded calendar, SR/SRS assignment, QoS,
  resource-domain guards, and resource consistency handling.

### CU-CP Authority Over NTN Resources

- CU-CP is authoritative for NTN RNTI lease pools and digital SR/SRS assignment.
- DU/MAC/Scheduler execute and report results.
- Terrestrial/default DU allocation paths must remain unchanged.
- Initial access cannot wait until CU-CP sees Initial UL to create a RNTI.
  Correct model:
  1. CU-CP computes active analog access beams.
  2. CU-CP pre-distributes per DU/cell/analog RNTI lease pools.
  3. DU consumes a lease in RAR.
  4. CU-CP validates the Initial UL RNTI against the applied pool.
  5. ICS success commits the lease as the UE C-RNTI.
- CU-CP sends digital SR/SRS assignments and treats service as ready only after
  DU-applied feedback.
- Resource consistency logic compares DU state against CU-CP authority and
  resends, clears, rolls back, or blocks conservatively.

### UE Capability Policy

- UE-specific NTN digital service, connected handover, and release/paging hints
  require parsed Rel-17 `nonTerrestrialNetwork-r17`.
- Signaling-only access may proceed while UE NTN capability is unknown.
- For current `leo_ngso` profile:
  - NGSO matches
  - both matches
  - absent `ntn-ScenarioSupport-r17` with NTN support is `implicit_both` and
    matches
  - GSO-only is base NTN-supported but profile-blocked for LEO/NGSO
  - `ntn-Parameters-r17` is observability-only in v1
- SIB19 broadcast is beam/cell network state, not UE-specific capability state.

## Task Chain Summary

Treat CUCP-004 as the practical start of the current NTN task chain. Earlier
items were bootstrap/history.

For a more detailed task-to-change lookup, use
`docs/ntn_cucp_task_change_index.md`.

- CUCP-004: project baseline for the NTN CU-CP work.
- CUCP-005: runtime taxonomy for candidate, active, loaded, draining,
  assistance, and UE runtime state.
- CUCP-006: full candidate inventory, not capped by served/loaded limits.
- CUCP-007: demand-driven loaded service calendar and SR/SRS intent.
- CUCP-008: CU-CP NTN assistance snapshot.
- CUCP-009: admission/mobility gates using NTN runtime state.
- CUCP-010: NGAP/core mapping, mapped cell, derived TAC, TAI.
- CUCP-011: switch-over/resilience.
- CUCP-012: observability/examples.
- CUCP-013: RRC/SIB19 assistance packaging contract.
- CUCP-014: QoS-aware service policy.
- CUCP-015: beam-derived TAC for paging/idle assistance.
- CUCP-016: service-area and paging observability.
- CUCP-017: analog/digital hex beam hierarchy.
- CUCP-018: analog/digital access DU assignment policy.
- CUCP-019: pre-service inter-DU relocation.
- CUCP-020: connected beam-to-beam mobility policy.
- CUCP-021: explicit access/service layer contract.
- CUCP-022: analog/digital resource-domain guards.
- CUCP-023: analog release after ICS and digital service ownership.
- CUCP-024: beam service resource manager as ownership/intent contract.
- CUCP-025: CU-CP-authoritative real RNTI and SR/SRS architecture.
- CUCP-026: RNTI lease pool F1AP distribution.
- CUCP-027: RNTI lease lifecycle and access readiness.
- CUCP-028: SR/SRS application feedback and scheduler consistency.
- CUCP-029: handover target resource reservation before RRC HO command.
- CUCP-030: resource consistency auditor.
- CUCP-031: repair executor and guarded recovery.
- CUCP-032: SIB19 DU SI broadcast application through private F1AP container.
- CUCP-033: UE NTN capability gate.
- CUCP-034: UE capability profile policy for `leo_ngso`.
- CUCP-035: versioned onboard L1 plan, two-cell partition, access-calendar
  dry-run, atomic activation, and `ntn_state` projection.
- CUCP-036: default-off cross-layer access-calendar deployment over private F1AP
  resource coordination. Two stable cells must resolve uniquely to one DU;
  DU/MAC report `ready` only after both cell slot threads consume and arm the
  same version/hash; configurable prepare horizon/guard and post-activation
  apply timeout drive rollback through an independent clear I/O lane. Scheduler
  validity uses an extended slot clock and zero visible L1 is explicit deny-all.
  Scheduler application currently means only
  filtering existing static SSB/PRACH opportunities. It does not consume
  `position_id`/`port_id`, steer an analog beam, enforce per-window
  SIB/Paging/RAR, or prove PHY/RU/RF application. Abort/clear restores the
  previous scheduler snapshot; static lookahead stays prefilled and current
  result final-suppression prevents stale PDU leakage. Far-future activation is
  deferred until the configurable plain-SFN mapping horizon, while long
  validity is tracked with `slot_point_extended`. DU execution profiles are
  rejected at startup if they exceed the private F1 256-position/2560-intent
  envelope or the scheduler 16384-slot cycle at `mu=4`; the non-execution
  inventory path can still retain 257 positions and report `schedule_overflow`.
  MAC wall-clock mapping uses nanosecond precision so the `mu=4` 62.5 us slot
  is not truncated.
  Real RF execution additionally requires a management-provided opaque
  hardware beam handle, a time-tagged vendor/O-RU bank-switch adapter and device
  telemetry. See `docs/ntn_access_calendar_cross_layer_execution.md`.
  The Web planner baseline identity source is the versioned management-center
  `app/onboard-cell-identity-registry.json`: 3528 satellites and 7056 explicit
  opaque 36-bit NCIs. Runtime Walker/ordinal NCI derivation is forbidden;
  non-baseline sweeps require an explicit matching registry.
- CUCP-037 hardens this boundary without claiming a transport that does not
  exist. RNTI lease ownership keys include `(DU, cell index, PCI, C-RNTI)`,
  because the two stable onboard cells may reuse one PCI and still have
  independent C-RNTI namespaces. Calendar deployment feedback is monotonic,
  and prepare/query responses must preserve the complete accepted intent count
  before activation.
- CUCP-037 also adds a CU-CP-private pure Initial UL event auditor. Given
  complete proposed sideband metadata, it matches satellite/catalog/schedule,
  source/calendar hashes, stable NCI/PCI, `G######` ownership, PRACH occasion
  phase and paired UL port against the current active plan. It returns
  `accept`, `reject` or `audit_only` with a machine-readable reason. Standard
  F1AP Initial UL does not carry that metadata today, so production access must
  not infer `position_id` from the legacy beam-to-NCI table. An `accept` result
  proves only a CU-CP active-plan/software-gate snapshot match, not trusted
  sideband provenance, DU-reconnect reconciliation, position steering or RF
  application.

## Protocol References

- 3GPP TS 38.300 family for NR architecture and NTN context.
- 3GPP TS 38.331 / ETSI TS 138 331 for RRC NTN assistance and UE capability
  ASN.1 fields.
- 3GPP TS 38.306 / ETSI TS 138 306 for UE capability semantics, including
  `nonTerrestrialNetwork-r17`, `ntn-ScenarioSupport-r17`, GSO/NGSO support, and
  implicit support behavior.
- 3GPP NTN overview:
  `https://www.3gpp.org/technologies/ntn-overview`
- ETSI TS 138 306 v17.0.0:
  `https://www.etsi.org/deliver/etsi_ts/138300_138399/138306/17.00.00_60/ts_138306v170000p.pdf`
- ETSI TS 138 331 v17.1.0:
  `https://www.etsi.org/deliver/etsi_ts/138300_138399/138331/17.01.00_60/ts_138331v170100p.pdf`

## Useful Code Areas

CU-CP public status and contracts:

- `include/srsran/cu_cp/cu_cp_command_handler.h`
- `include/srsran/cu_cp/ntn_location.h`
- `include/srsran/cu_cp/ntn_ue_capability.h`
- `include/srsran/cu_cp/ntn_beam_service_resources.h`

CU-CP implementation:

- `lib/cu_cp/cu_cp_impl.cpp`
- `lib/cu_cp/cu_cp_impl.h`
- `lib/cu_cp/ntn_mobility/`
- `lib/cu_cp/cell_meas_manager/`
- `lib/cu_cp/paging/`
- `lib/cu_cp/routines/mobility/`
- `lib/cu_cp/routines/ue_context_release_routine.*`

O-CU-CP observability:

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

Focused CU-CP tests:

- `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
- `tests/unittests/cu_cp/cu_cp_paging_test.cpp`
- `tests/unittests/cu_cp/ntn_mobility/`
- `tests/unittests/cu_cp/cu_cp_test_environment.*`

Task-scoped non-CU-CP areas used by previous resource-application tasks:

- private F1AP resource coordination containers
- DU resource snapshot/apply feedback
- MAC RNTI manager lease mode
- DU PUCCH/SRS resource manager tests
- Scheduler SR/SRS focused tests

These are not globally safe to edit. Touch them only when the requested feature
needs that layer.

## Common Pitfalls

- Do not restore the cancelled ground-cell partition. Ground L1/L2 positions
  have no permanent NCI/PCI. Do not introduce redundant `onboard_cell_index` or
  `SatelliteCellBinding` planning objects.
- Do not migrate NCI with an L1. NCI/PCI follow one of the two long-lived
  onboard cells; a position transfer changes the serving cell identity.
- Do not require PCI global uniqueness. Reuse it with a conflict graph whose
  edges represent co-channel, simultaneously visible long-lived onboard cells.
- Do not mix the current prototype `843/137/16/256`, historical China Web
  `53°:720/30/1`, and the global target `32/128`; all are different layers.
- Do not hardcode 128/256 as a protocol or hardware limit. It is the configured
  hard guarantee within the Web discrete calendar: the worst 80 ms phase has
  168 opportunities using four 2.5 ms sub-visits, and the model caps at 128/cell.
- Do not treat 128 digital resources as proof that 128 beams can transmit at
  full power simultaneously; power, bandwidth, and interference still need
  payload engineering.
- Do not skip idle L1 SSB visits. The planning target requires every L1 to meet
  the 80 ms SSB revisit deadline and the 640 ms PRACH revisit deadline.
- Do not describe position transfer as whole-cell NCI migration. Source and
  target have different NCI/PCI. A prepared target may broadcast for mobility,
  while primary ownership still remains singular at each epoch.
- Do not call an offline assignment proposal a serving transition before target
  ready, DU applied feedback, and the aligned activation epoch.
- Do not reuse the historical 120-second China audit as a global ownership
  period, handover count or coverage proof.
- Do not select `60°:3528/42/0`; its saved t=0 coarse snapshot has 23 uncovered
  L1. Do not call the F=1 one-day 720/720 fixed-step result continuous coverage;
  it is coarse/exact=false. Selection still requires an exact event audit.
- Do not assume adding satellites monotonically improves a Walker arrangement;
  scan plane count, slots, inclination, F, RAAN and phase on one frozen mask.
- Do not claim historical one-hop PCI proxy results prove the global conflict
  graph, continuous-time behavior or RF interference safety.
- Do not confuse candidate inventory with loaded service beams. Candidate
  inventory is never capped by loaded-beam limits.
- Do not treat analog access ownership as long-lived per UE. It is released
  after ICS success.
- Do not bind digital service beam at RRC setup if no PDU/DRB demand exists.
- Do not broadcast or recommend draining beams.
- Do not claim DU has applied a CU-CP decision unless an applied feedback path
  exists for that decision.
- Do not introduce broad unrelated layer changes just because an NTN feature
  touches CU/DU coordination.
- Do not rerun full build/test loops after every small edit. Use focused targets
  and explain skipped broad validation honestly.
