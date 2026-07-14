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

#include "cu_cp_test_environment.h"
#include "lib/cu_cp/cu_cp_impl.h"
#include "tests/test_doubles/e1ap/e1ap_test_message_validators.h"
#include "tests/test_doubles/f1ap/f1ap_test_message_validators.h"
#include "tests/test_doubles/ngap/ngap_test_message_validators.h"
#include "tests/unittests/cu_cp/test_helpers.h"
#include "tests/unittests/e1ap/common/e1ap_cu_cp_test_messages.h"
#include "tests/unittests/ngap/ngap_test_messages.h"
#include "srsran/cu_cp/cu_cp_command_handler.h"
#include "srsran/f1ap/ntn_ul_slot_resource_request.h"
#include "srsran/ran/gnb_id.h"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <utility>
#include <variant>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

nr_cell_identity make_default_env_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{411, 22}, sector_id).value();
}

ecef_coordinates_t make_ecef(double latitude_deg, double longitude_deg, double altitude_m)
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

ntn_location_mobility_config make_ntn_mobility_config()
{
  ntn_location_mobility_config cfg;
  cfg.enabled                       = true;
  cfg.served_beam_min_elevation_deg = 80.0;
  cfg.max_nof_served_beams          = 1;
  cfg.beams = {{"CN-BEAM-0001", make_default_env_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_default_env_nci(1), 0.0, 1.0, 50000.0, true}};
  return cfg;
}

ntn_location_mobility_config make_orbit_driven_ntn_mobility_config()
{
  ntn_location_mobility_config cfg = make_ntn_mobility_config();
  cfg.served_beam_min_elevation_deg = -90.0;
  cfg.satellite_state_update.source = ntn_satellite_state_source::circular_orbit;
  cfg.satellite_state_update.update_period = std::chrono::milliseconds{10};
  cfg.satellite_state_update.circular_altitude_m = 500000.0;
  cfg.satellite_state_update.circular_inclination_deg = 0.0;
  cfg.satellite_state_update.circular_epoch = std::chrono::system_clock::now();
  return cfg;
}

ntn_location_mobility_config make_multi_beam_hopping_ntn_mobility_config()
{
  ntn_location_mobility_config cfg;
  cfg.enabled                       = true;
  cfg.served_beam_min_elevation_deg = -90.0;
  cfg.max_nof_served_beams          = 3;
  cfg.beams = {{"CN-BEAM-0001", make_default_env_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_default_env_nci(1), 0.0, 1.0, 50000.0, true},
               {"CN-BEAM-0003", make_default_env_nci(2), 0.0, 2.0, 50000.0, true}};
  return cfg;
}

ntn_location_mobility_config make_rotating_multi_beam_hopping_ntn_mobility_config()
{
  ntn_location_mobility_config cfg;
  cfg.enabled                       = true;
  cfg.served_beam_min_elevation_deg = -90.0;
  cfg.max_nof_served_beams          = 2;
  cfg.served_beam_hopping_enabled   = true;
  cfg.beams = {{"CN-BEAM-0001", make_default_env_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_default_env_nci(1), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0003", make_default_env_nci(2), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0004", make_default_env_nci(3), 0.0, 0.0, 50000.0, true}};
  return cfg;
}

std::optional<unsigned> connect_du_for_ntn_beam_cells(cu_cp_test_environment&      env,
                                                      const std::vector<unsigned>& sector_ids,
                                                      gnb_du_id_t gnb_du_id = int_to_gnb_du_id(0x11))
{
  const std::optional<unsigned> du_idx = env.connect_new_du();
  if (!du_idx.has_value()) {
    return std::nullopt;
  }

  std::vector<test_helpers::served_cell_item_info> cells;
  cells.reserve(sector_ids.size());
  for (unsigned sector_id : sector_ids) {
    test_helpers::served_cell_item_info cell;
    cell.nci = make_default_env_nci(sector_id);
    cell.pci = sector_id + 1;
    cells.push_back(cell);
  }

  if (!env.run_f1_setup(du_idx.value(), gnb_du_id, cells)) {
    return std::nullopt;
  }
  return du_idx;
}

