# CU-CP NTN Code Map

## 1. Summary

This repository now has a reviewed CU-CP NTN baseline at `ai/cucp-accepted-base` and a future-task harness base at
`ai/cucp-harness-base`.

Future NTN work should treat `lib/cu_cp/`, `include/srsran/cu_cp/`, and `tests/unittests/cu_cp/` as the primary edit
surface. A small set of non-CU-CP paths has already been accepted as narrow control-plane exceptions because the accepted
CU-CP code depends on them for configuration, F1AP-CU contracts, NGAP reporting, RRC callbacks, and orbit/beam-table
support.

Do not use this map as permission to reopen quarantined DU, MAC, PHY, RU/RF, ZMQ, GIS, or broad shared-NTN work. Any new
path outside `ai_harness/context/allowed_paths.md` needs explicit task-level approval.

## Accepted CU-CP NTN schema after CUCP-002

CUCP-002 confirmed that this repository does not use a new `cu_cp.ntn.orbit_type`
or `cu_cp.ntn.sib19_enabled` schema.

The accepted CU-CP-facing NTN configuration path is:

- `mobility_config.ntn_location_mobility`

The accepted satellite state source values are:

- `manual`
- `circular_orbit`
- `tle`

GEO-like behavior is represented through the `circular_orbit` source rather than
a dedicated `orbit_type: geo` field.

Future Codex tasks must reuse this schema and must not introduce a parallel
`cu_cp.ntn` schema unless a human-approved task explicitly requests a schema
migration.

## 2. Confirmed CU-CP Paths

- `include/srsran/cu_cp/`
  Public CU-CP interfaces, configuration structures, command handlers, metrics hooks, UE messages, and operation
  controllers.

- `lib/cu_cp/`
  CU-CP implementation library. Important subareas for NTN work:
  - `lib/cu_cp/cu_cp_impl.cpp`, `lib/cu_cp/cu_cp_impl.h`, `lib/cu_cp/cu_cp_impl_interface.h`
  - `lib/cu_cp/cu_cp_controller/`
  - `lib/cu_cp/cell_meas_manager/`
  - `lib/cu_cp/mobility_manager/`
  - `lib/cu_cp/neighbor_cell_manager/`
  - `lib/cu_cp/ntn_mobility/`
  - `lib/cu_cp/routines/`
  - `lib/cu_cp/ue_manager/`
  - `lib/cu_cp/adapters/`

- `lib/cu_cp/CMakeLists.txt`
  Links CU-CP with `srsran_cu_cp_ntn_mobility`, F1AP-CU, NGAP, RRC, E1AP-CU-CP, and other CU-CP support libraries.

- `lib/cu_cp/ntn_mobility/CMakeLists.txt`
  Defines `srsran_cu_cp_ntn_mobility` and links it to `srsran_ntn`, `srsran_ran`, `srslog`, and `srsran_support`.

## 3. Approved Control-Plane Exception Paths

These paths are outside strict CU-CP directories but were accepted during review because the current CU-CP NTN baseline
uses them.

- `apps/units/o_cu_cp/cu_cp/*` exact files listed in `allowed_paths.md`
  o_cu_cp configuration structs, CLI schema, translators, validation, YAML output, and command wiring.

- `apps/units/o_cu_cp/o_cu_cp_builder.cpp`
  Application construction glue that passes CU-CP configuration into CU-CP runtime objects.

- `configs/CMakeLists.txt`, `configs/cu_cp_neighbor_cells_example.json`, `configs/leo_500km_beam_table.json`,
  `configs/leo_500km_cucp_ntn.yml`
  Example/config inputs for the accepted CU-CP NTN profile.

- `include/srsran/f1ap/cu_cp/f1ap_cu_ue_context_update.h`
  CU-side F1AP UE context setup/modification structures that carry the optional NTN UL slot request.

- `include/srsran/f1ap/ntn_ul_slot_resource_request.h`
  Shared control-plane payload helper for SR/SRS slot offset and period requests. This is accepted as a narrow interface
  dependency, not permission to modify F1AP-DU implementation.

- `lib/f1ap/cu_cp/procedures/ue_context_setup_procedure.cpp`,
  `lib/f1ap/cu_cp/procedures/ue_context_modification_procedure.cpp`
  CU-side F1AP procedure encoding paths for the accepted NTN slot request.

- `include/srsran/ngap/ngap.h`, `include/srsran/ngap/ngap_location_reporting.h`,
  `lib/ngap/ngap_asn1_converters.h`, `lib/ngap/ngap_impl.cpp`, `lib/ngap/ngap_impl.h`
  NGAP control-plane location reporting hooks used by CU-CP NTN location reporting.

