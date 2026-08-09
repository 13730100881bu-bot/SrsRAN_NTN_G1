# NTN CU-CP Feature Catalog

This catalog is the source of truth for future CU-CP-only NTN tasks. It lists
the intended feature domains, the CU-CP responsibility, and the boundary that
must not be crossed.

## 1. Satellite and constellation state

CU-CP owns a control-plane satellite catalog. It may store source, epoch,
ECEF/PV state, freshness, source priority, service validity, and selected
satellite id. Supported sources are `manual`, `circular_orbit`, and `tle`.

CU-CP does not own RF gateway selection, physical feeder link switching, or PHY
Doppler compensation.

## 2. NTN service area and cells

CU-CP may model Earth-fixed, quasi-Earth-fixed, and Earth-moving service areas.
The service area maps beams and cells to NR CGI, Mapped Cell, derived TAC, TAI,
PLMN, slice, service window, and optional jurisdiction policy.

CU-CP does not implement UE-side cell reselection.

## 3. Analog access and digital service beam hierarchy

CU-CP may model analog access beams as deterministic groups over digital service
beams. Analog beams gate access, reestablishment, handover target eligibility,
and coarse paging hints. Digital beams remain the true NCI/TAC/service-calendar
and QoS grain.

The default single-satellite LEO profile uses a ring-1 hex cluster: one center
digital beam plus six neighbouring digital beams, clipped at the circular
footprint edge when needed.

UE ownership is explicitly split after CUCP-023. Analog access beams own a UE
only during access, reestablishment, paging coarse narrowing, and pre-service
relocation. After Initial Context Setup succeeds, the UE releases per-UE analog
ownership. If it has no PDU/DRB, it is `control_only` and occupies no digital
service beam. Digital service ownership begins only when PDU/DRB demand binds a
digital beam.

### 3a. Onboard two-cell position-plan profile

The independent, default-disabled onboard profile models exactly two stable NR
logical cells per satellite. Their opaque 36-bit NCI and PCI are supplied by
the management center and remain attached to the moving satellite cells. Earth-
fixed `G######` L1 positions carry geometry and schedule ownership only; they do
not permanently own NCI/PCI and must never be inserted into the legacy one-beam-
per-NCI repository.

The two onboard cells may reuse one PCI. Cell-scoped runtime identities such as
C-RNTI leases therefore use DU cell identity in addition to PCI. The current DU
RNTI table is flat, so a C-RNTI value must still be unique across cells of the
same DU; different DUs may reuse the value. This profile must not derive NCI
from satellite id, cell ordinal, coordinates or position id.

## 4. SIB19 and RRC assistance

CU-CP may build an NTN assistance snapshot for RRC/SIB19 packaging:
ephemeris, Common TA, Koffset, Kmac, UL sync validity, reference location,
t-Service, and neighbour satellite assistance.

CUCP-032 can send a private dynamic SIB19 payload to an existing DU SI path and
observe applied/rejected/cleared feedback. This does not make SIB19 UE-specific,
and it does not create a new PHY broadcast scheduler.

## 5. Candidate beam inventory

CU-CP computes all configured beams that are enabled, visible, above admission
elevation, in the service window, compatible with PLMN/TAC/slice policy, and
supported by at least one DU capability record.

Candidate inventory is not capped by loaded service resource limits.

For the onboard profile, `candidate_inventory` instead means the complete
management-center visible L1 input. It is retained before capacity validation:
257 positions remain observable and produce `schedule_overflow`; they are not
silently truncated to 256.

## 6. Loaded service calendar

CU-CP only allocates antenna slot and SR/SRS request intent to beams with UE,
DRB, or handover demand. Empty candidate beams remain candidates and consume no
slot request.

The calendar is a CU-CP contract, not DU scheduler behavior.

Do not confuse the legacy demand-driven `loaded_service_calendar` with the
onboard `access_calendar_intent`. The latter is a periodic network-access plan
for SSB/SIB/Paging/RAR coalescing and paired PRACH/UL-beam windows, even when no
UE has digital service demand. CUCP-036 can deploy its checked SSB/PRACH portion
as a default-off software gate, but that feedback is not position/port or RF
execution evidence.

Analog access intent and digital service intent are separate CU-CP snapshots.
Analog intent describes access ownership by analog beam and selected DU. Digital
intent describes loaded service calendar and SR/SRS request state by digital
beam. Neither snapshot claims that DU, RF, or antenna hardware has executed the
intent.

