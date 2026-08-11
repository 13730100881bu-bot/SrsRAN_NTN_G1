# NTN CU-CP Runtime Contract

This document defines terms that future CU-CP NTN tasks must use consistently.

## candidate_inventory

All digital service beams that are currently eligible for CU-CP consideration
based on static configuration and runtime satellite/service-area state.

Candidates are not capped by loaded service beam limits. A candidate without UE,
DRB, or handover demand must not receive an antenna slot, SR request, or SRS
request.

In the independent onboard position-plan profile, `candidate_inventory` means
the complete visible `G######` L1 list received from the management center. It
is retained before schedule-capacity checks and is never truncated by the
128-per-cell or 256-per-satellite execution envelope. A visible position is not
necessarily assigned to this satellite.

## versioned_position_plan

One satellite's management-center input. Schema v3 contains an exact-keyed
planning context (`planning_run_id`, catalog id/hash, identity-registry
version/hash and access-profile id/hash), `satellite_id`, catalog/schedule
versions, canonical content hash, validity, activation epoch, exactly two
explicit stable onboard NCI/PCI identities, the complete visible L1 list in
`visible_l1_positions`, and the subset actually served by this satellite in
`assigned_l1_position_ids`. Each visible L1 carries the frozen 7-bit
`child_mask`; this stage validates and stores it without creating
digital-service runtime state. Every assigned id must occur exactly once in the
visible list. NCI is opaque and is not derived from satellite id, coordinates,
position id or cell ordinal.

Schema v3 capacity checks, two-cell partitioning and SSB/PRACH calendar
generation use only `assigned_l1_position_ids`. The complete visible list may
contain more than 256 entries and remains intact. A plan with 257 visible and
at most 256 assigned positions is valid; 257 assigned positions are rejected as
`schedule_overflow` without trimming the received observation.

The schema-v3 canonical hash covers both collections after deterministic
sorting. Exact-key parsing rejects missing or unknown fields, duplicate ids,
and an assignment outside the visible list. The management-center exporter
writes both collections into one plan. Its assignment sidecar is compatibility
information only and is not CU-CP assignment authority.

Schema v1 is accepted only for dry-run. DU execution rejects v1 because it is
not bound to the complete planning context. Schema v2 remains accepted and,
like v1, is normalized to `assigned = visible` so existing plans keep their old
meaning.

## bounded_position_plan_input

Plan producers and CU-CP use the same input bounds:

- one UTF-8 plan file is at most 4 MiB;
- `visible_l1_positions` and `assigned_l1_position_ids` each contain at most
  65,536 entries; and
- `planning_run_id`, catalog id, identity-registry version and access-profile id
  are each at most 256 UTF-8 bytes.

CU-CP reads a regular file in bounded chunks and checks the file before and
after reading. Oversize input, growth during the read and either oversized
array return `input_too_large` before array reservation, partitioning, calendar
generation or state persistence. This rejection changes only the latest
rejection status. Active and pending plans, accepted version high-water marks
and the most recently parsed successful inventory remain unchanged. The OAM
reason is machine readable; file paths and detailed I/O errors stay in logs.

The Node producer checks the same limits before output and writes through a
same-directory temporary file followed by sync and atomic replacement. A failed
write leaves the previous target intact. Limit checks do not alter the schema-v3
canonical hash. Recovery files are bounded at 16 MiB and continue to use state
schema v3. Recovery reads also compare initial and final file sizes; growth is
rejected before recovery. Persisted cell-assignment arrays and the cleanup queue
are checked against their 65,536-entry and 66-identity bounds before `reserve`.

## assigned_l1_position_ids

The exact `G######` subset that the current plan authorizes this satellite to
serve. It is the only collection subject to the configured 128-per-cell and
256-per-satellite limits and the only collection passed to partitioning and
access-calendar generation. It must not be reconstructed by trimming or ranking
the complete visible inventory onboard.

## onboard_cell_position_set

