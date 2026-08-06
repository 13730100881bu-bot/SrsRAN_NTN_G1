# NTN CU-CP Agent Memory

Last updated: 2026-08-06

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
  satellite. The legacy beam-table path remains a single-satellite LEO/NGSO
  control-plane prototype; do not describe the multi-satellite target as
  runtime-complete.
- The legacy runtime example uses 500 km, 50 degree minimum elevation and a
  15 km digital service radius. Do not confuse it with the global planning seed.
- Legacy deployment profile name: `leo_ngso`.
- The legacy beam-table prototype uses 843 digital service beams, 137 analog
  access beams, 16 active analog beams and 256 loaded digital beams. These are
  not the onboard position-plan profile's `2 cells × 16/64` resource limits.
- The final engineering design covers global land from 57 degrees south to
  57 degrees north with `Walker Delta 60°:2990/46/33` at 500 km: 46 planes,
  65 satellites per plane. The old 3,528-satellite model is a historical Web
  display comparison. `selectedScenario` remains empty until the seven-day
  event-driven acceptance report is complete; it no longer controls the
  engineering satellite-count decision.
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
  unique when PLMN identity and NCI are combined. The 2,990-satellite design
  needs 5,980 NCI values. The existing Web registry allocates and tests 7,056
  values only for the historical 3,528-satellite display model; a matching
  5,980-NCI registry still must be generated. Do not derive NCI semantics from
  satellite or beam IDs.
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
  behavior. The Web orbit view still displays the historical 3,528-satellite
  comparison model. The engineering design is 2,990 satellites, but do not
  describe continuous coverage, N-1, PCI interference or RF execution as
  passed until the corresponding acceptance evidence exists.
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
  and exact audit `status=not_run`. These fields track formal continuous
  acceptance; they no longer mean that the engineering satellite count is open.
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
- CUCP-039 makes execution-mode restart recovery conservative. When
  `du_execution_enabled=true`, a private `state_file` is required. CU-CP
  atomically records the highest accepted versions, active and pending plans,
  their two-cell L1 partition and hashes, activation/validity times, the last
  lower-layer software deployment state, and any calendar cleanup still owed.
  On restart, CU-CP validates that record again and queries DU before showing a
  recovered plan as `active` or `applied`; missing, partial, expired, or
  mismatched feedback fails closed. An expired deployment becomes a durable
  cleanup task identified by its exact version/hash, so cleanup cannot erase a
  different still-valid fallback plan. Read-only `ntn_state` output reports the
  state-file generation/hash/save result, version high-water marks, recovery
  stage/detail and whether writes are blocked. The feature remains default-off,
  so the terrestrial path does not load, save, query, or clear this NTN state.
  Here `applied` means only that the matching SSB/PRACH software gate was found;
  it is not antenna, beam steering, PHY, RU, or RF evidence.
- CUCP-040 closes the CU-CP side of the DU reconnect gap for onboard calendars.
  Prepare completions are bound to the DU connection generation and exact plan;
  query and clear also use an exact request-instance guard. Disconnect
  immediately hides previous application evidence, persists a recovery candidate and retries only after a
  matching two-cell NCI/PCI mapping is available on a live connection. A future
  pending plan remains behind its `activation_epoch`: `ready` is polled without
  clearing the current plan. Even an early `applied` response keeps the new plan
  hidden as pending and does not by itself clear a still-valid historical
  fallback; promotion still waits for the epoch. If that pending plan expires
  before a delayed timer can activate it, the historical fallback returns to
  live-DU reconciliation and the expired plan is cleaned separately. Cleanup queue head
  version/hash/reason and the active calendar hash are visible in `ntn_state`.
  Expired `not_sent` plans create no cleanup. An applied active plan queues its
  exact version/hash for cleanup at expiry even when a future plan is still
  pending and `not_sent`. A confirmed cleanup is removed
  from the live queue only after a durable state write; write failure leaves it
  unchanged in fail-closed memory, while a durably removed cleanup is not sent
  again after restart. If the file replacement succeeds but directory durability
  cannot be confirmed (`committed_not_durable`), CU-CP remains blocked because a
  crash may expose either the old or the new cleanup record.
  State schema v2 also stores a separate read-only summary of the latest
  successfully parsed management-center input: catalog/schedule version,
  content hash, activation epoch and the complete candidate inventory. It is
  not a byte-for-byte source-JSON copy. This preserves all 257 entries of a
  rejected overflow plan across cleanup and repeated restart without advancing the accepted-version
  high-water mark or making that input deployable. Schema v1 remains readable;
  only v1 may reconstruct this observation from its latest active/pending
  snapshot, while an explicit v2 `received_plan:null` remains empty.
