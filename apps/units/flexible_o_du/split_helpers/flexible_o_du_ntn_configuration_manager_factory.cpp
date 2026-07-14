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

#include "flexible_o_du_ntn_configuration_manager_factory.h"
#include "lib/du/du_high/du_manager/converters/asn1_sys_info_packer.h"
#include "srsran/du/du_high/du_manager/du_configurator.h"
#include "srsran/du/du_high/du_manager/du_manager.h"
#include "srsran/ntn/beam_hopping_controller.h"
#include "srsran/ntn/beam_hopping_table.h"
#include "srsran/ntn/ta_calculator.h"
#include "srsran/ntn/ntn_configuration_manager.h"
#include "srsran/ran/sib/system_info_config.h"
#include "srsran/srslog/srslog.h"
#include "srsran/support/timers.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

using namespace srsran;

#ifndef SRSRAN_HAS_ENTERPRISE_NTN

namespace {

static bool validate_beam_position_grid_config(const srs_ntn::beam_position_grid_config& cfg,
                                               srslog::basic_logger&                     logger)
{
  static constexpr double km_per_degree_lat = 111.0;
  static constexpr double pi                = 3.14159265358979323846;

  if (cfg.lat_north <= cfg.lat_south) {
    logger.warning("Discarding NTN beam hopping update. Cause: lat_north must be greater than lat_south");
    return false;
  }
  if (cfg.lon_east <= cfg.lon_west) {
    logger.warning("Discarding NTN beam hopping update. Cause: lon_east must be greater than lon_west");
    return false;
  }
  if (cfg.lat_south < -90.0 || cfg.lat_north > 90.0) {
    logger.warning("Discarding NTN beam hopping update. Cause: latitude bounds must be within [-90, 90] degrees");
    return false;
  }
  if (cfg.lon_west < -180.0 || cfg.lon_east > 180.0) {
    logger.warning("Discarding NTN beam hopping update. Cause: longitude bounds must be within [-180, 180] degrees");
    return false;
  }
  if (cfg.beam_radius_m <= 0.0) {
    logger.warning("Discarding NTN beam hopping update. Cause: beam_radius_m must be positive");
    return false;
  }
  if (cfg.overlap_factor <= 0.0 || cfg.overlap_factor > 1.0) {
    logger.warning("Discarding NTN beam hopping update. Cause: overlap_factor must be in the interval (0, 1]");
    return false;
  }

  const double d_km      = 2.0 * (cfg.beam_radius_m / 1000.0) * cfg.overlap_factor;
  const double delta_lat = d_km * (std::sqrt(3.0) / 2.0) / km_per_degree_lat;
  if (!std::isfinite(delta_lat) || delta_lat <= 0.0) {
    logger.warning("Discarding NTN beam hopping update. Cause: invalid beam spacing");
    return false;
  }

  const double n_rows_float = std::ceil((cfg.lat_north - cfg.lat_south) / delta_lat);
  if (n_rows_float <= 0.0 || n_rows_float > std::numeric_limits<uint16_t>::max()) {
    logger.warning("Discarding NTN beam hopping update. Cause: generated grid row count is out of range");
    return false;
  }

  const uint16_t n_rows = static_cast<uint16_t>(n_rows_float);
  uint64_t       total_positions = 0;
  for (uint16_t r = 0; r < n_rows; ++r) {
    const double lat_r   = cfg.lat_north - r * delta_lat;
    const double cos_lat = std::cos(lat_r * pi / 180.0);
    if (!std::isfinite(cos_lat) || std::abs(cos_lat) < 1e-6) {
      logger.warning("Discarding NTN beam hopping update. Cause: generated grid is too close to the geographic pole");
      return false;
    }

    const double delta_lon = d_km / (km_per_degree_lat * cos_lat);
    if (!std::isfinite(delta_lon) || delta_lon <= 0.0) {
      logger.warning("Discarding NTN beam hopping update. Cause: invalid generated longitude spacing");
      return false;
    }

    const double offset = (r % 2u == 1u) ? delta_lon / 2.0 : 0.0;
    const double span   = cfg.lon_east - cfg.lon_west - offset;
    if (span <= 0.0) {
      continue;
    }

    total_positions += static_cast<uint64_t>(std::floor(span / delta_lon) + 1.0);
    if (total_positions > std::numeric_limits<uint16_t>::max()) {
      logger.warning("Discarding NTN beam hopping update. Cause: generated grid exceeds {} beam positions",
                     std::numeric_limits<uint16_t>::max());
      return false;
    }
  }

  if (total_positions == 0) {
    logger.warning("Discarding NTN beam hopping update. Cause: generated grid has no beam positions");
    return false;
  }

  return true;
}

static bool validate_beam_hopping_table_inputs(const srs_ntn::beam_position_grid& grid,
                                               uint16_t                           n_active,
                                               uint16_t                           dwell_frames,
                                               srslog::basic_logger&              logger)
{
  if (n_active == 0 || n_active > srs_ntn::MAX_ACTIVE_BEAM_POSITIONS) {
    logger.warning("Discarding NTN beam hopping update. Cause: n_active={} is out of range", n_active);
    return false;
  }
  if (n_active % 3u != 0u) {
    logger.warning("Discarding NTN beam hopping update. Cause: n_active={} is not divisible by 3", n_active);
    return false;
  }
  if (dwell_frames == 0) {
    logger.warning("Discarding NTN beam hopping update. Cause: dwell_frames must be at least 1");
    return false;
  }

  const uint32_t cycle_frames = static_cast<uint32_t>(n_active) * static_cast<uint32_t>(dwell_frames);
  if (cycle_frames == 0 || cycle_frames > NOF_SFNS) {
    logger.warning("Discarding NTN beam hopping update. Cause: cycle_frames={} must not exceed NOF_SFNS={}",
                   cycle_frames,
                   NOF_SFNS);
    return false;
  }
  if (n_active > grid.positions.size()) {
    logger.warning("Discarding NTN beam hopping update. Cause: n_active={} exceeds generated beam positions={}",
                   n_active,
                   grid.positions.size());
    return false;
  }

  std::array<uint16_t, 3> color_counts = {};
  for (const auto& pos : grid.positions) {
    ++color_counts[pos.color];
  }

  const uint16_t per_color = n_active / 3u;
  for (unsigned color = 0; color != color_counts.size(); ++color) {
    if (color_counts[color] < per_color) {
      logger.warning("Discarding NTN beam hopping update. Cause: colour group {} has {} positions but {} are required",
                     color,
                     color_counts[color],
                     per_color);
      return false;
    }
  }

  return true;
}

class open_ntn_configuration_manager final : public srs_ntn::ntn_configuration_manager,
                                             public srs_ntn::sib19_update_notifier
{
public:
  open_ntn_configuration_manager(srs_ntn::ntn_configuration_manager_config cfg_,
                                 srs_du::du_configurator&                  du_cfgtr_,
                                 srs_du::du_manager_time_mapper_accessor&  du_time_mapper_accessor_,
                                 timer_manager&                            timers,
                                 task_executor&                            executor) :
    cfg(std::move(cfg_)),
    du_cfgtr(du_cfgtr_),
    du_time_mapper_accessor(du_time_mapper_accessor_),
    hopping_tick_timer(timers.create_unique_timer(executor)),
    logger(srslog::fetch_basic_logger("NTN"))
  {
  }

  bool handle_ntn_config_update(const srs_ntn::ntn_config_update_info& req) override
  {
    const auto cell_it = std::find_if(cfg.cells.begin(), cfg.cells.end(), [&req](const srs_ntn::ntn_cell_config& cell) {
      return cell.nr_cgi == req.nr_cgi;
    });
    if (cell_it == cfg.cells.end()) {
      logger.warning("Discarding NTN config update. Cause: No NTN cell with NR-CGI={} was found", req.nr_cgi.nci);
      return false;
    }

    if (req.stop_beam_hopping) {
      return stop_beam_hopping(req.nr_cgi);
    }

    std::optional<slot_point> tx_slot = du_time_mapper_accessor.get_time_mapper().get_slot_point(req.epoch_time);
    if (!tx_slot.has_value()) {
      logger.warning("Discarding NTN config update for NR-CGI={}. Cause: Slot-time mapping is not available",
                     req.nr_cgi.nci);
      return false;
    }

    const ecef_coordinates_t* sat_state = std::get_if<ecef_coordinates_t>(&req.ephemeris_info);
    if (req.beam_hopping.has_value()) {
      if (sat_state == nullptr) {
        logger.warning("Discarding NTN beam hopping update for NR-CGI={}. Cause: ECEF ephemeris is required",
                       req.nr_cgi.nci);
        return false;
      }
      return start_beam_hopping(*cell_it, req.beam_hopping.value(), *sat_state, tx_slot.value(), req);
    }

    if (sat_state != nullptr && update_active_beam_hopping_ephemeris(req.nr_cgi, *sat_state, tx_slot.value())) {
      logger.debug("Updated NTN beam hopping ephemeris for NR-CGI={} slot={}", req.nr_cgi.nci, tx_slot.value());
      return true;
    }

    sib19_info sib19 = make_sib19(*cell_it, req, tx_slot.value());
    submit_sib19_update(req.nr_cgi, cell_it->si_msg_idx, sib19, tx_slot.value());

    return true;
  }

  void on_sib19_update(const nr_cell_global_id_t& nr_cgi, const sib19_info& sib19, slot_point valid_from) override
  {
    std::optional<unsigned> si_msg_idx;
    {
      std::lock_guard<std::mutex> lock(hopping_mutex);
      auto                       session_it = hopping_sessions.find(nr_cgi);
      if (session_it != hopping_sessions.end()) {
        si_msg_idx = session_it->second.si_msg_idx;
      }
    }

    if (!si_msg_idx.has_value()) {
      logger.warning("Discarding generated NTN beam hopping SIB19 for NR-CGI={}. Cause: no active hopping session",
                     nr_cgi.nci);
      return;
    }

    submit_sib19_update(nr_cgi, si_msg_idx.value(), sib19, valid_from);
  }

private:
  void submit_sib19_update(const nr_cell_global_id_t& nr_cgi,
                           unsigned                   si_msg_idx,
                           const sib19_info&          sib19,
                           slot_point                 slot)
  {
    byte_buffer sib19_pdu = srs_du::asn1_packer::pack_sib19(sib19);

    std::vector<byte_buffer> si_messages;
    si_messages.push_back(std::move(sib19_pdu));

    srs_du::du_si_pdu_update_request du_req;
    du_req.nr_cgi      = nr_cgi;
    du_req.si_msg_idx  = si_msg_idx;
    du_req.sib_idx     = static_cast<unsigned>(sib_type::sib19);
    du_req.slot        = slot;
    du_req.si_messages = span<byte_buffer>(si_messages.data(), si_messages.size());
    du_cfgtr.handle_si_pdu_update(du_req);

    logger.info("Applied NTN SIB19 update for NR-CGI={} si_msg={} slot={}", nr_cgi.nci, si_msg_idx, slot);
  }

  sib19_info make_sib19(const srs_ntn::ntn_cell_config& cell,
                        const srs_ntn::ntn_config_update_info& req,
                        slot_point tx_slot) const
  {
    sib19_info sib19;

    sib19.distance_thres           = cell.ntn_cfg.distance_threshold;
    sib19.ref_location             = cell.ntn_cfg.reference_location;
    sib19.t_service                = cell.ntn_cfg.t_service;
    sib19.cell_specific_koffset    = static_cast<uint16_t>(cell.ntn_cfg.cell_specific_koffset);
    sib19.ephemeris_info           = req.ephemeris_info;
    sib19.epoch_time               = epoch_time_t{tx_slot.sfn(), tx_slot.subframe_index()};
    sib19.k_mac = cell.ntn_cfg.k_mac ? std::optional<uint16_t>(static_cast<uint16_t>(*cell.ntn_cfg.k_mac))
                                     : std::nullopt;
    sib19.ntn_ul_sync_validity_dur = static_cast<uint16_t>(req.ntn_ul_sync_validity_duration);
    sib19.polarization             = cell.ntn_cfg.polarization;
    sib19.ta_report                = cell.ntn_cfg.ta_report;

    if (req.ta_info.has_value()) {
      sib19.ta_info = req.ta_info;
      return sib19;
    }

    if (auto* sat_state = std::get_if<ecef_coordinates_t>(&req.ephemeris_info)) {
      const std::optional<geodetic_coordinates_t> gateway_location =
          req.ntn_gateway_location.has_value() ? req.ntn_gateway_location : cell.ntn_cfg.ntn_gateway_location;
      if (cell.ntn_cfg.reference_location.has_value()) {
        sib19.ta_info =
            srs_ntn::compute_beam_ta(cell.ntn_cfg.reference_location.value(), *sat_state, gateway_location).ta;
      }
    }

    return sib19;
  }

  bool start_beam_hopping(const srs_ntn::ntn_cell_config&          cell,
                          const srs_ntn::beam_hopping_update_info& hopping,
                          const ecef_coordinates_t&                sat_state,
                          slot_point                              valid_at,
                          const srs_ntn::ntn_config_update_info&  req)
  {
    if (!validate_beam_position_grid_config(hopping.grid, logger)) {
      return false;
    }

    srs_ntn::beam_position_grid grid = srs_ntn::make_beam_position_grid(hopping.grid);
    if (!validate_beam_hopping_table_inputs(grid, hopping.n_active, hopping.dwell_frames, logger)) {
      return false;
    }
    const size_t grid_positions = grid.positions.size();

    srs_ntn::beam_hopping_controller::config bh_cfg;
    bh_cfg.nr_cgi                   = cell.nr_cgi;
    bh_cfg.grid                     = std::move(grid);
    bh_cfg.hop_table                = srs_ntn::make_beam_hopping_table(
        bh_cfg.grid, hopping.n_active, hopping.dwell_frames);
    bh_cfg.cell_specific_koffset    = cell.ntn_cfg.cell_specific_koffset;
    bh_cfg.ntn_ul_sync_validity_dur = req.ntn_ul_sync_validity_duration;
    bh_cfg.gateway_location =
        req.ntn_gateway_location.has_value() ? req.ntn_gateway_location : cell.ntn_cfg.ntn_gateway_location;

    std::shared_ptr<srs_ntn::beam_hopping_controller> new_controller(
        srs_ntn::make_beam_hopping_controller(std::move(bh_cfg), *this, logger));

    {
      std::lock_guard<std::mutex> lock(hopping_mutex);
      beam_hopping_session& session = hopping_sessions[cell.nr_cgi];
      session.controller            = new_controller;
      session.si_msg_idx            = cell.si_msg_idx;
      session.last_tick_slot.reset();
    }

    new_controller->update_satellite_ephemeris(sat_state, valid_at);
    new_controller->on_slot_indication(valid_at);
    start_periodic_tick();

    logger.info("Started NTN beam hopping for NR-CGI={} active_beams={} dwell_frames={} grid_positions={}",
                cell.nr_cgi.nci,
                hopping.n_active,
                hopping.dwell_frames,
                grid_positions);
    return true;
  }

  bool stop_beam_hopping(const nr_cell_global_id_t& nr_cgi)
  {
    size_t removed = 0;
    {
      std::lock_guard<std::mutex> lock(hopping_mutex);
      removed = hopping_sessions.erase(nr_cgi);
    }

    if (removed == 0) {
      logger.info("NTN beam hopping stop requested for NR-CGI={}, but no active session was present", nr_cgi.nci);
      return true;
    }

    logger.info("Stopped NTN beam hopping for NR-CGI={}", nr_cgi.nci);
    return true;
  }

  bool update_active_beam_hopping_ephemeris(const nr_cell_global_id_t& nr_cgi,
                                            const ecef_coordinates_t&  sat_state,
                                            slot_point                 valid_at)
  {
    std::shared_ptr<srs_ntn::beam_hopping_controller> controller;
    {
      std::lock_guard<std::mutex> lock(hopping_mutex);
      auto session_it = hopping_sessions.find(nr_cgi);
      if (session_it == hopping_sessions.end()) {
        return false;
      }
      if (!session_it->second.controller) {
        return false;
      }
      controller = session_it->second.controller;
    }

    controller->update_satellite_ephemeris(sat_state, valid_at);
    controller->on_slot_indication(valid_at);
    return true;
  }

  void start_periodic_tick()
  {
    if (hopping_tick_started.exchange(true)) {
      return;
    }
    schedule_periodic_tick();
  }

  void schedule_periodic_tick()
  {
    hopping_tick_timer.set(std::chrono::milliseconds{1}, [this](timer_id_t) {
      on_periodic_tick();
      schedule_periodic_tick();
    });
    hopping_tick_timer.run();
  }

  void on_periodic_tick()
  {
    if (!has_active_beam_hopping_session()) {
      return;
    }

    std::optional<mac_cell_slot_time_info> last_mapping =
        du_time_mapper_accessor.get_time_mapper().get_last_mapping();
    if (!last_mapping.has_value()) {
      return;
    }

    std::vector<std::shared_ptr<srs_ntn::beam_hopping_controller>> controllers_to_tick;
    {
      std::lock_guard<std::mutex> lock(hopping_mutex);
      for (auto& session : hopping_sessions) {
        if (!session.second.controller) {
          continue;
        }
        if (session.second.last_tick_slot.has_value() &&
            session.second.last_tick_slot.value() == last_mapping->sl_tx) {
          continue;
        }
        session.second.last_tick_slot = last_mapping->sl_tx;
        controllers_to_tick.push_back(session.second.controller);
      }
    }

    for (const auto& controller : controllers_to_tick) {
      controller->on_slot_indication(last_mapping->sl_tx);
    }
  }

  bool has_active_beam_hopping_session() const
  {
    std::lock_guard<std::mutex> lock(hopping_mutex);
    for (const auto& session : hopping_sessions) {
      if (session.second.controller) {
        return true;
      }
    }
    return false;
  }

  struct beam_hopping_session {
    std::shared_ptr<srs_ntn::beam_hopping_controller> controller;
    unsigned                                          si_msg_idx = 0;
    std::optional<slot_point>                         last_tick_slot;
  };

  srs_ntn::ntn_configuration_manager_config cfg;
  srs_du::du_configurator&                  du_cfgtr;
  srs_du::du_manager_time_mapper_accessor&  du_time_mapper_accessor;
  unique_timer                              hopping_tick_timer;
  srslog::basic_logger&                     logger;
  mutable std::mutex                         hopping_mutex;
  std::map<nr_cell_global_id_t, beam_hopping_session> hopping_sessions;
  std::atomic<bool>                                      hopping_tick_started = false;
};

} // namespace

std::unique_ptr<srs_ntn::ntn_configuration_manager>
srsran::create_ntn_configuration_manager(const srs_ntn::ntn_configuration_manager_config& ntn_config,
                                         srs_du::du_configurator&                         du_cfgtr,
                                         srs_du::du_manager_time_mapper_accessor&         du_time_mapper_accessor,
                                         ru_controller&                                   ru_ctrl,
                                         timer_manager&                                   timers,
                                         task_executor&                                   executor)
{
  (void)ru_ctrl;
  return std::make_unique<open_ntn_configuration_manager>(
      ntn_config, du_cfgtr, du_time_mapper_accessor, timers, executor);
}

#endif // SRSRAN_HAS_ENTERPRISE_NTN