- `include/srsran/rrc/rrc_ue.h`, `lib/rrc/ue/rrc_ue_impl.h`, `lib/rrc/ue/rrc_ue_message_handlers.cpp`
  RRC UE callback path for NTN UE location reporting into CU-CP.

- `include/srsran/ntn/beam_hopping_table.h`, `lib/ntn/beam_hopping_table.cpp`,
  `include/srsran/ntn/orbit_propagator.h`, `lib/ntn/orbit_propagator.cpp`, `lib/ntn/CMakeLists.txt`
  Minimal shared NTN support needed by the accepted CU-CP satellite updater and beam-selection code.

- `tests/unittests/apps/units/o_cu_cp/`, `tests/unittests/apps/units/CMakeLists.txt`
  o_cu_cp configuration tests and their test-suite entry point.

- `tests/unittests/f1ap/common/f1ap_asn1_helpers_test.cpp`, `tests/unittests/f1ap/cu_cp/*` exact accepted files
  F1AP-CU test coverage for accepted CU-side slot request transport.

- `tests/unittests/ngap/ngap_ue_context_management_procedure_test.cpp`, `tests/unittests/ngap/test_helpers.h`
  NGAP location-reporting and helper coverage used by CU-CP NTN reporting tests.

- `utils/ntn/generate_leo_beam_table.py`
  Utility for generating beam-table configuration consumed by CU-CP examples.

## 4. Candidate Paths Needing Human Review

- `apps/units/o_cu_cp/` outside exact files in `allowed_paths.md`
  Candidate only when a task explicitly needs app-level CU-CP config or metrics glue.

- `lib/f1ap/cu_cp/` outside exact accepted files
  Candidate only for CU-side F1AP contracts. Do not include `lib/f1ap/du/`.

- `lib/ngap/` outside exact accepted files
  Candidate only for NGAP control-plane behavior requested by a CU-CP task.

- `lib/rrc/ue/` outside exact accepted files
  Candidate only for RRC UE control-plane callbacks or message handling needed by CU-CP.

- `include/srsran/ntn/` and `lib/ntn/` outside orbit propagator and beam hopping table exact files
  Candidate only when a CU-CP task proves a minimal shared NTN dependency is required. Broad shared NTN feature work
  stays quarantined.

- `tests/unittests/f1ap/common/`, `tests/unittests/f1ap/cu_cp/`, `tests/unittests/ngap/`
  Candidate only for tests that directly validate accepted CU-CP-adjacent contracts.

## 5. CU-CP Test Paths

- `tests/unittests/cu_cp/CMakeLists.txt`
  Main CU-CP test entry. The accepted baseline adds `cu_cp_ntn_mobility_test.cpp` to `cu_cp_test`.

- `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
  End-to-end-ish CU-CP NTN mobility/control-plane test coverage, including beam status, F1AP slot request handling,
  location reports, and NGAP reporting behavior.

- `tests/unittests/cu_cp/cell_meas_manager/`
  Measurement manager and NTN beam table/location controller tests and helpers.

- `tests/unittests/cu_cp/neighbor_cell_manager/`
  Neighbor-cell manager tests.

- `tests/unittests/cu_cp/ntn_mobility/`
  Focused unit tests for beam placement planner, satellite state updater, and served beam scheduler.

- `tests/unittests/cu_cp/du_processor/`
  CU-CP DU-processor tests. The path name contains `du_processor`, but it is under CU-CP tests and should not be
  confused with DU-high implementation.

- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`
  Accepted o_cu_cp configuration test surface.

## 6. RRC / SIB / System Information Paths

- `include/srsran/rrc/rrc_ue.h`
  Accepted RRC UE interface hook for NTN UE location reporting.

- `lib/rrc/ue/rrc_ue_impl.h`, `lib/rrc/ue/rrc_ue_message_handlers.cpp`
  Accepted RRC UE implementation/message handling path.

- `lib/rrc/ue/procedures/rrc_reconfiguration_procedure.*`
  Candidate only if a future CU-CP task needs RRC reconfiguration behavior and explicitly approves the path.

- `include/srsran/rrc/rrc_cell_context.h`, `include/srsran/rrc/rrc_config.h`, `include/srsran/rrc/rrc_du.h`
  RRC cell/DU-facing configuration surface. Treat as candidate review paths, not default allowed paths.

- SIB19/system information scheduling is not owned solely by CU-CP. If a task crosses into DU scheduling or RRC DU
  delivery behavior, stop and request an explicit exception.

## 7. F1AP-CU Paths