std::optional<unsigned> connect_du_for_ntn_beams(cu_cp_test_environment& env)
{
  return connect_du_for_ntn_beam_cells(env, {0, 1});
}

ntn_ue_location_report make_ntn_location_report(ue_index_t                         ue_index,
                                                nr_cell_identity                   serving_nci,
                                                std::chrono::steady_clock::time_point received_time)
{
  ntn_ue_location_report report;
  report.ue_index              = ue_index;
  report.serving_nci           = serving_nci;
  report.latitude_deg          = 0.0;
  report.longitude_deg         = 0.0;
  report.horizontal_accuracy_m = 25.0;
  report.received_time         = received_time;
  return report;
}

ntn_ue_location_report make_ntn_location_report(ue_index_t ue_index, nr_cell_identity serving_nci)
{
  return make_ntn_location_report(ue_index, serving_nci, std::chrono::steady_clock::now());
}

cu_cp_ntn_beam_status find_beam_status(const std::vector<cu_cp_ntn_beam_status>& beam_status,
                                       const std::string&                         beam_id)
{
  auto it = std::find_if(beam_status.begin(),
                         beam_status.end(),
                         [&beam_id](const cu_cp_ntn_beam_status& status) {
                           return status.beam_id == beam_id;
                         });
  EXPECT_NE(it, beam_status.end());
  return it != beam_status.end() ? *it : cu_cp_ntn_beam_status{};
}

struct connected_ngap_ntn_ue {
  unsigned     du_idx = 0;
  ue_index_t   ue_index = ue_index_t::invalid;
  ran_ue_id_t  ran_ue_id = ran_ue_id_t::invalid;
  amf_ue_id_t  amf_ue_id = amf_ue_id_t::invalid;
};

bool connect_cu_up_for_ue_admission(cu_cp_test_environment& env)
{
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  return cu_up_idx.has_value() && env.run_e1_setup(cu_up_idx.value());
}

std::optional<connected_ngap_ntn_ue> connect_ngap_ntn_ue(cu_cp_test_environment& env)
{
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  if (!du_idx.has_value()) {
    return std::nullopt;
  }
  if (!connect_cu_up_for_ue_admission(env)) {
    return std::nullopt;
  }

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  if (!env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601))) {
    return std::nullopt;
  }
  if (!env.authenticate_ue(du_idx.value(), du_ue_id, uint_to_amf_ue_id(0))) {
    return std::nullopt;
  }

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  if (ue_ctx == nullptr || !ue_ctx->cu_ue_id.has_value() || !ue_ctx->ran_ue_id.has_value() ||
      !ue_ctx->amf_ue_id.has_value()) {
    return std::nullopt;
  }

  connected_ngap_ntn_ue connected_ue;
  connected_ue.du_idx    = du_idx.value();
  connected_ue.ue_index  = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  connected_ue.ran_ue_id = ue_ctx->ran_ue_id.value();
  connected_ue.amf_ue_id = ue_ctx->amf_ue_id.value();
  return connected_ue;
}

cu_cp_impl_interface* get_cu_cp_impl(cu_cp_test_environment& env)
{
  return &static_cast<cu_cp_impl&>(env.get_cu_cp());
}

void assert_ngap_radio_cause(const ngap_cause_t& cause, ngap_cause_radio_network_t expected)
{
  ASSERT_TRUE(std::holds_alternative<ngap_cause_radio_network_t>(cause));
  ASSERT_EQ(std::get<ngap_cause_radio_network_t>(cause), expected);
}

void expect_and_ack_ntn_slot_update(cu_cp_test_environment&            env,
                                    unsigned                           du_idx,
                                    gnb_du_ue_f1ap_id_t                du_ue_id,
                                    gnb_cu_ue_f1ap_id_t                cu_ue_id,
                                    rnti_t                             crnti,
                                    f1ap_ntn_ul_slot_resource_request* decoded_request = nullptr)
{
  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));

  const auto& mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  ASSERT_TRUE(mod_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(mod_req->res_coordination_transfer_container);
  ASSERT_TRUE(slot_request.has_value());

  if (decoded_request != nullptr) {
    *decoded_request = *slot_request;
  }

  env.get_du(du_idx).push_ul_pdu(
      test_helpers::generate_ue_context_modification_response(du_ue_id, cu_ue_id, crnti, {}, {}, byte_buffer{}));
}

} // namespace

