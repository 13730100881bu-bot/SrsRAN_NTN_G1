# Allowed paths for future CU-CP NTN tasks

Path guard checks future task diffs against:

- `ai/cucp-harness-base`

These paths are allowed by default.

- `AGENTS.md`
- `ai_harness/`

Core CU-CP paths:

- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

Approved narrow control-plane exception paths:

- `apps/units/o_cu_cp/cu_cp/CMakeLists.txt`
- `apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h`
- `apps/units/o_cu_cp/cu_cp/cu_cp_config_translators.cpp`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config.h`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_cli11_schema.cpp`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_validator.cpp`
- `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_yaml_writer.cpp`
- `apps/units/o_cu_cp/o_cu_cp_builder.cpp`
- `configs/CMakeLists.txt`
- `configs/cu_cp_neighbor_cells_example.json`
- `configs/leo_500km_beam_table.json`
- `configs/leo_500km_cucp_ntn.yml`
- `include/srsran/f1ap/cu_cp/f1ap_cu_ue_context_update.h`
- `include/srsran/f1ap/ntn_ul_slot_resource_request.h`
- `include/srsran/ngap/ngap.h`
- `include/srsran/ngap/ngap_location_reporting.h`
- `include/srsran/ntn/beam_hopping_table.h`
- `include/srsran/ntn/orbit_propagator.h`
- `include/srsran/rrc/rrc_ue.h`
- `lib/f1ap/cu_cp/procedures/ue_context_modification_procedure.cpp`
- `lib/f1ap/cu_cp/procedures/ue_context_setup_procedure.cpp`
- `lib/ngap/ngap_asn1_converters.h`
- `lib/ngap/ngap_impl.cpp`
- `lib/ngap/ngap_impl.h`
- `lib/ntn/CMakeLists.txt`
- `lib/ntn/beam_hopping_table.cpp`
- `lib/ntn/orbit_propagator.cpp`
- `lib/rrc/ue/rrc_ue_impl.h`
- `lib/rrc/ue/rrc_ue_message_handlers.cpp`
- `tests/unittests/apps/units/CMakeLists.txt`
- `tests/unittests/apps/units/o_cu_cp/CMakeLists.txt`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/CMakeLists.txt`
- `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`
- `tests/unittests/f1ap/common/f1ap_asn1_helpers_test.cpp`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_modification_procedure_test.cpp`
- `tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_setup_procedure_test.cpp`
- `tests/unittests/ngap/ngap_ue_context_management_procedure_test.cpp`
- `tests/unittests/ngap/test_helpers.h`
- `utils/ntn/generate_leo_beam_table.py`

Approved exact planning artifact paths:

- `docs/superpowers/plans/2026-06-05-ntn-cucp-inventory-service-calendar-implementation.md`
- `docs/superpowers/plans/2026-06-06-ntn-cucp-ngap-switch-over.md`
- `docs/superpowers/specs/2026-06-05-ntn-cucp-inventory-service-calendar-design.md`
- `docs/superpowers/specs/2026-06-06-ntn-cucp-ngap-switch-over-design.md`

Do not add broad prefixes such as:

- `lib/`
- `include/`
- `apps/`

Do not add DU/MAC/PHY/RU/RF/ZMQ/GIS/O-DU paths.
