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

#pragma once

#include "beam_hopping_table.h"
#include "beam_position.h"
#include "srsran/ran/ntn.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/sib/system_info_config.h"
#include "srsran/ran/slot_point.h"
#include "srsran/srslog/srslog.h"
#include <mutex>
#include <optional>

namespace srsran {
namespace srs_ntn {

// ---------------------------------------------------------------------------
// SIB-19 update sink (implemented by the DU layer)
// ---------------------------------------------------------------------------

/// Abstract notifier called by beam_hopping_controller whenever the beam position changes
/// and a new SIB-19 PDU must be broadcast.
///
/// The DU-side implementation is responsible for:
///   1. Packing \p sib19 into a BCCH-DL-SCH byte buffer.
///   2. Calling du_configurator::handle_si_pdu_update() with slot = \p valid_from.
class sib19_update_notifier
{
public:
  virtual ~sib19_update_notifier() = default;

  /// Called when a new SIB-19 should be broadcast starting at \p valid_from.
  ///
  /// \param nr_cgi      NR-CGI of the logical cell whose SIB-19 must be updated.
  /// \param sib19       Content of the new SIB-19 message.
  /// \param valid_from  Radio slot at which this SIB-19 takes effect (= start of the new dwell).
  virtual void on_sib19_update(const nr_cell_global_id_t& nr_cgi,
                               const sib19_info&         sib19,
                               slot_point                valid_from) = 0;
};

// ---------------------------------------------------------------------------
// Beam hopping controller
// ---------------------------------------------------------------------------

/// Controls NTN beam hopping for a single logical cell (Mode B).
///
/// Responsibilities
/// ----------------
/// 1. Tracks the satellite's ECEF state (position + velocity), refreshed by external callers
///    (e.g., an orbit-propagator thread or the NTN ground-station interface).
/// 2. Receives a radio-frame tick via on_slot_indication() and detects dwell boundaries.
/// 3. At each dwell boundary, linearly extrapolates the satellite state to the next dwell,
///    computes the per-beam timing-advance (via ta_calculator), builds sib19_info, and
///    notifies the sib19_update_notifier to trigger a MAC SI PDU update.
///
/// Thread safety
/// -------------
/// update_satellite_ephemeris() may be called from a different thread than
/// on_slot_indication(); both are protected by an internal mutex.
///
/// Dwell scheduling
/// ------------------------
/// After the first immediate SIB-19 update, each tick pre-schedules the SIB-19 for the NEXT dwell derived from the
/// current slot count. This avoids relying on a timer tick landing exactly on the dwell boundary while still giving the
/// MAC roughly dwell_frames * 10 ms lead time before the beam hops.
class beam_hopping_controller
{
public:
  /// Static configuration (set once at construction).
  struct config {
    nr_cell_global_id_t                   nr_cgi;
    beam_position_grid                    grid;
    beam_hopping_table_t                  hop_table;
    /// K_offset broadcast in SIB-19 (constant for the whole service area).
    unsigned                              cell_specific_koffset = 0;
    /// ntn-UL-SyncValidityDuration broadcast in SIB-19 (seconds; must be a valid 3GPP enum
    /// value: 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 120, 180, 240, or 900).
    std::optional<unsigned>               ntn_ul_sync_validity_dur;
    /// Gateway geodetic position for feeder-link TA offset computation (optional).
    std::optional<geodetic_coordinates_t> gateway_location;
  };

  beam_hopping_controller(config                cfg,
                          sib19_update_notifier& notifier,
                          srslog::basic_logger&  logger);

  /// Update the satellite ECEF state.  May be called from any thread.
  ///
  /// \param sat_state  Satellite position (metres) and velocity (m/s) in ECEF.
  /// \param valid_at   Radio slot at which \p sat_state is valid.
  void update_satellite_ephemeris(const ecef_coordinates_t& sat_state, slot_point valid_at);

  /// Called once per radio frame by the DU cell's slot indication.
  /// Must be called from the DU worker thread.
  void on_slot_indication(slot_point current_slot);

private:
  /// Extrapolate the stored satellite state to \p target using a constant-velocity model.
  ecef_coordinates_t extrapolate_to(slot_point target) const;

  /// Build the sib19_info payload for beam \p beam_pos_id active from \p valid_from.
  sib19_info build_sib19(uint16_t           beam_pos_id,
                          slot_point         valid_from,
                          const ecef_coordinates_t& sat) const;

  // ---- state ----
  config                 cfg_;
  sib19_update_notifier& notifier_;
  srslog::basic_logger&  logger_;

  mutable std::mutex     sat_mutex_;
  ecef_coordinates_t     sat_state_{};
  slot_point             sat_valid_at_{};
  bool                   sat_initialized_ = false;

  /// beam_position_id of the most recently submitted SIB-19; INVALID_BEAM_POSITION_ID until
  /// the first submission.
  uint16_t pending_beam_id_ = INVALID_BEAM_POSITION_ID;
};

/// Factory function — preferred over direct construction.
std::unique_ptr<beam_hopping_controller>
make_beam_hopping_controller(beam_hopping_controller::config cfg,
                             sib19_update_notifier&          notifier,
                             srslog::basic_logger&           logger);

} // namespace srs_ntn
} // namespace srsran
