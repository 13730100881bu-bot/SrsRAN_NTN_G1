# NTN CU-CP Development Roadmap

This roadmap orders future implementation work after CUCP-004.

## CUCP-005 Runtime taxonomy

Separate candidate inventory, mobility eligible beams, active loaded beams,
draining beams, loaded service calendar, assistance snapshot, and UE runtime
context.

## CUCP-006 Candidate inventory

Compute all admissible candidate beams from satellite state, beam table,
service window, elevation, PLMN/TAC/slice, and DU support. Candidate inventory
must not be capped by loaded-beam limits.

## CUCP-007 Demand-driven service calendar

Generate CU-CP slot request intent only for beams with UE, DRB, reestablishment,
or handover demand.

## CUCP-008 NTN assistance contracts

Create CU-CP-side RRC/SIB19 assistance snapshots and contract tests.

## CUCP-009 Admission and mobility

Apply NTN state to UE setup, reestablishment, handover, PDU session demand,
handover retry, draining, and stale-ephemeris policy.

## CUCP-010 NGAP and core mapping

Implement Mapped Cell, derived TAC, TAI, location reporting, area-of-interest,
and AMF control behavior.

## CUCP-011 Switch-over and resilience

Model soft and hard service/feeder switch-over events, manual override, safe
degradation, and source priority.

## CUCP-012 Observability and examples

Expose runtime commands and update example configurations so operators can
inspect satellite, assistance, beam, and UE state.

## CUCP-013 RRC/SIB19 assistance packaging contract

Convert CU-CP `ntn_assistance_snapshot` state into bounded, ASN.1-verifiable
SIB19 packaging input without implementing DU system-information scheduling or
broadcast.

## CUCP-014 QoS-aware service policy

Use QoS, ARP, 5QI, GBR, preemption capability, and slice metadata to prioritize
loaded service beams, order CU-CP SR/SRS slot request intent, expose runtime QoS
summaries, and reject lower-priority new PDU-session demand that would displace
an existing higher-priority loaded service beam. This remains CU-CP policy only;
it does not preempt existing bearers or modify DU/MAC/PHY scheduling.

## CUCP-015 Paging and idle assistance

Derive single-satellite beam-based TAC values, use them for CU-CP NTN core
location context, add UE release recommended-cell hints for fresh active or
candidate NTN beams, and narrow incoming paging only when AMF-provided
recommended cells are applicable.

## CUCP-016 Service-area paging observability

Expose beam-derived TAC validity, paging recommendation eligibility, and
deterministic non-recommendation reasons through CU-CP beam/runtime snapshots
and O-CU-CP `ntn_state` / `ntn_beams` command output. Keep release paging hints
strictly tied to valid beam-derived TAC while preserving core-location fallback.

## CUCP-017 Analog/digital hex beam model

Split the single-layer beam inventory into analog access beams and digital
service beams. Analog access beams are deterministic 7-cell axial-hex clusters
used for CU-CP access, reestablishment, handover target and paging eligibility.
Digital service beams keep NCI, TAC, loaded service calendar, SR/SRS request
and QoS semantics. The default 500 km / 50 deg / 15 km LEO profile contains
843 digital beams and 137 analog beams.

## CUCP-018 Access DU assignment policy

Assign active analog access beams to DUs using CU-CP-only policy over DU child
digital support, previous ownership, analog distribution, and load. Prefer the
same DU for analog access and first digital service demand; reject new access or
PDU-session demand when the current UE DU cannot satisfy the access/service DU
contract. Do not perform automatic inter-DU migration or modify DU/MAC/PHY
scheduling.

## CUCP-019 Pre-service inter-DU relocation

Allow temporary signaling-only access from a non-selected DU when a deterministic
target access/service DU exists, then use existing intra-CU inter-DU handover
before first service demand. Block new PDU demand until relocation completes.

## CUCP-020 Connected beam-to-beam mobility

Coordinate connected UE beam-to-beam mobility by preloading the target digital
service beam into the loaded service calendar before invoking existing CU-CP
handover routines. This remains location-driven CU-CP policy and does not add
RSRP-driven NTN handover or DU/MAC/PHY behavior.

## CUCP-021 Access/service layer contract

Keep UE analog access context separate from digital service context. RRC setup
and reestablishment establish access context only; first PDU/DRB demand binds a
digital service beam by fresh UE location or access-cell fallback.

## CUCP-022 Resource-domain guard policy

Add CU-CP analog/digital resource-domain caps, reuse group ids, explicit
conflict groups, and deterministic blocked reasons. Use these guards for access
admission, digital service binding, loaded service placement, connected handover
preload, and OAM snapshots. Keep candidate inventory intact and do not implement
real RF reuse, power control, PRACH, or MAC scheduling.

## CUCP-023 Analog release and digital service ownership