One of the satellite's two stable NR logical cells plus the `G######` L1 ids
assigned to it for one plan version. Every authorized L1 appears in exactly one
of the two sets; visible but unassigned L1s appear in neither. The two NCI
values are distinct; PCI may be reused.

## onboard_runtime_position_mapping

The immutable CU-CP view of the positions served by the currently active
onboard plan. It records the satellite, catalog and schedule versions,
source/calendar hashes, plan validity and the two stable cell routes copied from
the live DU served-cell inventory. It does not retain pointers to DU contexts
and it is not a second source of plan state.

Only `assigned_l1_position_ids` enter this snapshot. Every assigned position
appears exactly once and retains its `child_mask`; visible-only positions do not
appear. One stable NCI may own many positions. Results returned by
`positions_for_nci(nci)` are ordered by `position_id`, so identical active input
produces an identical query result. The read-only queries are:

- `find_position(position_id)`, returning the position and its stable NCI/PCI;
- `positions_for_nci(nci)`, returning all positions owned by that cell;
- `resolve_cell_route(nci)`, returning the copied NCGI, TAC, DU cell and DU
  connection generation; and
- `classify_position_transition(old_position, new_position)`, returning
  `no_change` for the same position, `same_cell` for two positions under one
  NCI, `cell_change` when the owner NCI changes, and `unknown` when either
  position or the snapshot is unavailable.

Runtime mapping stages are `disabled`, `awaiting_active_plan`,
`awaiting_live_du`, `ready` and `stale`. `ready` requires onboard execution, an
active and still-valid plan, matching applied DU calendar state, unique live-DU
resolution of both configured NCI/PCI identities and the same DU connection
generation that supplied the application result. A pending plan does not alter
the ready snapshot. A failed update keeps the old valid snapshot, while an
activation replaces the complete snapshot at once.

A DU disconnect immediately hides the snapshot. Reconnect and process restart
both require a new exact DU query before rebuilding it; the mapping is not
persisted separately. An active plan with no assigned positions has a valid
zero-position snapshot, but neither cell is eligible for onboard paging
narrowing.

## onboard_cell_tai_and_paging

An onboard cell route uses the primary NCGI copied from its uniquely matched DU
served cell: NCGI is that cell's PLMN plus stable NCI, and TAI is the same PLMN
plus that cell's TAC. The complete PLMN+TAC pair must occur exactly once in the
NGAP supported-TA configuration. The route reports `ready`,
`supported_tai_missing`, `supported_tai_duplicate` or `plmn_tac_mismatch`.
Position queries remain observable when TAI is not ready, but onboard core
location and paging narrowing are disabled.

Before UE release, CU-CP may store an idle paging context containing its
authority, NCI, exact NCGI and TAI, schedule version, calendar hash and plan
`valid_until`. It may recommend that stable NCI only while every field still
matches the ready runtime mapping and the cell owns at least one assigned
position. A missing, stale or expired context does not guess a `position_id` and
does not narrow the normal TAI-based paging path. A current AMF recommendation
is preserved only when its complete NCGI matches a current cell route and the
Paging TAI list contains that route's complete PLMN+TAC.

The current route represents the DU cell's primary `cell.cgi`. If a UE selects a
secondary PLMN served by the same cell, CU-CP does not create onboard release
location, idle context or paging narrowing from that primary route. This is a
fail-closed boundary for the onboard hint only; ordinary paging remains
available. The onboard execution profile never derives TAC from `G######` and
does not call the legacy one-beam-per-NCI lookup for this decision.

## access_calendar_intent

A checked CU-CP planning item containing schedule version, stable cell NCI,
L1 position, cycle offset, duration, direction, purpose and cell-local port.
It is distinct from `loaded_service_calendar`. CUCP-036 can turn its SSB/PRACH
portion into a software scheduler gate, but the intent itself is not RF or
position-steering evidence.

## analog_access_beams

