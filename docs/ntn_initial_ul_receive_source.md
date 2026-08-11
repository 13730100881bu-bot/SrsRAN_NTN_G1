# NTN Initial UL Receive Source and Cross-Layer Flow

Last updated: 2026-08-11

Task: `CUCP-051`

Implementation conclusion: CUCP-051 connects a detected PRACH opportunity to
one current onboard L1 position using the detector-reported physical receive-
port identifier or the complete OFH PRACH BeamId/eAxC context, binds that result
to the allocated C-RNTI generation, retrieves the one-shot record through a
bounded private F1 query, and feeds the existing CU-CP Initial UL position
authorizer before RRC Setup admission.

The feature remains opt-in. With the receive mapping and CU-CP position check
disabled, the existing terrestrial PRACH decision, C-RNTI allocation order and
Initial UL processing path remain in use.

## 1. End-to-end flow

```text
active access calendar
  -> scheduled PRACH opportunity and calendar position
  -> upper-PHY PRACH detection and receive-port classification
  -> SDR physical-port or OFH BeamId/eAxC verification
  -> MAC C-RNTI allocation with lease generation
  -> one-shot MAC/DU observation
  -> private F1 NTPOSQ01/NTPOSR01 query
  -> CU-CP active-plan and runtime-route checks
  -> disabled/audit/strict RRC Setup decision
```

The position identity comes from the active calendar entry selected through the
runtime receive-port metadata. The implementation does not derive `position_id`
from the PRACH preamble, frequency-domain occasion, timing advance, coordinates,
NCI or a legacy beam identifier.

## 2. PRACH receive-port attribution

Receive-port attribution is an optional extension of the existing PRACH
detection output. For every detected preamble, the upper PHY can report:

- `unique`: the strongest port exceeds the second strongest port by at least
  the configured margin;
- `ambiguous`: more than one port remains plausible at that margin;
- `unavailable`: attribution was disabled or the per-port measurements cannot
  be used.

The default unique margin is `6 dB` and the configured range is `0..60 dB`.
For a single valid receive port, the exported finite margin is `120 dB`, which
fits the private F1 representation. The attribution calculation reuses the
per-port PRACH correlation power while preserving the original combined-port
detection metric, detection threshold, detected preamble, timing advance and
received-power result. When attribution is disabled, the new fields retain
their default `unavailable` values.

The scheduler attaches the exact schedule version, extended calendar-cycle
index and in-cycle PRACH offset to an authorized opportunity. These values are
echoed through the internal FAPI and MAC RACH structures. The extended cycle
index remains monotonic across the ordinary 10.24-second SFN wrap, so a later
consumer does not reconstruct the calendar position from the wrapped SFN.

## 3. Receive mapping and source authority

The DU high configuration provides one deployment-local mapping identified by
`version` and `hash`. Each entry is keyed by `NCI + cell_local_port` and selects
one backend:

| Backend | Required mapping | Accepted authority |
|---|---|---|
| SDR/ZMQ | one `physical_rx_port` | `sdr_rx_port_verified` |
| OFH | one `physical_rx_port`, PRACH `eAxC` and 15-bit `BeamId` | `ofh_beam_id_verified` |

The mapping is independent of the management-center plan and calendar
versions. Runtime validation requires:

- a nonzero mapping version and a non-empty hash of at most 128 bytes;
- at most 1,024 entries and at most 16 entries for one NCI;
- unique logical port and physical receive port within one NCI;
- cell-local logical port `0..65534`;
- physical receive port `0..254`;
- for OFH, unique PRACH eAxC `0..31` and BeamId `0..32767` within one NCI;
- no eAxC or BeamId fields on an SDR entry.

For SDR/ZMQ, a `unique` detector result must map its physical receive port to
exactly one calendar UL-beam intent. For OFH, the same match also requires the
RU capability declaration, Type-3 C-plane BeamId, matching PRACH U-plane eAxC,
logical port, position, schedule/calendar identity and mapping identity.