Release per-UE analog access ownership after Initial Context Setup success.
Control-only UEs keep CU-CP context but do not occupy digital service beams,
loaded calendar, SR/SRS intent, or digital caps until first PDU/DRB demand binds
a digital service beam. Expose separate analog access intent and digital service
intent snapshots for CU-CP observability only.

## CUCP-024 Beam service resource manager

Centralize analog access C-RNTI ownership and digital service SR/SRS slot intent
in a CU-CP-only manager. CU-CP observes and validates DU-reported C-RNTIs,
detects duplicate ownership, releases analog access ownership after ICS, and
generates/clears digital slot intent from service-bound digital contexts.

## CUCP-025 Real NTN RNTI and UL control resource allocation

Promote the beam service resource manager from ownership/intent tracking to a
CU-CP-authoritative NTN model. CU-CP owns NTN C-RNTI lease and digital SR/SRS
assignment decisions; DU/MAC/Scheduler execute, validate, and report results
under exact non-CU-CP task exceptions. With NTN inactive, terrestrial selection
and default outcomes remain unchanged; shared synchronization overhead is not a
performance-equivalence claim.

## CUCP-026 RNTI lease pool distribution

Distribute CU-CP-generated NTN RNTI lease pools to DU/MAC before access through
the private F1AP resource-coordination container. Active analog access beams are
eligible for new access only after the target DU/cell has applied a usable lease
pool.

## CUCP-027 RNTI lease lifecycle and access readiness

Close the access-number lifecycle: reserved, sent, applied, offered in RAR,
initial UL seen, committed, released, expired, and conflict. CU-CP rejects NTN
access when the RNTI is not from an applied pool for the correct DU/cell/analog
beam. With NTN inactive, terrestrial RNTI selection and default outcomes remain
unchanged; shared synchronization overhead is not a performance-equivalence
claim.

## CUCP-028 SR/SRS application feedback

Close the digital service SR/SRS loop. CU-CP sends NTN digital service SR/SRS
assignment through the UE context procedure, DU returns applied/rejected result,
and CU-CP commits digital service context only after DU-applied feedback. The
scheduler is validated against the applied UE configuration.

## CUCP-029 Connected handover target resource reservation

Require connected beam-to-beam handover targets to reserve a target NTN C-RNTI
and receive DU-applied target SR/SRS resources before source-side RRC handover
reconfiguration is sent. Failure rolls back target reservations while preserving
source service resources.

## CUCP-030 Resource consistency auditor and repair loop

Actively audit DU NTN resource state through a private F1AP resource
coordination query, compare it against CU-CP authoritative RNTI lease and
digital SR/SRS state, and generate conservative resend, clear, conflict, or
rollback repair actions. Committed UEs are not forcibly released solely because
an audit mismatch is detected.

## CUCP-031 Resource repair executor and guarded recovery

Execute CUCP-030 repair actions through existing CU-CP/F1AP control paths:
resend missing RNTI lease pools, reapply or clear SR/SRS resources, roll back
stale handover target reservations, and block repeated resource conflicts after
one retry. The inactive terrestrial functional path and generated ASN.1 remain
unchanged; allocator synchronization cost is outside that compatibility claim.

## CUCP-032 SIB19 DU SI broadcast application

Promote CU-CP SIB19 assistance from packaging contract to DU-applied dynamic SI
payload updates. CU-CP sends active/candidate SIB19 update payloads through the
private F1AP resource coordination container, clears draining or stale dynamic
payloads, and exposes desired/sent/applied/rejected/cleared/stale state. DU SI
scheduling must already exist; generated ASN.1, PHY, PRACH, HARQ, TA, RU/RF,
O-DU and flexible_o_du remain out of scope.

## CUCP-033 UE capability gating

Parse existing UE capability RAT container bytes and expose a CU-CP NTN
capability summary. UEs may complete signaling-only access while capability is
unknown, but UE-specific NTN digital service binding, connected handover target
preload/resource reservation, and release/paging recommendations require Rel-17
`nonTerrestrialNetwork-r17` support. SIB19 beam/cell broadcast remains driven by
beam state, not by individual UE capability.

## CUCP-034 UE capability profile policy

Extend CUCP-033 with Rel-17 NTN scenario profile matching. The v1 deployment
profile is fixed to `leo_ngso`: NGSO, both, and implicit-both UE capability
profiles may enter NTN digital service, connected handover target reservation,
and release/paging hints; GSO-only UEs remain signaling-only and are reported as
profile-blocked. `ntn-Parameters-r17` is recorded for observability only.

## CUCP-035 Versioned onboard position plan

Add a default-disabled management-center input for one satellite's complete
visible `G######` inventory, two explicit stable onboard NCI/PCI identities,
catalog/schedule versions, hashes, validity and activation epoch. Validate the
complete input, partition every L1 exactly once, dry-run the configurable
SSB/PRACH calendar, and atomically promote only a fully checked plan. This
profile is independent from the legacy per-beam NCI model and does not derive
NCI from a position, satellite ordinal or coordinates.

