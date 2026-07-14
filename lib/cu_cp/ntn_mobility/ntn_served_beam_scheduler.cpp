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

#include "ntn_served_beam_scheduler.h"
#include <algorithm>
#include <utility>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

std::vector<std::string> get_visible_beam_membership_ids(const std::vector<ntn_served_beam_candidate>& candidates)
{
  std::vector<std::string> visible_beam_ids;
  visible_beam_ids.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    visible_beam_ids.push_back(candidate.beam_id);
  }
  std::sort(visible_beam_ids.begin(), visible_beam_ids.end());
  return visible_beam_ids;
}

void apply_hopping_window(ntn_served_beam_schedule& schedule, const std::vector<std::string>& beam_ids)
{
  schedule.beam_ids.clear();
  for (auto& candidate : schedule.candidates) {
    candidate.in_hopping_window = false;
  }

  for (const auto& beam_id : beam_ids) {
    auto it = std::find_if(schedule.candidates.begin(),
                           schedule.candidates.end(),
                           [&beam_id](const ntn_served_beam_candidate& candidate) {
                             return candidate.beam_id == beam_id;
                           });
    if (it == schedule.candidates.end()) {
      continue;
    }
    it->in_hopping_window = true;
    schedule.beam_ids.push_back(beam_id);
  }
}

} // namespace

bool ntn_served_beam_update_handler::update_ntn_served_beam_candidates(
    const std::vector<ntn_served_beam_candidate>& candidates)
{
  std::vector<std::string> beam_ids;
  beam_ids.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    if (candidate.in_hopping_window) {
      beam_ids.push_back(candidate.beam_id);
    }
  }
  return update_ntn_served_beams(beam_ids);
}

ntn_served_beam_scheduler::ntn_served_beam_scheduler(ntn_served_beam_scheduler_config cfg_) : cfg(std::move(cfg_)) {}

ntn_served_beam_schedule ntn_served_beam_scheduler::compute_schedule(const ecef_coordinates_t& satellite) const
{
  ntn_served_beam_schedule schedule;
  schedule.candidates = select_ntn_served_beam_candidates_by_elevation(
      cfg.beams, satellite, cfg.min_elevation_deg, 0);

  const unsigned window_size =
      cfg.max_nof_served_beams == 0 ? static_cast<unsigned>(schedule.candidates.size()) : cfg.max_nof_served_beams;
  const bool rotate_window = cfg.served_beam_hopping_enabled && window_size != 0 &&
                             schedule.candidates.size() > static_cast<size_t>(window_size);
  const unsigned window_start =
      rotate_window ? next_hopping_start_index % static_cast<unsigned>(schedule.candidates.size()) : 0;
  const unsigned nof_window_candidates =
      std::min(window_size, static_cast<unsigned>(schedule.candidates.size()));

  std::vector<std::string> window_beam_ids;
  window_beam_ids.reserve(nof_window_candidates);
  for (unsigned i = 0; i != nof_window_candidates; ++i) {
    const unsigned candidate_index =
        rotate_window ? (window_start + i) % static_cast<unsigned>(schedule.candidates.size()) : i;
    window_beam_ids.push_back(schedule.candidates[candidate_index].beam_id);
  }
  apply_hopping_window(schedule, window_beam_ids);

  std::vector<std::string> visible_beam_ids = get_visible_beam_membership_ids(schedule.candidates);
  schedule.changed = schedule.beam_ids != current_beam_ids || visible_beam_ids != current_visible_beam_ids;
  return schedule;
}

ntn_served_beam_schedule ntn_served_beam_scheduler::update_from_satellite_state(
    const ecef_coordinates_t&        satellite,
    ntn_served_beam_update_handler& update_handler)
{
  ntn_served_beam_schedule schedule = compute_schedule(satellite);
  std::vector<std::string> visible_beam_ids = get_visible_beam_membership_ids(schedule.candidates);
  if (visible_beam_ids != current_visible_beam_ids) {
    next_hopping_start_index       = 0;
    current_hopping_dwell_updates = 0;
    schedule                      = compute_schedule(satellite);
    visible_beam_ids              = get_visible_beam_membership_ids(schedule.candidates);
  }

  const unsigned window_size =
      cfg.max_nof_served_beams == 0 ? static_cast<unsigned>(schedule.candidates.size()) : cfg.max_nof_served_beams;
  const bool rotate_window = cfg.served_beam_hopping_enabled && window_size != 0 &&
                             schedule.candidates.size() > static_cast<size_t>(window_size);
  if (rotate_window && visible_beam_ids == current_visible_beam_ids && !current_beam_ids.empty()) {
    const unsigned dwell_updates = std::max(1U, cfg.served_beam_hopping_dwell_updates);
    if (current_hopping_dwell_updates >= dwell_updates) {
      next_hopping_start_index =
          (next_hopping_start_index + window_size) % static_cast<unsigned>(schedule.candidates.size());
      current_hopping_dwell_updates = 0;
      schedule                      = compute_schedule(satellite);
      visible_beam_ids              = get_visible_beam_membership_ids(schedule.candidates);
    } else {
      ++current_hopping_dwell_updates;
      apply_hopping_window(schedule, current_beam_ids);
      schedule.changed = false;
    }
  }

  if (!schedule.changed) {
    return schedule;
  }

  schedule.applied = update_handler.update_ntn_served_beam_candidates(schedule.candidates);
  if (schedule.applied) {
    current_beam_ids = schedule.beam_ids;
    current_visible_beam_ids = std::move(visible_beam_ids);

    if (rotate_window) {
      current_hopping_dwell_updates = 1;
    } else {
      next_hopping_start_index = 0;
      current_hopping_dwell_updates = 0;
    }
  }
  return schedule;
}