TEST(cu_cp_ntn_mobility_test, default_cu_cp_rejects_ntn_satellite_state_updates)
{
  cu_cp_test_environment env;

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_FALSE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_selects_runtime_served_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0002"}));
}

TEST(cu_cp_ntn_mobility_test, manual_update_drives_runtime_served_beam_without_auto_updater)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  for (unsigned i = 0; i != 20; ++i) {
    env.tick();
  }
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));
}

TEST(cu_cp_ntn_mobility_test, user_accessed_beam_receives_cucp_antenna_slot_after_pdu_session_setup)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  cu_cp_ntn_beam_status beam_status = find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::active);
  ASSERT_EQ(beam_status.nof_antenna_slots, 0U);
  ASSERT_EQ(beam_status.antenna_slot_period, 0U);
  ASSERT_EQ(beam_status.sr_slot_period, 0U);
  ASSERT_EQ(beam_status.srs_slot_period, 0U);

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  beam_status = find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.nof_ues, 1U);
  ASSERT_EQ(beam_status.nof_antenna_slots, 1U);
  ASSERT_EQ(beam_status.antenna_slot_index, 0U);
  ASSERT_EQ(beam_status.antenna_slot_period, 1U);
  ASSERT_EQ(beam_status.sr_slot_offset, 0U);
  ASSERT_EQ(beam_status.sr_slot_period, 1U);
  ASSERT_EQ(beam_status.srs_slot_offset, 0U);
  ASSERT_EQ(beam_status.srs_slot_period, 1U);

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
}

TEST(cu_cp_ntn_mobility_test, online_ntn_ue_loaded_beam_reconfiguration_carries_cucp_sr_srs_slot_request)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.sr_slot_period, 1U);
  ASSERT_EQ(beam_status.srs_slot_period, 1U);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));

  const auto& mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  ASSERT_TRUE(mod_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> decoded_slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(mod_req->res_coordination_transfer_container);
  ASSERT_TRUE(decoded_slot_request.has_value());
  ASSERT_TRUE(decoded_slot_request->sr_slot_offset.has_value());
  ASSERT_TRUE(decoded_slot_request->srs_slot_offset.has_value());
  ASSERT_TRUE(decoded_slot_request->sr_slot_period.has_value());
  ASSERT_TRUE(decoded_slot_request->srs_slot_period.has_value());
  ASSERT_EQ(*decoded_slot_request->sr_slot_offset, beam_status.sr_slot_offset);
  ASSERT_EQ(*decoded_slot_request->srs_slot_offset, beam_status.srs_slot_offset);
  ASSERT_EQ(*decoded_slot_request->sr_slot_period, beam_status.sr_slot_period);
  ASSERT_EQ(*decoded_slot_request->srs_slot_period, beam_status.srs_slot_period);
}

