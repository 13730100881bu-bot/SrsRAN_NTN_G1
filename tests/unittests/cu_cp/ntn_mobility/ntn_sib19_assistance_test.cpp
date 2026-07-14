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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "lib/cu_cp/ntn_mobility/ntn_sib19_assistance_builder.h"
#include "srsran/asn1/rrc_nr/sys_info.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

static nr_cell_identity make_nci(unsigned idx)
{
  return nr_cell_identity::create(gnb_id_t{411, 22}, idx).value();
}

static ecef_coordinates_t make_satellite()
{
  return {1.3, 2.6, 3.9, 0.24, 0.30, 0.36};
}

static ta_info_t make_ta_info()
{
  return {0.004072 * 100.0, 0.0002 * 50.0, 0.00002 * 7.0, 0.004072 * 10.0};
}

static ntn_assistance_beam_snapshot make_beam(std::string beam_id, ntn_assistance_beam_state state, unsigned idx)
{
  ntn_assistance_beam_snapshot beam;
  beam.beam_id                = std::move(beam_id);
  beam.nci                    = make_nci(idx);
  beam.state                  = state;
  beam.reference_location     = {0.0, 0.0, 0.0};
  beam.ta_info                = make_ta_info();
  beam.cell_specific_koffset  = 150;
  beam.k_mac                  = 64;
  beam.ul_sync_validity_s     = 120;
  beam.t_service              = 123456U;
  return beam;
}

static ntn_assistance_snapshot make_valid_assistance()
{
  ntn_assistance_snapshot assistance;
  assistance.valid          = true;
  assistance.invalid_reason = ntn_assistance_invalid_reason::none;
  assistance.satellite_ecef = make_satellite();
  assistance.beams.push_back(make_beam("CN-BEAM-0001", ntn_assistance_beam_state::active_loaded, 1));
  assistance.beams.push_back(make_beam("CN-BEAM-0002", ntn_assistance_beam_state::candidate, 2));
  assistance.beams.push_back(make_beam("CN-BEAM-0003", ntn_assistance_beam_state::draining, 3));
  return assistance;
}

static asn1::rrc_nr::sib19_r17_s pack_and_unpack(const asn1::rrc_nr::sib19_r17_s& sib19)
{
  byte_buffer pdu;
  asn1::bit_ref bref(pdu);
  EXPECT_EQ(sib19.pack(bref), asn1::SRSASN_SUCCESS);

  asn1::rrc_nr::sib19_r17_s decoded;
  asn1::cbit_ref            cbit(pdu);
  EXPECT_EQ(decoded.unpack(cbit), asn1::SRSASN_SUCCESS);
  return decoded;
}

TEST(ntn_sib19_assistance_builder, builds_valid_contract_entries_for_all_beam_states)
{
  ntn_sib19_assistance_request request;
  request.assistance = make_valid_assistance();
  request.epoch_time = epoch_time_t{512, 3};

  const ntn_sib19_assistance_snapshot contract = build_ntn_sib19_assistance_snapshot(request);

  ASSERT_TRUE(contract.valid);
  EXPECT_EQ(contract.invalid_reason, ntn_assistance_invalid_reason::none);
  ASSERT_EQ(contract.entries.size(), 3);
  EXPECT_EQ(contract.entries[0].beam_id, "CN-BEAM-0001");
  EXPECT_EQ(contract.entries[0].state, ntn_assistance_beam_state::active_loaded);
  EXPECT_EQ(contract.entries[1].state, ntn_assistance_beam_state::candidate);
  EXPECT_EQ(contract.entries[2].state, ntn_assistance_beam_state::draining);
  EXPECT_TRUE(contract.entries[0].epoch_time.has_value());
  EXPECT_EQ(contract.entries[0].epoch_time->sfn, 512U);
  EXPECT_EQ(contract.entries[0].epoch_time->subframe_number, 3U);
}

TEST(ntn_sib19_assistance_builder, invalid_assistance_produces_no_broadcastable_entries)
{
  ntn_assistance_snapshot assistance;
  assistance.valid          = false;
  assistance.invalid_reason = ntn_assistance_invalid_reason::stale_satellite_state;
  assistance.beams.push_back(make_beam("CN-BEAM-0001", ntn_assistance_beam_state::active_loaded, 1));

  ntn_sib19_assistance_request request;
  request.assistance = assistance;

  const ntn_sib19_assistance_snapshot contract = build_ntn_sib19_assistance_snapshot(request);

  ASSERT_FALSE(contract.valid);
  EXPECT_EQ(contract.invalid_reason, ntn_assistance_invalid_reason::stale_satellite_state);
  EXPECT_TRUE(contract.entries.empty());
}

TEST(ntn_sib19_assistance_builder, max_entries_bounds_only_contract_output)
{
  ntn_sib19_assistance_request request;
  request.assistance  = make_valid_assistance();
  request.max_entries = 2;

  const ntn_sib19_assistance_snapshot contract = build_ntn_sib19_assistance_snapshot(request);

  ASSERT_TRUE(contract.valid);
  ASSERT_EQ(contract.entries.size(), 2);
  EXPECT_EQ(request.assistance.beams.size(), 3);
}

