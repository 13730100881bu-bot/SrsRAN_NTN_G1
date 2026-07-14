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

#include "srsran/cu_cp/neighbor_cell_manager_config.h"
#include "fmt/format.h"
#include "srsran/ran/band_helper.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

nr_cell_identity make_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{0x19b, 32}, sector_id).value();
}

std::string make_neighbor_cell_json()
{
  return fmt::format(R"json(
{{
  "version": 1,
  "region": "test",
  "cells": [
    {{
      "nci": {},
      "satellite_id": "SAT-A",
      "gnb_id_bit_length": 32,
      "pci": 10,
      "band": 78,
      "ssb_arfcn": 632628,
      "ssb_scs": 30,
      "ssb_period": 20,
      "ssb_offset": 0,
      "ssb_duration": 1
    }},
    {{
      "nci": {},
      "satellite_id": "SAT-B",
      "gnb_id_bit_length": 32,
      "pci": 11,
      "band": 78,
      "ssb_arfcn": 632628,
      "ssb_scs": 30,
      "ssb_period": 20,
      "ssb_offset": 0,
      "ssb_duration": 1
    }}
  ],
  "xn_relations": [
    {{
      "source_satellite_id": "SAT-A",
      "target_satellite_id": "SAT-B",
      "bidirectional": true,
      "handover_allowed": true
    }}
  ],
  "relations": [
    {{
      "serving_nci": {},
      "neighbor_nci": {},
      "report_cfg_ids": [2],
      "handover_allowed": true
    }},
    {{
      "serving_nci": {},
      "neighbor_nci": {},
      "report_cfg_ids": [3],
      "handover_allowed": false
    }}
  ]
}}
)json",
                     make_nci(0).value(),
                     make_nci(1).value(),
                     make_nci(0).value(),
                     make_nci(1).value(),
                     make_nci(1).value(),
                     make_nci(0).value());
}

std::string make_neighbor_cell_json_without_xn_relation()
{
  return fmt::format(R"json(
{{
  "version": 1,
  "region": "test",
  "cells": [
    {{
      "nci": {},
      "satellite_id": "SAT-A",
      "gnb_id_bit_length": 32,
      "pci": 10,
      "band": 78,
      "ssb_arfcn": 632628,
      "ssb_scs": 30,
      "ssb_period": 20,
      "ssb_offset": 0,
      "ssb_duration": 1
    }},
    {{
      "nci": {},
      "satellite_id": "SAT-B",
      "gnb_id_bit_length": 32,
      "pci": 11,
      "band": 78,
      "ssb_arfcn": 632628,
      "ssb_scs": 30,
      "ssb_period": 20,
      "ssb_offset": 0,
      "ssb_duration": 1
    }}
  ],
  "xn_relations": [],
  "relations": [
    {{
      "serving_nci": {},
      "neighbor_nci": {},
      "report_cfg_ids": [2],
      "handover_allowed": true
    }}
  ]
}}
)json",
                     make_nci(0).value(),
                     make_nci(1).value(),
                     make_nci(0).value(),
                     make_nci(1).value());
}

rrc_report_cfg_nr make_dummy_report_cfg()
{
  rrc_event_trigger_cfg event_trigger;
  return rrc_report_cfg_nr{event_trigger};
}

} // namespace

TEST(neighbor_cell_manager_test, when_json_is_parsed_then_cells_and_relations_are_available)
{
  auto cfg = parse_neighbor_cell_info_json(make_neighbor_cell_json());
  ASSERT_TRUE(cfg.has_value()) << cfg.error();

  ASSERT_EQ(cfg->version, 1);
  ASSERT_EQ(cfg->region, "test");
  ASSERT_EQ(cfg->cells.size(), 2);
  ASSERT_EQ(cfg->xn_relations.size(), 1);
  ASSERT_EQ(cfg->relations.size(), 2);
  ASSERT_EQ(cfg->cells[0].satellite_id, "SAT-A");
  ASSERT_EQ(cfg->cells[1].satellite_id, "SAT-B");
  ASSERT_EQ(cfg->cells[1].serving_cell_cfg.nci, make_nci(1));
  ASSERT_TRUE(cfg->cells[1].serving_cell_cfg.pci.has_value());
  ASSERT_EQ(cfg->cells[1].serving_cell_cfg.pci.value(), 11);
  ASSERT_TRUE(cfg->cells[1].serving_cell_cfg.band.has_value());
  ASSERT_EQ(nr_band_to_uint(cfg->cells[1].serving_cell_cfg.band.value()), 78);
}