`software_attributed` is available only when the detector reports no physical
port and the active calendar has one unique candidate. It is accepted in
`audit` mode and rejected by `strict`. If the detector provides a unique
physical port but the configured mapping, OFH capability, eAxC or BeamId does
not match, the record carries `rx_mapping_mismatch` or
`ofh_beam_capability_unavailable`; the implementation does not replace that
failed device check with calendar-only attribution.

## 4. C-RNTI generation binding

MAC uses `allocate_for_cell_with_generation()` for contention-based Initial
UL. In NTN lease mode it returns the selected C-RNTI and the current nonzero
lease generation as one atomic result. The original `allocate_for_cell()` API
remains available and returns the same C-RNTI sequence. The terrestrial path
continues to return generation zero and keeps its existing allocation order.
Zero and `UINT32_MAX` are invalid as authoritative NTN generations; exhausted
generation space stops the corresponding new NTN allocation.

The Initial UL record includes:

- cell, NCI/PCI, C-RNTI and lease generation;
- unique `observation_id`;
- schedule version, calendar hash, cycle index and PRACH offset;
- `position_id` and cell-local UL port;
- source authority, physical receive port and confidence margin;
- mapping version/hash and, for OFH, the PRACH eAxC and BeamId.

An accepted RNTI `replace` or `clear` invalidates every DU observation for the
cell. An accepted `retire` invalidates only the accepted C-RNTIs, while `add`
preserves unrelated generation-bound observations. MAC also removes an older
correlation if a new PRACH reuses the same cell/C-RNTI with a different
generation. Reuse of one C-RNTI therefore requires the newer generation to
match at MAC, DU and CU-CP.

## 5. Bounded one-shot storage

MAC correlates the detected PRACH with the later Msg3 UL-CCCH and keeps at most
1,024 pending records for one second. It creates no record when no NTN calendar
is installed. DU then stores the record under the current
`gnb_du_ue_f1ap_id`, also with a 1,024-record and one-second bound.

DU accepts no replacement for an existing F1 UE key or duplicate
`observation_id`. The first exact query consumes the record. A retry with the
same nonce and complete target returns the same result; another nonce receives
`observation_consumed`. Expiry and UE deletion remove the affected record, and
F1 connection loss clears all records tied to that connection. Calendar apply
retains only the accepted active schedule/hash; clear or rollback removes only
the exact schedule/hash being cleared. The RNTI operation rules above preserve
unrelated observations. These records are not persisted.

## 6. Private F1 query

The standard `InitialULRRCMessageTransfer` remains unchanged. Before processing
that message into the ordinary RRC creation flow, F1AP CU can issue a private
`GNB-DU Resource Coordination` request:

- query magic: `NTPOSQ01`;
- result magic: `NTPOSR01`;
- maximum private container size: `1 KiB`;
- default timeout: `50 ms`, configurable within `10..200 ms`.

The query and result both carry and echo:

- query generation and nonzero nonce;
- current CU-assigned F1 connection token;
- gNB-DU ID, NCGI, DU cell index and PCI;
- current `gnb_du_ue_f1ap_id`;
- C-RNTI and expected lease generation.

The decoder rejects oversize, truncated, trailing, malformed or identity-
mismatched containers. F1 connection stop cancels in-flight queries, and a
late result from an old token cannot update the new connection state. Only one
query may be active for one DU UE identity on the current connection. CU-CP
performs connection-loss invalidation synchronously before queued DU repository
removal; the idempotent fallback removal cannot revive a cancelled query or
resume RRC creation.

CU-CP converts an accepted F1 result into the existing observation contract.
It derives the PRACH event time from the active plan activation epoch,
calendar-cycle index and in-cycle offset instead of accepting a DU wall-clock
timestamp. It then performs the existing plan, position owner, NCI/PCI, live DU
route, schedule/hash, PRACH window and logical-port checks.

## 7. Admission modes