- CUCP-041 closes the nested replacement corner of that recovery model. If a
  future plan has already been confirmed by DU, a newer checked plan may replace
  it while the historical plan remains hidden. When the newer deployment then
  fails, CU-CP now returns the historical plan to live-DU verification in the
  same process; it no longer depends on a restart to rediscover that fallback.
  State save/load preserves the fallback target, accepted version high-water and
  the complete most recently received candidate inventory. That inventory is a
  read-only audit of the latest parsed management input, not the accepted or
  active plan: a rejected replay can replace this observation, but cannot lower
  accepted versions or change the running plan. Sender authentication and a
  protected monotonic observation history remain separate future work.
- CUCP-042 closes exact-deadline expiry for a hidden historical fallback. Its
  own `valid_until` is now a timer deadline: at that instant the old plan
  permanently loses fallback eligibility, and one exact
  `schedule_version`/`calendar_hash` cleanup task is queued with reason
  `historical_fallback_expired` before any same-instant pending activation. The
  pending plan, accepted-version high-water, latest received input and partition
  remain intact. State schema v2 preserves the pending plan and outstanding
  cleanup together, so DU loss postpones only transmission and restart continues
  cleanup without restoring the expired plan. A later update failure cannot
  resurrect it; queue or state-write failure remains fail closed. No public OAM
  field, protocol container or lower-layer interface is added, and this remains
  software-calendar rather than RF evidence.
- CUCP-043 makes plan activation and state-file replacement one visible
  decision. A pending plan is no longer published as active from a prepare or
  query response; the activation timer performs the switch and keeps it hidden
  if the matching state transition cannot be saved. A blocked state file still
  advances every `valid_until`, preserving exact cleanup work while suppressing
  new preparation, activation, recovery confirmation and clear transmission.
  If file replacement did not commit, CU-CP restores the last saved snapshot
  and applies expiry only. If replacement committed but directory durability is
  uncertain, it keeps memory aligned with the replaced file, hides live DU
  evidence and requires restart reconciliation instead of rolling back to old
  bytes. Cleanup capacity is now a bound of 66 total identities: 64 historical
  tasks plus the persisted active and pending snapshots. Expiry converts a live
  snapshot into a queue task without increasing that total, so a full valid
  state cannot silently lose the next exact cleanup. This remains private
  CU-CP software-state handling; no protocol, DU/MAC, PHY, RU/RF, Web/GIS or
  generated ASN.1 change is involved.
- Remaining recovery protections are narrower. Because version high-water marks
  and snapshots are kept in the same file, replacing or deleting the whole file
  cannot be detected without a separate trusted monotonic anchor. F1AP DU stop
  still lacks explicit cancellation of every common transaction; CU-CP now
  ignores any late calendar completion from that old connection, but lifecycle
  cancellation and noisy teardown logs should still be hardened separately.
  `state_store_error` is also a detailed diagnostic string and can include the
  configured state path; a future OAM hardening pass should expose a stable
  machine code while leaving path details only in logs.
- In the legacy beam-table prototype, analog access beams are access groups over
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
- With NTN inactive, terrestrial/default DU allocation selection and outcomes
  must remain unchanged. Shared synchronization overhead is not a claim of
  performance equivalence.
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
- A periodic audit response is authoritative per domain, not merely because
  `accepted=true`. Before CUCP-038, the one-second DU audit could return an
  accepted empty snapshot and trigger false repair. Private codec v2 carries
  `rnti_snapshot_complete` and `ue_slot_snapshot_complete`; decoded v1 results
  fail safe with both domains incomplete.
- MAC owns a per-cell lease ledger with `pending -> consumed_by_mac -> expired`
  state and enforces `expiry_ms`, refreshing expiry before replace validation.
  A replace is all-or-nothing and retains terminal history. An exact
  same-generation add is a no-op for pending/consumed/expired records; it does
  not refresh expiry or resurrect state. `allocate_for_cell` chooses the mode
  under the shared allocator lock. A returned terrestrial TC-RNTI is recorded
  for 10 seconds so the first concurrent NTN update can reject a collision; the
  record does not filter terrestrial selection before NTN activation, so the
  original RNTI sequence remains unchanged. Synchronization cost is not a
  performance-equivalence claim.
- DU validates gNB-DU, full NCGI and PCI on pool updates, checks the snapshot
  cell, and returns the real RNTI snapshot with per-entry generation. CU-CP
  accepts a lease result only when generation and the full accepted/rejected
  set match. Missing or malformed ACK becomes `ack_unknown`; a complete audit
  retries the same generation, while an ordinary in-flight pool waits and
  blocks a newer low-water generation. CU-CP promotes matching pending
  evidence, reconciles consumed/expired state and never resurrects a consumed,
  Initial-UL-seen, committed or expired lease. Explicit audit rejection reaches
  conflict observability; a later accepted complete audit resolves the generic
  target blocker.
