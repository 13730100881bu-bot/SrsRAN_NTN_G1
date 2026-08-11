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

#include "apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h"
#include "apps/units/flexible_o_du/o_du_high/du_high/du_high_config_cli11_schema.h"
#include "apps/units/flexible_o_du/o_du_high/du_high/du_high_config_translators.h"
#include "apps/units/flexible_o_du/o_du_high/du_high/du_high_config_validator.h"
#include "apps/units/flexible_o_du/o_du_high/du_high/du_high_config_yaml_writer.h"
#include "srsran/du/du_high/du_high_configuration.h"
#include "srsran/support/config_parsers.h"
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <string_view>
#include <yaml-cpp/yaml.h>

using namespace srsran;

namespace {

constexpr uint64_t test_nci = 0x123450001ULL;

du_high_unit_ntn_initial_ul_rx_mapping_config make_valid_mapping()
{
  du_high_unit_ntn_initial_ul_rx_mapping_config mapping;
  mapping.enabled          = true;
  mapping.version          = 7;
  mapping.hash             = "sha256:initial-ul-rx-mapping";
  mapping.unique_margin_db = 6.0F;
  mapping.entries.push_back({.nci              = test_nci,
                             .cell_local_port  = 1,
                             .backend          = "sdr",
                             .physical_rx_port = 2});
  mapping.entries.push_back({.nci              = test_nci,
                             .cell_local_port  = 3,
                             .backend          = "ofh",
                             .physical_rx_port = 4,
                             .prach_eaxc       = 5,
                             .beam_id          = 0x1234});
  return mapping;
}

du_high_unit_config make_default_config()
{
  du_high_unit_config config;
  CLI::App            app;
  autoderive_du_high_parameters_after_parsing(app, config);
  return config;
}

du_high_parsed_config parse_config(std::string_view yaml)
{
  du_high_parsed_config parsed;
  CLI::App              app;
  app.config_formatter(create_yaml_config_parser());
  configure_cli11_with_du_high_config_schema(app, parsed);
  std::istringstream input{std::string(yaml)};
  app.parse_from_stream(input);
  autoderive_du_high_parameters_after_parsing(app, parsed.config);
  return parsed;
}

} // namespace

TEST(du_high_ntn_initial_ul_rx_mapping_config, default_configuration_is_disabled)
{
  du_high_unit_config config = make_default_config();
  EXPECT_FALSE(config.ntn_initial_ul_rx_mapping.enabled);
  EXPECT_TRUE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping.entries.resize(1025U);
  EXPECT_FALSE(validate_du_high_config(config));
}

TEST(du_high_ntn_initial_ul_rx_mapping_config, accepts_sdr_and_ofh_entries)
{
  du_high_unit_config config = make_default_config();
  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  EXPECT_TRUE(validate_du_high_config(config));
}

TEST(du_high_ntn_initial_ul_rx_mapping_config, rejects_invalid_identity_limits_and_backend_fields)
{
  du_high_unit_config config = make_default_config();

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.version = 0;
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.hash.clear();
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.hash.assign(129, 'h');
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.unique_margin_db = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[0].nci = uint64_t{1} << 36U;
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[0].physical_rx_port = 255;
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[0].physical_rx_port = 254;
  EXPECT_TRUE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[0].backend = "invalid";
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[0].prach_eaxc = 1;
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[1].beam_id.reset();
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[1].beam_id = 0x8000;
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[1].prach_eaxc = 32;
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[1].prach_eaxc = 31;
  EXPECT_TRUE(validate_du_high_config(config));
}