| Mode | Private query and admission behavior |
|---|---|
| `disabled` | No private position query. The existing Initial UL path runs directly. This is the default. |
| `audit` | Query and check when available, record the outcome, and continue ordinary admission for every result. `software_attributed` is allowed for this purpose. |
| `strict` | Require a generation-bound `sdr_rx_port_verified` or `ofh_beam_id_verified` observation and every CU-CP check. A failure rejects RRC Setup before access ownership is written. |

Enabling `audit` or `strict` requires onboard position-plan DU execution. At
startup, `strict` also requires an observation provider that declares C-RNTI
generation authority and device-verification capability. The runtime private-F1
source provides that interface. Runtime `strict` availability remains
cell-specific and becomes ready only after the current active calendar, live DU
route and a detector-reported SDR port or complete OFH software-chain
observation agree.

The source state for each DU cell follows:

```text
disabled -> awaiting_calendar -> awaiting_rx_backend -> ready -> stale
```

A plan activation or mapping change invalidates prior readiness and observations
whose schedule/hash or mapping identity is no longer current. DU disconnect or
connection-token change invalidates the connection-bound records. A reconnect
starts with a new token and must produce a current observation. An empty
assigned-position set is a deny-all baseline for `strict`. RRC reestablishment,
resume and handover keep their existing admission paths.

## 8. Read-only status

The existing `ntn_state` command reports both the CU-CP consumer state and the
receive-source state:

- validation mode, provider/query-interface state and pending observations;
- overall `initial_access_rx_beam_state`, backend and strict availability;
- per-cell `initial_access_rx_state`, backend and strict availability;
- overall and per-cell receive mapping version/hash;
- process-cumulative software, SDR and OFH observation counts;
- process-cumulative ambiguous, no-port, query-timeout and generation-mismatch
  counts;
- the latest machine-readable reason.

Current source state, backend and mapping identity project only connected DUs.
The cumulative counters may retain history from a disconnected DU until process
restart.

Stable receive/query reasons include `receive_port_unavailable`,
`ambiguous_receive_position`, `rnti_generation_mismatch`,
`observation_query_timeout`, `observation_query_unsupported`,
`stale_connection_observation`, `rx_mapping_mismatch` and
`ofh_beam_capability_unavailable`.

## 9. Compatibility and execution boundary

CUCP-051 changes task-authorized internal PRACH result, FAPI, MAC, DU, private
F1, CU-CP and OFH paths. It adds no standard F1AP IE and changes no generated
ASN.1. It also leaves the normal PRACH detection decision, preamble selection,
TA, HARQ, ordinary scheduler policy and downlink beam control unchanged.

An older CU does not issue the private query. If a current CU receives no
supported private result from an older DU, `audit` records the failure and
continues, while `strict` rejects that access attempt. No capability is guessed
from a legacy response.

The implemented acceptance boundary is focused cross-layer tests and scripted
code-level simulations of the SDR/ZMQ-configured receive-port and OFH
BeamId/eAxC paths. The scenarios orchestrate GTest filters; they do not inject
live ZMQ IQ samples or exercise a physical RU, UHD, antenna switching or over-
the-air traffic. `ofh_beam_id_verified` therefore records a complete software-
chain context, not vendor RU telemetry or proof of physical BeamId execution.
Live UHD multi-channel calibration, vendor RU Type-3 BeamId interoperability,
antenna switching and over-the-air acceptance remain hardware-integration work
after digital L2 service binding and transmit-side device control.

## 10. Main code areas