CU-CP control-plane access groups over digital service beams. In the default
hex model, one analog access beam contains one center digital beam plus six
ring-1 neighbours unless the circular footprint clips the cluster at the edge.

Analog access beams may gate new access, reestablishment, handover target
eligibility, and paging recommendation eligibility. They do not carry NCI,
beam-derived TAC, loaded service calendar, SR/SRS request, or RF beamforming
semantics.

## digital_service_beams

The fine-grained 15 km service beams that carry NCI, Mapped Cell, beam-derived
TAC, TAI, loaded service calendar, SR/SRS request intent, QoS policy, and
per-beam observability.

## mobility_eligible_beams

The subset of digital service beams that may be used by measurement/location
mobility as target beams. This normally includes candidates and active loaded
beams whose parent analog access beam is eligible, and excludes draining,
inactive, or analog-ineligible beams.

## active_loaded

A beam with current UE, DRB, reestablishment, or handover demand. Only
`active_loaded` beams belong to the loaded service calendar and may receive
CU-CP slot request intent.

## draining

A beam that must not admit new UEs but may keep existing UE/DRB control-plane
contracts while handover, release, or service-window cleanup completes.

## inactive

A beam that is disabled, unknown, below release criteria, outside the service
window, unsupported by DU capability, or otherwise unavailable.

## loaded_service_calendar

The CU-CP ordered set of `active_loaded` and still-loaded `draining` beams that
need slot request intent. It produces antenna slot index, number of slots,
period, SR offset/period, and SRS offset/period.

This is not a DU scheduler allocation.

## ntn_beam_service_resource_manager

The CU-CP module that owns NTN C-RNTI lease pools, access ownership records and
digital service slot-resource intent state. It distributes leases, validates
Initial UL ownership, releases analog access ownership after Initial Context
Setup, and creates or clears digital SR/SRS intent for service-bound UEs. C-RNTI
identity is keyed by `(DU, DU cell index, PCI, C-RNTI)` because separate onboard
cells may reuse PCI. The key remains cell-scoped, but a C-RNTI value is unique
across cells of one DU because the current DU RNTI table is flat; different DUs
may reuse it. A lease update containing the same C-RNTI twice is rejected before
any entry is inserted. It does not implement raw PRACH detection or the
terrestrial allocator.

## ntn_resource_audit_snapshot

The periodic private F1 resource-audit result has two independently qualified
domains: `rnti_snapshot_complete` and `ue_slot_snapshot_complete`. CU-CP may
compare absence and generate repair only inside a domain whose completeness bit
is true. `accepted=true` means the request was processed; it does not make an
empty domain authoritative. Codec v2 carries both bits and a `generation_id`
for every RNTI entry. A decoded v1 result sets both domains incomplete and each
entry generation to zero, so a legacy `accepted=true` empty result cannot cause
false repair. Compatibility is one-way: v2 CU decodes v1 fail safe, but rolling
a v2 DU under a v1 CU is unsupported; deploy the CU and DU at the same version.

The RNTI domain is backed by MAC's per-cell lease ledger. Entries progress from
`pending` to `consumed_by_mac` and then `expired`; `expiry_ms` is enforced by a
monotonic MAC clock. Expiry is refreshed before a replace is validated. A
replace becomes visible atomically; failure leaves the old ledger unchanged.
Terminal history is kept through replace so audit cannot resurrect a consumed
RNTI. An exact same-generation `add` retry is a no-op for pending, consumed or
expired entries and neither refreshes expiry nor changes terminal state. DU
translates that ledger into its complete RNTI snapshot after validating gNB-DU
identity, full NCGI, PCI and snapshot cell index.