TEST(du_high_ntn_initial_ul_rx_mapping_config, rejects_duplicate_receive_identities_per_nci)
{
  du_high_unit_config config = make_default_config();

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[1].cell_local_port = 1;
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  config.ntn_initial_ul_rx_mapping.entries[1].physical_rx_port = 2;
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  auto duplicate_ofh = config.ntn_initial_ul_rx_mapping.entries[1];
  duplicate_ofh.cell_local_port  = 6;
  duplicate_ofh.physical_rx_port = 7;
  duplicate_ofh.beam_id          = 0x1235;
  config.ntn_initial_ul_rx_mapping.entries.push_back(duplicate_ofh);
  EXPECT_FALSE(validate_du_high_config(config));

  config.ntn_initial_ul_rx_mapping = make_valid_mapping();
  duplicate_ofh = config.ntn_initial_ul_rx_mapping.entries[1];
  duplicate_ofh.cell_local_port  = 6;
  duplicate_ofh.physical_rx_port = 7;
  duplicate_ofh.prach_eaxc       = 6;
  config.ntn_initial_ul_rx_mapping.entries.push_back(duplicate_ofh);
  EXPECT_FALSE(validate_du_high_config(config));
}

TEST(du_high_ntn_initial_ul_rx_mapping_config, parses_yaml_and_translates_to_mac_configuration)
{
  du_high_parsed_config parsed = parse_config(R"yaml(
ntn_initial_ul_rx_mapping:
  enabled: true
  version: 9
  hash: sha256:deployment-mapping-v9
  unique_margin_db: 7.5
  entries:
    - nci: 4886691841
      cell_local_port: 1
      backend: sdr
      physical_rx_port: 2
    - nci: 4886691841
      cell_local_port: 3
      backend: ofh
      physical_rx_port: 4
      prach_eaxc: 5
      beam_id: 4660
)yaml");

  ASSERT_TRUE(validate_du_high_config(parsed.config));
  ASSERT_EQ(parsed.config.ntn_initial_ul_rx_mapping.entries.size(), 2U);

  srs_du::du_high_configuration translated;
  generate_du_high_config(translated, parsed.config);
  const mac_ntn_rx_mapping_config& mac_mapping = translated.ran.mac_cfg.ntn_initial_ul_rx_mapping;
  ASSERT_TRUE(mac_mapping.enabled);
  EXPECT_EQ(mac_mapping.version, 9U);
  EXPECT_EQ(mac_mapping.hash, "sha256:deployment-mapping-v9");
  EXPECT_FLOAT_EQ(mac_mapping.unique_margin_db, 7.5F);
  ASSERT_EQ(mac_mapping.entries.size(), 2U);
  EXPECT_EQ(mac_mapping.entries[0].nci.value(), test_nci);
  EXPECT_EQ(mac_mapping.entries[0].backend, mac_ntn_rx_backend::sdr);
  EXPECT_EQ(mac_mapping.entries[1].backend, mac_ntn_rx_backend::ofh);
  EXPECT_EQ(mac_mapping.entries[1].prach_eaxc, 5U);
  EXPECT_EQ(mac_mapping.entries[1].beam_id, 0x1234U);
}

TEST(du_high_ntn_initial_ul_rx_mapping_config, yaml_writer_preserves_backend_specific_fields)
{
  du_high_unit_config config = make_default_config();
  config.ntn_initial_ul_rx_mapping = make_valid_mapping();

  YAML::Node node;
  fill_du_high_config_in_yaml_schema(node, config);

  const YAML::Node mapping = node["ntn_initial_ul_rx_mapping"];
  ASSERT_TRUE(mapping.IsMap());
  EXPECT_TRUE(mapping["enabled"].as<bool>());
  EXPECT_EQ(mapping["version"].as<uint64_t>(), 7U);
  EXPECT_FLOAT_EQ(mapping["unique_margin_db"].as<float>(), 6.0F);
  ASSERT_EQ(mapping["entries"].size(), 2U);
  EXPECT_EQ(mapping["entries"][0]["backend"].as<std::string>(), "sdr");
  EXPECT_FALSE(mapping["entries"][0]["prach_eaxc"]);
  EXPECT_EQ(mapping["entries"][1]["prach_eaxc"].as<unsigned>(), 5U);
  EXPECT_EQ(mapping["entries"][1]["beam_id"].as<unsigned>(), 0x1234U);
}
