# NTN CU-CP Runtime Contract

This document defines terms that future CU-CP NTN tasks must use consistently.

## candidate_inventory

All digital service beams that are currently eligible for CU-CP consideration
based on static configuration and runtime satellite/service-area state.

Candidates are not capped by loaded service beam limits. A candidate without UE,
DRB, or handover demand must not receive an antenna slot, SR request, or SRS
request.

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

The CU-CP module that owns access C-RNTI ownership records and digital service
slot-resource intent state. It observes DU-reported C-RNTIs, detects duplicate
`(DU, PCI, C-RNTI)` ownership, releases analog access ownership after Initial
Context Setup, and creates or clears digital SR/SRS intent for service-bound
UEs. It does not allocate real C-RNTIs and does not implement DU scheduler
behavior.

## ntn_assistance_snapshot

The CU-CP snapshot used by RRC/SIB19 contracts. It contains satellite epoch,
ephemeris, Common TA, Koffset, Kmac, UL sync validity, reference location,
t-Service, and optional neighbour satellite assistance.

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