TEST(cu_cp_ntn_mobility_test, online_ntn_ue_reconfiguration_is_not_repeated_when_slot_request_is_unchanged)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));

  const auto& first_mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  ASSERT_TRUE(first_mod_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> first_slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(first_mod_req->res_coordination_transfer_container);
  ASSERT_TRUE(first_slot_request.has_value());
  ASSERT_TRUE(first_slot_request->sr_slot_offset.has_value());
  ASSERT_TRUE(first_slot_request->srs_slot_offset.has_value());
  ASSERT_TRUE(first_slot_request->sr_slot_period.has_value());
  ASSERT_TRUE(first_slot_request->srs_slot_period.has_value());

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_modification_response(
          du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601), {}, {}, byte_buffer{}));
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.1, 500000.0)));
  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::active);
  ASSERT_EQ(beam_status.sr_slot_period, 1U);
  ASSERT_EQ(beam_status.srs_slot_period, 1U);
  ASSERT_EQ(beam_status.sr_slot_offset, *first_slot_request->sr_slot_offset);
  ASSERT_EQ(beam_status.srs_slot_offset, *first_slot_request->srs_slot_offset);
  ASSERT_EQ(beam_status.sr_slot_period, *first_slot_request->sr_slot_period);
  ASSERT_EQ(beam_status.srs_slot_period, *first_slot_request->srs_slot_period);

  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, online_ntn_ue_reconfiguration_clears_slot_request_after_drb_release)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const rnti_t              crnti    = to_rnti(0x4601);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            crnti,
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_cp_e1ap_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_up_e1ap_id.has_value());

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));
  const auto& slot_mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  ASSERT_TRUE(slot_mod_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> first_slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(slot_mod_req->res_coordination_transfer_container);
  ASSERT_TRUE(first_slot_request.has_value());
  ASSERT_FALSE(is_empty(*first_slot_request));

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_modification_response(
          du_ue_id, ue_ctx->cu_ue_id.value(), crnti, {}, {}, byte_buffer{}));
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));

  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_release_command(
      uint_to_amf_ue_id(0), ue_ctx->ran_ue_id.value(), pdu_session_id_t::min));

  e1ap_message e1ap_pdu;
  ASSERT_TRUE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_bearer_context_release_command(e1ap_pdu));

  env.get_cu_up(cu_up_idx.value())
      .push_tx_pdu(generate_bearer_context_release_complete(ue_ctx->cu_cp_e1ap_id.value(),
                                                            ue_ctx->cu_up_e1ap_id.value()));
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_modification_response(
          du_ue_id, ue_ctx->cu_ue_id.value(), crnti));
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu));

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          du_ue_id,
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          generate_rrc_reconfiguration_complete_pdu(0, 8)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_release_response(ngap_pdu));

  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));

  const auto& clear_mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  ASSERT_TRUE(clear_mod_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> clear_slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(clear_mod_req->res_coordination_transfer_container);
  ASSERT_TRUE(clear_slot_request.has_value());
  ASSERT_TRUE(is_empty(*clear_slot_request));
}

TEST(cu_cp_ntn_mobility_test, initial_context_setup_for_ntn_user_loaded_beam_carries_cucp_sr_srs_slot_request)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(connect_cu_up_for_ue_admission(env));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), du_ue_id, amf_ue_id_t::min));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.sr_slot_period, 1U);
  ASSERT_EQ(beam_status.srs_slot_period, 1U);

  env.get_amf().push_tx_pdu(
      generate_valid_initial_context_setup_request_message(ue_ctx->amf_ue_id.value(), ue_ctx->ran_ue_id.value()));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request(f1ap_pdu));

  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> decoded_slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(decoded_slot_request.has_value());
  ASSERT_TRUE(decoded_slot_request->sr_slot_offset.has_value());
  ASSERT_TRUE(decoded_slot_request->srs_slot_offset.has_value());
  ASSERT_TRUE(decoded_slot_request->sr_slot_period.has_value());
  ASSERT_TRUE(decoded_slot_request->srs_slot_period.has_value());
  ASSERT_EQ(*decoded_slot_request->sr_slot_offset, beam_status.sr_slot_offset);
  ASSERT_EQ(*decoded_slot_request->srs_slot_offset, beam_status.srs_slot_offset);
  ASSERT_EQ(*decoded_slot_request->sr_slot_period, beam_status.sr_slot_period);
  ASSERT_EQ(*decoded_slot_request->srs_slot_period, beam_status.srs_slot_period);
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_keeps_candidate_until_du_capacity_later_becomes_available)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));
}

TEST(cu_cp_ntn_mobility_test, current_served_beams_are_cleared_when_no_du_can_keep_visible_beams_active)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ASSERT_TRUE(env.drop_du_connection(du_idx.value()));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());
}