CU-CP applies a lease result only when its generation and complete
accepted/rejected set match the update. Missing or malformed results leave the
pool `sent_to_du` with an `ack_unknown` reason; a complete audit then resends the
same generation. A fresh in-flight pool is not resent and prevents creation of
a newer low-water generation. Matching `sent_to_du/pending` evidence promotes
the pool to `applied_by_du`; `consumed_by_mac` and `expired` observations are
reconciled into the CU lifecycle. Explicit DU audit rejection is preserved as a
target conflict, and a later accepted complete audit without a new conflict
resolves its generic blocker. Only a still-unused lease absent from a complete
snapshot is eligible for resend. Leases already consumed by DU, seen on Initial
UL, committed or expired are terminal for resend purposes; conflict-free
low-water refill creates a new lease instead of resurrecting an old C-RNTI.

CUCP-049 completes this lifecycle with safe retirement. A released or expired
C-RNTI enters `retire_pending`, then `retire_sent`, and becomes `retired` only
after a complete snapshot from the current DU connection reports the same
generation as expired and MAC atomically confirms that every number in the
batch is unused. MAC keeps compact generation history after retirement: a
duplicate retire is idempotent, an older add or retire is rejected, and reuse
requires a strictly larger generation. Restart and reconnect quarantine unknown
DU records and require a fresh complete snapshot. An old DU without retirement
capability may continue the earlier pool flow, but uncertain retired values
remain unavailable. Allocation scans `0x4601..0xffef` per DU and publishes no
partial pool when eight safe values cannot be found.

The MAC terrestrial allocator and NTN ledger use the same lock and
`allocate_for_cell` chooses the mode inside that lock. Every tracked NTN RNTI is
excluded from terrestrial allocation. A returned terrestrial TC-RNTI is
recorded for 10 seconds between RACH allocation and UE insertion, allowing a
concurrent first NTN update to reject a collision. Before any NTN mode is
active, that record is not used to filter terrestrial selection, so the original
RNTI sequence and functional behavior remain unchanged; once NTN is active it
becomes the allocation guard. This does add synchronization overhead to the
allocator and is not a performance-equivalence claim.

CUCP-050 makes the UE-slot domain authoritative on a supported current
connection. A versioned SR/SRS request carries a nonzero
`assignment_generation` and an explicit `set` or `clear` operation. An exact
retry keeps its generation. A resource-content change, a clear, or a new
assignment after clear advances the generation. DU returns
`assignment_generation_conflict` for the same generation with different
content, `stale_assignment_generation` for an older generation and
`slot_assignment_generation_exhausted` when a next generation cannot be
represented.

DU stages the request and publishes an active entry only after resource
allocation and the MAC/scheduler configuration transaction both succeed. Its
registry records the actual SR/SRS parameters rather than the requested
parameters. A successful clear or UE removal removes the active entry, while
the generation high-water remains until the related context is destroyed. The
per-cell snapshot is
bounded to `MAX_NOF_DU_UES` (1,024). A duplicate identity, cross-cell entry,
failed rollback, truncation or inconsistent internal state returns
`ue_slot_snapshot_complete=false`; zero active entries is a valid complete
snapshot.

The DU registry keeps its internal UE and cell identity, assignment generation
and actual SR/SRS request. When the audit response is built, DU joins that record
with the current C-RNTI and both current F1 UE IDs. The authoritative F1 entry
therefore contains both current F1 UE IDs, NCGI, PCI, C-RNTI, assignment
generation and the actual request. The audit target contains gNB-DU ID, NCGI,
DU cell, PCI, audit generation and a nonzero local
`connection_token`; the response must echo them exactly. CU-CP resolves the pair
of F1 UE IDs only through the current F1 context and then checks the current
DU, cell, PCI and C-RNTI. It does not recover identity from an old `ue_index_t`,
an ordinal or C-RNTI alone.

The comparison results are:

- `matched`: identity, generation and actual parameters match;
- `missing`: CU-CP has an expectation and DU has no entry; resend the original
  generation once;
- `conflict`: the same generation has different parameters, DU is ahead, or
  identity/cell content contradicts the current UE; do not overwrite;
- `quarantined`: the DU entry cannot be resolved to the current F1 UE; do not
  clear it;