## CUCP-036 Access-calendar software execution gate

Deploy the checked two-cell calendar through the private F1AP resource-
coordination container and apply it as a default-off DU/MAC scheduler software
gate. Require matching version/hash and accepted intent counts at prepare and
query time, monotonic deployment feedback, and rollback that preserves the old
active plan. `applied` proves only that the matching software gate snapshot was
consumed; it is not position/port steering or PHY/RU/RF evidence.

## CUCP-037 Initial UL active-plan audit contract

Provide a CU-CP-private, side-effect-free auditor for complete proposed Initial
UL sideband metadata. It compares satellite/catalog/schedule/hash, stable
NCI/PCI, L1 owner, absolute occasion phase, paired PRACH/UL-beam window and
cell-local port against the current active plan. The result is
`accept/reject/audit_only` with a machine-readable reason and an explicit
no-RF-evidence label. No production F1AP transport carries this metadata yet;
standard Initial UL must not infer it from the legacy beam-to-NCI mapping.

## CUCP-038 Authoritative resource-audit snapshots

Close the fail-safe gap in the periodic DU resource audit. The previous one-
second audit could receive `accepted=true` with an empty snapshot and CU-CP
could misinterpret the absence of entries as missing DU state, producing false
repair actions. Version the private audit-result codec with independent
`rnti_snapshot_complete` and `ue_slot_snapshot_complete` flags; legacy v1
results decode as incomplete in both domains. MAC retains a per-cell lease
ledger across `pending -> consumed_by_mac -> expired`, enforces `expiry_ms`, and
DU returns that real RNTI snapshot with each lease's `generation_id`. CU-CP
accepts a distribution result only when its generation and complete lease set
match atomically. A missing or malformed result becomes `ack_unknown`; a fresh
ordinary `sent_to_du` pool waits, while an unknown ACK is repaired with the
same generation. An unresolved pool blocks low-water creation of a newer
generation. Matching `sent_to_du/pending` evidence promotes the pool to
`applied_by_du`; consumed/expired leases are reconciled only in a domain marked
complete. An explicit DU audit rejection reaches conflict accounting instead
of being discarded as transport failure. CU-CP never re-inserts consumed,
Initial-UL-seen, committed or expired leases; conflict-free low-water refill
may issue new leases only after prior delivery is conclusive.

MAC refreshes expiry before each atomic replace, preserves terminal history,
and treats an exact same-generation `add` as a no-op even after consumption or
expiry, without extending expiry or resurrecting state. Its single-lock
`allocate_for_cell` decision excludes all tracked NTN leases from terrestrial
allocation and protects an outstanding terrestrial TC-RNTI for a 10-second
RACH reservation guard. A C-RNTI value is unique across cells of one DU because
the DU table is flat; different DUs may reuse it. DU validates stable gNB-DU,
NCGI and PCI identity before applying a pool and verifies the returned snapshot
cell. SR/SRS repair records the DU's actual `applied_request`. The current
UE-slot domain remains incomplete until DU has a reliable mapping to CU-global
UE identity. Codec v2 requires same-version CU/DU rollout; only new CU decoding
of legacy v1 is supported. Terminal-history garbage collection and durable
C-RNTI reuse remain bounded-stage follow-up work. Snapshot lookup is indexed to
avoid quadratic comparison growth, but this is not endurance evidence. This is
software-state evidence, not RAR transmission, raw PRACH, trusted Initial UL
position or RF execution.

The same hardening pass rejects wrong-generation SIB19 results at F1 and keeps
an older CU completion from overwriting a newer record; update and clear also
require their exact expected result status. Production DU requests are FIFO
through full completion, but application-level stale-generation replay is not
yet rejected by a DU high-water mark and remains part of the anti-replay work.

## CUCP-048 Initial UL position consumer

Consume a private Initial UL position observation in CU-CP before RRC Setup
admission. Use one exact DU/cell/C-RNTI/connection-generation key, bounded
one-second storage, one-time consumption and PRACH event-time freshness. Keep the default `disabled` path,
provide non-blocking `audit`, and make `strict` fail startup without a ready
injected source. Match the active plan, stable cell owner, PRACH time, uplink
port and live DU generation before existing ownership and capacity checks.
Temporary accepted contexts are non-persistent and end at ICS, UE removal,
setup failure, plan activation, DU disconnect or restart. This stage is the
CU-CP consumer only; a production lower-layer producer is a separate task.

## Global sequencing rule

Do not begin a later task if it depends on terminology or runtime state that has
not been introduced by an earlier task. If a task needs a non-CU-CP path, update
the task card with an exact exception path and get human review before editing.