- `include/srsran/f1ap/cu_cp/`
  F1AP-CU public interfaces. Exact accepted NTN touchpoint: `f1ap_cu_ue_context_update.h`.

- `include/srsran/f1ap/ntn_ul_slot_resource_request.h`
  Accepted shared helper for `SRSNTN02` vendor payload encoding/decoding of SR/SRS slot offset and period requests.

- `lib/f1ap/cu_cp/procedures/ue_context_setup_procedure.cpp`
  Accepted CU-side setup transport of the optional NTN slot request.

- `lib/f1ap/cu_cp/procedures/ue_context_modification_procedure.cpp`
  Accepted CU-side modification transport of set/clear NTN slot requests.

- `tests/unittests/f1ap/cu_cp/`
  CU-side procedure tests. Use exact accepted files by default.

- Forbidden counterpart: `include/srsran/f1ap/du/`, `lib/f1ap/du/`, `tests/unittests/f1ap/du/`.

## 8. NGAP Paths

- `include/srsran/ngap/ngap.h`
  Accepted public NGAP interface extension for location-reporting control/reporting.

- `include/srsran/ngap/ngap_location_reporting.h`
  Accepted CU-CP-facing NGAP location reporting types.

- `lib/ngap/ngap_impl.cpp`, `lib/ngap/ngap_impl.h`, `lib/ngap/ngap_asn1_converters.h`
  Accepted NGAP implementation/conversion paths for location reporting.

- `tests/unittests/ngap/ngap_ue_context_management_procedure_test.cpp`, `tests/unittests/ngap/test_helpers.h`
  Accepted NGAP test/helper changes.

- Other `lib/ngap/` files are candidate paths only when a task explicitly needs NGAP control-plane behavior.

## 9. E1 Paths

- `include/srsran/e1ap/cu_cp/`
  CU-CP side E1AP public contracts.

- `lib/e1ap/cu_cp/`
  CU-CP side E1AP implementation and bearer context procedures.

- `lib/cu_cp/adapters/e1ap_adapters.h`, `lib/cu_cp/cu_up_processor/`, `lib/cu_cp/routines/pdu_session_*`
  CU-CP integration surfaces for CU-UP/E1-related UE context and PDU-session behavior.

The accepted NTN baseline does not currently require E1AP changes. Treat future E1AP edits as task-specific candidates
requiring clear justification.

## 10. UE Context / Mobility / Measurement Paths

- `lib/cu_cp/ue_manager/`, `include/srsran/cu_cp/cu_cp_ue_messages.h`, `include/srsran/cu_cp/cu_cp_types.h`
  UE registry, UE context state, and CU-CP UE-facing message types.

- `lib/cu_cp/cell_meas_manager/`, `include/srsran/cu_cp/cell_meas_manager_config.h`
  Measurement configuration, static NTN beam table parsing, UE location ingestion, and NTN location mobility controller.

- `lib/cu_cp/mobility_manager/`, `include/srsran/cu_cp/mobility_manager_config.h`
  Mobility manager policy and handover trigger handling.

- `lib/cu_cp/neighbor_cell_manager/`, `include/srsran/cu_cp/neighbor_cell_manager_config.h`
  Neighbor-cell relationship support used by measurement/mobility logic.

- `lib/cu_cp/routines/mobility/`
  Intra-CU and inter-CU handover routines. Edit only when a CU-CP mobility task requires it.

- `lib/cu_cp/routines/initial_context_setup_routine.*`
  Accepted UE setup path that can carry NTN F1AP slot requests.

- `lib/cu_cp/routines/ue_batch_release_routine.*`
  Accepted administrative UE release routine.

## 11. CU-CP Satellite Updater Paths

- `include/srsran/cu_cp/cell_meas_manager_config.h`
  Defines `ntn_satellite_state_update_config` and `ntn_location_mobility_config`.

- `include/srsran/cu_cp/cu_cp_command_handler.h`
  Defines `cu_cp_ntn_command_handler`, including satellite state update and beam status commands.

- `lib/cu_cp/ntn_mobility/ntn_satellite_state_updater.h`
- `lib/cu_cp/ntn_mobility/ntn_satellite_state_updater.cpp`
  Creates circular-orbit/TLE propagators and periodically pushes satellite ECEF state into CU-CP.

- `lib/cu_cp/ntn_mobility/ntn_served_beam_scheduler.*`
  Selects visible/active/candidate/draining served beams from satellite state and configured capacity.

- `lib/cu_cp/ntn_mobility/ntn_beam_placement_planner.*`
  Maps selected beams to DUs and placement states.