CUCP-024 through CUCP-031 centralize the UE-facing resource contract in a CU-CP
beam service resource manager. The manager owns and distributes NTN C-RNTI
lease pools, validates cell-scoped Initial UL ownership, releases per-UE analog
ownership after Initial Context Setup, and owns digital service SR/SRS intent,
application feedback, audit and repair state. With NTN inactive, the terrestrial
RNTI selection sequence and default outcome remain unchanged; the shared
synchronization overhead is not a performance-equivalence claim.

CUCP-038 makes periodic resource repair conditional on authoritative snapshot
completeness. The private audit-result codec v2 reports
`rnti_snapshot_complete` and `ue_slot_snapshot_complete` independently; a v1
result is accepted for compatibility but both domains fail safe to incomplete.
An empty complete snapshot means the domain is authoritatively empty, while an
empty incomplete snapshot must not trigger repair. MAC keeps the real per-cell
lease ledger as `pending`, `consumed_by_mac` or `expired` and enforces the
distributed `expiry_ms`; DU exposes this RNTI domain as complete and carries the
per-entry `generation_id`. A lease result is applied only when its generation
and full accepted/rejected set match the update. Missing, partial, duplicate or
contradictory results become `ack_unknown`; the complete audit then retries the
original generation. A normal in-flight pool waits for its ACK and blocks a new
low-water generation. A matching pending DU record promotes a CU lease from
`sent_to_du` to `applied_by_du`; stale generations, duplicates and unknown
RNTIs block that domain instead of triggering refill. Explicit audit rejection
also reaches the conflict path; a later accepted complete audit resolves its
generic target blocker. CUCP-038 originally left the UE-slot domain incomplete
because DU could not reliably identify every entry on the current F1 connection.
CUCP-050 completes that mapping with current F1 UE identities and an exact
connection target.

CU-CP reconciles `consumed_by_mac` to its consumed state and DU expiry to its
expired state. It may resend only an unused CU-CP lease missing from a complete
RNTI snapshot. It must not resurrect a lease already consumed, observed on
Initial UL, committed or expired; low-water replenishment creates new leases
instead. MAC refreshes expiry before validating a whole replace, retains
terminal history and accepts exact same-generation retries as no-op without
refreshing expiry or resurrecting consumed/expired state. Atomic
`allocate_for_cell` shares one interlock with terrestrial RACH. A terrestrial
TC-RNTI returned but not yet attached is recorded for 10 seconds so the first
concurrent NTN update can reject a collision; before NTN activation that record
does not change the terrestrial RNTI selection sequence. The same DU cannot
track one C-RNTI value in two cells; different DUs can. DU also rejects a pool
with the wrong gNB-DU, NCGI or PCI and rejects a complete snapshot for the wrong
cell. SR/SRS repair caches the DU-reported `applied_request`, not merely the
requested shape. CUCP-049 completes guarded C-RNTI retirement and reuse: the
current DU must report the same expired generation, MAC rechecks the whole batch
atomically, compact generation history blocks delayed messages, and reconnect
or restart requires another complete audit before reuse. Unknown pending or
consumed DU records remain isolated. A legacy DU can keep the original pool flow
but cannot make an uncertain retired number reusable.

CUCP-050 gives versioned SR/SRS `set/clear` operations a nonzero
`assignment_generation`. DU stages each request and publishes a new active
assignment only after resource allocation and the MAC/scheduler configuration
transaction both succeed. The DU registry stores the actual SR/SRS parameters;
the audit response joins them with the current C-RNTI and F1 UE identities. The
complete snapshot is bounded to 1,024 entries and identifies each UE with the current
`gnb_cu_ue_f1ap_id`, `gnb_du_ue_f1ap_id`, NCGI, PCI and C-RNTI. The request and
response also bind gNB-DU ID, cell, audit generation and a nonzero connection
token. Missing expected entries can be reapplied once with the same generation;
resolved DU-only entries can be cleared once with the next generation. This
one-attempt limit is keyed by the exact UE, operation, generation and resource
content. A parameter conflict, newer DU generation, duplicate identity or
unresolved F1 UE is blocked or quarantined rather than overwritten. Repair is
followed by a new complete snapshot before the DU returns to `reconciled`.

The RNTI and UE-slot domains are independent, so an incomplete SR/SRS snapshot
leaves a safe RNTI retirement on its own lifecycle. Old DUs retain the existing
applied feedback and RNTI audit without enabling automatic UE-slot repair. New
DUs continue to answer the earlier request format for old CUs. Disconnect
invalidates the connection token, in-flight result and DU-applied status; no
state file is added. Build, test and scripted software-flow results are recorded
in `docs/ntn_ue_slot_audit_recovery.md`.

Read-only status aggregates `matched`, `missing`, `conflict` and `quarantined`
from each target's latest complete snapshot. `repaired` counts successful repair
acknowledgements on the current DU connection and target set; reconnect or a
target-set change starts a new count.

