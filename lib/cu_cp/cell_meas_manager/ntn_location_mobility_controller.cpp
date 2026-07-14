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

#include "ntn_location_mobility_controller.h"
#include "cell_meas_manager_impl.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

constexpr double earth_radius_m = 6371000.0;
constexpr double pi             = 3.14159265358979323846;

double deg_to_rad(double value)
{
  return value * pi / 180.0;
}

double normalized_longitude_delta_deg(double lhs, double rhs)
{
  double delta = std::fmod(lhs - rhs + 540.0, 360.0);
  if (delta < 0.0) {
    delta += 360.0;
  }
  return delta - 180.0;
}

double distance_m(double latitude_deg, double longitude_deg, const ntn_beam_position& beam)
{
  const double lat1 = deg_to_rad(latitude_deg);
  const double lat2 = deg_to_rad(beam.center_latitude_deg);
  const double dlat = deg_to_rad(beam.center_latitude_deg - latitude_deg);
  const double dlon = deg_to_rad(normalized_longitude_delta_deg(beam.center_longitude_deg, longitude_deg));

  const double sin_dlat = std::sin(dlat / 2.0);
  const double sin_dlon = std::sin(dlon / 2.0);
  const double a        = sin_dlat * sin_dlat + std::cos(lat1) * std::cos(lat2) * sin_dlon * sin_dlon;
  const double c        = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
  return earth_radius_m * c;
}

bool is_valid_location(const ntn_ue_location_report& report)
{
  return report.ue_index != ue_index_t::invalid && std::isfinite(report.latitude_deg) &&
         std::isfinite(report.longitude_deg) && report.latitude_deg >= -90.0 && report.latitude_deg <= 90.0 &&
         report.longitude_deg >= -180.0 && report.longitude_deg <= 180.0;
}

int source_priority(ntn_ue_location_report_source source)
{
  return source == ntn_ue_location_report_source::ue_assistance_info ? 2 : 1;
}

bool is_neighbor(const cell_meas_manager_cfg& cfg, nr_cell_identity serving_nci, nr_cell_identity target_nci)
{
  const auto cell_it = cfg.cells.find(serving_nci);
  if (cell_it == cfg.cells.end()) {
    return false;
  }

  return std::any_of(cell_it->second.ncells.begin(),
                     cell_it->second.ncells.end(),
                     [target_nci](const neighbor_cell_meas_config& ncell) { return ncell.nci == target_nci; });
}

struct ntn_beam_match {
  std::string      beam_id;
  nr_cell_identity target_nci = nr_cell_identity::min();
  double           distance_m = std::numeric_limits<double>::max();
  double           margin_m   = -std::numeric_limits<double>::max();
};

std::optional<ntn_beam_match> find_best_target_beam(const std::vector<const ntn_beam_position*>& served_beams,
                                                    const ntn_ue_location_report&                report)
{
  std::optional<ntn_beam_match> best_match;
  for (const auto* beam : served_beams) {
    if (beam == nullptr || !beam->enabled || beam->nci == report.serving_nci) {
      continue;
    }

    const double beam_distance_m = distance_m(report.latitude_deg, report.longitude_deg, *beam);
    const double margin_m        = beam->coverage_radius_m - beam_distance_m;
    if (margin_m < 0.0) {
      continue;
    }

    if (!best_match.has_value() || margin_m > best_match->margin_m) {
      best_match.emplace();
      best_match->beam_id    = beam->beam_id;
      best_match->target_nci = beam->nci;
      best_match->distance_m = beam_distance_m;
      best_match->margin_m   = margin_m;
    }
  }

  return best_match;
}

bool serving_beam_still_covers_ue(const std::vector<const ntn_beam_position*>& served_beams,
                                  const ntn_location_mobility_config&          ntn_cfg,
                                  const ntn_ue_location_report&                report)
{
  for (const auto* beam : served_beams) {
    if (beam == nullptr || !beam->enabled || beam->nci != report.serving_nci) {
      continue;
    }

    if (distance_m(report.latitude_deg, report.longitude_deg, *beam) <=
        beam->coverage_radius_m + ntn_cfg.boundary_hysteresis_m) {
      return true;
    }
  }

  return false;
}