TEST(cu_cp_ntn_mobility_test, unchanged_satellite_state_recomputes_placement_after_du_capacity_changes)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  const ecef_coordinates_t satellite = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ASSERT_TRUE(env.drop_du_connection(du_idx.value()));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0001").state, cu_cp_ntn_beam_assignment_state::candidate);
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_keeps_unsupported_du_beam_as_candidate)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beam_cells(env, {0}).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_selects_multi_beam_hopping_window)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_multi_beam_hopping_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beam_cells(env, {0, 1, 2}).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  std::vector<std::string> served_beam_ids = ntn_handler.get_current_ntn_served_beam_ids();
  ASSERT_EQ(served_beam_ids.size(), 3);
  ASSERT_EQ(served_beam_ids.front(), "CN-BEAM-0002");
  ASSERT_NE(std::find(served_beam_ids.begin(), served_beam_ids.end(), "CN-BEAM-0001"), served_beam_ids.end());
  ASSERT_NE(std::find(served_beam_ids.begin(), served_beam_ids.end(), "CN-BEAM-0003"), served_beam_ids.end());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 2.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0002", "CN-BEAM-0001"}));
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_rotates_multi_beam_hopping_window)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_rotating_multi_beam_hopping_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beam_cells(env, {0, 1, 2, 3}).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0004"}));

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0001").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_FALSE(find_beam_status(beam_status, "CN-BEAM-0001").in_hopping_window);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0002").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_FALSE(find_beam_status(beam_status, "CN-BEAM-0002").in_hopping_window);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0003").state, cu_cp_ntn_beam_assignment_state::active);
  ASSERT_TRUE(find_beam_status(beam_status, "CN-BEAM-0003").in_hopping_window);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0004").state, cu_cp_ntn_beam_assignment_state::active);
  ASSERT_TRUE(find_beam_status(beam_status, "CN-BEAM-0004").in_hopping_window);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_keeps_hopping_window_for_configured_dwell_updates)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_rotating_multi_beam_hopping_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_hopping_dwell_updates = 3;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beam_cells(env, {0, 1, 2, 3}).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0004"}));
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_exposes_only_active_beams_from_partial_multi_beam_plan)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_multi_beam_hopping_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beam_cells(env, {0, 1}).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  const std::vector<std::string> served_beam_ids = ntn_handler.get_current_ntn_served_beam_ids();
  ASSERT_EQ(served_beam_ids.size(), 2);
  ASSERT_EQ(served_beam_ids.front(), "CN-BEAM-0002");
  ASSERT_NE(std::find(served_beam_ids.begin(), served_beam_ids.end(), "CN-BEAM-0001"), served_beam_ids.end());
  ASSERT_EQ(std::find(served_beam_ids.begin(), served_beam_ids.end(), "CN-BEAM-0003"), served_beam_ids.end());

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0002").state, cu_cp_ntn_beam_assignment_state::active);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0001").state, cu_cp_ntn_beam_assignment_state::active);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0003").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0003").du_index, du_index_t::invalid);
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_retires_unloaded_beam_in_runtime_plan)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::active);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0002").state, cu_cp_ntn_beam_assignment_state::active);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0001").state, cu_cp_ntn_beam_assignment_state::inactive);
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0002"}));
}

TEST(cu_cp_ntn_mobility_test, orbit_driven_satellite_state_update_waits_for_du_capacity)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_orbit_driven_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids().size(), 1);
}

TEST(cu_cp_ntn_mobility_test, circular_orbit_config_drives_runtime_served_beam_updates)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_orbit_driven_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{100},
                             [&ntn_handler]() { return ntn_handler.get_current_ntn_served_beam_ids().size() == 1; },
                             false));

  const std::vector<std::string> served_beams = ntn_handler.get_current_ntn_served_beam_ids();
  ASSERT_EQ(served_beams.size(), 1);
  ASSERT_TRUE(served_beams.front() == "CN-BEAM-0001" || served_beams.front() == "CN-BEAM-0002");
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), served_beams.front()).state,
            cu_cp_ntn_beam_assignment_state::active);
}

TEST(cu_cp_ntn_mobility_test, satellite_state_and_ue_location_trigger_intra_du_ntn_handover)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = -90.0;
  params.ntn_location_mobility->max_nof_served_beams          = 2;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const std::vector<std::string> served_beams = ntn_handler.get_current_ntn_served_beam_ids();
  ASSERT_EQ(served_beams.size(), 2);
  ASSERT_NE(std::find(served_beams.begin(), served_beams.end(), "CN-BEAM-0001"), served_beams.end());
  ASSERT_NE(std::find(served_beams.begin(), served_beams.end(), "CN-BEAM-0002"), served_beams.end());

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ntn_ue_location_report report =
      make_ntn_location_report(uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value())),
                               make_default_env_nci(0),
                               std::chrono::steady_clock::now());
  report.longitude_deg = 1.0;
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(f1ap_pdu));

  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
}

