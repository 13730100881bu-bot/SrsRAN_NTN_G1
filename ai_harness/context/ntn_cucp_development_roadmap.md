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
no-RF-evidence label. At the CUCP-037 boundary no production transport carried
this metadata. CUCP-051 later adds a private F1 resource-coordination query;
standard Initial UL still must not infer it from the legacy beam-to-NCI mapping.

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
cell. SR/SRS repair records the DU's actual `applied_request`. CUCP-038 originally
left the UE-slot domain incomplete because its DU snapshot could not reliably
name a UE on the current F1 connection. CUCP-050 closes that gap with the pair
of current F1 UE IDs and an exact live-connection target. Codec v2 requires
same-version CU/DU rollout; only new CU decoding of legacy v1 is supported.
CUCP-049 completes safe C-RNTI retirement and reuse. Snapshot lookup is indexed
to avoid quadratic comparison growth. This scope covers software resource
state. RAR transmission, raw PRACH, trusted Initial UL position and RF execution
use their respective runtime interfaces. CUCP-051 later implements the Initial
UL receive-source interface; RAR and transmit/RF execution remain separate.

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
CU-CP consumer contract. CUCP-051 supplies its runtime lower-layer source in
production code.

## CUCP-049 Safe C-RNTI retirement and reuse

Completed. Retire an NTN access C-RNTI only after CU-CP has no live UE owner,
the current DU connection returns a complete snapshot with the same generation
in `expired`, and MAC atomically rechecks the whole retirement batch. Preserve
compact generation history so delayed messages cannot remove a newer reuse.
After reconnect or CU-CP restart, isolate unknown DU records and finish a fresh
audit before retirement or allocation resumes. Legacy DUs continue the original
pool flow without making uncertain retired numbers reusable. The allocator scans
the bounded per-DU namespace and publishes a new eight-number pool only when all
eight numbers are safe.

## CUCP-050 Complete UE SR/SRS audit and reconnect recovery

CUCP-050 assigns a nonzero assignment generation to every versioned SR/SRS
`set/clear` operation. DU stages the request, then publishes a new active
assignment only after resource allocation and the MAC/scheduler configuration
transaction both succeed. Its registry stores the actual SR/SRS parameters;
the audit response joins those parameters with the current C-RNTI and both
current F1 UE IDs. A complete per-cell snapshot contains at most 1,024 entries
and is bound to the exact gNB-DU, cell, audit generation and connection token.

CU-CP reapplies a missing expected assignment once with its original generation,
or clears a resolved DU-only assignment once with the next generation. The
one-attempt bound applies to the exact UE, operation, assignment generation and
resource content. Same-generation parameter differences, a newer DU generation,
duplicate identity or an unresolved F1 UE remain blocked or quarantined; no
automatic overwrite or UE release occurs. Disconnect invalidates the old token,
capability and applied state. Reconnect requires another complete snapshot
before the DU becomes `reconciled`.

RNTI retirement and UE-slot reconciliation are independent domains. A legacy DU
keeps prior feedback and RNTI behavior without enabling automatic UE-slot repair;
a new DU continues to answer the earlier request format for an older CU. No new
state file or public ASN.1 field is introduced, and NTN default off does not
start the audit. Build and test results and the scripted software-flow simulation are
recorded in `docs/ntn_ue_slot_audit_recovery.md`.

## CUCP-051 Verified Initial UL receive source

CUCP-051 connects the active access calendar to the runtime PRACH receive-port
metadata path. The optional upper-PHY result classifies each detected preamble as `unique`,
`ambiguous` or `unavailable` from the strongest and second-strongest receive
ports, using a configurable `6 dB` default margin. The combined detection
metric, detection threshold, preamble, TA and received-power result retain their
existing behavior. Scheduler, FAPI and MAC carry the exact schedule version,
extended calendar-cycle index and in-cycle opportunity offset selected for the
PRACH request.

One deployment-local, versioned receive mapping resolves `NCI + logical port`
to either an SDR/ZMQ physical receive port or an OFH PRACH eAxC and 15-bit
BeamId. SDR verification requires an exact physical-port match. OFH verification
also requires the declared RU capability, Type-3 C-plane BeamId and matching
U-plane eAxC/context. Calendar-only `software_attributed` records remain useful
for `audit` and are never accepted by `strict`. A measured port that fails its
mapping returns a specific failure instead of falling back to calendar-only
attribution.

MAC allocates the Initial UL C-RNTI together with its lease generation and
stores one bounded record. DU binds that record to the current DU UE F1 ID. A
private `NTPOSQ01/NTPOSR01` query, carried in the existing resource-coordination
container, echoes the exact gNB-DU/cell/PCI/DU UE/C-RNTI/generation target, a
connection token, query generation and nonce. The container is limited to
1 KiB, and CU-CP waits `50 ms` by default within the configured `10..200 ms`
range before applying `audit` or `strict` policy. Standard Initial UL ASN.1 is
unchanged.

DU and MAC stores each keep at most 1,024 records for one second. The same nonce
may retrieve the same result again; another nonce cannot consume it. Mapping
replacement and F1 connection loss invalidate stale records. An accepted RNTI
`replace/clear` invalidates the cell, `retire` invalidates only the accepted
RNTIs, and `add` preserves unrelated observations. Calendar application retains
the active schedule/hash; clear or rollback erases only its exact target.
CU-CP reconstructs PRACH time from the active plan epoch and calendar position,
then reuses the CUCP-048 plan/position/port authorizer. `disabled` remains the
default and issues no private query. Read-only status distinguishes overall and
per-cell receive state, backend, strict availability, mapping identity and
failure counters.

The implemented completion point covers focused cross-layer tests and two
scripted code-level simulations for the SDR/ZMQ-configured receive-port path and
the OFH BeamId/eAxC path. Those scenarios orchestrate GTest filters; they do not
inject live ZMQ IQ samples or exercise a physical RU, UHD, antenna switching or
over-the-air traffic. The next tasks are L1-to-L2 digital-service binding,
transmit-side device control, and live RU/UHD and antenna integration. See
`docs/ntn_initial_ul_receive_source.md`.

Closeout evidence passed 5/5 selected NTN mobility tests, 18/18 exact CU-CP
Initial UL tests and 3/3 configuration tests, and the `srsran_cu_cp` target
built successfully. The receive-port scenario exited zero with 33/33 tests and
no skips. The OFH scenario exited zero and passed with 45 matched tests: 37
passed, zero failed and eight platform-conditioned skips. Both scenarios used
code-level CTest orchestration without `-Build`; they are not live ZMQ IQ or
physical-RU runs. `git diff --check` passed and cleanup found zero residual
processes.

## Global sequencing rule

Do not begin a later task if it depends on terminology or runtime state that has
not been introduced by an earlier task. If a task needs a non-CU-CP path, update
the task card with an exact exception path and get human review before editing.