unsigned get_required_consecutive_location_reports(const ntn_location_mobility_config& ntn_cfg,
                                                   ntn_ue_location_report_source       source)
{
  unsigned required_location_reports = std::max(1U, ntn_cfg.required_consecutive_location_reports);
  if (source == ntn_ue_location_report_source::measurement_report &&
      ntn_cfg.measurement_report_period.count() > 0 && ntn_cfg.time_to_trigger.count() > 0) {
    const auto ttt_ms    = ntn_cfg.time_to_trigger.count();
    const auto period_ms = ntn_cfg.measurement_report_period.count();
    // The first report creates the candidate at t0; ceil(TTT / period) only counts the following report intervals.
    required_location_reports =
        std::max<unsigned>(required_location_reports, (ttt_ms + period_ms - 1) / period_ms + 1);
  }

  return required_location_reports;
}

bool is_candidate_ready(const ntn_location_mobility_config& ntn_cfg,
                        const ntn_candidate_beam_state&     state,
                        const ntn_ue_location_report&       report)
{
  if (state.consecutive_location_reports < get_required_consecutive_location_reports(ntn_cfg, report.source)) {
    return false;
  }

  return ntn_cfg.time_to_trigger.count() == 0 ||
         report.received_time - state.candidate_since >= ntn_cfg.time_to_trigger;
}

bool has_same_beam_membership(const std::vector<std::string>& lhs, const std::vector<std::string>& rhs)
{
  if (lhs.size() != rhs.size()) {
    return false;
  }

  std::unordered_set<std::string> lhs_ids;
  lhs_ids.reserve(lhs.size());
  for (const auto& beam_id : lhs) {
    lhs_ids.insert(beam_id);
  }

  for (const auto& beam_id : rhs) {
    if (lhs_ids.count(beam_id) == 0) {
      return false;
    }
  }
  return true;
}

bool should_clear_candidate_for_served_beam_update(const std::optional<ntn_candidate_beam_state>& candidate,
                                                   const std::unordered_set<std::string>& new_served_beam_ids)
{
  if (!candidate.has_value()) {
    return false;
  }
  return new_served_beam_ids.count(candidate->target_beam_id) == 0;
}

const char* ntn_handover_failure_cause_to_string(ntn_handover_failure_cause cause)
{
  switch (cause) {
    case ntn_handover_failure_cause::none:
      return "none";
    case ntn_handover_failure_cause::source_preparation_failed:
      return "source_preparation_failed";
    case ntn_handover_failure_cause::target_ue_removed:
      return "target_ue_removed";
    case ntn_handover_failure_cause::target_reconfiguration_timeout:
      return "target_reconfiguration_timeout";
    case ntn_handover_failure_cause::target_security_context_missing:
      return "target_security_context_missing";
    case ntn_handover_failure_cause::target_bearer_context_modification_failed:
      return "target_bearer_context_modification_failed";
  }
  return "unknown";
}

} // namespace

ntn_location_mobility_controller::ntn_location_mobility_controller(
    cell_meas_manager_cfg&                cfg_,
    cell_meas_mobility_manager_notifier& mobility_mng_notifier_,
    ue_manager&                          ue_mng_,
    srslog::basic_logger&                logger_) :
  cfg(cfg_), mobility_mng_notifier(mobility_mng_notifier_), ue_mng(ue_mng_), logger(logger_)
{
}

void ntn_location_mobility_controller::rebuild_beam_lookup()
{
  beam_lookup.clear();
  for (const auto& beam : cfg.ntn_location_mobility.beams) {
    beam_lookup.emplace(beam.beam_id, &beam);
  }

  std::vector<const ntn_beam_position*> resolved_beams;
  if (!current_served_beam_ids.empty() && resolve_served_beams(current_served_beam_ids, resolved_beams)) {
    current_served_beams = std::move(resolved_beams);
  } else if (!current_served_beam_ids.empty()) {
    current_served_beam_ids.clear();
    current_served_beams.clear();
  }
}

bool ntn_location_mobility_controller::resolve_served_beams(
    const std::vector<std::string>&        beam_ids,
    std::vector<const ntn_beam_position*>& resolved_beams) const
{
  resolved_beams.clear();
  resolved_beams.reserve(beam_ids.size());

  std::unordered_set<std::string> seen_beam_ids;
  for (const auto& beam_id : beam_ids) {
    if (beam_id.empty()) {
      logger.warning("Ignoring NTN served beam update with empty beam id");
      return false;
    }
    if (!seen_beam_ids.emplace(beam_id).second) {
      logger.warning("Ignoring NTN served beam update with duplicate beam id '{}'", beam_id);
      return false;
    }

    const auto beam_it = beam_lookup.find(beam_id);
    if (beam_it == beam_lookup.end()) {
      logger.warning("Ignoring NTN served beam update with unknown beam id '{}'", beam_id);
      return false;
    }
    if (!beam_it->second->enabled) {
      logger.warning("Ignoring NTN served beam update with disabled beam id '{}'", beam_id);
      return false;
    }
    resolved_beams.push_back(beam_it->second);
  }

  return true;
}

