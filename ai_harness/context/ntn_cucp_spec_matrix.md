# NTN CU-CP Spec Matrix

## Accepted CU-CP NTN schema

The accepted CU-CP-facing NTN configuration path remains:

- `mobility_config.ntn_location_mobility`

The accepted satellite state source values remain:

- `manual`
- `circular_orbit`
- `tle`

Future tasks must reuse this schema. Do not introduce a parallel `cu_cp.ntn`
schema unless a human-approved task explicitly requests a migration.

## Feature classification

| Area | Harness classification | Codex may implement? | Notes |
|---|---|---:|---|
| NTN configuration validation | CU-CP implements | Yes | Positive and negative tests required. |
| Satellite runtime state | CU-CP implements | Yes | Source, epoch, ECEF/PV, freshness, validity, source priority. |
| Multi-satellite catalog | CU-CP implements | Yes | Control-plane catalog only; no RF gateway switching. |
| Service area model | CU-CP implements | Yes | Earth-fixed, quasi-Earth-fixed, Earth-moving service area metadata. |
| Analog/digital beam hierarchy | CU-CP implements | Yes | Analog access beams are CU-CP eligibility clusters over digital service beams; no RF beamforming control. |
| Access/service DU assignment policy | CU-CP implements | Yes | CU-CP selects analog access DUs, prefers same-DU digital service demand, and gates new demand; no DU migration or scheduler change. |
| Analog/digital resource-domain guard policy | CU-CP implements | Yes | CU-CP models analog/digital caps, reuse ids, explicit conflict groups and blocking reasons for admission, service binding, placement and observability; no RF or MAC enforcement. |
| Candidate beam inventory | CU-CP implements | Yes | Digital service beam inventory must not be capped by loaded-beam resource limits. |
| Mobility eligible beam set | CU-CP implements | Yes | Used by measurement/location mobility target selection. |
| Loaded service calendar | CU-CP implements | Yes | Emits CU-CP digital service demand for beams with UE/DRB/HO demand only; SR/SRS execution requires DU-applied feedback in NTN service tasks. |
| Beam service resource manager | CU-CP implements | Yes | Centralizes analog access C-RNTI lease lifecycle, digital SR/SRS assignment state, DU feedback, rollback, and observability for NTN. |
| NTN C-RNTI lease pools | CU-CP implements with exact DU/MAC exception | Task-specific | CU-CP is authoritative in NTN mode; DU/MAC consume pre-distributed leases for RAR and report state. Terrestrial RNTI allocation remains unchanged. |
| NTN digital SR/SRS application feedback | CU-CP implements with exact F1AP/DU/Scheduler exception | Task-specific | CU-CP assigns NTN digital service SR/SRS, DU applies/rejects, and scheduler tests verify applied UE config. No PHY/RF execution claim. |
| NTN connected handover target resource reservation | CU-CP implements with exact F1AP/DU exception | Task-specific | Target C-RNTI and target SR/SRS must be ready before source RRC handover command; rollback preserves source service resources. |
| NTN resource consistency audit and repair | CU-CP implements with exact F1AP/DU exception | Task-specific | CU-CP actively queries DU NTN resource snapshots, compares RNTI lease and SR/SRS state against CU-CP authority, and executes conservative resend, clear, rollback, or conflict repair actions through existing control paths. |
| NTN SIB19 DU SI broadcast application | CU-CP implements with exact F1AP/DU/MAC exception | Task-specific | CU-CP sends active/candidate dynamic SIB19 payloads to DU through private F1AP resource coordination and clears draining/stale payloads; DU SI scheduling must already be configured. |
| NTN admission policy | CU-CP implements | Yes | Elevation, service window, location accuracy, PLMN/TAC/slice, capacity. |
| UE NTN capability gate | CU-CP implements | Yes | UE-specific NTN service, connected handover, and release/paging hints require parsed Rel-17 `nonTerrestrialNetwork-r17`; signaling-only access may continue while unknown. |
| UE NTN capability profile policy | CU-CP implements | Yes | For the fixed `leo_ngso` deployment, NGSO/both/implicit-both Rel-17 NTN UE profiles match; GSO-only UEs are supported but profile-blocked from UE-specific NTN service, connected HO, and release/paging hints. |
| Connected-mode NTN mobility | CU-CP implements | Yes | Candidate target, draining policy, retry and failure handling. |
| UE NTN runtime context | CU-CP implements | Yes | Location, accuracy, matched beam, pending HO, Mapped Cell/TAI. |
| Core-network location reporting | CU-CP implements | Yes | NGAP LocationReport, AMF control, throttling, area-of-interest. |
| Mapped Cell / derived TAC / TAI | CU-CP implements | Yes | Control-plane mapping from UE/beam/service area. |
| Service/feeder link switch-over events | CU-CP implements | Yes | Soft/hard event policy; no physical gateway implementation. |
| NTN observability commands | CU-CP implements | Yes | Runtime snapshots for satellite, assistance, beams, UEs, service-area TAC validity, and paging eligibility. |
| Security and resilience gates | CU-CP implements | Yes | Stale ephemeris, location accuracy, manual override, safe degradation. |
| RRC/SIB19 assistance construction | CU-CP contract plus task-scoped realization | Task-specific | CU-CP builds assistance packaging and, in CUCP-032, may apply dynamic SIB19 payloads to preconfigured DU SI slots through exact F1AP/DU/MAC exceptions. |
| F1AP-CU SR/SRS slot request | CU-CP contract plus task-scoped realization | Task-specific | CU-CP may send private NTN SR/SRS request/result containers under exact F1AP/DU exceptions; ordinary tasks remain contract-only. |
| E1AP / PDU-session QoS influence | CU-CP contract only | Task-specific | Only when directly needed by CU-CP UE/session policy. |
| Paging / idle / inactive assistance | CU-CP observability only | Limited | CU-CP may preserve context, beam-derived TAC release hints, paging narrowing, and service-area eligibility reasons; UE reselection is out of scope. |
| DU SI scheduling | Out of scope | No | Requires explicit non-CU-CP exception. |
| DU/MAC scheduler | Out of scope | No | Do not modify. |
| HARQ timing / Koffset execution | Out of scope | No | CU-CP may compute assistance values only. |
| TA scheduler long RTT behavior | Out of scope | No | Do not modify. |
| PRACH behavior | Out of scope | No | Do not modify. |
| PHY Doppler compensation | Out of scope | No | Do not modify. |
| RU/RF/radio driver beamforming | Out of scope | No | Do not modify. |
| O-DU / flexible_o_du | Out of scope | No | Do not modify. |
| GIS site behavior | Out of scope | No | Do not modify. |

## Default CU-CP NTN policy

- Configuration root: `mobility_config.ntn_location_mobility`.
- New access and HO admission elevation: `50 deg`.
- Loaded service beam limit: `max_nof_loaded_service_beams = 0` means no CU-CP cap.
- Default LEO beam hierarchy: `843` digital service beams, `137` analog access beams, `109` full clusters and `28` edge partial clusters.
- Default example active windows: `max_nof_active_analog_access_beams = 16`, `max_nof_loaded_digital_service_beams = 64`.
- Stale ephemeris: stop new access, keep existing UEs in draining when possible.
- NGAP reporting: AMF control enabled, local forwarding disabled by default.

## General rule

If a requested NTN behavior cannot be implemented within CU-CP scope, stop,
report the limitation, and propose the smallest exact non-CU-CP exception path
for human review. Do not modify DU/MAC/PHY/RU/RF/PRACH/HARQ/TA/GIS paths unless
the active task card explicitly grants exact non-CU-CP exception paths.