- a resolved DU-only entry may receive one `generation + 1` clear.

After any repair, CU-CP requests another complete snapshot and declares the DU
`reconciled` only when every entry matches. The exact combination of UE,
operation, assignment generation and resource content is automatically repaired
at most once and never directly releases a UE. DU disconnect discards
capability, token, in-flight responses and the reconciled/applied view while
preserving the current process's expected assignments. Reconnect must obtain a
fresh snapshot. CU-CP restart does not rebuild local UE identity from a DU
snapshot, and no new state file is used.

The RNTI and UE-slot completeness decisions are independent. An incomplete
UE-slot result cannot interrupt RNTI retirement that already meets its own
conditions. If a DU does not support the authoritative identity snapshot, CU-CP
uses the earlier audit format and keeps ordinary SR/SRS applied feedback and
RNTI handling, but disables automatic UE-slot repair. A new DU continues to
answer the earlier request format when connected to an old CU.

The read-only `ntn_state` output includes UE-slot audit state, capability,
complete-snapshot state, `matched`, `missing`, `conflict`, `quarantined` and
`repaired` counts, assignment generation high-water and the latest reason. The
first four counts aggregate each target's latest complete snapshot. `repaired`
counts successful repair acknowledgements on the current DU connection and
target set. Reconnect or a target-set change resets those values; the overall
assignment generation high-water is the maximum current value among DUs.
These values cover CU-CP and DU software resource state. RAR transmission, raw
PRACH detection, authenticated Initial UL position metadata and PHY/RU/RF state
belong to their respective runtime interfaces. CUCP-051 implements the Initial
UL receive-source path through its task-authorized PRACH/FAPI/MAC/DU/private-F1/
OFH changes; live hardware remains in the device integration task. The local
connection token blocks results from an older connection; sender authentication
and end-to-end anti-replay remain separate work. Build and test results are recorded in
`docs/ntn_ue_slot_audit_recovery.md`.

## onboard_plan_deployment_stage

The separate software-execution state for a checked pending calendar:
`not_sent -> preparing -> ready -> applied`, with terminal
`rejected/unsupported`. Matching feedback is monotonic and idempotent; a delayed
response cannot regress `applied` to `ready` or `not_sent`. Prepare and query
acceptance require matching catalog/schedule version, source/calendar hashes and
the complete accepted intent count for both cells.

`applied` is evidence that the matching SSB/PRACH software gate is installed.
It is not evidence of antenna steering, a transmitted or received beam, PHY
execution, RU state, or RF output.

For the schema-v3 execution reference case, 300 visible positions and 87
assigned positions produce a calendar for only those 87 ids: 696 SSB intents,
87 PRACH ROs and 87 matching UL-beam intents, 870 intents in total. The plan
passes prepare and application before the activation timer publishes it. A
restart queries DU before restoring the 300/87 state. An assignment of 257 ids
returns `schedule_overflow` and leaves the old active plan in service. An empty
assignment prepares two empty cell calendars as an explicit deny-all software
gate.

## onboard_plan_recovery_state

When DU calendar execution is enabled, CU-CP requires a private `state_file`.
Dry-run mode does not require it. The file is limited to 16 MiB, is atomically
replaced and records:

- the highest accepted catalog and schedule versions;
- the active and pending plans, including their exact two-cell L1 partition,
  source/calendar hashes, activation epoch and validity;
- the last known lower-layer software deployment state; and
- calendar cleanup tasks that still need confirmation; and
- a read-only summary of the latest successfully parsed management-center
  input: catalog/schedule version, content hash, activation epoch and the
  complete visible inventory plus the assigned-id subset. This is not a
  byte-for-byte copy of the source JSON and is never deployment authority.

State schema v3 stores both collections explicitly. State schemas v1 and v2
remain readable and are normalized to `assigned = visible`; all subsequent
writes use schema v3. A received observation may contain more than 256 visible
positions. It may also describe a rejected 257-assigned-position overflow whose
version is above the accepted high-water; it does not become partition,
identity, activation or DU-application authority. Cleanup and repeated restart
must preserve both collections without truncation. An explicit
`received_plan:null` stays empty.