bool ntn_location_mobility_controller::update_served_beams(const std::vector<std::string>& beam_ids)
{
  if (!cfg.ntn_location_mobility.enabled) {
    logger.debug("Ignoring NTN served beam update because location mobility is disabled");
    return false;
  }

  std::vector<const ntn_beam_position*> resolved_beams;
  if (!resolve_served_beams(beam_ids, resolved_beams)) {
    return false;
  }

  if (has_same_beam_membership(beam_ids, current_served_beam_ids)) {
    logger.debug("Ignoring unchanged NTN served beam set with {} beams", current_served_beams.size());
    return true;
  }

  current_served_beam_ids = beam_ids;
  current_served_beams    = std::move(resolved_beams);
  std::unordered_set<std::string> new_served_beam_ids;
  new_served_beam_ids.reserve(current_served_beam_ids.size());
  for (const auto& beam_id : current_served_beam_ids) {
    new_served_beam_ids.insert(beam_id);
  }
  for (cu_cp_ue* ue : ue_mng.get_ues()) {
    if (ue != nullptr &&
        should_clear_candidate_for_served_beam_update(ue->get_meas_context().ntn_candidate_beam, new_served_beam_ids)) {
      clear_candidate_beam(ue->get_meas_context());
    }
  }
  mobility_mng_notifier.on_ntn_served_beams_updated(current_served_beam_ids);
  logger.debug("Updated NTN served beam set with {} beams", current_served_beams.size());
  return true;
}

