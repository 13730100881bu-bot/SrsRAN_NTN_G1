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

#include "srsran/ntn/beam_hopping_controller.h"
#include "srsran/ntn/ta_calculator.h"
#include "srsran/srslog/srslog.h"
#include "srsran/support/srsran_assert.h"

using namespace srsran;
using namespace srsran::srs_ntn;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

beam_hopping_controller::beam_hopping_controller(config                cfg,
                                                 sib19_update_notifier& notifier,
                                                 srslog::basic_logger&  logger) :
  cfg_(std::move(cfg)), notifier_(notifier), logger_(logger)
{
  srsran_assert(cfg_.hop_table.n_active > 0, "Hopping table must have at least one active beam");
  srsran_assert(cfg_.hop_table.dwell_frames > 0, "dwell_frames must be at least 1");
  srsran_assert(!cfg_.grid.positions.empty(), "Beam position grid must not be empty");
}

// ---------------------------------------------------------------------------
// Satellite ephemeris update (may be called from any thread)
// ---------------------------------------------------------------------------

void beam_hopping_controller::update_satellite_ephemeris(const ecef_coordinates_t& sat_state,
                                                          slot_point                valid_at)
{
  std::lock_guard<std::mutex> lock(sat_mutex_);
  sat_state_       = sat_state;
  sat_valid_at_    = valid_at;
  sat_initialized_ = true;

  logger_.debug("NTN bhc: ephemeris updated at SFN={} slot={} pos=({:.0f},{:.0f},{:.0f})m",
                valid_at.sfn(), valid_at.slot_index(),
                sat_state.position_x, sat_state.position_y, sat_state.position_z);
}

// ---------------------------------------------------------------------------
// Per-frame slot tick (DU worker thread)
// ---------------------------------------------------------------------------

void beam_hopping_controller::on_slot_indication(slot_point sl)
{
  // Snapshot the satellite state under the mutex; release before any further work.
  ecef_coordinates_t sat_snap{};
  slot_point         sat_valid_snap{};
  {
    std::lock_guard<std::mutex> lock(sat_mutex_);
    if (!sat_initialized_) {
      return; // No ephemeris yet; nothing to do.
    }
    sat_snap       = sat_state_;
    sat_valid_snap = sat_valid_at_;
  }

  const uint32_t nof_slots_per_frame = sl.nof_slots_per_frame();
  const uint32_t dwell_slots         = cfg_.hop_table.dwell_frames * nof_slots_per_frame;

  // First ever call submits the current dwell immediately. Later ticks submit the next dwell, even if the tick arrives
  // a few slots after the exact dwell boundary.
  const bool     first_call = (pending_beam_id_ == INVALID_BEAM_POSITION_ID);
  const uint32_t next_dwell_count =
      ((sl.count() / dwell_slots) * dwell_slots + dwell_slots) % sl.nof_slots_per_hyper_system_frame();
  const slot_point target_slot = first_call ? sl : slot_point(sl.scs(), next_dwell_count);

  const uint16_t target_beam = cfg_.hop_table.get_active_beam(target_slot.sfn());

  if (target_beam == pending_beam_id_ && !first_call) {
    // Identical beam position — no update needed (should not happen with a proper 3-phase table).
    return;
  }

  // Extrapolate satellite position to the target slot.
  const ecef_coordinates_t sat_at_target = [&]() {
    const int    slot_diff = target_slot - sat_valid_snap;
    const double dt_s      = static_cast<double>(slot_diff) /
                             static_cast<double>(sl.nof_slots_per_subframe() * 1000u);
    ecef_coordinates_t s   = sat_snap;
    s.position_x += sat_snap.velocity_vx * dt_s;
    s.position_y += sat_snap.velocity_vy * dt_s;
    s.position_z += sat_snap.velocity_vz * dt_s;
    return s;
  }();

  // Build and submit the SIB-19 for the target beam position.
  const sib19_info sib19 = build_sib19(target_beam, target_slot, sat_at_target);
  notifier_.on_sib19_update(cfg_.nr_cgi, sib19, target_slot);
  pending_beam_id_ = target_beam;

  const beam_position_t& pos = cfg_.grid.get_position(target_beam);
  logger_.info("NTN bhc: beam hop id={} ({:.4f}°N,{:.4f}°E) → SFN={}",
               target_beam, pos.center_lat, pos.center_lon, target_slot.sfn());
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

sib19_info beam_hopping_controller::build_sib19(uint16_t                  beam_pos_id,
                                                 slot_point                valid_from,
                                                 const ecef_coordinates_t& sat) const
{
  const beam_position_t& pos = cfg_.grid.get_position(beam_pos_id);

  sib19_info sib19;

  // Geographic reference location of this beam position.
  sib19.ref_location = geodetic_coordinates_t{pos.center_lat, pos.center_lon, 0.0};

  // Coverage radius as distance threshold (50-metre steps per TS 38.331).
  sib19.distance_thres = pos.distance_threshold;

  // Epoch time: this SIB-19 becomes valid at the start of the dwell.
  sib19.epoch_time = epoch_time_t{valid_from.sfn(), valid_from.subframe_index()};

  // Cell-specific K_offset (constant for the service area).
  sib19.cell_specific_koffset = static_cast<uint16_t>(cfg_.cell_specific_koffset);

  // UL sync validity duration.
  sib19.ntn_ul_sync_validity_dur = cfg_.ntn_ul_sync_validity_dur
                                        ? std::optional<uint16_t>(static_cast<uint16_t>(*cfg_.ntn_ul_sync_validity_dur))
                                        : std::nullopt;

  // Timing advance: compute from beam centre and satellite ECEF state.
  const ta_calc_result ta = compute_beam_ta(
      geodetic_coordinates_t{pos.center_lat, pos.center_lon, 0.0}, sat, cfg_.gateway_location);
  sib19.ta_info = ta.ta;

  // Satellite ephemeris (ECEF state vector).
  sib19.ephemeris_info = ecef_coordinates_t{sat};

  return sib19;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<beam_hopping_controller>
srsran::srs_ntn::make_beam_hopping_controller(beam_hopping_controller::config cfg,
                                              sib19_update_notifier&          notifier,
                                              srslog::basic_logger&           logger)
{
  return std::make_unique<beam_hopping_controller>(std::move(cfg), notifier, logger);
}