The saved deployment state is history, not live evidence. At restart CU-CP
rechecks the schema, hashes, planning context, satellite and cell identities,
versions and validity. It then queries DU for the exact plan version/hash before
showing that plan as `active` or `applied`. Only complete matching feedback for
both cells can restore those labels. Missing, incomplete, expired or mismatched
feedback fails closed and cannot replace a still-valid old plan.

The runtime position mapping is not stored in this file. It is rebuilt from the
revalidated active plan only after the live-DU query above succeeds.

If a saved deployment has expired, CU-CP records an exact cleanup task and keeps
that task across later restarts until clear feedback is confirmed. A cleanup
task is tied to the expired version/hash and must not clear a different active
fallback plan.

The same rule applies while running: an applied active plan that reaches
`valid_until` queues its exact version/hash even when a future plan is still
pending and `not_sent`. An expired plan that was never sent creates no DU clear.

A confirmed clear uses a two-step durable commit: CU-CP first writes the next
state without the live queue head, and removes that head from memory only after
the write is durable. A write that did not replace the state file restores the
last saved controller snapshot and keeps any exact cleanup task. If replacement
completed but directory durability cannot be confirmed, CU-CP does not roll
memory back to bytes that no longer match the file; it hides live application
evidence and requires restart reconciliation instead. Both cases block new
deployment work.
This is at-least-once cleanup: after an uncommitted failure the same exact
version/hash may be retried, so DU clear must be idempotent. The guarantee is no
silent loss, not exactly-once delivery.

A blocked state store does not freeze plan validity. CU-CP continues processing
`valid_until`, so expired active, pending, recovery and hidden-fallback plans
are removed from usable state and retain exact cleanup identities. New
preparation, activation, recovery confirmation and cleanup transmission stay
suppressed until a safe restart. A newly applied pending plan is published as
active only after the matching transition is durably saved.

Recovery storage bounds the combined cleanup responsibility, not only the
historical queue. At most 66 calendar identities may require later cleanup:
64 historical tasks plus the two persisted live snapshots (`active` and
`pending`). When either live snapshot expires, it becomes a queue entry without
increasing that total. Recovery state above the bound is rejected with
`too_many_cleanup_claims`; an exact task is never silently discarded.

DU application evidence is connection-scoped. Disconnect immediately hides
the old `active/applied` evidence and moves the affected plan to reconciliation.
Prepare completions are guarded by DU connection generation and exact plan;
query and clear also carry request-instance guards. Only a complete response
from the current live connection for the same two cells and version/hash can
restore evidence. Responses from an older connection are ignored.

An early `applied` result for a future plan does not cross its
`activation_epoch` and does not by itself clear a still-valid historical active
fallback.

If that hidden fallback reaches its own `valid_until` before the future plan is
decided, CU-CP permanently removes it from fallback eligibility at that deadline
and queues one exact (`schedule_version`, `calendar_hash`) cleanup task with
reason `historical_fallback_expired`. This deadline is processed before a
same-instant pending-plan activation. A DU disconnection delays transmission,
not creation of the cleanup obligation; state schema v3 preserves the pending
plan and outstanding cleanup across restart. A later update failure cannot
restore the expired fallback. Queue or state-write failure remains fail closed
and must not expose the old plan as usable. This is software-calendar cleanup,
not RF or device cleanup evidence.

The read-only `ntn_state` view exposes whether a state file is configured and
required, its schema/generation/hash, the last save result and error, whether
writes are blocked, the catalog/schedule high-water marks, and recovery
stage/detail. It also exposes the active calendar hash and cleanup queue head
version/hash/reason. Visible and assigned L1 counts are reported separately,
followed by the two per-cell assigned counts. These fields describe CU-CP
storage and DU software reconciliation; they are not RF telemetry. With the
onboard NTN profile disabled, none of this changes the terrestrial path.