Codec v2 is a same-version deployment boundary: a new CU can fail-safe decode
v1, but an old CU cannot decode v2. These changes are opt-in NTN resource
behavior.

## 7. Admission

CU-CP may gate UE setup, reestablishment, handover target preparation, and PDU
session admission using satellite freshness, service window, location accuracy,
beam eligibility, PLMN/TAC/slice, UE/DRB capacity, QoS/ARP, and emergency
priority.

UE NTN capability is a UE-specific hard gate after signaling-only access. CU-CP
may allow RRC setup and Initial Context Setup while capability is unknown, but
digital service binding, NTN connected handover, and NTN release/paging hints
require a parsed NR UE capability with Rel-17 `nonTerrestrialNetwork-r17`
present.

CUCP-034 adds Rel-17 scenario profile matching for the current `leo_ngso`
deployment. NGSO, both, or absent `ntn-ScenarioSupport-r17` with NTN support
(`implicit_both`) are compatible with LEO/NGSO. GSO-only UEs remain supported at
the base NTN capability level, but are profile-blocked from UE-specific NTN
service, connected handover, and release/paging hints in this deployment.
`ntn-Parameters-r17` is recorded for observability only in v1.

CUCP-037 defines a private audit for complete proposed Initial UL sideband
metadata. It matches the active satellite/catalog/schedule/hash, stable cell
identity, position owner, PRACH occasion and paired UL port.

CUCP-048 connects that audit to RRC Setup through an injected, thread-safe
observation provider. The provider uses the exact DU/cell/C-RNTI/connection-
generation key, a 1,024-record bound, a one-second lifetime and one-time
consumption. A PRACH event must not be in the future and must be less than one
second old. `audit` records outcomes without rejecting the UE. `strict` fails
CU-CP startup without a ready provider and rejects a mismatch before access
ownership is created. Temporary accepted contexts are cleared by ICS, UE
removal, plan activation, DU disconnect and restart. Standard F1AP Initial UL,
generated ASN.1 and lower-layer code are unchanged, and `position_id` is never
derived from legacy beam-to-NCI state.

## 7a. Resource-domain guard policy

CU-CP may model analog access and digital service resource domains with caps,
reuse group ids, explicit conflict groups, and deterministic blocking reasons.
These guards influence access admission, first digital service binding, loaded
service placement, handover preload, and OAM snapshots. A cap value of `0`
means unlimited. Candidate inventory is never removed by resource-domain caps.

Only explicit conflict groups are hard guards. Adjacent or sibling digital beams
without configured conflict groups may be active-loaded together. This is a
control-plane policy contract only; it is not RF frequency reuse, power control,
PRACH, or MAC scheduling behavior.

## 8. Connected mobility

CU-CP may trigger and validate NTN mobility using UE location, measurement
reports, time-to-trigger, report gaps, beam boundary hysteresis, service window,
and draining state. Candidate beams may be handover targets. Draining beams
must not accept new UEs.

## 9. NGAP and core-network mapping

CU-CP may generate NR CGI, Mapped Cell, derived TAC, TAI, timestamped user
location, area-of-interest reports, change-of-serving-cell reports, and
LocationReportingControl responses.

## 10. PDU session and QoS influence

CU-CP may use UE/DRB count, QoS, ARP, slice, and emergency priority to order
loaded beams, choose draining policy, and reject lower-priority new demand.
E1AP changes are task-specific and must stay CU-CP-side.

## 11. Switch-over and resilience

CU-CP may process soft and hard service/feeder switch-over events. Soft events
prepare migration and reduce new admission. Hard events stop new admission and
trigger draining, handover, or release for affected beams.

## 12. Observability and OAM

CU-CP should expose `ntn_state`, `ntn_assistance`, `ntn_beams`, and `ntn_ues`
style snapshots showing state, validity, reasons, UE/DRB load, and control-plane
contracts.

## 13. Strict exclusions

Future work is CU-CP-only unless a task grants exact non-CU-CP paths. Completed
task-scoped exceptions such as CUCP-036 do not authorize further DU/MAC changes.
HARQ timing, TA scheduler, raw PRACH detection, PHY/lower PHY, RU/RF/radio
drivers, ZMQ channel behavior, O-DU/flexible_o_du behavior, generated ASN.1 and
GIS-site behavior remain excluded without explicit authorization.

Resource-audit completeness covers CU-CP and DU/MAC software state. RAR
transmission, raw PRACH detection, authenticated Initial UL position metadata
and PHY/RU/RF telemetry remain separate work. CUCP-050 binds its UE-slot query
to a current local connection token and current F1 UE identities; transport
sender authentication and end-to-end freshness/anti-replay remain outside this
task.