- UE-slot audit remains incomplete because DU does not yet have a reliable
  CU-global UE identity mapping for every local SR/SRS entry. Do not infer
  authoritative absence from its empty snapshot.
- Resource audit is MAC/DU software-state evidence, not RAR transmission, raw
  PRACH, trusted Initial UL position evidence or PHY/RU/RF telemetry. DU
  connection-epoch/stable audit-target binding, authentication,
  freshness/anti-replay, UE-slot identity mapping and terminal-history GC/RNTI
  reuse remain follow-up risks. Snapshot lookup is indexed, but without durable
  GC/reuse this is not an endurance or long-duration namespace-exhaustion proof.
  Codec v2 requires same-version CU/DU deployment; its v1 compatibility is
  decode-only and fail safe. Successful SR/SRS repair must cache the DU's actual
  `applied_request`.

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
  The currently implemented historical-display identity source is the
  versioned management-center `app/onboard-cell-identity-registry.json`: 3528
  satellites and 7056 explicit opaque 36-bit NCIs. Runtime Walker/ordinal NCI
  derivation is forbidden; the final 2,990-satellite design requires its own
  explicit matching 5,980-NCI registry.
- CUCP-037 hardens this boundary without claiming a transport that does not
  exist. RNTI lease ownership keys include `(DU, cell index, PCI, C-RNTI)`,
  because the two stable onboard cells may reuse one PCI. The current DU RNTI
  table is flat, so C-RNTI values remain unique across cells of one DU and may
  be reused only across different DUs. Calendar deployment feedback is monotonic,
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
- CUCP-038 makes the periodic DU resource audit fail safe. Codec v2 qualifies
  the RNTI and UE-slot domains independently, while v1 decodes as incomplete.
  MAC retains pending/consumed/expired lease history and enforces expiry; DU
  returns the real RNTI snapshot. CU-CP atomically validates ACK generation and
  lease membership, converts uncertain delivery to same-generation repair,
  prevents in-flight generation stacking, and reconciles terminal state without
  resending consumed, Initial-UL-seen, committed or expired leases. Generic
  audit-rejection blockers recover after a later clean complete audit. The
  UE-slot domain remains incomplete until reliable CU-global identity mapping
  exists. SIB19 completion also requires matching request/current generation,
  in-flight state and operation result. This is software-state evidence only
  and does not close connection epoch, authentication, anti-replay, long-term
  lease GC/reuse, Initial UL position or RF evidence gaps.
- CUCP-039: private durable onboard-plan state and restart reconciliation. DU
  execution requires `state_file`; historical `applied` data is hidden until a
  complete matching DU query succeeds, while expired deployments retain exact
  cleanup work across restart. Read-only recovery status is exposed without
  changing the default terrestrial path. Whole-file rollback/deletion remains
  open; DU connection-generation binding was added by CUCP-040.
- CUCP-040/041/042/043: disconnect invalidates old DU evidence, future activation does
  not clear the historical plan early, cleanup survives failed durable writes,
  and a failed nested replacement returns to live verification of the hidden
  fallback. A hidden fallback is removed and queued for exact cleanup at its own
  validity deadline, including while DU is disconnected. State-write failure
  cannot expose a new active plan or freeze expiry, and the bounded recovery
  state reserves room for exact cleanup of both saved live plans. These are
  software-control guarantees, not RF execution evidence.
- CUCP-044 adds the management-center dual-set contract. Schema v3 carries the
  complete `visible_l1_positions` inventory and the independent
  `assigned_l1_position_ids` subset in one hash-bound plan. Only the assigned
  subset is partitioned across the two stable onboard cells and scheduled; the
  visible inventory is retained even above 256. State schema v3 persists both
  sets and reads schema v1/v2 with their historical `assigned=visible`
  semantics. Read-only status reports visible and assigned counts separately.
  The Node exporter and C++ use one fixed canonical hash vector. This changes
  no F1AP, DU, MAC, PHY, RU/RF or generated ASN.1 interface.
- CUCP-045 bounds schema-v3 input and closes its CU-CP execution/restart path.
  Plans are limited to 4 MiB, both position arrays to 65,536 entries and the
  planning context identifiers to 256 UTF-8 bytes; recovery state is limited to
  16 MiB without changing state schema v3. Oversize or growing input returns
  `input_too_large` before partition/calendar work and leaves active, pending,
  accepted high-water and the latest successfully parsed inventory unchanged.
  Recovery reads reject state-file growth, and persisted cell assignments plus
  cleanup tasks are bounded before vector reservation.
  The Node producer uses the same limits and same-directory atomic replacement.
  The 300-visible/87-assigned case sends only 87 positions (870 intents),
  activates at the configured epoch and restores only after a DU query. An
  empty assignment installs two empty calendars, while 257 assigned positions
  return `schedule_overflow` and preserve the old active plan. The default-off
  path does not read the plan file. No lower-layer interface changed.