ntn_location_report_result ntn_location_mobility_controller::report_ue_location(const ntn_ue_location_report& report)
{
  const auto& ntn_cfg = cfg.ntn_location_mobility;
  if (!ntn_cfg.enabled) {
    logger.debug("ue={}: Ignoring NTN location report because location mobility is disabled", report.ue_index);
    return ntn_location_report_result::disabled;
  }

  if (!is_valid_location(report)) {
    logger.warning("ue={}: Ignoring invalid NTN location report lat={} lon={}",
                   report.ue_index,
                   report.latitude_deg,
                   report.longitude_deg);
    return ntn_location_report_result::invalid;
  }

  if (cfg.cells.find(report.serving_nci) == cfg.cells.end()) {
    logger.debug("ue={}: Ignoring NTN location report for unknown serving nci={:#x}",
                 report.ue_index,
                 report.serving_nci);
    return ntn_location_report_result::unknown_cell;
  }

  cu_cp_ue* ue = ue_mng.find_ue(report.ue_index);
  if (ue == nullptr) {
    logger.debug("ue={}: Ignoring NTN location report for unknown UE", report.ue_index);
    return ntn_location_report_result::unknown_ue;
  }
  auto& ue_meas_context = ue->get_meas_context();

  const auto now = std::chrono::steady_clock::now();
  if (ntn_cfg.location_max_age.count() > 0 && now - report.received_time > ntn_cfg.location_max_age) {
    logger.debug("ue={}: Ignoring stale NTN location report", report.ue_index);
    return ntn_location_report_result::stale;
  }

  if (ntn_cfg.max_horizontal_accuracy_m.has_value() && report.horizontal_accuracy_m.has_value() &&
      report.horizontal_accuracy_m.value() > ntn_cfg.max_horizontal_accuracy_m.value()) {
    logger.debug("ue={}: Ignoring NTN location report with horizontal accuracy {}m above configured {}m",
                 report.ue_index,
                 report.horizontal_accuracy_m.value(),
                 ntn_cfg.max_horizontal_accuracy_m.value());
    return ntn_location_report_result::inaccurate;
  }

  bool same_location_sample = false;
  if (ue_meas_context.last_ntn_location_report.has_value()) {
    const auto& last_report = ue_meas_context.last_ntn_location_report.value();
    if (report.received_time < last_report.received_time) {
      logger.debug("ue={}: Ignoring out-of-order NTN location report", report.ue_index);
      return ntn_location_report_result::out_of_order;
    }
    if (report.received_time == last_report.received_time) {
      if (source_priority(report.source) <= source_priority(last_report.source)) {
        logger.debug("ue={}: Ignoring duplicate NTN location report sample", report.ue_index);
        return ntn_location_report_result::duplicate;
      }
      same_location_sample = true;
    }
  }
  ue_meas_context.last_ntn_location_report = report;

  if (current_served_beams.empty()) {
    clear_candidate_beam(ue_meas_context);
    logger.debug("ue={}: NTN location report ignored because no served beams are currently configured",
                 report.ue_index);
    return ntn_location_report_result::accepted;
  }

  if (serving_beam_still_covers_ue(current_served_beams, ntn_cfg, report)) {
    clear_candidate_beam(ue_meas_context);
    logger.debug("ue={}: NTN location remains inside serving beam nci={:#x}", report.ue_index, report.serving_nci);
    return ntn_location_report_result::accepted;
  }

  const std::optional<ntn_beam_match> target_match = find_best_target_beam(current_served_beams, report);
  if (!target_match.has_value()) {
    clear_candidate_beam(ue_meas_context);
    logger.debug("ue={}: NTN location does not match any currently served target beam", report.ue_index);
    return ntn_location_report_result::accepted;
  }

  if (!is_neighbor(cfg, report.serving_nci, target_match->target_nci)) {
    clear_candidate_beam(ue_meas_context);
    logger.debug("ue={}: Matched NTN beam id={} nci={:#x}, but it is not configured as a neighbor of serving nci={:#x}",
                 report.ue_index,
                 target_match->beam_id,
                 target_match->target_nci,
                 report.serving_nci);
    return ntn_location_report_result::accepted;
  }

  const auto target_cell_it = cfg.cells.find(target_match->target_nci);
  if (target_cell_it == cfg.cells.end() || !target_cell_it->second.serving_cell_cfg.pci.has_value()) {
    clear_candidate_beam(ue_meas_context);
    logger.debug("ue={}: Matched NTN beam id={} nci={:#x}, but target cell config is incomplete",
                 report.ue_index,
                 target_match->beam_id,
                 target_match->target_nci);
    return ntn_location_report_result::accepted;
  }

  auto& state = ue_meas_context.ntn_candidate_beam;
  const bool candidate_changed = !state.has_value() || state->target_beam_id != target_match->beam_id;
  const bool report_gap_exceeded =
      state.has_value() && state->consecutive_location_reports > 0 && ntn_cfg.max_report_gap.count() > 0 &&
      report.received_time - state->last_report_time > ntn_cfg.max_report_gap;

  if (candidate_changed || report_gap_exceeded) {
    state.emplace();
    state->target_beam_id                 = target_match->beam_id;
    state->target_nci                     = target_match->target_nci;
    state->candidate_since                = report.received_time;
    state->last_report_time               = report.received_time;
    state->consecutive_location_reports   = 1;
    state->handover_triggered             = false;
    state->handover_triggered_time.reset();
    state->accepted_handover_attempt_id = 0;
    state->accepted_target_beam_id.clear();
    state->accepted_target_nci = nr_cell_identity::min();
  } else {
    state->last_report_time = report.received_time;
    if (!same_location_sample) {
      ++state->consecutive_location_reports;
    }
  }

  if (state->handover_triggered && state->handover_triggered_time.has_value() &&
      ntn_cfg.handover_retry_timeout.count() > 0 &&
      report.received_time - state->handover_triggered_time.value() >= ntn_cfg.handover_retry_timeout) {
    logger.debug("ue={}: NTN candidate beam id={} nci={:#x} retry timeout expired after {}ms",
                 report.ue_index,
                 state->accepted_target_beam_id,
                 state->accepted_target_nci,
                 std::chrono::duration_cast<std::chrono::milliseconds>(report.received_time -
                                                                       state->handover_triggered_time.value())
                     .count());
    state->handover_triggered = false;
    state->handover_triggered_time.reset();
    state->accepted_handover_attempt_id = 0;
    state->accepted_target_beam_id.clear();
    state->accepted_target_nci = nr_cell_identity::min();
  }

  logger.debug("ue={}: NTN candidate beam id={} nci={:#x} distance={:.1f}m margin={:.1f}m reports={} triggered={}",
               report.ue_index,
               state->target_beam_id,
               state->target_nci,
               target_match->distance_m,
               target_match->margin_m,
               state->consecutive_location_reports,
               state->handover_triggered);

  if (!state->handover_triggered && is_candidate_ready(ntn_cfg, state.value(), report)) {
    const auto& target_cell_cfg   = target_cell_it->second.serving_cell_cfg;
    const uint64_t handover_attempt_id = ++ue_meas_context.next_ntn_handover_attempt_id;
    ntn_location_handover_trigger trigger;
    trigger.ue_index                     = report.ue_index;
    trigger.serving_nci                  = report.serving_nci;
    trigger.handover_attempt_id          = handover_attempt_id;
    trigger.target_beam_id               = state->target_beam_id;
    trigger.target_gnb_id                = target_cell_cfg.nci.gnb_id(target_cell_cfg.gnb_id_bit_length);
    trigger.target_nci                   = target_cell_cfg.nci;
    trigger.target_pci                   = target_cell_cfg.pci.value();
    trigger.served_beam_ids_snapshot     = current_served_beam_ids;
    trigger.last_location_report         = report;
    trigger.candidate_since              = state->candidate_since;
    trigger.last_report_time             = state->last_report_time;
    trigger.consecutive_location_reports = state->consecutive_location_reports;

    state->handover_triggered = mobility_mng_notifier.on_ntn_location_handover_required(trigger);
    if (state->handover_triggered) {
      state->handover_triggered_time     = report.received_time;
      state->accepted_handover_attempt_id = handover_attempt_id;
      state->accepted_target_beam_id      = state->target_beam_id;
      state->accepted_target_nci          = state->target_nci;
    }
    if (!state->handover_triggered) {
      logger.debug("ue={}: NTN handover trigger for beam id={} nci={:#x} was not accepted",
                   report.ue_index,
                   state->target_beam_id,
                   state->target_nci);
    }
  }

  return ntn_location_report_result::accepted;
}