TEST(ntn_sib19_assistance_converter, packs_and_decodes_sib19_r17_fields)
{
  ntn_sib19_assistance_request request;
  request.assistance = make_valid_assistance();
  request.epoch_time = epoch_time_t{17, 4};

  const ntn_sib19_assistance_snapshot contract = build_ntn_sib19_assistance_snapshot(request);
  ASSERT_TRUE(contract.valid);
  ASSERT_FALSE(contract.entries.empty());

  const expected<asn1::rrc_nr::sib19_r17_s, std::string> sib19_result =
      make_asn1_rrc_sib19_from_ntn_assistance(contract.entries.front());
  ASSERT_TRUE(sib19_result.has_value()) << sib19_result.error();

  const asn1::rrc_nr::sib19_r17_s decoded = pack_and_unpack(sib19_result.value());

  ASSERT_TRUE(decoded.ntn_cfg_r17_present);
  EXPECT_TRUE(decoded.t_service_r17_present);
  EXPECT_EQ(decoded.t_service_r17, 123456U);
  ASSERT_EQ(decoded.ref_location_r17.length(), 11U);
  EXPECT_EQ(*decoded.ref_location_r17.begin(), 0x03U);

  const asn1::rrc_nr::ntn_cfg_r17_s& cfg = decoded.ntn_cfg_r17;
  ASSERT_TRUE(cfg.epoch_time_r17_present);
  EXPECT_EQ(cfg.epoch_time_r17.sfn_r17, 17U);
  EXPECT_EQ(cfg.epoch_time_r17.sub_frame_nr_r17, 4U);
  ASSERT_TRUE(cfg.cell_specific_koffset_r17_present);
  EXPECT_EQ(cfg.cell_specific_koffset_r17, 150U);
  ASSERT_TRUE(cfg.kmac_r17_present);
  EXPECT_EQ(cfg.kmac_r17, 64U);
  ASSERT_TRUE(cfg.ntn_ul_sync_validity_dur_r17_present);
  EXPECT_EQ(cfg.ntn_ul_sync_validity_dur_r17.to_number(), 120U);
  ASSERT_TRUE(cfg.ta_info_r17_present);
  EXPECT_EQ(cfg.ta_info_r17.ta_common_r17, 110U);
  EXPECT_EQ(cfg.ta_info_r17.ta_common_drift_r17, 50);
  EXPECT_EQ(cfg.ta_info_r17.ta_common_drift_variant_r17, 7U);
  ASSERT_TRUE(cfg.ephemeris_info_r17_present);
  const asn1::rrc_nr::position_velocity_r17_s& pv = cfg.ephemeris_info_r17.position_velocity_r17();
  EXPECT_EQ(pv.position_x_r17, 1);
  EXPECT_EQ(pv.position_y_r17, 2);
  EXPECT_EQ(pv.position_z_r17, 3);
  EXPECT_EQ(pv.velocity_vx_r17, 4);
  EXPECT_EQ(pv.velocity_vy_r17, 5);
  EXPECT_EQ(pv.velocity_vz_r17, 6);
}

TEST(ntn_sib19_assistance_converter, does_not_fabricate_epoch_time_without_sfn_input)
{
  ntn_sib19_assistance_request request;
  request.assistance = make_valid_assistance();

  const ntn_sib19_assistance_snapshot contract = build_ntn_sib19_assistance_snapshot(request);
  ASSERT_TRUE(contract.valid);

  const expected<asn1::rrc_nr::sib19_r17_s, std::string> sib19_result =
      make_asn1_rrc_sib19_from_ntn_assistance(contract.entries.front());
  ASSERT_TRUE(sib19_result.has_value()) << sib19_result.error();

  const asn1::rrc_nr::sib19_r17_s decoded = pack_and_unpack(sib19_result.value());
  ASSERT_TRUE(decoded.ntn_cfg_r17_present);
  EXPECT_FALSE(decoded.ntn_cfg_r17.epoch_time_r17_present);
}

TEST(ntn_sib19_assistance_converter, rejects_unsupported_and_out_of_range_values_without_fatal_error)
{
  ntn_sib19_assistance_request request;
  request.assistance = make_valid_assistance();
  ntn_sib19_assistance_snapshot contract = build_ntn_sib19_assistance_snapshot(request);
  ASSERT_TRUE(contract.valid);
  ASSERT_FALSE(contract.entries.empty());

  ntn_sib19_assistance_entry invalid_ul_sync = contract.entries.front();
  invalid_ul_sync.ul_sync_validity_s         = 7;
  EXPECT_FALSE(make_asn1_rrc_sib19_from_ntn_assistance(invalid_ul_sync).has_value());

  ntn_sib19_assistance_entry invalid_koffset = contract.entries.front();
  invalid_koffset.cell_specific_koffset      = 2000;
  EXPECT_FALSE(make_asn1_rrc_sib19_from_ntn_assistance(invalid_koffset).has_value());

  ntn_sib19_assistance_entry invalid_kmac = contract.entries.front();
  invalid_kmac.k_mac                     = 999;
  EXPECT_FALSE(make_asn1_rrc_sib19_from_ntn_assistance(invalid_kmac).has_value());

  ntn_sib19_assistance_entry invalid_ephemeris = contract.entries.front();
  invalid_ephemeris.satellite_ecef->position_x = 1.0e12;
  EXPECT_FALSE(make_asn1_rrc_sib19_from_ntn_assistance(invalid_ephemeris).has_value());
}