This file is a recovery aid, not a trust anchor. Its own high-water marks live
inside the same file, so replacing the entire file with an older valid copy or
deleting it cannot yet be distinguished from an earlier state or first boot. A
separate trusted monotonic anchor is needed for that protection. CU-CP now binds
calendar feedback to its local DU connection generation and request instance,
but that local freshness guard is not sender authentication or transport-level
anti-replay.

## initial_access_plan_audit

A CU-CP-private comparison of complete Initial UL position metadata with the
current active plan. It checks satellite/version/hash, stable NCI/PCI, position
owner, PRACH occasion phase, paired UL-beam window and cell-local port. The
result is `accept`, `reject` or `audit_only` with a machine-readable reason.

CUCP-048 adds a bounded consumer in front of RRC Setup. Its exact lookup key is
`DU + DU cell + C-RNTI + DU connection generation`. The injected provider keeps
at most 1,024 records for one second, detects ambiguous keys and replayed
observation IDs, and permits one consumption attempt per record. CU-CP also
requires the PRACH event to be non-future and less than one second old, plus a
ready runtime mapping, a valid DU-applied active plan and an exact DU cell route
before continuing ordinary admission.

The policy modes are `disabled`, `audit` and `strict`. `disabled` is the
default. `audit` records the result and continues the existing admission path.
`strict` rejects before access ownership is written and can start only with a
ready generation-authoritative, device-verification-capable source. A
successful strict check creates a non-persistent UE context that is removed at
ICS completion, UE removal, setup failure, plan activation, DU disconnect and
restart.

The observation provider is a private C++ extension contract exposed for
programmatic `cu_cp_configuration` injection. CUCP-051 also constructs the
runtime private-F1 source when position validation is enabled. Standard
Initial UL messages and generated ASN.1 remain unchanged. The consumer never
derives `position_id` from NCI, coordinates or legacy beam state. See
`docs/ntn_initial_ul_position_consumer.md` and
`docs/ntn_initial_ul_receive_source.md`.

## initial_ul_receive_provenance

The cross-layer source used by CUCP-051 to associate one detected PRACH with an
active onboard L1 position and the exact C-RNTI lease generation allocated for
that access attempt.

The optional detector output is one of `unique`, `ambiguous` or `unavailable`.
`unique` requires the strongest receive port to meet the configured margin over
the second strongest; the default margin is `6 dB`. The attribution calculation
does not change the combined-port PRACH detection metric, threshold, preamble,
TA or power result. An authorized PRACH request also carries the exact calendar
schedule version, extended cycle index and in-cycle offset through scheduler,
FAPI and MAC.

One valid receive mapping has a nonzero version, a bounded hash and entries
keyed by `NCI + cell_local_port`. An SDR entry maps to one physical receive
port. An OFH entry maps to one physical receive port, PRACH eAxC and 15-bit
BeamId. Keys and backend identities are unique within the NCI. The total is
bounded to 1,024 entries and 16 entries per NCI; physical ports are `0..254`,
OFH eAxC values are `0..31`, and BeamId values are `0..32767`.

Source authority has three values:

- `software_attributed`: one calendar candidate with no usable measured port;
- `sdr_rx_port_verified`: the unique physical receive port and SDR mapping
  select the same calendar UL-beam intent;
- `ofh_beam_id_verified`: the unique receive port, RU capability, Type-3
  C-plane BeamId, U-plane eAxC, calendar identity and mapping identity all
  select the same intent.

`software_attributed` can be consumed by `audit` and cannot authorize `strict`.
A unique measured port that does not match the configured mapping returns
`rx_mapping_mismatch`; absent OFH BeamId capability returns
`ofh_beam_capability_unavailable`. Neither case is converted into software
attribution.