TEST(cu_cp_ntn_mobility_test, satellite_state_and_ue_location_trigger_inter_du_ntn_handover_and_retry_after_failure)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = -90.0;
  params.ntn_location_mobility->max_nof_served_beams          = 2;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> source_du_idx =
      connect_du_for_ntn_beam_cells(env, {0}, int_to_gnb_du_id(0x11));
  ASSERT_TRUE(source_du_idx.has_value());
  const std::optional<unsigned> target_du_idx =
      connect_du_for_ntn_beam_cells(env, {1}, int_to_gnb_du_id(0x12));
  ASSERT_TRUE(target_du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const std::vector<std::string> served_beams = ntn_handler.get_current_ntn_served_beam_ids();
  ASSERT_EQ(served_beams.size(), 2);
  ASSERT_NE(std::find(served_beams.begin(), served_beams.end(), "CN-BEAM-0001"), served_beams.end());
  ASSERT_NE(std::find(served_beams.begin(), served_beams.end(), "CN-BEAM-0002"), served_beams.end());

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(source_du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(source_du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, source_du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const auto location_time = std::chrono::steady_clock::now();
  ntn_ue_location_report report =
      make_ntn_location_report(uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value())),
                               make_default_env_nci(0),
                               location_time);
  report.longitude_deg = 1.0;
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(target_du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(f1ap_pdu));

  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());

  env.get_du(target_du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
  const auto report_metrics = env.get_cu_cp().get_metrics_handler().request_metrics_report();
  ASSERT_EQ(report_metrics.ues.size(), 1);

  report.received_time = location_time + std::chrono::milliseconds{1};
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  f1ap_message retry_f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(target_du_idx.value(), retry_f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(retry_f1ap_pdu));

  const auto& retry_setup_req = retry_f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(retry_setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());

  env.get_du(target_du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(retry_setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
  const auto retry_report_metrics = env.get_cu_cp().get_metrics_handler().request_metrics_report();
  ASSERT_EQ(retry_report_metrics.ues.size(), 1);
}

TEST(cu_cp_ntn_mobility_test, ntn_enabled_rsrp_measurement_report_does_not_trigger_handover)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->du_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          ue_ctx->du_ue_id.value(),
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          make_byte_buffer("000800410004015f741fe0804bf183fcaa6e9699").value()));

  f1ap_message f1ap_pdu;
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, ntn_disabled_rsrp_measurement_report_still_triggers_handover)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility          = make_ntn_mobility_config();
  params.ntn_location_mobility->enabled = false;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->du_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          ue_ctx->du_ue_id.value(),
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          make_byte_buffer("000800410004015f741fe0804bf183fcaa6e9699").value()));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(f1ap_pdu));

  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_rejects_unknown_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index = uint_to_ue_index(42);

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);

  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::unknown_local_ue_ngap_id);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_rejects_when_amf_control_is_disabled)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->core_network_reporting.amf_control_enabled = false;
  cu_cp_test_environment env(std::move(params));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index = uint_to_ue_index(42);

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);

  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::unspecified);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_direct_rejects_when_no_accepted_location_is_available)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index               = ue->ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::direct;

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::unspecified);
}

TEST(cu_cp_ntn_mobility_test, accepted_ntn_location_report_is_forwarded_to_ngap_location_report)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->core_network_reporting.local_forwarding_enabled = true;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_TRUE(connect_cu_up_for_ue_admission(env));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), du_ue_id, uint_to_amf_ue_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue_index, make_default_env_nci(0)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));

  const auto& asn1_report = ngap_pdu.pdu.init_msg().value.location_report();
  ASSERT_EQ(asn1_report->ran_ue_ngap_id, ran_ue_id_to_uint(ue_ctx->ran_ue_id.value()));
  ASSERT_EQ(asn1_report->amf_ue_ngap_id, amf_ue_id_to_uint(ue_ctx->amf_ue_id.value()));
  ASSERT_EQ(asn1_report->location_report_request_type.event_type.value, asn1::ngap::event_type_opts::direct);
  ASSERT_EQ(asn1_report->user_location_info.type().value,
            asn1::ngap::user_location_info_c::types_opts::user_location_info_nr);

  const auto& nr_info = asn1_report->user_location_info.user_location_info_nr();
  ASSERT_TRUE(nr_info.ie_exts_present);
  ASSERT_TRUE(nr_info.ie_exts.nr_ntn_tai_info_present);
}