void ntn_location_mobility_controller::handle_handover_result(const ntn_handover_result& result)
{
  if (!cfg.ntn_location_mobility.enabled) {
    return;
  }
  if (result.source_ue_index == ue_index_t::invalid) {
    logger.debug("Ignoring NTN handover result with invalid source UE index");
    return;
  }
  if (result.context.target_beam_id.empty()) {
    logger.debug("ue={}: Ignoring NTN handover result with empty target beam id", result.source_ue_index);
    return;
  }

  cu_cp_ue* ue = ue_mng.find_ue(result.source_ue_index);
  if (ue == nullptr) {
    logger.debug("ue={}: Ignoring NTN handover result because source UE is no longer available",
                 result.source_ue_index);
    return;
  }

  auto& ue_meas_context = ue->get_meas_context();
  if (result.context.handover_attempt_id != 0 &&
      result.context.handover_attempt_id <= ue_meas_context.last_handled_ntn_handover_result_attempt_id) {
    logger.debug("ue={}: Ignoring already handled NTN handover result attempt={} beam={} target_nci={:#x}",
                 result.source_ue_index,
                 result.context.handover_attempt_id,
                 result.context.target_beam_id,
                 result.context.target_nci);
    return;
  }

  if (!ue_meas_context.ntn_candidate_beam.has_value()) {
    logger.debug("ue={}: Ignoring NTN handover result because no candidate beam is pending",
                 result.source_ue_index);
    return;
  }

  const ntn_candidate_beam_state& state = ue_meas_context.ntn_candidate_beam.value();
  if (!state.handover_triggered || state.accepted_handover_attempt_id != result.context.handover_attempt_id ||
      state.accepted_target_beam_id != result.context.target_beam_id ||
      state.accepted_target_nci != result.context.target_nci) {
    logger.debug("ue={}: Ignoring stale NTN handover result attempt={} beam={} target_nci={:#x}",
                 result.source_ue_index,
                 result.context.handover_attempt_id,
                 result.context.target_beam_id,
                 result.context.target_nci);
    return;
  }

  if (result.success) {
    logger.info("ue={}: NTN handover completed attempt={} beam={} target_nci={:#x}",
                result.source_ue_index,
                result.context.handover_attempt_id,
                result.context.target_beam_id,
                result.context.target_nci);
  } else {
    logger.info("ue={}: NTN handover failed attempt={} beam={} target_nci={:#x} cause={}. Candidate will be rebuilt",
                result.source_ue_index,
                result.context.handover_attempt_id,
                result.context.target_beam_id,
                result.context.target_nci,
                ntn_handover_failure_cause_to_string(result.failure_cause));
  }

  ue_meas_context.last_handled_ntn_handover_result_attempt_id = result.context.handover_attempt_id;
  clear_candidate_beam(ue_meas_context);
}

void ntn_location_mobility_controller::clear_candidate_beam(cell_meas_manager_ue_context& ue_meas_context)
{
  ue_meas_context.ntn_candidate_beam.reset();
}