Contention-based access uses `allocate_for_cell_with_generation()` to return
the C-RNTI and its lease generation atomically. The legacy allocator API keeps
the same RNTI result, and a terrestrial allocation carries generation zero.
MAC records at most 1,024 pending observations for one second and correlates
one record with Msg3 UL-CCCH. DU stores it under the current
`gnb_du_ue_f1ap_id` with the same capacity and lifetime.

CU-CP retrieves the record before ordinary Initial UL processing by using a
private `GNB-DU Resource Coordination` container. `NTPOSQ01` and `NTPOSR01`
are limited to 1 KiB. Query and result echo query generation, nonce, live
connection token, gNB-DU ID, NCGI, DU cell, PCI, DU UE F1 ID, C-RNTI and
expected lease generation. The default wait is 50 ms and the configured range
is 10..200 ms. A same-nonce retry returns the same consumed result; a different
nonce cannot consume it. Oversize, truncated, trailing, malformed, stale-token
or identity-mismatched results are rejected.

CU-CP calculates the PRACH event time from the active plan activation epoch,
calendar-cycle index and in-cycle offset. It does not trust a DU wall-clock
timestamp. It then applies `initial_access_plan_audit`. Per-DU-cell source state
uses `disabled`, `awaiting_calendar`, `awaiting_rx_backend`, `ready` and
`stale`. Mapping replacement and F1 connection loss invalidate stale records
and readiness. An accepted RNTI `replace/clear` invalidates the cell, `retire`
invalidates only accepted RNTIs, and `add` preserves unrelated observations.
Calendar application retains the active schedule/hash; clear or rollback
erases only the exact target.

Current source state, backend and mapping identity project only connected DUs.
Observation and failure counters are process-cumulative and can retain history
from a disconnected DU until process restart.

The implemented runtime boundary covers focused cross-layer tests and scripted
code-level simulations of the SDR/ZMQ-configured receive-port and OFH
BeamId/eAxC paths. Those simulations do not inject live ZMQ IQ samples or
exercise a physical RU, UHD, antenna switching or over-the-air traffic. Live
UHD channel calibration, vendor RU BeamId interoperability and over-the-air
acceptance belong to the device integration task.

CUCP-051 closeout passed 5/5 selected `ntn_mobility_test` cases, 18/18 exact
CU-CP Initial UL cases and 3/3 configuration cases, and `srsran_cu_cp` built.
The receive-port code-level CTest scenario exited zero with 33/33 and no skips.
The OFH code-level CTest scenario exited zero and passed with 45 matched tests:
37 passed, zero failed and eight platform-conditioned skips. Neither scenario
used `-Build` or exercised live ZMQ IQ or physical RU hardware. The final
`git diff --check` passed and cleanup found zero residual processes.

## ntn_assistance_snapshot

The CU-CP snapshot used by RRC/SIB19 contracts. It contains satellite epoch,
ephemeris, Common TA, Koffset, Kmac, UL sync validity, reference location,
t-Service, and optional neighbour satellite assistance.

SIB19 F1 feedback must match the request generation. CU-CP applies it only
while the same generation is still in the expected `sent_to_du` or `clear_sent`
state, and update/clear require `applied`/`clear_applied` respectively. Production
DU resource-coordination requests are FIFO through full MAC completion, so
responses cannot overtake each other. A replayed lower-generation request is
not yet rejected by a DU high-water mark and remains part of the broader
authentication/freshness/anti-replay follow-up.

## ntn_ue_runtime_context

The CU-CP NTN state attached to a UE: location, location source, horizontal
accuracy, serving beam, candidate target beam, Mapped Cell, derived TAC, TAI,
pending handover, draining state, and last accepted location report.

## Service freshness rule

If satellite state or assistance validity expires, CU-CP stops new NTN access
for affected service areas and moves loaded beams to draining when possible.

## Backward compatibility rule

The old `max_nof_served_beams` field is a compatibility alias only. Future code
should use `max_nof_loaded_service_beams` semantics. Neither field may cap
`candidate_inventory`.
