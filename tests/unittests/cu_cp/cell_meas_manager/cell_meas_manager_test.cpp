/*
 *
 * Copyright 2021-2026 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "cell_meas_manager_test_helpers.h"
#include "lib/cu_cp/ntn_mobility/ntn_served_beam_selector.h"
#include "srsran/ran/plmn_identity.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace srsran;
using namespace srs_cu_cp;

static nr_cell_identity get_ntn_test_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{0x19b, 32}, sector_id).value();
}

static ecef_coordinates_t make_ecef(double latitude_deg, double longitude_deg, double altitude_m)
{
  constexpr double wgs84_a_m = 6378137.0;
  constexpr double wgs84_f   = 1.0 / 298.257223563;
  constexpr double wgs84_e2  = 2.0 * wgs84_f - wgs84_f * wgs84_f;
  constexpr double pi        = 3.14159265358979323846;

  const double lat     = latitude_deg * pi / 180.0;
  const double lon     = longitude_deg * pi / 180.0;
  const double sin_lat = std::sin(lat);
  const double cos_lat = std::cos(lat);
  const double n       = wgs84_a_m / std::sqrt(1.0 - wgs84_e2 * sin_lat * sin_lat);

  ecef_coordinates_t ecef{};
  ecef.position_x = (n + altitude_m) * cos_lat * std::cos(lon);
  ecef.position_y = (n + altitude_m) * cos_lat * std::sin(lon);
  ecef.position_z = (n * (1.0 - wgs84_e2) + altitude_m) * sin_lat;
  return ecef;
}

static ntn_ue_location_report make_ntn_location_report(ue_index_t                                  ue_index,
                                                       nr_cell_identity                            serving_nci,
                                                       double                                      longitude_deg,
                                                       std::chrono::steady_clock::time_point       time,
                                                       ntn_ue_location_report_source source =
                                                           ntn_ue_location_report_source::measurement_report)
{
  ntn_ue_location_report report;
  report.ue_index              = ue_index;
  report.serving_nci           = serving_nci;
  report.latitude_deg          = 0.0;
  report.longitude_deg         = longitude_deg;
  report.horizontal_accuracy_m = 25.0;
  report.received_time         = time;
  report.source                = source;
  return report;
}

static ntn_handover_result make_ntn_handover_result(const ntn_location_handover_trigger& trigger,
                                                    bool                                 success,
                                                    ntn_handover_failure_cause failure_cause =
                                                        ntn_handover_failure_cause::none)
{
  ntn_handover_result result;
  result.source_ue_index                         = trigger.ue_index;
  result.context.handover_attempt_id             = trigger.handover_attempt_id;
  result.context.target_beam_id                  = trigger.target_beam_id;
  result.context.serving_nci                     = trigger.serving_nci;
  result.context.target_nci                      = trigger.target_nci;
  result.context.consecutive_location_reports    = trigger.consecutive_location_reports;
  result.context.candidate_age                   = std::chrono::duration_cast<std::chrono::milliseconds>(
      trigger.last_report_time - trigger.candidate_since);
  result.success       = success;
  result.failure_cause = failure_cause;
  return result;
}

TEST_F(cell_meas_manager_test, when_empty_cell_config_is_used_validation_fails)
{
  cell_meas_config cell_cfg;
  ASSERT_FALSE(is_complete(cell_cfg.serving_cell_cfg));
}

TEST_F(cell_meas_manager_test, when_valid_cell_config_is_used_validation_succeeds)
{
  cell_meas_config cell_cfg;
  cell_cfg.serving_cell_cfg.nci                 = nr_cell_identity::create(0x19b0).value();
  cell_cfg.serving_cell_cfg.gnb_id_bit_length   = 32;
  cell_cfg.serving_cell_cfg.pci                 = 1;
  cell_cfg.serving_cell_cfg.band.emplace()      = nr_band::n78;
  cell_cfg.serving_cell_cfg.ssb_arfcn.emplace() = 632628;
  cell_cfg.serving_cell_cfg.ssb_scs.emplace()   = subcarrier_spacing::kHz30;
  rrc_ssb_mtc ssb_mtc;
  ssb_mtc.dur                                 = 1;
  ssb_mtc.periodicity_and_offset.periodicity  = rrc_periodicity_and_offset::periodicity_t::sf5;
  ssb_mtc.periodicity_and_offset.offset       = 0;
  cell_cfg.serving_cell_cfg.ssb_mtc.emplace() = ssb_mtc;
  ASSERT_TRUE(is_complete(cell_cfg.serving_cell_cfg));
}

TEST_F(cell_meas_manager_test, when_empty_config_is_used_validation_succeeds)
{
  cell_meas_manager_cfg cfg = {};
  ASSERT_TRUE(is_valid_configuration(cfg));
}

TEST_F(cell_meas_manager_test, when_ntn_beam_table_json_is_parsed_then_static_beams_are_available)
{
  const std::string json = R"json(
{
  "version": 1,
  "region": "china",
  "satellite_height_m": 500000,
  "beams": [
    {
      "beam_id": "CN-BEAM-0001",
      "nci": 6576,
      "center_latitude_deg": 39.9,
      "center_longitude_deg": 116.4,
      "coverage_radius_m": 230000,
      "enabled": true
    },
    {
      "beam_id": "CN-BEAM-0002",
      "nci": "0x19b1",
      "center_latitude_deg": 31.2,
      "center_longitude_deg": 121.5,
      "coverage_radius_m": 230000,
      "enabled": false
    }
  ]
}
)json";

  auto table = parse_ntn_beam_table_json(json);
  ASSERT_TRUE(table.has_value()) << table.error();
  ASSERT_EQ(table->version, 1);
  ASSERT_EQ(table->region, "china");
  ASSERT_TRUE(table->satellite_height_m.has_value());
  ASSERT_EQ(table->beams.size(), 2);
  ASSERT_EQ(table->beams[0].beam_id, "CN-BEAM-0001");
  ASSERT_EQ(table->beams[0].nci, nr_cell_identity::create(0x19b0).value());
  ASSERT_DOUBLE_EQ(table->beams[0].coverage_radius_m, 230000.0);
  ASSERT_TRUE(table->beams[0].downlink_enabled);
  ASSERT_TRUE(table->beams[0].uplink_enabled);
  ASSERT_EQ(table->beams[1].beam_id, "CN-BEAM-0002");
  ASSERT_EQ(table->beams[1].nci, nr_cell_identity::create(0x19b1).value());
  ASSERT_FALSE(table->beams[1].enabled);
  ASSERT_TRUE(table->beams[1].downlink_enabled);
  ASSERT_TRUE(table->beams[1].uplink_enabled);
}

TEST_F(cell_meas_manager_test, when_ntn_beam_table_link_direction_is_parsed_then_flags_are_available)
{
  const std::string json = R"json(
{
  "version": 1,
  "analog_beams": [
    {
      "analog_beam_id": "ANALOG-0001",
      "center_hex_q": 0,
      "center_hex_r": 0,
      "center_digital_beam_id": "DL-ONLY",
      "child_digital_beam_ids": ["DL-ONLY", "UL-ONLY"],
      "is_edge_partial": true,
      "downlink_enabled": true,
      "uplink_enabled": true
    }
  ],
  "beams": [
    {
      "beam_id": "DL-ONLY",
      "analog_beam_id": "ANALOG-0001",
      "hex_q": 0,
      "hex_r": 0,
      "nci": "0x19b0",
      "center_latitude_deg": 31.2304,
      "center_longitude_deg": 121.4737,
      "coverage_radius_m": 15000,
      "downlink_enabled": true,
      "uplink_enabled": false
    },
    {
      "beam_id": "UL-ONLY",
      "analog_beam_id": "ANALOG-0001",
      "hex_q": 1,
      "hex_r": 0,
      "nci": "0x19b1",
      "center_latitude_deg": 31.2304,
      "center_longitude_deg": 121.6337,
      "coverage_radius_m": 15000,
      "downlink_enabled": false,
      "uplink_enabled": true
    }
  ]
}
)json";

  auto table = parse_ntn_beam_table_json(json);
  ASSERT_TRUE(table.has_value()) << table.error();
  ASSERT_EQ(table->analog_beams.size(), 1);
  ASSERT_TRUE(table->analog_beams.front().downlink_enabled);
  ASSERT_TRUE(table->analog_beams.front().uplink_enabled);
  ASSERT_EQ(table->beams.size(), 2);
  ASSERT_TRUE(table->beams[0].downlink_enabled);
  ASSERT_FALSE(table->beams[0].uplink_enabled);
  ASSERT_FALSE(table->beams[1].downlink_enabled);
  ASSERT_TRUE(table->beams[1].uplink_enabled);
}

TEST_F(cell_meas_manager_test, when_ntn_beam_table_disables_both_link_directions_then_parse_fails)
{
  const std::string json = R"json(
{
  "version": 1,
  "beams": [
    {
      "beam_id": "DISABLED-LINK",
      "nci": "0x19b0",
      "center_latitude_deg": 31.2304,
      "center_longitude_deg": 121.4737,
      "coverage_radius_m": 15000,
      "downlink_enabled": false,
      "uplink_enabled": false
    }
  ]
}
)json";

  auto table = parse_ntn_beam_table_json(json);
  ASSERT_FALSE(table.has_value());
  ASSERT_NE(table.error().find("at least one link direction"), std::string::npos);
}

TEST_F(cell_meas_manager_test, when_ntn_hierarchical_beam_table_json_is_parsed_then_analog_clusters_are_available)
{
  const std::string json = R"json(
{
  "version": 1,
  "region": "leo-hex",
  "satellite_height_m": 500000,
  "analog_beams": [
    {
      "analog_beam_id": "LEO500-ANALOG-0001",
      "center_hex_q": 0,
      "center_hex_r": 0,
      "center_digital_beam_id": "LEO500-DIGI-0001",
      "child_digital_beam_ids": [
        "LEO500-DIGI-0001",
        "LEO500-DIGI-0002",
        "LEO500-DIGI-0003",
        "LEO500-DIGI-0004",
        "LEO500-DIGI-0005",
        "LEO500-DIGI-0006",
        "LEO500-DIGI-0007"
      ],
      "is_edge_partial": false
    }
  ],
  "beams": [
    {
      "beam_id": "LEO500-DIGI-0001",
      "analog_beam_id": "LEO500-ANALOG-0001",
      "hex_q": 0,
      "hex_r": 0,
      "nci": "0x19b0",
      "center_latitude_deg": 31.2304,
      "center_longitude_deg": 121.4737,
      "coverage_radius_m": 15000,
      "enabled": true
    },
    {
      "beam_id": "LEO500-DIGI-0002",
      "analog_beam_id": "LEO500-ANALOG-0001",
      "hex_q": 1,
      "hex_r": 0,
      "nci": "0x19b1",
      "center_latitude_deg": 31.2304,
      "center_longitude_deg": 121.6337,
      "coverage_radius_m": 15000,
      "enabled": true
    },
    {
      "beam_id": "LEO500-DIGI-0003",
      "analog_beam_id": "LEO500-ANALOG-0001",
      "hex_q": 1,
      "hex_r": -1,
      "nci": "0x19b2",
      "center_latitude_deg": 31.3653,
      "center_longitude_deg": 121.5537,
      "coverage_radius_m": 15000,
      "enabled": true
    },
    {
      "beam_id": "LEO500-DIGI-0004",
      "analog_beam_id": "LEO500-ANALOG-0001",
      "hex_q": 0,
      "hex_r": -1,
      "nci": "0x19b3",
      "center_latitude_deg": 31.3653,
      "center_longitude_deg": 121.3937,
      "coverage_radius_m": 15000,
      "enabled": true
    },
    {
      "beam_id": "LEO500-DIGI-0005",
      "analog_beam_id": "LEO500-ANALOG-0001",
      "hex_q": -1,
      "hex_r": 0,
      "nci": "0x19b4",
      "center_latitude_deg": 31.2304,
      "center_longitude_deg": 121.3137,
      "coverage_radius_m": 15000,
      "enabled": true
    },
    {
      "beam_id": "LEO500-DIGI-0006",
      "analog_beam_id": "LEO500-ANALOG-0001",
      "hex_q": -1,
      "hex_r": 1,
      "nci": "0x19b5",
      "center_latitude_deg": 31.0955,
      "center_longitude_deg": 121.3937,
      "coverage_radius_m": 15000,
      "enabled": true
    },
    {
      "beam_id": "LEO500-DIGI-0007",
      "analog_beam_id": "LEO500-ANALOG-0001",
      "hex_q": 0,
      "hex_r": 1,
      "nci": "0x19b6",
      "center_latitude_deg": 31.0955,
      "center_longitude_deg": 121.5537,
      "coverage_radius_m": 15000,
      "enabled": true
    }
  ]
}
)json";

  auto table = parse_ntn_beam_table_json(json);
  ASSERT_TRUE(table.has_value()) << table.error();
  ASSERT_EQ(table->analog_beams.size(), 1);
  ASSERT_EQ(table->analog_beams.front().analog_beam_id, "LEO500-ANALOG-0001");
  ASSERT_EQ(table->analog_beams.front().center_digital_beam_id, "LEO500-DIGI-0001");
  ASSERT_FALSE(table->analog_beams.front().is_edge_partial);
  ASSERT_EQ(table->analog_beams.front().child_digital_beam_ids.size(), 7);
  ASSERT_EQ(table->beams.size(), 7);
  ASSERT_EQ(table->beams.front().analog_beam_id, "LEO500-ANALOG-0001");
  ASSERT_TRUE(table->beams.front().hex_q.has_value());
  ASSERT_TRUE(table->beams.front().hex_r.has_value());
  ASSERT_EQ(table->beams.front().hex_q.value(), 0);
  ASSERT_EQ(table->beams.front().hex_r.value(), 0);
}

TEST_F(cell_meas_manager_test, when_ntn_beam_table_resource_policy_is_parsed_then_defaults_and_overrides_are_merged)
{
  const std::string json = R"json(
{
  "version": 1,
  "resource_policy": {
    "analog": {
      "max_loaded_digital_children": 2,
      "max_service_bound_ues": 9
    },
    "digital": {
      "max_ues": 3,
      "max_drbs": 4,
      "reuse_group_id": "reuse-default",
      "conflict_group_ids": ["conflict-default"]
    }
  },
  "analog_beams": [
    {
      "analog_beam_id": "LEO500-ANALOG-0001",
      "center_hex_q": 0,
      "center_hex_r": 0,
      "center_digital_beam_id": "LEO500-DIGI-0001",
      "child_digital_beam_ids": ["LEO500-DIGI-0001"],
      "is_edge_partial": true,
      "resource_policy": {
        "max_loaded_digital_children": 1,
        "max_access_only_ues": 5
      }
    }
  ],
  "beams": [
    {
      "beam_id": "LEO500-DIGI-0001",
      "analog_beam_id": "LEO500-ANALOG-0001",
      "nci": "0x19b0",
      "center_latitude_deg": 31.2304,
      "center_longitude_deg": 121.4737,
      "coverage_radius_m": 15000,
      "resource_policy": {
        "max_drbs": 2,
        "reuse_group_id": "reuse-local",
        "conflict_group_ids": ["conflict-local"]
      }
    }
  ]
}
)json";

  auto table = parse_ntn_beam_table_json(json);
  ASSERT_TRUE(table.has_value()) << table.error();
  ASSERT_TRUE(table->analog_beams.front().resource_policy.has_value());
  EXPECT_EQ(table->analog_beams.front().resource_policy->max_access_only_ues, 5U);
  EXPECT_EQ(table->analog_beams.front().resource_policy->max_service_bound_ues, 9U);
  EXPECT_EQ(table->analog_beams.front().resource_policy->max_loaded_digital_children, 1U);
  ASSERT_TRUE(table->beams.front().resource_policy.has_value());
  EXPECT_EQ(table->beams.front().resource_policy->max_ues, 3U);
  EXPECT_EQ(table->beams.front().resource_policy->max_drbs, 2U);
  EXPECT_EQ(table->beams.front().resource_policy->reuse_group_id, "reuse-local");
  EXPECT_EQ(table->beams.front().resource_policy->conflict_group_ids,
            std::vector<std::string>({"conflict-local"}));
}

TEST_F(cell_meas_manager_test, when_ntn_beam_table_resource_policy_is_invalid_then_parsing_fails)
{
  const std::string negative_cap_json = R"json(
{
  "beams": [
    {
      "beam_id": "CN-BEAM-0001",
      "nci": "0x19b0",
      "center_latitude_deg": 39.9,
      "center_longitude_deg": 116.4,
      "coverage_radius_m": 230000,
      "resource_policy": {
        "max_ues": -1
      }
    }
  ]
}
)json";
  ASSERT_FALSE(parse_ntn_beam_table_json(negative_cap_json).has_value());

  const std::string duplicate_conflict_json = R"json(
{
  "beams": [
    {
      "beam_id": "CN-BEAM-0001",
      "nci": "0x19b0",
      "center_latitude_deg": 39.9,
      "center_longitude_deg": 116.4,
      "coverage_radius_m": 230000,
      "resource_policy": {
        "conflict_group_ids": ["conflict-a", "conflict-a"]
      }
    }
  ]
}
)json";
  ASSERT_FALSE(parse_ntn_beam_table_json(duplicate_conflict_json).has_value());

  const std::string empty_reuse_json = R"json(
{
  "beams": [
    {
      "beam_id": "CN-BEAM-0001",
      "nci": "0x19b0",
      "center_latitude_deg": 39.9,
      "center_longitude_deg": 116.4,
      "coverage_radius_m": 230000,
      "resource_policy": {
        "reuse_group_id": ""
      }
    }
  ]
}
)json";
  ASSERT_FALSE(parse_ntn_beam_table_json(empty_reuse_json).has_value());
}

TEST_F(cell_meas_manager_test, when_ntn_hierarchical_beam_table_has_invalid_cluster_then_parsing_fails)
{
  const std::string unknown_parent_json = R"json(
{
  "analog_beams": [],
  "beams": [
    {
      "beam_id": "LEO500-DIGI-0001",
      "analog_beam_id": "LEO500-ANALOG-MISSING",
      "nci": "0x19b0",
      "center_latitude_deg": 31.2304,
      "center_longitude_deg": 121.4737,
      "coverage_radius_m": 15000
    }
  ]
}
)json";
  ASSERT_FALSE(parse_ntn_beam_table_json(unknown_parent_json).has_value());

  const std::string non_edge_partial_json = R"json(
{
  "analog_beams": [
    {
      "analog_beam_id": "LEO500-ANALOG-0001",
      "center_hex_q": 0,
      "center_hex_r": 0,
      "center_digital_beam_id": "LEO500-DIGI-0001",
      "child_digital_beam_ids": ["LEO500-DIGI-0001"],
      "is_edge_partial": false
    }
  ],
  "beams": [
    {
      "beam_id": "LEO500-DIGI-0001",
      "analog_beam_id": "LEO500-ANALOG-0001",
      "nci": "0x19b0",
      "center_latitude_deg": 31.2304,
      "center_longitude_deg": 121.4737,
      "coverage_radius_m": 15000
    }
  ]
}
)json";
  ASSERT_FALSE(parse_ntn_beam_table_json(non_edge_partial_json).has_value());

  const std::string duplicate_child_json = R"json(
{
  "analog_beams": [
    {
      "analog_beam_id": "LEO500-ANALOG-0001",
      "center_hex_q": 0,
      "center_hex_r": 0,
      "center_digital_beam_id": "LEO500-DIGI-0001",
      "child_digital_beam_ids": ["LEO500-DIGI-0001"],
      "is_edge_partial": true
    },
    {
      "analog_beam_id": "LEO500-ANALOG-0002",
      "center_hex_q": 3,
      "center_hex_r": -1,
      "center_digital_beam_id": "LEO500-DIGI-0001",
      "child_digital_beam_ids": ["LEO500-DIGI-0001"],
      "is_edge_partial": true
    }
  ],
  "beams": [
    {
      "beam_id": "LEO500-DIGI-0001",
      "analog_beam_id": "LEO500-ANALOG-0001",
      "nci": "0x19b0",
      "center_latitude_deg": 31.2304,
      "center_longitude_deg": 121.4737,
      "coverage_radius_m": 15000
    }
  ]
}
)json";
  ASSERT_FALSE(parse_ntn_beam_table_json(duplicate_child_json).has_value());
}

TEST_F(cell_meas_manager_test, when_ntn_beam_table_has_invalid_geometry_then_parsing_fails)
{
  const std::string json = R"json(
{
  "beams": [
    {
      "beam_id": "CN-BEAM-0001",
      "nci": "0x19b0",
      "center_latitude_deg": 91.0,
      "center_longitude_deg": 116.4,
      "coverage_radius_m": 230000
    }
  ]
}
)json";

  auto table = parse_ntn_beam_table_json(json);
  ASSERT_FALSE(table.has_value());
}

TEST_F(cell_meas_manager_test, when_ntn_beam_table_has_duplicate_ids_or_ncis_then_parsing_fails)
{
  const std::string duplicate_id_json = R"json(
{
  "beams": [
    {
      "beam_id": "CN-BEAM-0001",
      "nci": "0x19b0",
      "center_latitude_deg": 39.9,
      "center_longitude_deg": 116.4,
      "coverage_radius_m": 230000
    },
    {
      "beam_id": "CN-BEAM-0001",
      "nci": "0x19b1",
      "center_latitude_deg": 39.9,
      "center_longitude_deg": 116.8,
      "coverage_radius_m": 230000
    }
  ]
}
)json";
  ASSERT_FALSE(parse_ntn_beam_table_json(duplicate_id_json).has_value());

  const std::string duplicate_nci_json = R"json(
{
  "beams": [
    {
      "beam_id": "CN-BEAM-0001",
      "nci": "0x19b0",
      "center_latitude_deg": 39.9,
      "center_longitude_deg": 116.4,
      "coverage_radius_m": 230000
    },
    {
      "beam_id": "CN-BEAM-0002",
      "nci": "0x19b0",
      "center_latitude_deg": 39.9,
      "center_longitude_deg": 116.8,
      "coverage_radius_m": 230000
    }
  ]
}
)json";
  ASSERT_FALSE(parse_ntn_beam_table_json(duplicate_nci_json).has_value());
}

TEST_F(cell_meas_manager_test, when_empty_config_is_used_then_no_neighbor_cells_are_available)
{
  create_empty_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));
  nr_cell_identity            nci      = nr_cell_identity::create(0x19b0).value();
  std::optional<rrc_meas_cfg> meas_cfg = manager->get_measurement_config(ue_index, nci);

  // Make sure meas_cfg is empty.
  verify_empty_meas_cfg(meas_cfg);
}

TEST_F(cell_meas_manager_test, when_serving_cell_not_found_no_neighbor_cells_are_available)
{
  create_default_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));
  nr_cell_identity            nci      = nr_cell_identity::create(0x19b5).value();
  std::optional<rrc_meas_cfg> meas_cfg = manager->get_measurement_config(ue_index, nci);

  // Make sure meas_cfg is empty.
  verify_empty_meas_cfg(meas_cfg);
}

TEST_F(cell_meas_manager_test, when_serving_cell_found_then_neighbor_cells_are_available)
{
  create_default_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  for (unsigned nci_val = 0x19b0; nci_val < 0x19b2; ++nci_val) {
    std::optional<rrc_meas_cfg> meas_cfg =
        manager->get_measurement_config(ue_index, nr_cell_identity::create(nci_val).value());
    check_default_meas_cfg(meas_cfg, meas_obj_id_t::min);
    verify_meas_cfg(meas_cfg);
  }
}

TEST_F(cell_meas_manager_test, when_inexisting_cell_config_is_updated_then_config_is_added)
{
  create_default_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));
  const nr_cell_identity nci = nr_cell_identity::create(0x19b1).value();

  // get current config
  std::optional<cell_meas_config> cell_cfg = manager->get_cell_config(nci);
  ASSERT_TRUE(cell_cfg.has_value());

  // update config for cell 3
  auto& cell_cfg_val                                = cell_cfg.value();
  cell_cfg_val.serving_cell_cfg.gnb_id_bit_length   = 32;
  cell_cfg_val.serving_cell_cfg.nci                 = nr_cell_identity::create(0x19b3).value();
  cell_cfg_val.serving_cell_cfg.band.emplace()      = nr_band::n78;
  cell_cfg_val.serving_cell_cfg.ssb_arfcn.emplace() = 632628;
  cell_cfg_val.serving_cell_cfg.ssb_scs.emplace()   = subcarrier_spacing::kHz30;

  // Make sure meas_cfg is created.
  std::optional<rrc_meas_cfg> meas_cfg = manager->get_measurement_config(ue_index, nci);
  check_default_meas_cfg(meas_cfg, meas_obj_id_t::min);
  verify_meas_cfg(meas_cfg);
}

TEST_F(cell_meas_manager_test, when_incomplete_cell_config_is_updated_then_valid_meas_config_is_created)
{
  create_default_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));
  const nr_cell_identity nci = nr_cell_identity::create(0x19b1).value();

  // get current config
  std::optional<cell_meas_config> cell_cfg = manager->get_cell_config(nci);
  ASSERT_TRUE(cell_cfg.has_value());

  // update config for cell 1
  auto& cell_cfg_val                                = cell_cfg.value();
  cell_cfg_val.serving_cell_cfg.band.emplace()      = nr_band::n78;
  cell_cfg_val.serving_cell_cfg.ssb_arfcn.emplace() = 632628;
  cell_cfg_val.serving_cell_cfg.ssb_scs.emplace()   = subcarrier_spacing::kHz30;

  // Make sure meas_cfg is created.
  std::optional<rrc_meas_cfg> meas_cfg = manager->get_measurement_config(ue_index, nci);
  check_default_meas_cfg(meas_cfg, meas_obj_id_t::min);
  verify_meas_cfg(meas_cfg);
}

TEST_F(cell_meas_manager_test, when_empty_cell_config_is_used_then_meas_cfg_is_not_set)
{
  // Create a manager without ncells and without report config.
  create_manager_without_ncells_and_periodic_report();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));
  nr_cell_identity            nci      = nr_cell_identity::create(0x19b0).value();
  std::optional<rrc_meas_cfg> meas_cfg = manager->get_measurement_config(ue_index, nci);

  // Make sure meas_cfg is empty.
  verify_empty_meas_cfg(meas_cfg);
}

TEST_F(cell_meas_manager_test, when_old_meas_config_is_provided_old_ids_are_removed)
{
  create_default_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));
  const nr_cell_identity initial_nci = nr_cell_identity::create(0x19b0).value();

  // Make sure meas_cfg is created (no previous meas config provided)
  std::optional<rrc_meas_cfg> initial_meas_cfg = manager->get_measurement_config(ue_index, initial_nci);
  check_default_meas_cfg(initial_meas_cfg, meas_obj_id_t::min);
  verify_meas_cfg(initial_meas_cfg);

  const nr_cell_identity      target_nci      = nr_cell_identity::create(0x19b1).value();
  std::optional<rrc_meas_cfg> target_meas_cfg = manager->get_measurement_config(ue_index, target_nci, initial_meas_cfg);

  // Make sure initial IDs are release again.
  ASSERT_EQ(target_meas_cfg.value().meas_obj_to_rem_list.at(0),
            initial_meas_cfg.value().meas_obj_to_add_mod_list.at(0).meas_obj_id);

  ASSERT_EQ(target_meas_cfg.value().meas_id_to_rem_list.at(0),
            initial_meas_cfg.value().meas_id_to_add_mod_list.at(0).meas_id);

  ASSERT_EQ(target_meas_cfg.value().report_cfg_to_rem_list.at(0),
            initial_meas_cfg.value().report_cfg_to_add_mod_list.at(0).report_cfg_id);

  // The new config should reuse the IDs again.
  check_default_meas_cfg(target_meas_cfg, meas_obj_id_t::min);
  verify_meas_cfg(target_meas_cfg);
}

TEST_F(cell_meas_manager_test, when_only_event_based_reports_configured_then_meas_objects_are_created)
{
  create_manager_with_incomplete_cells_and_periodic_report_at_target_cell();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));
  const nr_cell_identity initial_nci = nr_cell_identity::create(0x19b0).value();
  const nr_cell_identity target_nci  = nr_cell_identity::create(0x19b1).value();

  // Make sure no meas_cfg is created (incomplete cell config)
  ASSERT_FALSE(manager->get_measurement_config(ue_index, initial_nci).has_value());
  ASSERT_FALSE(manager->get_measurement_config(ue_index, target_nci).has_value());

  serving_cell_meas_config serving_cell_cfg;
  serving_cell_cfg.gnb_id_bit_length   = 32;
  serving_cell_cfg.nci                 = initial_nci;
  serving_cell_cfg.pci                 = 1;
  serving_cell_cfg.band.emplace()      = nr_band::n78;
  serving_cell_cfg.ssb_arfcn.emplace() = 632628;
  serving_cell_cfg.ssb_scs.emplace()   = subcarrier_spacing::kHz30;
  {
    rrc_ssb_mtc ssb_mtc;
    ssb_mtc.dur                                = 1;
    ssb_mtc.periodicity_and_offset.periodicity = rrc_periodicity_and_offset::periodicity_t::sf5;
    ssb_mtc.periodicity_and_offset.offset      = 0;
    serving_cell_cfg.ssb_mtc.emplace()         = ssb_mtc;
  }

  // Update cell config for cell 1
  ASSERT_TRUE(manager->update_cell_config(initial_nci, serving_cell_cfg));

  // Update cell config for cell 2
  serving_cell_cfg.nci = target_nci;
  ASSERT_TRUE(manager->update_cell_config(target_nci, serving_cell_cfg));

  // Make sure meas_cfg is created and contains measurement objects to add mod
  std::optional<rrc_meas_cfg> initial_meas_cfg = manager->get_measurement_config(ue_index, initial_nci);
  ASSERT_TRUE(initial_meas_cfg.has_value());
  ASSERT_EQ(initial_meas_cfg.value().meas_obj_to_add_mod_list.size(), 1);
  ASSERT_TRUE(initial_meas_cfg.value().meas_obj_to_add_mod_list.begin()->meas_obj_nr.has_value());
  ASSERT_EQ(initial_meas_cfg.value().meas_obj_to_add_mod_list.begin()->meas_obj_nr.value().ssb_freq,
            serving_cell_cfg.ssb_arfcn);
  ASSERT_EQ(initial_meas_cfg.value().report_cfg_to_add_mod_list.size(), 1);

  std::optional<rrc_meas_cfg> target_meas_cfg = manager->get_measurement_config(ue_index, target_nci, initial_meas_cfg);
  ASSERT_TRUE(target_meas_cfg.has_value());
  ASSERT_EQ(target_meas_cfg.value().meas_obj_to_add_mod_list.size(), 1);
  ASSERT_TRUE(target_meas_cfg.value().meas_obj_to_add_mod_list.begin()->meas_obj_nr.has_value());
  ASSERT_EQ(target_meas_cfg.value().meas_obj_to_add_mod_list.begin()->meas_obj_nr.value().ssb_freq,
            serving_cell_cfg.ssb_arfcn);
  ASSERT_EQ(target_meas_cfg.value().report_cfg_to_add_mod_list.size(), 2);
}

TEST_F(cell_meas_manager_test, when_invalid_cell_config_update_received_then_config_is_not_updated)
{
  create_manager_with_incomplete_cells_and_periodic_report_at_target_cell();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));
  const nr_cell_identity initial_nci = nr_cell_identity::create(0x19b0).value();
  const nr_cell_identity target_nci  = nr_cell_identity::create(0x19b1).value();

  // Make sure no meas_cfg is created (incomplete cell config)
  ASSERT_FALSE(manager->get_measurement_config(ue_index, initial_nci).has_value());
  ASSERT_FALSE(manager->get_measurement_config(ue_index, target_nci).has_value());

  serving_cell_meas_config serving_cell_cfg;
  serving_cell_cfg.gnb_id_bit_length = 32;
  serving_cell_cfg.nci               = initial_nci;
  serving_cell_cfg.pci               = 1;
  serving_cell_cfg.band              = nr_band::n78;
  serving_cell_cfg.ssb_arfcn         = 632628;
  serving_cell_cfg.ssb_scs           = subcarrier_spacing::kHz30;
  {
    rrc_ssb_mtc ssb_mtc;
    ssb_mtc.dur                                = 1;
    ssb_mtc.periodicity_and_offset.periodicity = rrc_periodicity_and_offset::periodicity_t::sf5;
    ssb_mtc.periodicity_and_offset.offset      = 0;
    serving_cell_cfg.ssb_mtc                   = ssb_mtc;
  }

  // Update cell config for cell 1
  ASSERT_TRUE(manager->update_cell_config(initial_nci, serving_cell_cfg));

  // Update cell config for cell 2 with different scs for same ssb_freq
  serving_cell_cfg.nci     = target_nci;
  serving_cell_cfg.ssb_scs = subcarrier_spacing::kHz15;

  ASSERT_FALSE(manager->update_cell_config(target_nci, serving_cell_cfg));

  // Make sure meas_cfg is created for cell 1 and contains measurement objects to add mod
  std::optional<rrc_meas_cfg> initial_meas_cfg = manager->get_measurement_config(ue_index, initial_nci);
  ASSERT_TRUE(initial_meas_cfg.has_value());
  ASSERT_TRUE(initial_meas_cfg.value().meas_obj_to_add_mod_list.empty());
  ASSERT_TRUE(initial_meas_cfg.value().report_cfg_to_add_mod_list.empty());

  std::optional<rrc_meas_cfg> target_meas_cfg = manager->get_measurement_config(ue_index, target_nci, initial_meas_cfg);
  ASSERT_FALSE(target_meas_cfg.has_value());
}

TEST_F(cell_meas_manager_test, when_periodic_ntn_location_reports_are_stable_then_handover_is_requested)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const nr_cell_identity target_nci  = get_ntn_test_nci(1);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  ASSERT_TRUE(mobility_manager.ntn_events.empty());

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  ASSERT_TRUE(mobility_manager.ntn_events.empty());

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);
  ASSERT_EQ(mobility_manager.ntn_events.front().ue_index, ue_index);
  ASSERT_EQ(mobility_manager.ntn_events.front().serving_nci, serving_nci);
  ASSERT_EQ(mobility_manager.ntn_events.front().handover_attempt_id, 1);
  ASSERT_EQ(mobility_manager.ntn_events.front().target_beam_id, "CN-BEAM-0002");
  ASSERT_EQ(mobility_manager.ntn_events.front().target_nci, target_nci);
  ASSERT_EQ(mobility_manager.ntn_events.front().target_pci, 2);
  ASSERT_EQ(mobility_manager.ntn_events.front().consecutive_location_reports, 3);
  ASSERT_EQ(mobility_manager.ntn_events.front().served_beam_ids_snapshot.size(), 3);
  ASSERT_NE(std::find(mobility_manager.ntn_events.front().served_beam_ids_snapshot.begin(),
                      mobility_manager.ntn_events.front().served_beam_ids_snapshot.end(),
                      "CN-BEAM-0002"),
            mobility_manager.ntn_events.front().served_beam_ids_snapshot.end());
}

TEST_F(cell_meas_manager_test, when_ntn_target_beam_is_not_currently_served_then_handover_is_not_requested)
{
  create_ntn_location_manager();
  ASSERT_TRUE(manager->update_ntn_served_beams({"CN-BEAM-0001"}));

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));

  ASSERT_TRUE(mobility_manager.ntn_events.empty());
}

TEST_F(cell_meas_manager_test, when_multi_beam_target_is_active_and_neighbor_then_handover_is_requested)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(1);
  const nr_cell_identity target_nci  = get_ntn_test_nci(2);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 2.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 2.0, base_time + std::chrono::milliseconds(500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 2.0, base_time + std::chrono::milliseconds(1000)));

  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);
  ASSERT_EQ(mobility_manager.ntn_events.front().target_beam_id, "CN-BEAM-0003");
  ASSERT_EQ(mobility_manager.ntn_events.front().target_nci, target_nci);
  ASSERT_EQ(mobility_manager.ntn_events.front().served_beam_ids_snapshot,
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"}));
}

TEST_F(cell_meas_manager_test, when_ntn_served_beams_are_updated_then_mobility_manager_is_notified)
{
  create_ntn_location_manager();
  ASSERT_EQ(mobility_manager.ntn_served_beam_updates.size(), 1);
  ASSERT_EQ(mobility_manager.ntn_served_beam_updates.back(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"}));

  ASSERT_TRUE(manager->update_ntn_served_beams({"CN-BEAM-0001"}));

  ASSERT_EQ(mobility_manager.ntn_served_beam_updates.size(), 2);
  ASSERT_EQ(mobility_manager.ntn_served_beam_updates.back(), std::vector<std::string>({"CN-BEAM-0001"}));
}

TEST_F(cell_meas_manager_test, when_ntn_served_beam_update_is_unchanged_then_mobility_manager_is_not_notified)
{
  create_ntn_location_manager();
  ASSERT_EQ(mobility_manager.ntn_served_beam_updates.size(), 1);
  ASSERT_EQ(mobility_manager.ntn_served_beam_updates.back(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"}));

  ASSERT_TRUE(manager->update_ntn_served_beams({"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"}));

  ASSERT_EQ(mobility_manager.ntn_served_beam_updates.size(), 1);
}

TEST_F(cell_meas_manager_test, when_ntn_served_beam_update_only_reorders_then_pending_candidate_is_preserved)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  ASSERT_TRUE(mobility_manager.ntn_events.empty());

  ASSERT_TRUE(manager->update_ntn_served_beams({"CN-BEAM-0003", "CN-BEAM-0002", "CN-BEAM-0001"}));
  ASSERT_EQ(mobility_manager.ntn_served_beam_updates.size(), 1);

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);
  ASSERT_EQ(mobility_manager.ntn_events.back().target_beam_id, "CN-BEAM-0002");
}

TEST_F(cell_meas_manager_test, when_ntn_served_beam_set_changes_but_candidate_target_remains_then_candidate_is_preserved)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  ASSERT_TRUE(mobility_manager.ntn_events.empty());

  ASSERT_TRUE(manager->update_ntn_served_beams({"CN-BEAM-0002", "CN-BEAM-0003"}));

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);
  ASSERT_EQ(mobility_manager.ntn_events.back().target_beam_id, "CN-BEAM-0002");
}

TEST_F(cell_meas_manager_test, when_ntn_served_beam_set_changes_then_pending_candidate_is_reset)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  ASSERT_TRUE(mobility_manager.ntn_events.empty());

  ASSERT_TRUE(manager->update_ntn_served_beams({"CN-BEAM-0001"}));
  ASSERT_TRUE(manager->update_ntn_served_beams({"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"}));

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_TRUE(mobility_manager.ntn_events.empty());

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);
}

TEST_F(cell_meas_manager_test, when_orbit_selected_served_beams_are_applied_then_mobility_manager_is_notified)
{
  create_ntn_location_manager();

  const std::vector<ntn_beam_position> beams = {
      {"CN-BEAM-0001", get_ntn_test_nci(0), 0.0, 0.0, 50000.0, true},
      {"CN-BEAM-0002", get_ntn_test_nci(1), 0.0, 1.0, 50000.0, true}};

  const std::vector<ntn_served_beam_candidate> candidates =
      select_ntn_served_beam_candidates_by_elevation(beams, make_ecef(0.0, 1.0, 500000.0), 80.0, 1);
  ASSERT_EQ(candidates.size(), 1);
  ASSERT_EQ(candidates.front().beam_id, "CN-BEAM-0002");
  ASSERT_NEAR(candidates.front().elevation_deg, 90.0, 1e-6);

  ASSERT_TRUE(manager->update_ntn_served_beams({candidates.front().beam_id}));

  ASSERT_EQ(mobility_manager.ntn_served_beam_updates.size(), 2);
  ASSERT_EQ(mobility_manager.ntn_served_beam_updates.back(), std::vector<std::string>({"CN-BEAM-0002"}));
}

TEST_F(cell_meas_manager_test, when_ntn_handover_trigger_is_rejected_then_later_reports_can_retry)
{
  create_ntn_location_manager();
  mobility_manager.accept_ntn_handover = false;

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);

  mobility_manager.accept_ntn_handover = true;
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1500)));

  ASSERT_EQ(mobility_manager.ntn_events.size(), 2);
  ASSERT_EQ(mobility_manager.ntn_events.back().target_beam_id, "CN-BEAM-0002");
}

TEST_F(cell_meas_manager_test, when_ntn_handover_failure_is_reported_then_candidate_is_rebuilt_before_retry_timeout)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);

  manager->handle_ntn_handover_result(make_ntn_handover_result(
      mobility_manager.ntn_events.back(), false, ntn_handover_failure_cause::source_preparation_failed));

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2500)));

  ASSERT_EQ(mobility_manager.ntn_events.size(), 2);
  ASSERT_EQ(mobility_manager.ntn_events.back().handover_attempt_id, 2);
  ASSERT_EQ(mobility_manager.ntn_events.back().target_beam_id, "CN-BEAM-0002");
}

TEST_F(cell_meas_manager_test, when_ntn_handover_result_has_stale_attempt_id_then_it_is_ignored)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);
  ASSERT_EQ(mobility_manager.ntn_events.back().handover_attempt_id, 1);

  ntn_handover_result stale_result = make_ntn_handover_result(
      mobility_manager.ntn_events.back(), false, ntn_handover_failure_cause::source_preparation_failed);
  stale_result.context.handover_attempt_id = 0;
  manager->handle_ntn_handover_result(stale_result);

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2000)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2500)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(3000)));

  ASSERT_EQ(mobility_manager.ntn_events.size(), 2);
  ASSERT_EQ(mobility_manager.ntn_events.back().handover_attempt_id, 2);
}

TEST_F(cell_meas_manager_test, when_ntn_handover_result_is_repeated_then_it_is_only_handled_once)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);
  ASSERT_EQ(mobility_manager.ntn_events.back().handover_attempt_id, 1);

  ntn_handover_result repeated_result = make_ntn_handover_result(
      mobility_manager.ntn_events.back(), false, ntn_handover_failure_cause::source_preparation_failed);
  manager->handle_ntn_handover_result(repeated_result);
  manager->handle_ntn_handover_result(repeated_result);

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2500)));

  ASSERT_EQ(mobility_manager.ntn_events.size(), 2);
  ASSERT_EQ(mobility_manager.ntn_events.back().handover_attempt_id, 2);
}

TEST_F(cell_meas_manager_test, when_ntn_handover_trigger_is_accepted_then_it_is_not_repeated_before_retry_timeout)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2000)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2500)));

  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);
}

TEST_F(cell_meas_manager_test, when_ntn_handover_trigger_stays_pending_past_timeout_then_it_can_retry)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1500)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2000)));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2500)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(3000)));

  ASSERT_EQ(mobility_manager.ntn_events.size(), 2);
  ASSERT_EQ(mobility_manager.ntn_events.back().target_beam_id, "CN-BEAM-0002");
}

TEST_F(cell_meas_manager_test, when_ue_assistance_location_is_stable_for_ttt_then_handover_is_requested)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(
      ue_index, serving_nci, 1.0, base_time, ntn_ue_location_report_source::ue_assistance_info));
  manager->report_ue_location(make_ntn_location_report(ue_index,
                                                       serving_nci,
                                                       1.0,
                                                       base_time + std::chrono::milliseconds(500),
                                                       ntn_ue_location_report_source::ue_assistance_info));
  ASSERT_TRUE(mobility_manager.ntn_events.empty());

  manager->report_ue_location(make_ntn_location_report(ue_index,
                                                       serving_nci,
                                                       1.0,
                                                       base_time + std::chrono::milliseconds(1000),
                                                       ntn_ue_location_report_source::ue_assistance_info));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);
  ASSERT_EQ(mobility_manager.ntn_events.front().target_nci, get_ntn_test_nci(1));
}

TEST_F(cell_meas_manager_test, when_ntn_location_reports_have_large_gap_then_candidate_beam_is_reset)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity serving_nci = get_ntn_test_nci(0);
  const auto             base_time   = std::chrono::steady_clock::now();

  manager->report_ue_location(make_ntn_location_report(ue_index, serving_nci, 1.0, base_time));
  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1000)));
  ASSERT_TRUE(mobility_manager.ntn_events.empty());

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(1500)));
  ASSERT_TRUE(mobility_manager.ntn_events.empty());

  manager->report_ue_location(
      make_ntn_location_report(ue_index, serving_nci, 1.0, base_time + std::chrono::milliseconds(2000)));
  ASSERT_EQ(mobility_manager.ntn_events.size(), 1);
}

TEST_F(cell_meas_manager_test, when_ntn_location_report_is_too_inaccurate_then_it_is_ignored)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  ntn_ue_location_report report =
      make_ntn_location_report(ue_index, get_ntn_test_nci(0), 1.0, std::chrono::steady_clock::now());
  report.horizontal_accuracy_m = 1000.0;

  ASSERT_EQ(manager->report_ue_location(report), ntn_location_report_result::inaccurate);
  ASSERT_TRUE(mobility_manager.ntn_events.empty());
}

TEST_F(cell_meas_manager_test, when_ntn_location_report_is_accepted_then_result_is_reported)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  ntn_ue_location_report report =
      make_ntn_location_report(ue_index, get_ntn_test_nci(0), 1.0, std::chrono::steady_clock::now());

  ASSERT_EQ(manager->report_ue_location(report), ntn_location_report_result::accepted);
  ASSERT_TRUE(manager->get_last_ue_location_report(ue_index).has_value());
}

TEST_F(cell_meas_manager_test, when_ntn_location_mobility_is_enabled_then_rsrp_measurement_does_not_trigger_handover)
{
  create_ntn_location_manager();

  ue_index_t ue_index = ue_mng.add_ue(uint_to_du_index(0));
  ASSERT_TRUE(ue_mng.set_plmn(ue_index, plmn_identity::test_value()));

  const nr_cell_identity            serving_nci = get_ntn_test_nci(0);
  std::optional<rrc_meas_cfg>       meas_cfg    = manager->get_measurement_config(ue_index, serving_nci);
  ASSERT_TRUE(meas_cfg.has_value());
  const auto periodic_meas_it = std::find_if(meas_cfg->meas_id_to_add_mod_list.begin(),
                                             meas_cfg->meas_id_to_add_mod_list.end(),
                                             [](const rrc_meas_id_to_add_mod& meas_id) {
                                               return meas_id.report_cfg_id == uint_to_report_cfg_id(1);
                                             });
  ASSERT_NE(periodic_meas_it, meas_cfg->meas_id_to_add_mod_list.end());

  rrc_meas_results meas_results;
  meas_results.meas_id = periodic_meas_it->meas_id;

  rrc_meas_quant_results serving_quant_results;
  serving_quant_results.rsrp = 10;
  rrc_meas_result_serv_mo serving_mo;
  serving_mo.serv_cell_id = 0;
  serving_mo.meas_result_serving_cell.cell_results.results_ssb_cell = serving_quant_results;
  meas_results.meas_result_serving_mo_list.emplace(serving_mo.serv_cell_id, serving_mo);

  rrc_meas_quant_results neighbor_quant_results;
  neighbor_quant_results.rsrp = 90;
  rrc_meas_result_nr neighbor_result;
  neighbor_result.pci = 2;
  neighbor_result.cell_results.results_ssb_cell = neighbor_quant_results;
  meas_results.meas_result_neigh_cells.emplace().meas_result_list_nr.push_back(neighbor_result);

  manager->report_measurement(ue_index, meas_results);
  ASSERT_TRUE(mobility_manager.events.empty());
  ASSERT_TRUE(mobility_manager.ntn_events.empty());
}