- `lib/cu_cp/ntn_mobility/ntn_beam_assignment_repository.*`
  Stores current beam assignment state.

- `lib/cu_cp/ntn_mobility/ntn_served_beam_selector.*`
  Selection helper for served beam decisions.

## 12. orbit_propagator / beam_hopping_table Dependency Notes

- `include/srsran/ntn/orbit_propagator.h`, `lib/ntn/orbit_propagator.cpp`
  Accepted because `ntn_satellite_state_updater` includes and constructs `srs_ntn::circular_orbit_propagator` and
  `srs_ntn::tle_orbit_propagator`.

- `include/srsran/ntn/beam_hopping_table.h`, `lib/ntn/beam_hopping_table.cpp`
  Accepted because orbit/beam scheduling types use `active_beam_set_t` and beam-position grid helpers.

- `lib/ntn/CMakeLists.txt`
  Accepted so `srsran_ntn` builds the accepted orbit propagator implementation.

- `include/srsran/ntn/beam_hopping_controller.h`, `lib/ntn/beam_hopping_controller.cpp`,
  `include/srsran/ntn/ntn_configuration_manager.h`, `lib/ntn/beam_hopping_controller.cpp`,
  `lib/ntn/beam_hopping_table.cpp` beyond accepted changes, and broad `tests/unittests/ntn/` are not generally open.
  Use only exact accepted files unless a future task explicitly approves more shared NTN work.

## 13. o_cu_cp and Config Glue Paths

- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config.h`
  App-level CU-CP unit config, including `cu_cp_unit_ntn_location_mobility_config`.

- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_cli11_schema.cpp`
  CLI schema for CU-CP app config.

- `apps/units/o_cu_cp/cu_cp/cu_cp_config_translators.cpp`
  Translates app config into CU-CP runtime config.

- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_validator.cpp`
  App-level config validation.

- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_yaml_writer.cpp`
  YAML writer support for CU-CP config.

- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
  CU-CP command-line command wiring.

- `apps/units/o_cu_cp/o_cu_cp_builder.cpp`
  Construction glue from o_cu_cp app into CU-CP.

- `configs/leo_500km_cucp_ntn.yml`, `configs/leo_500km_beam_table.json`,
  `configs/cu_cp_neighbor_cells_example.json`
  Example data/configs for the accepted CU-CP NTN harness.

## 14. Forbidden Paths for This Harness

These paths are quarantined or out of scope unless a task explicitly asks for a non-CU-CP exception and the owner
approves it.

- `apps/units/flexible_o_du/`
- `include/srsran/f1ap/du/`
- `lib/f1ap/du/`
- `tests/unittests/f1ap/du/`
- `lib/du/`
- `tests/unittests/du_manager/`
- `include/srsran/ntn/beam_hopping_controller.h`
- `include/srsran/ntn/ntn_configuration_manager.h`
- `lib/ntn/beam_hopping_controller.cpp`
- `tests/unittests/ntn/`
- `tools/gis_remote_service_site/`
- `docs/superpowers/plans/2026-05-23-gis-*`
- `scripts/create_ai_comm_eval_ppt_polished.ps1`

Path-name caution: `lib/cu_cp/ntn_mobility/ntn_served_beam_scheduler.*` is CU-CP-owned despite containing the word
`scheduler`. The forbidden scheduler category means DU/MAC scheduling, not CU-CP control-plane beam selection.

## 15. Recommended Updates to allowed_paths.md

No immediate update is required. The current `allowed_paths.md` is intentionally conservative:

- broad CU-CP prefixes for CU-CP-owned code and tests;
- exact non-CU-CP exception files already accepted by review;
- no broad `lib/`, `include/`, `apps/`, `lib/f1ap/`, `lib/ngap/`, or `lib/rrc/` entries.

If a future task needs additional exception paths, add exact files or the narrowest safe prefix after human review.

## 16. Open Questions

- Should future CU-CP tasks continue to use shared `lib/ntn/orbit_propagator.*`, or should the orbit update model be
  re-homed into CU-CP if it remains CU-CP-only?
- What is the minimum approved path set for SIB19/system-information packaging without crossing into DU scheduling?
- Which NGAP location-reporting events are required for the first NTN CU-CP milestone: direct reporting only, change of
  serving cell, UE presence in area of interest, or all accepted event types?
- When end-to-end SR/SRS realization becomes necessary, which DU-side files should be proposed as explicit exceptions
  instead of reopening the whole DU resource-management area?
- Should `allowed_paths.md` include a future narrow prefix for `lib/e1ap/cu_cp/`, or should E1AP remain task-by-task
  only until an NTN CU-CP task needs it?
