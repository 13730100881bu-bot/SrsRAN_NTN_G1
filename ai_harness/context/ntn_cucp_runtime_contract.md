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
128-per-cell or 256-per-satellite execution envelope.

## versioned_position_plan

One satellite's management-center input: `satellite_id`, catalog/schedule
versions, canonical content hash, validity, activation epoch, exactly two
explicit stable onboard NCI/PCI identities, and the complete visible L1 list.
NCI is opaque and is not derived from satellite id, coordinates, position id or
cell ordinal.

## onboard_cell_position_set

One of the satellite's two stable NR logical cells plus the `G######` L1 ids
assigned to it for one plan version. Every candidate L1 appears exactly once.
The two NCI values are distinct; PCI may be reused.

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

The MAC terrestrial allocator and NTN ledger use the same lock and
`allocate_for_cell` chooses the mode inside that lock. Every tracked NTN RNTI is
excluded from terrestrial allocation. A returned terrestrial TC-RNTI is
recorded for 10 seconds between RACH allocation and UE insertion, allowing a
concurrent first NTN update to reject a collision. Before any NTN mode is
active, that record is not used to filter terrestrial selection, so the original
RNTI sequence and functional behavior remain unchanged; once NTN is active it
becomes the allocation guard. This does add synchronization overhead to the
allocator and is not a performance-equivalence claim.

The UE-slot domain currently reports incomplete. The present DU state does not
provide a reliable mapping from every local slot assignment to CU-global
`ue_index_t`, so an empty UE-slot list cannot authorize apply/clear repair.
When an SR/SRS repair succeeds, CU-CP caches the DU result's
`applied_request` (falling back to the request only when absent), so a DU-adjusted
offset or period does not create a repeated false mismatch.

This snapshot is MAC/DU software-state evidence. It is not proof of RAR
transmission, raw PRACH detection, trusted Initial UL position metadata, or
PHY/RU/RF execution. Complete snapshot lookup is indexed rather than quadratic,
but no endurance test has been run. The read-only `ntn_state` output exposes
consumed leases, audit generation/counters, per-domain incomplete counts and the
latest audit reason. The audit request
still lacks stable NCI and DU connection-epoch binding, and the flow is not yet
protected by sender authentication, freshness or anti-replay. Terminal-history
garbage collection and a durable C-RNTI reuse policy also remain follow-up work;
without them, long-duration namespace exhaustion is not closed.

## onboard_plan_deployment_stage

The separate software-execution state for a checked pending calendar:
`not_sent -> preparing -> ready -> applied`, with terminal
`rejected/unsupported`. Matching feedback is monotonic and idempotent; a delayed
response cannot regress `applied` to `ready` or `not_sent`. Prepare and query
acceptance require matching catalog/schedule version, source/calendar hashes and
the complete accepted intent count for both cells.

## initial_access_plan_audit

A CU-CP-private, side-effect-free comparison of complete proposed Initial UL
sideband metadata with the current active plan. It checks satellite/version/
hash, stable NCI/PCI, L1 owner, PRACH occasion phase, paired UL-beam window and
cell-local port, returning `accept`, `reject` or `audit_only` plus a machine-
readable reason.

No production F1AP Initial UL transport carries all of this metadata today.
Therefore this contract must not infer `position_id` from legacy beam-to-NCI
state. `accept` means only that supplied metadata matches the CU-CP active-plan
and current software-gate snapshot; it does not authenticate the sender or add
receive-time freshness/anti-replay, is not durable across DU reconnect without
reconciliation, and is not PHY/RU/RF proof.

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
