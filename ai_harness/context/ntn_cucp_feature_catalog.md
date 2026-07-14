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
C-RNTI leases therefore use DU cell identity in addition to PCI. This profile
must not derive NCI from satellite id, cell ordinal, coordinates or position id.

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
application feedback, audit and repair state. Terrestrial allocation remains
unchanged.

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

CUCP-037 defines a private pure audit for complete proposed Initial UL sideband
metadata. It can match the active satellite/catalog/schedule/hash, stable cell
identity, L1 owner, PRACH occasion and paired UL port, returning
`accept/reject/audit_only`. Standard F1AP Initial UL does not carry this complete
metadata, so the auditor is not yet a production transport or admission hook.
It must not infer `position_id` from the legacy beam-to-NCI mapping, and an
accepted audit is not RF evidence.

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