TEST(neighbor_cell_manager_test, when_neighbor_is_queried_by_serving_cell_and_pci_then_relation_is_found)
{
  auto cfg = parse_neighbor_cell_info_json(make_neighbor_cell_json());
  ASSERT_TRUE(cfg.has_value()) << cfg.error();

  neighbor_cell_manager manager(cfg.value());

  ASSERT_TRUE(manager.has_xn_handover_relation("SAT-A", "SAT-B"));
  ASSERT_TRUE(manager.has_xn_handover_relation("SAT-B", "SAT-A"));
  ASSERT_FALSE(manager.has_xn_handover_relation("SAT-A", "SAT-C"));

  auto neighbors = manager.get_neighbors(make_nci(0));
  ASSERT_EQ(neighbors.size(), 1);
  ASSERT_EQ(neighbors.front().neighbor_nci, make_nci(1));

  auto relation = manager.find_neighbor(make_nci(0), 11);
  ASSERT_TRUE(relation.has_value());
  ASSERT_EQ(relation->neighbor_nci, make_nci(1));

  ASSERT_FALSE(manager.find_neighbor(make_nci(0), 12).has_value());
}

TEST(neighbor_cell_manager_test, when_xn_relation_is_missing_then_cross_satellite_neighbor_is_not_available)
{
  auto cfg = parse_neighbor_cell_info_json(make_neighbor_cell_json_without_xn_relation());
  ASSERT_TRUE(cfg.has_value()) << cfg.error();

  neighbor_cell_manager manager(cfg.value());

  ASSERT_FALSE(manager.has_xn_handover_relation("SAT-A", "SAT-B"));
  ASSERT_TRUE(manager.get_neighbors(make_nci(0)).empty());
  ASSERT_FALSE(manager.find_neighbor(make_nci(0), 11).has_value());
}

TEST(neighbor_cell_manager_test, when_config_is_merged_then_measurement_neighbor_list_is_populated)
{
  auto cfg = parse_neighbor_cell_info_json(make_neighbor_cell_json());
  ASSERT_TRUE(cfg.has_value()) << cfg.error();

  cell_meas_manager_cfg meas_cfg;
  meas_cfg.report_config_ids.emplace(uint_to_report_cfg_id(2), make_dummy_report_cfg());
  meas_cfg.report_config_ids.emplace(uint_to_report_cfg_id(3), make_dummy_report_cfg());

  neighbor_cell_manager manager(cfg.value());
  auto                  error = manager.merge_into(meas_cfg);
  ASSERT_FALSE(error.has_value()) << error.value_or("");

  ASSERT_EQ(meas_cfg.cells.size(), 2);
  ASSERT_EQ(meas_cfg.cells.at(make_nci(0)).ncells.size(), 1);
  ASSERT_EQ(meas_cfg.cells.at(make_nci(0)).ncells.front().nci, make_nci(1));
  ASSERT_EQ(meas_cfg.cells.at(make_nci(0)).ncells.front().report_cfg_ids.front(), uint_to_report_cfg_id(2));
  ASSERT_TRUE(meas_cfg.cells.at(make_nci(1)).ncells.empty());
}

TEST(neighbor_cell_manager_test, when_xn_relation_is_missing_then_cross_satellite_relation_is_not_merged)
{
  auto cfg = parse_neighbor_cell_info_json(make_neighbor_cell_json_without_xn_relation());
  ASSERT_TRUE(cfg.has_value()) << cfg.error();

  cell_meas_manager_cfg meas_cfg;
  meas_cfg.report_config_ids.emplace(uint_to_report_cfg_id(2), make_dummy_report_cfg());

  neighbor_cell_manager manager(cfg.value());
  auto                  error = manager.merge_into(meas_cfg);
  ASSERT_FALSE(error.has_value()) << error.value_or("");

  ASSERT_EQ(meas_cfg.cells.size(), 2);
  ASSERT_TRUE(meas_cfg.cells.at(make_nci(0)).ncells.empty());
}

TEST(neighbor_cell_manager_test, when_relation_references_missing_report_config_then_merge_fails)
{
  auto cfg = parse_neighbor_cell_info_json(make_neighbor_cell_json());
  ASSERT_TRUE(cfg.has_value()) << cfg.error();

  cell_meas_manager_cfg  meas_cfg;
  neighbor_cell_manager  manager(cfg.value());
  std::optional<std::string> error = manager.merge_into(meas_cfg);

  ASSERT_TRUE(error.has_value());
}