TEST(cu_cp_ntn_mobility_test, accepted_ntn_location_report_is_not_forwarded_when_serving_beam_is_not_active)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->core_network_reporting.local_forwarding_enabled = true;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(connect_cu_up_for_ue_admission(env));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), du_ue_id, uint_to_amf_ue_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue_index, make_default_env_nci(0)));

  ngap_message ngap_pdu;
  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));

  ngap_location_reporting_control control;
  control.ue_index               = ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::direct;
  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::unspecified);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_direct_sends_last_accepted_ntn_location)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_TRUE(connect_cu_up_for_ue_admission(env));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), du_ue_id, uint_to_amf_ue_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue_index, make_default_env_nci(0)));

  ngap_location_reporting_control control;
  control.ue_index               = ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::direct;

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_TRUE(response.accepted);

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));

  const auto& asn1_report = ngap_pdu.pdu.init_msg().value.location_report();
  ASSERT_EQ(asn1_report->ran_ue_ngap_id, ran_ue_id_to_uint(ue_ctx->ran_ue_id.value()));
  ASSERT_EQ(asn1_report->amf_ue_ngap_id, amf_ue_id_to_uint(ue_ctx->amf_ue_id.value()));
  ASSERT_EQ(asn1_report->location_report_request_type.event_type.value, asn1::ngap::event_type_opts::direct);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_direct_sends_last_location_while_serving_beam_is_draining)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0)));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::draining);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::draining);

  ngap_location_reporting_control control;
  control.ue_index               = ue->ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::direct;

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_TRUE(response.accepted);

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->location_report_request_type.event_type.value,
            asn1::ngap::event_type_opts::direct);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_change_of_serving_cell_reports_only_on_cell_change)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->max_nof_served_beams = 2;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());
  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const auto base_time = std::chrono::steady_clock::now();
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time));

  ngap_location_reporting_control control;
  control.ue_index               = ue->ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::change_of_serving_cell;

  ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_TRUE(response.accepted);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time + std::chrono::milliseconds{1}));

  ngap_message ngap_pdu;
  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));

  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(1), base_time + std::chrono::milliseconds{2}));

  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->ran_ue_ngap_id, ran_ue_id_to_uint(ue->ran_ue_id));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->amf_ue_ngap_id, amf_ue_id_to_uint(ue->amf_ue_id));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->location_report_request_type.event_type.value,
            asn1::ngap::event_type_opts::change_of_serve_cell);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(1), base_time + std::chrono::milliseconds{3}));

  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_change_of_serving_cell_uses_draining_report_as_baseline)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->max_nof_served_beams = 2;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const auto base_time = std::chrono::steady_clock::now();
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::draining);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::draining);

  ngap_location_reporting_control control;
  control.ue_index               = ue->ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::change_of_serving_cell;

  ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_TRUE(response.accepted);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(1), base_time + std::chrono::milliseconds{1}));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->location_report_request_type.event_type.value,
            asn1::ngap::event_type_opts::change_of_serve_cell);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(1), base_time + std::chrono::milliseconds{2}));
  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_stop_change_of_serving_cell_disables_reports)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());
  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const auto base_time = std::chrono::steady_clock::now();
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time));

  ngap_location_reporting_control control;
  control.ue_index               = ue->ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::change_of_serving_cell;
  ASSERT_TRUE(cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control).accepted);

  control.request_type.event_type = ngap_location_reporting_event_type::stop_change_of_serving_cell;
  ASSERT_TRUE(cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control).accepted);

  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(1), base_time + std::chrono::milliseconds{1}));

  ngap_message ngap_pdu;
  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_rejects_overlapping_area_of_interest_ref_id)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index                              = ue->ue_index;
  control.request_type.event_type               = ngap_location_reporting_event_type::ue_presence_in_area_of_interest;
  control.request_type.area_of_interest_ref_ids = {7};
  ASSERT_TRUE(cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control).accepted);

  control.request_type.area_of_interest_ref_ids = {7, 8};
  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::multiple_location_report_ref_id_instances);
}