| Function | Main paths |
|---|---|
| PRACH port classification | `include/srsran/phy/upper/channel_processors/prach_detection_result.h`, `lib/phy/upper/channel_processors/prach_detector_generic_impl.*` |
| Calendar identity through FAPI/MAC | `include/srsran/fapi/messages/`, `lib/fapi_adaptor/`, `include/srsran/mac/mac_cell_rach_handler.h`, `lib/scheduler/ntn_access_calendar_gate.h` |
| Receive mapping and one-shot MAC correlation | `include/srsran/mac/mac_ntn_initial_ul_position.h`, `lib/mac/mac_ntn_initial_ul_position_manager.h` |
| C-RNTI generation allocation | `lib/mac/rnti_manager.h`, `lib/mac/mac_sched/mac_rach_handler.cpp` |
| OFH BeamId/eAxC context | `include/srsran/ran/prach/verified_prach_rx_context.h`, `include/srsran/ofh/`, `lib/ofh/`, `lib/ru/ofh/` |
| DU observation store | `lib/du/du_high/du_manager/du_ue/du_ntn_initial_ul_position_store.h` |
| Private F1 codec and procedure | `include/srsran/f1ap/ntn_initial_ul_position_query.h`, `lib/f1ap/cu_cp/`, `lib/f1ap/du/` |
| CU-CP query, policy and status | `lib/cu_cp/cu_cp_impl.*`, `lib/cu_cp/ntn_mobility/ntn_initial_ul_position_authorizer.*`, `apps/units/o_cu_cp/cu_cp/` |
| DU/OFH configuration | `apps/units/flexible_o_du/o_du_high/du_high/`, `apps/units/flexible_o_du/split_7_2/`, `apps/units/flexible_o_du/split_8/` |

## 11. Validation record

The completed rows below record the focused CUCP-051 closeout evidence.

| Validation area | Target or scenario | Result |
|---|---|---|
| PRACH port attribution and disabled regression | `prach_detector_port_attribution_test`, `uplink_processor_test` | PASS: detector 5/5 and upper PHY 5/5, 10/10 selected tests |
| Scheduler calendar and internal FAPI propagation | `common_scheduler_test`, `mac_fapi_prach_adaptor_test`, `fapi_phy_prach_adaptor_test`, `phy_to_fapi_results_event_fastpath_translator_test`, `fapi_to_mac_data_msg_fastpath_translator_test` | PASS: scheduler 5/5 plus FAPI 2/2 + 1/1 + 2/2 + 7/7, 17/17 selected tests |
| C-RNTI generation and MAC record lifecycle | `mac_test` focused filters | PASS: 20/20 selected tests |
| OFH BeamId/eAxC software chain | focused C-plane, U-plane, notifier, RU-adapter and writer tests; `srsran_ru_ofh` | PASS: first group 8/8, U-plane group 8 passed/8 skipped, remaining groups 28/28; total 44 passed and 8 platform-conditioned skips; `srsran_ru_ofh` built |
| DU mapping, one-shot store and query lifecycle | `du_high_unit_config_test`, `ue_manager_test`, `du_manager_procedure_test`, `du_ue_config_test` | PASS: 17/17 selected config/store/query tests; `du_ue_config_test` built |
| Private F1 query codec and connection guards | `f1ap_cu_test`, `f1ap_du_test` focused filters | PASS: F1 CU 18/18 and F1 DU 6/6, 24/24 selected tests |
| CU-CP policy, lifecycle and status | `ntn_mobility_test`, `cu_cp_test`, `cu_cp_unit_config_test`, `srsran_cu_cp` | PASS: 5/5 selected NTN mobility tests, 18/18 exact CU-CP Initial UL tests and 3/3 configuration tests; `srsran_cu_cp` built |
| SDR/ZMQ receive-port code-level simulation | `ntn_initial_ul_rx_port_sim` | PASS: exit 0, 33/33 passed, no skips; code-level CTest without `-Build`, not live ZMQ IQ |
| OFH BeamId/eAxC code-level simulation | `ntn_initial_ul_ofh_beam_sim` | PASS: exit 0 and scenario passed; 45 matched, 37 passed, 0 failed and 8 platform-conditioned skips; code-level CTest without `-Build`, not physical RU hardware |
| Text and process cleanup | `git diff --check` and residual-process check | PASS: `git diff --check`; 0 residual processes |

## 12. Next tasks

The next implementation order is:

1. bind trusted L1 access position to L2 digital-service selection;
2. connect transmit beam ports to the device control interface;
3. run live RU/UHD, antenna and over-the-air integration.