- CUCP-046 adds authenticated schema-v4 management plans and an independent
  software version record. Schema v4 keeps the schema-v3 visible/assigned
  contract and adds `authentication.algorithm`, `key_id` and an ASN.1 DER
  ECDSA signature in canonical base64. The fixed algorithm is
  `ecdsa-p256-sha256`; CU-CP selects only locally configured P-256 public keys,
  rejects duplicate JSON members, bounds cross-language integers and permits
  only delimiter-safe identifiers in the signed canonical form. It then
  reports their SHA-256 SPKI fingerprint and re-verifies the complete signed
  high-water source after restart. Signed execution reserves the new plan
  identity in `version_anchor_file`, durably stores state schema v4, commits the
  version record and only then permits DU software-calendar preparation.
  Restart requires the signed state and independent version record to agree on
  satellite, planning context, two NCI/PCI identities, catalog/schedule
  versions, content hash and key ID; a mismatch or rollback fails closed.
  OAM exposes signature and version-record status without public protocol
  changes. The anchor mode is explicitly `software_only`: it catches ordinary
  state-only rollback but cannot replace HSM/TPM/trusted monotonic storage or
  detect a coordinated rollback/deletion of both local files. `applied` remains
  the installed software access calendar, not PHY/RU/RF or over-the-air execution.
  See `docs/ntn_schema_v4_signed_plan.md`.
- CUCP-047 builds one immutable runtime mapping from the active onboard plan and
  the current live-DU cell inventory. One stable NCI may own many L1 positions;
  the mapping becomes `ready` only while the plan is active, the matching DU
  calendar is applied, validity has not expired, and both cell routes still
  match the DU connection generation. Disconnect and restart hide the mapping
  until a fresh DU query succeeds. Onboard release and paging narrowing use the
  DU cell's exact NCGI and TAI, never a TAC derived from `G######`. A UE using a
  secondary served PLMN currently fails closed for these onboard hints, while
  ordinary paging remains available. Legacy beam-derived behavior and the
  default-off terrestrial path are unchanged.

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
- Do not assume an onboard NCI identifies one L1. One stable onboard cell may
  own many L1 positions in the same active plan, so the onboard execution path
  must not use `find_ntn_beam_id_by_nci` as a one-to-one lookup.
- Do not derive TAC from `G######`, coordinates, or a position ordinal. Onboard
  TAI comes from the uniquely matched live DU cell. The current route binds that
  cell's primary NCGI; a secondary served PLMN is not silently promoted to an
  authoritative onboard paging route.
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
- The final engineering design uses 2,990 satellites in 46 planes with 65
  satellites per plane. The current 3,528-satellite Web animation is only a
  historical display comparison and must not be called the current design.
- The 2,990-satellite design passed coverage and unique assignment at 720
  fixed-step samples, with a peak of 87 assigned L1 per satellite. It remains
  coarse/exact=false: seven-day continuous coverage, N-1 and radio/link checks
  are pre-deployment acceptance work, not an unresolved satellite-count choice.
- Do not compare the complete visible count with the 256 assignment limit.
  Visibility inventory remains complete; only the unique actual assignment is
  limited to 256 L1 per satellite and 128 per onboard cell.
- Use schema v3 whenever management-center visibility and service ownership
  differ. It carries `visible_l1_positions` and `assigned_l1_position_ids`
  independently; only the latter enters the two-cell partition and calendar.
  Schema v1/v2 intentionally keep their historical `assigned=visible`
  behavior and must not be used to encode a larger visibility inventory with a
  smaller service subset.
- Use schema v4 when deployment policy requires authenticated management input.
  Keep the signing private key outside CU-CP and the repository; configure only
  bounded local P-256 public keys with unique `key_id` values. In signed mode,
  schema v1/v2/v3 must be rejected as `signature_required`, and restart must
  re-verify the complete signed high-water plan rather than trusting saved
  version numbers alone.
- Treat `version_anchor_mode=software_only` literally. The separate file can
  detect a state-only rollback or missing counterpart, but both files share one
  host trust domain. Coordinated rollback/deletion requires HSM, TPM or another
  trusted monotonic store to detect.
- Never read plan or recovery files without an explicit bound. Current limits
  are 4 MiB per plan, 16 MiB per recovery state, 65,536 entries in either
  schema-v3 position array and 256 UTF-8 bytes for each planning context
  identifier. Reject size or growth errors as `input_too_large` before `reserve`
  and leave all previously accepted state untouched. Recovery cell-assignment
  and cleanup arrays also need their 65,536 and 66 bounds before reservation.
- Do not aggregate PRACH at satellite level. Every assigned L1 needs its own
  planned PRACH opportunity and corresponding uplink beam intent.
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
