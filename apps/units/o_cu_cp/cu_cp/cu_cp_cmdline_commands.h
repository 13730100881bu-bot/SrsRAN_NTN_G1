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

#include "apps/services/cmdline/cmdline_command.h"
#include "apps/services/cmdline/cmdline_command_dispatcher_utils.h"
#include "srsran/adt/expected.h"
#include "srsran/cu_cp/cu_cp_command_handler.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/rnti.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace srsran {

inline const char* ntn_beam_state_to_string(srs_cu_cp::cu_cp_ntn_beam_assignment_state state)
{
  switch (state) {
    case srs_cu_cp::cu_cp_ntn_beam_assignment_state::inactive:
      return "inactive";
    case srs_cu_cp::cu_cp_ntn_beam_assignment_state::candidate:
      return "candidate";
    case srs_cu_cp::cu_cp_ntn_beam_assignment_state::active_loaded:
      return "active_loaded";
    case srs_cu_cp::cu_cp_ntn_beam_assignment_state::draining:
      return "draining";
  }
  return "unknown";
}

inline bool ntn_beam_status_matches_filter(const srs_cu_cp::cu_cp_ntn_beam_status& status, std::string_view filter)
{
  if (filter == "all") {
    return true;
  }
  if (filter == "window") {
    return status.in_hopping_window;
  }
  if (filter == "scheduled") {
    return status.sr_slot_period != 0 || status.srs_slot_period != 0;
  }
  if (filter == "active" || filter == "active_loaded") {
    return status.state == srs_cu_cp::cu_cp_ntn_beam_assignment_state::active_loaded;
  }
  if (filter == "candidate") {
    return status.state == srs_cu_cp::cu_cp_ntn_beam_assignment_state::candidate;
  }
  if (filter == "draining") {
    return status.state == srs_cu_cp::cu_cp_ntn_beam_assignment_state::draining;
  }
  if (filter == "inactive") {
    return status.state == srs_cu_cp::cu_cp_ntn_beam_assignment_state::inactive;
  }
  return false;
}

inline bool is_valid_ntn_beam_filter(std::string_view filter)
{
  return filter == "summary" || filter == "all" || filter == "window" || filter == "scheduled" ||
         filter == "active_loaded" || filter == "active" || filter == "candidate" || filter == "draining" ||
         filter == "inactive";
}

inline std::string format_ntn_antenna_slot(const srs_cu_cp::cu_cp_ntn_beam_status& status)
{
  if (status.nof_antenna_slots == 0) {
    return "-";
  }
  if (status.nof_antenna_slots == 1) {
    return fmt::format("{}/{}", status.antenna_slot_index, status.antenna_slot_period);
  }
  return fmt::format("{}-{}/{}",
                     status.antenna_slot_index,
                     status.antenna_slot_index + status.nof_antenna_slots - 1,
                     status.antenna_slot_period);
}

inline std::string format_ntn_periodic_slot(unsigned offset, unsigned period)
{
  return period == 0 ? "-" : fmt::format("{}/{}", offset, period);
}

inline std::string format_ntn_predictive_offset(std::optional<std::chrono::milliseconds> offset)
{
  return offset.has_value() ? std::to_string(offset->count()) : "-";
}

inline const char* format_ntn_link_direction(const srs_cu_cp::cu_cp_ntn_beam_status& status)
{
  if (status.downlink_enabled && status.uplink_enabled) {
    return "both";
  }
  if (status.downlink_enabled) {
    return "downlink_only";
  }
  if (status.uplink_enabled) {
    return "uplink_only";
  }
  return "disabled";
}

inline std::string format_ntn_derived_tac(const srs_cu_cp::cu_cp_ntn_beam_status& status)
{
  return status.derived_tac.has_value() ? std::to_string(status.derived_tac.value()) : status.derived_tac_reason;
}

inline std::string format_ntn_paging_recommendation(const srs_cu_cp::cu_cp_ntn_beam_status& status)
{
  return status.paging_recommendable ? "yes" : status.paging_recommendation_reason;
}

inline std::string format_ntn_paired_uplink_access(const srs_cu_cp::cu_cp_ntn_beam_status& status)
{
  if (!status.paired_uplink_access_ready) {
    return status.access_pair_reason;
  }
  return status.paired_uplink_beam_id.empty() ? "yes" : status.paired_uplink_beam_id;
}

inline std::string format_ntn_du_index(srs_cu_cp::du_index_t du_index)
{
  return du_index == srs_cu_cp::du_index_t::invalid ? "-" : std::to_string(du_index_to_uint(du_index));
}

inline std::string format_ntn_resource_domain(const srs_cu_cp::cu_cp_ntn_beam_status& status)
{
  return status.resource_domain_eligible ? "eligible" : status.resource_domain_reason;
}

inline std::string format_ntn_reuse_group(const srs_cu_cp::cu_cp_ntn_beam_status& status)
{
  return status.reuse_group_id.empty() ? "-" : status.reuse_group_id;
}

inline std::string format_ntn_conflict_groups(const srs_cu_cp::cu_cp_ntn_beam_status& status)
{
  if (status.conflict_group_ids.empty()) {
    return "-";
  }
  std::string result;
  for (const auto& group_id : status.conflict_group_ids) {
    if (!result.empty()) {
      result += ",";
    }
    result += group_id;
  }
  return result;
}

inline const char* ntn_assistance_invalid_reason_to_string(srs_cu_cp::ntn_assistance_invalid_reason reason)
{
  switch (reason) {
    case srs_cu_cp::ntn_assistance_invalid_reason::none:
      return "none";
    case srs_cu_cp::ntn_assistance_invalid_reason::disabled:
      return "disabled";
    case srs_cu_cp::ntn_assistance_invalid_reason::no_satellite_state:
      return "no_satellite_state";
    case srs_cu_cp::ntn_assistance_invalid_reason::stale_satellite_state:
      return "stale_satellite_state";
  }
  return "unknown";
}

inline const char* ntn_assistance_beam_state_to_string(srs_cu_cp::ntn_assistance_beam_state state)
{
  switch (state) {
    case srs_cu_cp::ntn_assistance_beam_state::candidate:
      return "candidate";
    case srs_cu_cp::ntn_assistance_beam_state::active_loaded:
      return "active_loaded";
    case srs_cu_cp::ntn_assistance_beam_state::draining:
      return "draining";
  }
  return "unknown";
}

inline const char* ntn_service_beam_policy_to_string(srs_cu_cp::ntn_service_beam_policy policy)
{
  switch (policy) {
    case srs_cu_cp::ntn_service_beam_policy::normal:
      return "normal";
    case srs_cu_cp::ntn_service_beam_policy::prepare:
      return "prepare";
    case srs_cu_cp::ntn_service_beam_policy::block_new_demand:
      return "block_new_demand";
    case srs_cu_cp::ntn_service_beam_policy::drain:
      return "drain";
    case srs_cu_cp::ntn_service_beam_policy::release_allowed:
      return "release_allowed";
  }
  return "unknown";
}

inline ecef_coordinates_t make_ecef_from_geodetic(double latitude_deg, double longitude_deg, double altitude_m)
{
  constexpr double wgs84_a_m = 6378137.0;
  constexpr double wgs84_f   = 1.0 / 298.257223563;
  constexpr double wgs84_e2  = 2.0 * wgs84_f - wgs84_f * wgs84_f;
  constexpr double pi        = 3.14159265358979323846;

  const double latitude_rad  = latitude_deg * pi / 180.0;
  const double longitude_rad = longitude_deg * pi / 180.0;
  const double sin_latitude  = std::sin(latitude_rad);
  const double cos_latitude  = std::cos(latitude_rad);
  const double prime_vertical_radius_m =
      wgs84_a_m / std::sqrt(1.0 - wgs84_e2 * sin_latitude * sin_latitude);

  ecef_coordinates_t ecef;
  ecef.position_x = (prime_vertical_radius_m + altitude_m) * cos_latitude * std::cos(longitude_rad);
  ecef.position_y = (prime_vertical_radius_m + altitude_m) * cos_latitude * std::sin(longitude_rad);
  ecef.position_z =
      (prime_vertical_radius_m * (1.0 - wgs84_e2) + altitude_m) * sin_latitude;
  return ecef;
}

inline bool inject_ntn_satellite_state(srs_cu_cp::cu_cp_command_handler& cu_cp, const ecef_coordinates_t& satellite)
{
  const bool accepted = cu_cp.get_ntn_command_handler().handle_ntn_satellite_state_update(satellite);
  fmt::print("NTN satellite state {}. ecef=({:.3f},{:.3f},{:.3f})\n",
             accepted ? "accepted" : "rejected",
             satellite.position_x,
             satellite.position_y,
             satellite.position_z);
  return accepted;
}

enum class ntn_diagnose_status { ok, warn, blocker };

struct ntn_diagnose_entry {
  std::string         area;
  ntn_diagnose_status status;
  std::string         reason;
  std::string         next_step;
};

inline const char* ntn_diagnose_status_to_string(ntn_diagnose_status status)
{
  switch (status) {
    case ntn_diagnose_status::ok:
      return "ok";
    case ntn_diagnose_status::warn:
      return "warn";
    case ntn_diagnose_status::blocker:
      return "blocker";
  }
  return "unknown";
}

inline unsigned ntn_diagnose_status_rank(ntn_diagnose_status status)
{
  switch (status) {
    case ntn_diagnose_status::ok:
      return 0;
    case ntn_diagnose_status::warn:
      return 1;
    case ntn_diagnose_status::blocker:
      return 2;
  }
  return 0;
}

inline bool ntn_diagnose_is_valid_filter(std::string_view filter)
{
  return filter == "summary" || filter == "access" || filter == "service" || filter == "mobility" ||
         filter == "paging" || filter == "resources" || filter == "all";
}

inline bool ntn_diagnose_matches_filter(const ntn_diagnose_entry& entry, std::string_view filter)
{
  return filter == "all" || filter == entry.area;
}

inline void ntn_diagnose_add_ok_if_area_clean(std::vector<ntn_diagnose_entry>& entries,
                                              std::string_view                  area,
                                              std::string_view                  reason)
{
  const bool has_area_entry = std::any_of(entries.begin(), entries.end(), [area](const ntn_diagnose_entry& entry) {
    return entry.area == area;
  });
  if (!has_area_entry) {
    entries.push_back({std::string(area), ntn_diagnose_status::ok, std::string(reason), "none"});
  }
}

inline const char* ntn_repair_mode_to_string(srs_cu_cp::ntn_repair_mode mode)
{
  switch (mode) {
    case srs_cu_cp::ntn_repair_mode::dry_run:
      return "dry-run";
    case srs_cu_cp::ntn_repair_mode::apply:
      return "apply";
  }
  return "unknown";
}

inline const char* ntn_repair_scope_to_string(srs_cu_cp::ntn_repair_scope scope)
{
  switch (scope) {
    case srs_cu_cp::ntn_repair_scope::resources:
      return "resources";
    case srs_cu_cp::ntn_repair_scope::sib19:
      return "sib19";
    case srs_cu_cp::ntn_repair_scope::all:
      return "all";
  }
  return "unknown";
}

inline std::optional<srs_cu_cp::ntn_repair_mode> parse_ntn_repair_mode(std::string_view value)
{
  if (value == "dry-run" || value == "dry_run") {
    return srs_cu_cp::ntn_repair_mode::dry_run;
  }
  if (value == "apply") {
    return srs_cu_cp::ntn_repair_mode::apply;
  }
  return std::nullopt;
}

inline std::optional<srs_cu_cp::ntn_repair_scope> parse_ntn_repair_scope(std::string_view value)
{
  if (value == "resources") {
    return srs_cu_cp::ntn_repair_scope::resources;
  }
  if (value == "sib19") {
    return srs_cu_cp::ntn_repair_scope::sib19;
  }
  if (value == "all") {
    return srs_cu_cp::ntn_repair_scope::all;
  }
  return std::nullopt;
}

/// Application command to trigger a handover.
class handover_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;

public:
  explicit handover_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  // See interface for documentation.
  std::string_view get_name() const override { return "ho"; }

  // See interface for documentation.
  std::string_view get_description() const override { return " <serving pci> <rnti> <target pci>: force UE handover"; }

  // See interface for documentation.
  void execute(span<const std::string> args) override
  {
    if (args.size() != 3) {
      fmt::print("Invalid handover command structure. Usage: ho <serving pci> <rnti> <target pci>\n");
      return;
    }

    const auto*                     arg         = args.begin();
    expected<unsigned, std::string> serving_pci = app_services::parse_int<unsigned>(*arg);
    if (not serving_pci.has_value()) {
      fmt::print("Invalid serving PCI.\n");
      return;
    }
    ++arg;
    expected<unsigned, std::string> rnti = app_services::parse_unsigned_hex<unsigned>(*arg);
    if (not rnti.has_value()) {
      fmt::print("Invalid UE RNTI.\n");
      return;
    }
    ++arg;
    expected<unsigned, std::string> target_pci = app_services::parse_int<unsigned>(*arg);
    if (not target_pci.has_value()) {
      fmt::print("Invalid target PCI.\n");
      return;
    }

    cu_cp.get_mobility_command_handler().trigger_handover(static_cast<pci_t>(serving_pci.value()),
                                                          static_cast<rnti_t>(rnti.value()),
                                                          static_cast<pci_t>(target_pci.value()));
    fmt::print("Handover triggered for UE with pci={} rnti={} to pci={}.\n",
               serving_pci.value(),
               static_cast<rnti_t>(rnti.value()),
               target_pci.value());
  }
};

/// Application command to inspect the current NTN beam placement plan.
class ntn_beams_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;
  static constexpr unsigned         max_row_limit = 1024;

public:
  explicit ntn_beams_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  // See interface for documentation.
  std::string_view get_name() const override { return "ntn_beams"; }

  // See interface for documentation.
  std::string_view get_description() const override
  {
    return " [summary|all|window|scheduled|active_loaded|candidate|draining|inactive] [limit]: show NTN beam placement state";
  }

  // See interface for documentation.
  void execute(span<const std::string> args) override
  {
    if (args.size() > 2) {
      fmt::print("Invalid NTN beam command structure. Usage: ntn_beams "
                 "[summary|all|window|scheduled|active_loaded|candidate|draining|inactive] [limit]\n");
      return;
    }

    const std::string_view filter = args.empty() ? std::string_view{"summary"} : std::string_view{args[0]};
    if (!is_valid_ntn_beam_filter(filter)) {
      fmt::print("Invalid NTN beam filter. Usage: ntn_beams "
                 "[summary|all|window|scheduled|active_loaded|candidate|draining|inactive] [limit]\n");
      return;
    }

    unsigned row_limit = 128;
    if (args.size() == 2) {
      expected<unsigned, std::string> parsed_limit = app_services::parse_int<unsigned>(args[1]);
      if (!parsed_limit.has_value()) {
        fmt::print("Invalid NTN beam row limit.\n");
        return;
      }
      row_limit = parsed_limit.value();
    }
    if (row_limit > max_row_limit) {
      fmt::print("NTN beam row limit capped to {} rows.\n", max_row_limit);
      row_limit = max_row_limit;
    }

    const std::vector<srs_cu_cp::cu_cp_ntn_beam_status> beam_status =
        cu_cp.get_ntn_command_handler().get_current_ntn_beam_status();
    if (beam_status.empty()) {
      fmt::print("No NTN beam placement plan is available.\n");
      return;
    }

    unsigned nof_active_loaded   = 0;
    unsigned nof_candidate       = 0;
    unsigned nof_draining        = 0;
    unsigned nof_inactive        = 0;
    unsigned nof_hopping_window  = 0;
    unsigned nof_scheduled       = 0;
    unsigned nof_sr_scheduled    = 0;
    unsigned nof_srs_scheduled   = 0;
    unsigned nof_downlink_ready  = 0;
    unsigned nof_uplink_ready    = 0;
    unsigned nof_downlink_visible = 0;
    unsigned nof_uplink_access_ready = 0;
    unsigned nof_access_roundtrip_ready = 0;
    unsigned nof_paired_uplink_access_ready = 0;
    unsigned nof_downlink_only_without_ul_pair = 0;
    unsigned nof_bidirectional_service_ready = 0;
    unsigned nof_filtered_status = 0;
    for (const auto& status : beam_status) {
      switch (status.state) {
        case srs_cu_cp::cu_cp_ntn_beam_assignment_state::active_loaded:
          ++nof_active_loaded;
          break;
        case srs_cu_cp::cu_cp_ntn_beam_assignment_state::candidate:
          ++nof_candidate;
          break;
        case srs_cu_cp::cu_cp_ntn_beam_assignment_state::draining:
          ++nof_draining;
          break;
        case srs_cu_cp::cu_cp_ntn_beam_assignment_state::inactive:
          ++nof_inactive;
          break;
      }
      if (status.in_hopping_window) {
        ++nof_hopping_window;
      }
      if (status.sr_slot_period != 0 || status.srs_slot_period != 0) {
        ++nof_scheduled;
      }
      if (status.sr_slot_period != 0) {
        ++nof_sr_scheduled;
      }
      if (status.srs_slot_period != 0) {
        ++nof_srs_scheduled;
      }
      if (status.downlink_ready) {
        ++nof_downlink_ready;
      }
      if (status.uplink_ready) {
        ++nof_uplink_ready;
      }
      if (status.downlink_visible) {
        ++nof_downlink_visible;
      }
      if (status.uplink_access_ready) {
        ++nof_uplink_access_ready;
      }
      if (status.access_roundtrip_ready) {
        ++nof_access_roundtrip_ready;
      }
      if (status.paired_uplink_access_ready) {
        ++nof_paired_uplink_access_ready;
      }
      if (status.downlink_visible && !status.uplink_access_ready && !status.paired_uplink_access_ready) {
        ++nof_downlink_only_without_ul_pair;
      }
      if (status.bidirectional_service_ready) {
        ++nof_bidirectional_service_ready;
      }
      if (filter != "summary" && ntn_beam_status_matches_filter(status, filter)) {
        ++nof_filtered_status;
      }
    }

    fmt::print("NTN beams: total={} active_loaded={} candidate={} draining={} inactive={} hopping_window={} "
               "scheduled={} sr={} srs={} downlink_ready={} uplink_ready={} downlink_visible={} "
               "uplink_access_ready={} access_roundtrip_ready={} paired_access_ready={} "
               "downlink_only_without_ul_pair={} bidirectional_service_ready={}\n",
               beam_status.size(),
               nof_active_loaded,
               nof_candidate,
               nof_draining,
               nof_inactive,
               nof_hopping_window,
               nof_scheduled,
               nof_sr_scheduled,
               nof_srs_scheduled,
               nof_downlink_ready,
               nof_uplink_ready,
               nof_downlink_visible,
               nof_uplink_access_ready,
               nof_access_roundtrip_ready,
               nof_paired_uplink_access_ready,
               nof_downlink_only_without_ul_pair,
               nof_bidirectional_service_ready);
    fmt::print("NTN beam state guide: active_loaded=serving_or_ready candidate=visible_not_serving "
               "draining=leaving_service inactive=not_currently_usable\n");
    fmt::print("NTN link guide: link=both|downlink_only|uplink_only "
               "downlink_ready=SIB19_or_paging_possible uplink_ready=UL_control_resources_possible "
               "access_roundtrip_ready=UE_can_be_paged_and_answer "
               "paired_access_ready=DL_page_with_separate_UL_response "
               "bidirectional_service_ready=PDU_or_handover_target_possible\n");
    fmt::print("NTN beam table guide: resource=capacity_or_conflict_check "
               "headroom=capacity_reserved_for_moves sched_reason=why_selected\n");

    if (filter == "summary") {
      return;
    }

    fmt::print("{:<18} {:<18} {:<16} {:<9} {:<8} {:<13} {:<14} {:<8} {:<8} {:<10} {:<10} {:<10} {:<10} {:<6} {:>8} {:>6} {:<14} {:<6} {:<9} {:<10} {:<32} {:<28} {:<16} {:<24} {:<12} {:>9} {:>5} {:>5} {:>8} {:>8} {:>8} {:>5} {:>6} {:>8} {:<18} {:<8} {:<24} {:<7} {:<5} {:<17} {:<24} {:<24} {:<17} {:<20} {:<15} {:>10} {:>12} {:>10} {:<24}\n",
               "beam",
               "analog",
               "satellite",
               "entry_ms",
               "exit_ms",
               "state",
               "link",
               "dl_ready",
               "ul_ready",
               "dl_visible",
               "ul_access",
               "roundtrip",
               "svc_ready",
               "window",
               "score",
               "rank",
               "sched_reason",
               "du",
               "access_du",
               "service_du",
               "du_policy",
               "resource",
               "reuse",
               "conflict",
               "nci",
               "elev_deg",
               "ues",
               "drbs",
               "slot",
               "sr",
               "srs",
               "nslot",
               "res_w",
               "res_pct",
               "res_reason",
               "headroom",
               "headroom_reason",
               "blocked",
               "drain",
               "policy",
               "reason",
               "tac",
               "paging",
               "analog_access",
               "sib19",
               "sib19_gen",
               "sib19_bytes",
               "sib19_hash",
               "sib19_reason");
    unsigned nof_printed = 0;
    for (const auto& status : beam_status) {
      if (!ntn_beam_status_matches_filter(status, filter)) {
        continue;
      }
      if (nof_printed >= row_limit) {
        break;
      }
      const std::string du_index   = format_ntn_du_index(status.du_index);
      const std::string access_du  = format_ntn_du_index(status.access_du_index);
      const std::string service_du = format_ntn_du_index(status.service_du_index);
      const std::string du_policy  = status.du_assignment_reason.empty() ? "-" : status.du_assignment_reason;
      const std::string resource   = format_ntn_resource_domain(status);
      const std::string reuse      = format_ntn_reuse_group(status);
      const std::string conflict   = format_ntn_conflict_groups(status);
      const std::string antenna_slot = format_ntn_antenna_slot(status);
      const std::string sr_slot      = format_ntn_periodic_slot(status.sr_slot_offset, status.sr_slot_period);
      const std::string srs_slot     = format_ntn_periodic_slot(status.srs_slot_offset, status.srs_slot_period);
      const std::string derived_tac  = format_ntn_derived_tac(status);
      const std::string paging       = format_ntn_paging_recommendation(status);
      const std::string paired_ul    = format_ntn_paired_uplink_access(status);
      const std::string analog_beam = status.analog_beam_id.empty() ? "-" : status.analog_beam_id;
      const std::string satellite   = status.serving_satellite_id.empty() ? "-" : status.serving_satellite_id;
      const std::string entry_offset = format_ntn_predictive_offset(status.predictive_entry_offset);
      const std::string exit_offset  = format_ntn_predictive_offset(status.predictive_exit_offset);
      const std::string analog_access =
          status.analog_access_eligible ? (status.analog_edge_partial ? "yes_edge" : "yes")
                                        : status.analog_access_reason;
      const std::string window_rank =
          status.window_rank == std::numeric_limits<unsigned>::max() ? "-" : fmt::format("{}", status.window_rank);
      fmt::print("{:<18} {:<18} {:<16} {:<9} {:<8} {:<13} {:<14} {:<8} {:<8} {:<10} {:<10} {:<10} {:<10} {:<6} {:>8} {:>6} {:<14} {:<6} {:<9} {:<10} {:<32} {:<28} {:<16} {:<24} {:#012x} {:>9.2f} {:>5} {:>5} {:>8} {:>8} {:>8} {:>5} {:>6} {:>8.1f} {:<18} {:<8} {:<24} {:<7} {:<5} {:<17} {:<24} {:<24} {:<17} {:<20} {:<15} {:>10} {:>12} {:#010x} {:<24}\n",
                 status.beam_id,
                 analog_beam,
                 satellite,
                 entry_offset,
                 exit_offset,
                 ntn_beam_state_to_string(status.state),
                 format_ntn_link_direction(status),
                 status.downlink_ready ? "yes" : "no",
                 status.uplink_ready ? "yes" : "no",
                 status.downlink_visible ? "yes" : "no",
                 status.uplink_access_ready ? "yes" : "no",
                 status.access_roundtrip_ready ? "yes" : "no",
                 status.bidirectional_service_ready ? "yes" : "no",
                 status.in_hopping_window ? "yes" : "no",
                 status.scheduling_score,
                 window_rank,
                 status.scheduling_reason,
                 du_index,
                 access_du,
                 service_du,
                 du_policy,
                 resource,
                 reuse,
                 conflict,
                 status.nci.value(),
                 status.elevation_deg,
                 status.nof_ues,
                 status.nof_drbs,
                 antenna_slot,
                 sr_slot,
                 srs_slot,
                 status.nof_antenna_slots,
                 status.resource_weight,
                 status.resource_share * 100.0,
                 status.resource_weight_reason,
                 status.headroom_reserved ? "yes" : "no",
                 status.headroom_reason,
                 status.new_demand_blocked ? "yes" : "no",
                 status.drain_forced ? "yes" : "no",
                 ntn_service_beam_policy_to_string(status.service_policy),
                 status.state_reason,
                 derived_tac,
                 paging,
                 analog_access,
                 status.sib19_broadcast_state,
                 status.sib19_broadcast_generation,
                 status.sib19_packed_bytes,
                 status.sib19_packed_hash,
                 status.sib19_broadcast_reason);
      fmt::print("  access_pair beam={} roundtrip={} paired_access_ready={} paired_ul={} pair_beam={} "
                 "pair_nci={} pair_du={} pair_reason={}\n",
                 status.beam_id,
                 status.access_roundtrip_ready ? "yes" : "no",
                 status.paired_uplink_access_ready ? "yes" : "no",
                 paired_ul,
                 status.paired_uplink_beam_id.empty() ? "-" : status.paired_uplink_beam_id,
                 status.paired_uplink_nci.has_value() ? fmt::format("{:#012x}", status.paired_uplink_nci->value())
                                                       : "-",
                 format_ntn_du_index(status.paired_uplink_du_index),
                 status.access_pair_reason);
      ++nof_printed;
    }
    if (nof_printed < nof_filtered_status) {
      fmt::print("Showing {} of {} matching beams. Increase [limit] to inspect more rows.\n",
                 nof_printed,
                 nof_filtered_status);
    }
  }
};

/// Application command to inspect the current NTN CU-CP runtime summary.
class ntn_state_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;

public:
  explicit ntn_state_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  std::string_view get_name() const override { return "ntn_state"; }

  std::string_view get_description() const override { return ": show CU-CP NTN runtime summary"; }

  void execute(span<const std::string> args) override
  {
    if (!args.empty()) {
      fmt::print("Invalid NTN state command structure. Usage: ntn_state\n");
      return;
    }

    const srs_cu_cp::cu_cp_ntn_runtime_status status =
        cu_cp.get_ntn_command_handler().get_current_ntn_runtime_status();
    const unsigned nof_move_waiting = status.nof_pre_service_relocations_pending +
                                      status.nof_connected_handovers_candidate +
                                      status.nof_connected_handovers_preloaded +
                                      status.nof_connected_handovers_resource_preparing;
    const unsigned nof_move_active =
        status.nof_pre_service_relocations_active + status.nof_connected_handovers_active;
    const bool access_ready      = status.nof_active_analog_access_beams > 0;
    const bool service_ready     = status.nof_loaded_digital_service_beams > 0 || status.nof_active_loaded_beams > 0;
    const bool reserved_capacity = status.nof_ntn_headroom_reserved_beams > 0 ||
                                   status.nof_ntn_target_reservation_held > 0 ||
                                   status.nof_ntn_target_reservation_created > 0;
    const bool safety_guard_active = status.nof_ntn_handover_skipped_by_pair_cooldown > 0 ||
                                     status.nof_ntn_handover_skipped_by_preheat_ready_guard > 0 ||
                                     status.nof_ntn_preheat_demote_deferred_by_reservation > 0;
    fmt::print("NTN legacy location-mobility state: enabled={} satellite={} assistance={} invalid_reason={} "
               "automatic_source_updates={}\n",
               status.enabled ? "yes" : "no",
               status.satellite_state_available ? "yes" : "no",
               status.assistance_valid ? "valid" : "invalid",
               ntn_assistance_invalid_reason_to_string(status.assistance_invalid_reason),
               status.automatic_source_updates_allowed ? "yes" : "no");
    const auto& position_plan = status.onboard_position_plan;
    fmt::print("NTN onboard position plan: enabled={} du_execution={} stage={} deployment={} satellite_id={} "
               "last_rejection={} rejected_schedule_version={}\n",
               position_plan.enabled ? "yes" : "no",
               position_plan.du_execution_enabled ? "yes" : "no",
               position_plan.stage,
               position_plan.deployment_stage,
               position_plan.satellite_id,
               position_plan.last_rejection,
               position_plan.last_rejected_schedule_version);
    fmt::print("NTN onboard planning context: schema_version={} planning_run_id={} access_profile_id={} "
               "access_profile_hash={} identity_authority={}\n",
               position_plan.schema_version,
               position_plan.planning_run_id,
               position_plan.access_profile_id,
               position_plan.access_profile_hash,
               position_plan.identity_authority);
    fmt::print("NTN onboard plan authentication: required={} status={} trusted_keys={} key_id={} "
               "key_fingerprint={}\n",
               position_plan.signature_required ? "yes" : "no",
               position_plan.signature_status,
               position_plan.trusted_signing_key_count,
               position_plan.signing_key_id,
               position_plan.signing_key_fingerprint);
    fmt::print("NTN onboard state store: file_configured={} file_required={} schema_version={} generation={} "
               "state_hash={} status={} error={} write_blocked={} last_save_unix_ms={}\n",
               position_plan.state_file_configured ? "yes" : "no",
               position_plan.state_file_required ? "yes" : "no",
               position_plan.state_schema_version,
               position_plan.state_generation,
               position_plan.state_hash,
               position_plan.state_store_status,
               position_plan.state_store_error,
               position_plan.state_write_blocked ? "yes" : "no",
               position_plan.last_state_save_unix_ms);
    fmt::print("NTN onboard version anchor: configured={} mode={} status={} error={} generation={} "
               "anchor_hash={} committed_catalog_version={} committed_schedule_version={} reserved_schedule_version={}\n",
               position_plan.version_anchor_configured ? "yes" : "no",
               position_plan.version_anchor_mode,
               position_plan.version_anchor_status,
               position_plan.version_anchor_error,
               position_plan.version_anchor_generation,
               position_plan.version_anchor_hash,
               position_plan.version_anchor_catalog_version,
               position_plan.version_anchor_schedule_version,
               position_plan.version_anchor_reserved_version);
    fmt::print("NTN onboard recovery: stage={} detail={} schedule_version={} catalog_version_high_water={} "
               "schedule_version_high_water={} evidence=persisted_state_is_not_du_or_rf_evidence\n",
               position_plan.recovery_stage,
               position_plan.recovery_detail,
               position_plan.recovery_schedule_version,
               position_plan.catalog_version_high_water,
               position_plan.schedule_version_high_water);
    fmt::print("NTN onboard position plan received: present={} catalog_version={} schedule_version={} content_hash={} "
               "visible_l1={} assigned_l1={} activation_epoch_unix_ms={}\n",
               position_plan.received_plan_present ? "yes" : "no",
               position_plan.received_catalog_version,
               position_plan.received_schedule_version,
               position_plan.received_content_hash,
               position_plan.candidate_l1_positions,
               position_plan.assigned_l1_positions,
               position_plan.received_activation_epoch_unix_ms);
    fmt::print("NTN onboard position plan active: catalog_version={} schedule_version={} content_hash={} "
               "calendar_hash={} activation_epoch_unix_ms={}\n",
               position_plan.active_catalog_version,
               position_plan.active_schedule_version,
               position_plan.active_content_hash,
               position_plan.active_calendar_hash,
               position_plan.active_activation_epoch_unix_ms);
    fmt::print("NTN onboard position plan pending: catalog_version={} schedule_version={} content_hash={} "
               "calendar_hash={} activation_epoch_unix_ms={}\n",
               position_plan.pending_catalog_version,
               position_plan.pending_schedule_version,
               position_plan.pending_content_hash,
               position_plan.pending_calendar_hash,
               position_plan.pending_activation_epoch_unix_ms);
    fmt::print("NTN onboard runtime mapping: stage={} detail={} schedule_version={} calendar_hash={} "
               "mapped_l1={} paging={} valid_idle_contexts={} initial_access_position_check={}\n",
               position_plan.runtime_mapping_stage,
               position_plan.runtime_mapping_detail,
               position_plan.runtime_mapping_schedule_version,
               position_plan.runtime_mapping_calendar_hash,
               position_plan.runtime_mapped_l1_positions,
               position_plan.paging_state,
               position_plan.valid_idle_paging_contexts,
               position_plan.initial_access_position_check);
    fmt::print("NTN access calendar intent: schedule_version={} intents={} ssb={} prach_ro={} prach_ul_beam={} "
               "max_ssb_interval_ms={} max_prach_interval_ms={} "
               "prach_ro_without_beam={} resource_conflicts={} deployment_detail={} evidence={}\n",
               position_plan.audited_schedule_version,
               position_plan.calendar_intents,
               position_plan.ssb_intents,
               position_plan.prach_ro_intents,
               position_plan.prach_ul_beam_intents,
               position_plan.max_ssb_interval_ms,
               position_plan.max_prach_interval_ms,
               position_plan.prach_ro_without_beam,
               position_plan.resource_conflicts,
               position_plan.deployment_detail,
               position_plan.execution_evidence);
    fmt::print("NTN calendar clear queue: depth={} in_flight={} head_schedule_version={} head_calendar_hash={} "
               "head_reason={}\n",
               position_plan.clear_queue_depth,
               position_plan.clear_in_flight ? "yes" : "no",
               position_plan.clear_queue_head_schedule_version,
               position_plan.clear_queue_head_calendar_hash,
               position_plan.clear_queue_head_reason);
    for (unsigned i = 0; i != position_plan.static_opportunities.size(); ++i) {
      const auto& preflight = position_plan.static_opportunities[i];
      const std::string numerology = preflight.performed ? fmt::format("{}", preflight.numerology) : "n/a";
      fmt::print("NTN static opportunity preflight: schedule_version={} nci={:#x} pci={} performed={} passed={} "
                 "numerology={} ssb={}/{} prach={}/{} max_ssb_gap_slots={} max_prach_gap_slots={} "
                 "first_unmatched={} evidence=static_scheduler_opportunity_only_no_position_or_rf_evidence\n",
                 position_plan.static_preflight_schedule_version,
                 position_plan.cells[i].nci,
                 position_plan.cells[i].pci,
                 preflight.performed ? "yes" : "no",
                 preflight.passed ? "yes" : "no",
                 numerology,
                 preflight.matched_ssb,
                 preflight.expected_ssb,
                 preflight.matched_prach,
                 preflight.expected_prach,
                 preflight.max_ssb_gap_slots,
                 preflight.max_prach_gap_slots,
                 preflight.first_unmatched);
    }
    for (const auto& cell : position_plan.cells) {
      fmt::print("NTN onboard cell: nci={:#x} pci={} active_l1={} pending_l1={} mapped_l1={} capacity={} "
                 "plmn={} tac={} tai_status={} analog_ports={}/{} digital_planning_capacity={} "
                 "digital_binding={}\n",
                 cell.nci,
                 cell.pci,
                 cell.active_l1_positions,
                 cell.pending_l1_positions,
                 cell.mapped_l1_positions,
                 cell.capacity,
                 cell.runtime_plmn,
                 cell.runtime_tac,
                 cell.runtime_tai_status,
                 cell.analog_ports_used,
                 cell.analog_port_capacity,
                 cell.digital_planning_capacity,
                 cell.digital_binding_state);
    }
    fmt::print("NTN readable summary: access_ready={} service_ready={} move_waiting={} move_active={} "
               "reserved_capacity={} safety_guard_active={}\n",
               access_ready ? "yes" : "no",
               service_ready ? "yes" : "no",
               nof_move_waiting,
               nof_move_active,
               reserved_capacity ? "yes" : "no",
               safety_guard_active ? "yes" : "no");
    fmt::print("NTN term guide: active_loaded=serving_or_ready candidate=visible_not_serving "
               "draining=leaving_service inactive=not_currently_usable\n");
    fmt::print("NTN operation guide: preheat=prepare_target_beams "
               "headroom=capacity_reserved_for_moves reservation=reserved_move_capacity "
               "scheduling_guard=anti_ping_pong_safety resource_domain=capacity_or_conflict_limits\n");
    fmt::print("NTN beams: total={} candidate={} active_loaded={} loaded_service={} draining={} inactive={} "
               "mobility_eligible={} downlink_ready={} uplink_ready={} downlink_visible={} "
               "uplink_access_ready={} access_roundtrip_ready={} paired_access_ready={} "
               "downlink_only_without_ul_pair={} bidirectional_service_ready={}\n",
               status.nof_total_beams,
               status.nof_candidate_beams,
               status.nof_active_loaded_beams,
               status.nof_loaded_service_beams,
               status.nof_draining_beams,
               status.nof_inactive_beams,
               status.nof_mobility_eligible_beams,
               status.nof_downlink_ready_beams,
               status.nof_uplink_ready_beams,
               status.nof_downlink_visible_beams,
               status.nof_uplink_access_ready_beams,
               status.nof_access_roundtrip_ready_beams,
               status.nof_paired_uplink_access_ready_beams,
               status.nof_downlink_only_without_ul_pair_beams,
               status.nof_bidirectional_service_ready_beams);
    fmt::print("NTN link guide: downlink_ready=SIB19_or_paging_possible "
               "uplink_ready=UL_control_resources_possible "
               "access_roundtrip_ready=UE_can_be_paged_and_answer "
               "paired_access_ready=DL_page_with_separate_UL_response "
               "bidirectional_service_ready=PDU_or_handover_target_possible\n");
    fmt::print("NTN service area: valid_tac={} invalid_tac={} paging_recommendable={}\n",
               status.nof_valid_service_area_beams,
               status.nof_invalid_service_area_beams,
               status.nof_paging_recommendable_beams);
    fmt::print("NTN analog access: total={} full={} partial={} active={} loaded_digital={}\n",
               status.nof_total_analog_access_beams,
               status.nof_full_analog_access_beams,
               status.nof_partial_analog_access_beams,
               status.nof_active_analog_access_beams,
               status.nof_loaded_digital_service_beams);
    fmt::print("NTN access DU: assigned_analog={} unassigned_analog={} same_du_service={} split_du_service={}\n",
               status.nof_access_du_assigned_analog_beams,
               status.nof_access_du_unassigned_analog_beams,
               status.nof_same_du_service_beams,
               status.nof_split_du_service_beams);
    fmt::print("NTN pre-service relocation: pending={} active={} blocked={}\n",
               status.nof_pre_service_relocations_pending,
               status.nof_pre_service_relocations_active,
               status.nof_pre_service_relocations_blocked);
    fmt::print("NTN connected handover: candidate={} preloaded={} resource_preparing={} resource_applied={} active={} blocked={} rollback={}\n",
               status.nof_connected_handovers_candidate,
               status.nof_connected_handovers_preloaded,
               status.nof_connected_handovers_resource_preparing,
               status.nof_connected_handovers_resource_applied,
               status.nof_connected_handovers_active,
               status.nof_connected_handovers_blocked,
               status.nof_connected_handovers_rollback);
    fmt::print("NTN UE access/service: access_only={} binding_pending={} binding_blocked={} service_bound={}\n",
               status.nof_ntn_access_only_ues,
               status.nof_ntn_service_binding_pending_ues,
               status.nof_ntn_service_binding_blocked_ues,
               status.nof_ntn_service_bound_ues);
    fmt::print("NTN UE ownership: access_active={} control_only={} analog_released={} digital_service_bound={}\n",
               status.nof_ntn_access_active_ues,
               status.nof_ntn_control_only_ues,
               status.nof_ntn_analog_released_ues,
               status.nof_ntn_digital_service_bound_ues);
    fmt::print("NTN UE binding_source: location={} access_cell_fallback={}\n",
               status.nof_ntn_location_bound_service_ues,
               status.nof_ntn_access_cell_fallback_service_ues);
    fmt::print("NTN UE location_report: rrc_received={} rrc_decoded={} rrc_unsupported={} rrc_decode_failed={} accepted={} rejected={}\n",
               status.nof_ntn_rrc_location_reports_received,
               status.nof_ntn_rrc_location_reports_decoded,
               status.nof_ntn_rrc_location_reports_unsupported,
               status.nof_ntn_rrc_location_reports_decode_failed,
               status.nof_ntn_location_reports_accepted,
               status.nof_ntn_location_reports_rejected);
    fmt::print("NTN UE location_watchdog: fresh={} missing={} stale={} release_pending={} evaluations={} "
               "refresh_requested={} release_requested={} scheduled={} skipped={} last_reason={}\n",
               status.nof_ntn_location_fresh_ues,
               status.nof_ntn_location_missing_ues,
               status.nof_ntn_location_stale_ues,
               status.nof_ntn_location_release_pending_ues,
               status.nof_ntn_location_watchdog_evaluations,
               status.nof_ntn_location_watchdog_refresh_requested,
               status.nof_ntn_location_watchdog_release_requested,
               status.nof_ntn_location_watchdog_release_scheduled,
               status.nof_ntn_location_watchdog_release_skipped,
               status.last_ntn_location_watchdog_release_reason);
    fmt::print("NTN UE location_request: desired_ues={} configured_ues={} pending_ues={} included={} removed={} "
               "reconfig_sent={} reconfig_failed={} skipped_capability={} skipped_state={}\n",
               status.nof_ntn_rrc_location_request_desired_ues,
               status.nof_ntn_rrc_location_request_configured_ues,
               status.nof_ntn_rrc_location_request_pending_ues,
               status.nof_ntn_rrc_location_request_configs_included,
               status.nof_ntn_rrc_location_request_configs_removed,
               status.nof_ntn_rrc_location_request_reconfig_sent,
               status.nof_ntn_rrc_location_request_reconfig_failed,
               status.nof_ntn_rrc_location_request_configs_skipped_capability,
               status.nof_ntn_rrc_location_request_configs_skipped_state);
    fmt::print("NTN NRPPa transport: dl_ue_received={} dl_ue_forwarded={} dl_ue_dropped={} "
               "dl_non_ue_received={} dl_non_ue_forwarded={} dl_non_ue_dropped={} "
               "ul_ue_received={} ul_ue_sent={} ul_ue_dropped={} "
               "ul_non_ue_received={} ul_non_ue_sent={} ul_non_ue_dropped={} last_drop={} "
               "trp_received={} trp_decoded={} trp_responded={} trp_failed={} unsupported_proc={} "
               "unsupported_info={} trp_empty={} trp_last={} "
               "standard_decode_ok={} standard_decode_fail={} standard_encode_resp={} standard_encode_fail={} "
               "minimal_fallback_decode={} standard_last={} "
               "pos_info_received={} pos_info_decoded={} pos_info_forwarded={} pos_info_responded={} "
               "pos_info_failed={} pos_info_dropped={} pos_info_last={} "
               "measurement_received={} measurement_decoded={} measurement_forwarded={} measurement_responded={} "
               "measurement_failed={} measurement_dropped={} measurement_last={} "
               "activation_received={} activation_decoded={} activation_forwarded={} activation_responded={} "
               "activation_failed={} activation_dropped={} activation_last={} "
               "deactivation_received={} deactivation_decoded={} deactivation_forwarded={} deactivation_acked={} "
               "deactivation_failed={} deactivation_dropped={} deactivation_last={} "
               "assist_ctrl_received={} assist_ctrl_decoded={} assist_ctrl_forwarded={} assist_ctrl_feedback={} "
               "assist_ctrl_failed={} assist_ctrl_dropped={} assist_ctrl_unsupported_fields={} "
               "assist_ctrl_last={}\n",
               status.nof_ntn_nrppa_dl_ue_received,
               status.nof_ntn_nrppa_dl_ue_forwarded,
               status.nof_ntn_nrppa_dl_ue_dropped,
               status.nof_ntn_nrppa_dl_non_ue_received,
               status.nof_ntn_nrppa_dl_non_ue_forwarded,
               status.nof_ntn_nrppa_dl_non_ue_dropped,
               status.nof_ntn_nrppa_ul_ue_received,
               status.nof_ntn_nrppa_ul_ue_sent,
               status.nof_ntn_nrppa_ul_ue_dropped,
               status.nof_ntn_nrppa_ul_non_ue_received,
               status.nof_ntn_nrppa_ul_non_ue_sent,
               status.nof_ntn_nrppa_ul_non_ue_dropped,
               status.last_ntn_nrppa_dropped_reason,
               status.nof_ntn_nrppa_trp_requests_received,
               status.nof_ntn_nrppa_trp_requests_decoded,
               status.nof_ntn_nrppa_trp_responses_sent,
               status.nof_ntn_nrppa_trp_failures_sent,
               status.nof_ntn_nrppa_unsupported_procedures,
               status.nof_ntn_nrppa_trp_unsupported_info_items,
               status.nof_ntn_nrppa_trp_empty_results,
               status.last_ntn_nrppa_trp_reason,
               status.nof_ntn_nrppa_standard_decode_success,
               status.nof_ntn_nrppa_standard_decode_failure,
               status.nof_ntn_nrppa_standard_encode_responses,
               status.nof_ntn_nrppa_standard_encode_failures,
               status.nof_ntn_nrppa_minimal_fallback_decodes,
               status.last_ntn_nrppa_standard_decode_reason,
               status.nof_ntn_nrppa_positioning_info_requests_received,
               status.nof_ntn_nrppa_positioning_info_requests_decoded,
               status.nof_ntn_nrppa_positioning_info_requests_forwarded,
               status.nof_ntn_nrppa_positioning_info_responses_sent,
               status.nof_ntn_nrppa_positioning_info_failures_sent,
               status.nof_ntn_nrppa_positioning_info_dropped,
               status.last_ntn_nrppa_positioning_info_reason,
               status.nof_ntn_nrppa_measurement_requests_received,
               status.nof_ntn_nrppa_measurement_requests_decoded,
               status.nof_ntn_nrppa_measurement_requests_forwarded,
               status.nof_ntn_nrppa_measurement_responses_sent,
               status.nof_ntn_nrppa_measurement_failures_sent,
               status.nof_ntn_nrppa_measurement_dropped,
               status.last_ntn_nrppa_measurement_reason,
               status.nof_ntn_nrppa_activation_requests_received,
               status.nof_ntn_nrppa_activation_requests_decoded,
               status.nof_ntn_nrppa_activation_requests_forwarded,
               status.nof_ntn_nrppa_activation_responses_sent,
               status.nof_ntn_nrppa_activation_failures_sent,
               status.nof_ntn_nrppa_activation_dropped,
               status.last_ntn_nrppa_activation_reason,
               status.nof_ntn_nrppa_deactivation_requests_received,
               status.nof_ntn_nrppa_deactivation_requests_decoded,
               status.nof_ntn_nrppa_deactivation_requests_forwarded,
               status.nof_ntn_nrppa_deactivation_acks_sent,
               status.nof_ntn_nrppa_deactivation_failures_sent,
               status.nof_ntn_nrppa_deactivation_dropped,
               status.last_ntn_nrppa_deactivation_reason,
               status.nof_ntn_nrppa_assistance_control_requests_received,
               status.nof_ntn_nrppa_assistance_control_requests_decoded,
               status.nof_ntn_nrppa_assistance_control_requests_forwarded,
               status.nof_ntn_nrppa_assistance_control_feedbacks_sent,
               status.nof_ntn_nrppa_assistance_control_failures_sent,
               status.nof_ntn_nrppa_assistance_control_dropped,
               status.nof_ntn_nrppa_assistance_control_unsupported_fields,
               status.last_ntn_nrppa_assistance_control_reason);
    fmt::print("NTN idle paging: contexts={} with_5g_s_tmsi={} expired={} ue_hits={} tac_fallbacks={} "
               "recommendations={} skipped={} last_reason={}\n",
               status.nof_ntn_idle_paging_contexts,
               status.nof_ntn_idle_paging_contexts_with_5g_s_tmsi,
               status.nof_ntn_idle_paging_contexts_expired,
               status.nof_ntn_idle_paging_ue_hits,
               status.nof_ntn_idle_paging_tac_fallbacks,
               status.nof_ntn_idle_paging_recommendations,
               status.nof_ntn_idle_paging_skipped,
               status.last_ntn_idle_paging_reason);
    fmt::print("NTN paired access: contexts={} responses={} service_bindings={} blocked={} "
               "last_reason={}\n",
               status.nof_ntn_paired_access_contexts,
               status.nof_ntn_paired_access_responses,
               status.nof_ntn_service_bindings_from_paired_access,
               status.nof_ntn_paired_access_blocked,
               status.last_ntn_paired_access_reason);
    fmt::print("NTN service pair: bound={} blocked={} last_reason={}\n",
               status.nof_ntn_service_pair_bound_ues,
               status.nof_ntn_service_pair_blocked_ues,
               status.last_ntn_service_pair_reason);
    fmt::print("NTN inactive: contexts={} expired={} suspend_requested={} suspend_succeeded={} suspend_failed={} "
               "resume_requested={} resume_succeeded={} resume_failed={} ngap_suspend_resp={} ngap_suspend_fail={} "
               "ngap_resume_resp={} ngap_resume_fail={} paging_hits={} fallback_releases={} last_reason={}\n",
               status.nof_ntn_inactive_contexts,
               status.nof_ntn_inactive_contexts_expired,
               status.nof_ntn_inactive_suspend_requested,
               status.nof_ntn_inactive_suspend_succeeded,
               status.nof_ntn_inactive_suspend_failed,
               status.nof_ntn_inactive_resume_requested,
               status.nof_ntn_inactive_resume_succeeded,
               status.nof_ntn_inactive_resume_failed,
               status.nof_ntn_inactive_ngap_suspend_responses,
               status.nof_ntn_inactive_ngap_suspend_failures,
               status.nof_ntn_inactive_ngap_resume_responses,
               status.nof_ntn_inactive_ngap_resume_failures,
               status.nof_ntn_inactive_paging_hits,
               status.nof_ntn_inactive_fallback_releases,
               status.last_ntn_inactive_reason);
    fmt::print("NTN beam_hopping: requested={} scheduled={} skipped={}\n",
               status.nof_ntn_beam_hopping_ues_requested,
               status.nof_ntn_beam_hopping_ues_scheduled,
               status.nof_ntn_beam_hopping_ues_skipped);
    fmt::print("NTN predictive window: valid={} upcoming={} drain_soon={} handover_requested={} scheduled={} skipped={} "
               "timeline_steps={} horizon_ms={} lead_ms={} entries={} exits={} earliest_upcoming_ms={} earliest_drain_ms={}\n",
               status.predictive_window_valid ? "yes" : "no",
               status.nof_ntn_predictive_upcoming_beams,
               status.nof_ntn_predictive_drain_soon_beams,
               status.nof_ntn_predictive_beam_hopping_ues_requested,
               status.nof_ntn_predictive_beam_hopping_ues_scheduled,
               status.nof_ntn_predictive_beam_hopping_ues_skipped,
               status.nof_ntn_predictive_timeline_steps,
               status.ntn_predictive_service_window_horizon.count(),
               status.ntn_predictive_handover_lead_time.count(),
               status.nof_ntn_predictive_timeline_entry_beams,
               status.nof_ntn_predictive_timeline_exit_beams,
               format_ntn_predictive_offset(status.earliest_ntn_predictive_upcoming_offset),
               format_ntn_predictive_offset(status.earliest_ntn_predictive_drain_offset));
    fmt::print("NTN multi-satellite window: valid={} current_satellites={} next_satellites={} visible_beams={} owner_changes={}\n",
               status.multi_satellite_window_valid ? "yes" : "no",
               status.nof_ntn_current_window_satellites,
               status.nof_ntn_next_window_satellites,
               status.nof_ntn_multi_satellite_visible_beams,
               status.nof_ntn_satellite_owner_changes);
    fmt::print("NTN handover_preferred: requested={} scheduled={} skipped={}\n",
               status.nof_ntn_handover_preferred_ues_requested,
               status.nof_ntn_handover_preferred_ues_scheduled,
               status.nof_ntn_handover_preferred_ues_skipped);
    fmt::print("NTN load_balancing: evaluations={} admission_steered={} handover_requested={} scheduled={} skipped={} "
               "same_analog_scheduled={} cross_analog_scheduled={} skipped_projected_capacity={} "
               "skipped_cold_analog={} source_beam={} target_beam={} source_analog={} target_analog={} "
               "last_reason={}\n",
               status.nof_ntn_load_balancing_evaluations,
               status.nof_ntn_load_balancing_admission_steered,
               status.nof_ntn_load_balancing_handover_requested,
               status.nof_ntn_load_balancing_handover_scheduled,
               status.nof_ntn_load_balancing_handover_skipped,
               status.nof_ntn_load_balancing_same_analog_scheduled,
               status.nof_ntn_load_balancing_cross_analog_scheduled,
               status.nof_ntn_load_balancing_skipped_projected_capacity,
               status.nof_ntn_load_balancing_skipped_cold_analog,
               status.last_ntn_load_balancing_source_beam_id,
               status.last_ntn_load_balancing_target_beam_id,
               status.last_ntn_load_balancing_source_analog_id,
               status.last_ntn_load_balancing_target_analog_id,
               status.last_ntn_load_balancing_reason);
    fmt::print("NTN service pair handover: targets={} scheduled={} skipped={} committed={} rolled_back={} "
               "context_cleared={} last_reason={} completion_reason={}\n",
               status.nof_ntn_service_pair_handover_targets,
               status.nof_ntn_service_pair_handover_scheduled,
               status.nof_ntn_service_pair_handover_skipped,
               status.nof_ntn_service_pair_handover_committed,
               status.nof_ntn_service_pair_handover_rolled_back,
               status.nof_ntn_service_pair_handover_context_cleared,
               status.last_ntn_service_pair_handover_reason,
               status.last_ntn_service_pair_handover_completion_reason);
    fmt::print("NTN preheat: requested={} sent={} applied={} skipped={} demoted={} "
               "skipped_by_capacity={} skipped_by_policy={} cold_analog_requested={} "
               "source_analog={} target_analog={} last_reason={}\n",
               status.nof_ntn_preheat_requested,
               status.nof_ntn_preheat_sent,
               status.nof_ntn_preheat_applied,
               status.nof_ntn_preheat_skipped,
               status.nof_ntn_preheat_demoted,
               status.nof_ntn_preheat_skipped_by_capacity,
               status.nof_ntn_preheat_skipped_by_policy,
               status.nof_ntn_cold_analog_preheat_requested,
               status.last_ntn_preheat_source_analog_id,
               status.last_ntn_preheat_target_analog_id,
               status.last_ntn_preheat_reason);
    fmt::print("NTN scheduling_guard: reservation_created={} held={} consumed={} expired={} "
               "admission_blocked_by_reservation={} skipped_pair_cooldown={} skipped_preheat_ready_guard={} "
               "preheat_demote_deferred={} source_beam={} target_beam={} source_analog={} target_analog={} "
               "last_reason={}\n",
               status.nof_ntn_target_reservation_created,
               status.nof_ntn_target_reservation_held,
               status.nof_ntn_target_reservation_consumed,
               status.nof_ntn_target_reservation_expired,
               status.nof_ntn_admission_blocked_by_target_reservation,
               status.nof_ntn_handover_skipped_by_pair_cooldown,
               status.nof_ntn_handover_skipped_by_preheat_ready_guard,
               status.nof_ntn_preheat_demote_deferred_by_reservation,
               status.last_ntn_scheduling_guard_source_beam_id,
               status.last_ntn_scheduling_guard_target_beam_id,
               status.last_ntn_scheduling_guard_source_analog_id,
               status.last_ntn_scheduling_guard_target_analog_id,
               status.last_ntn_scheduling_guard_reason);
    fmt::print("NTN beam_scheduling: evaluations={} demand_prioritized={} legacy_fallback={} sticky_kept={} last_reason={}\n",
               status.nof_ntn_beam_scheduling_evaluations,
               status.nof_ntn_beam_scheduling_demand_prioritized_windows,
               status.nof_ntn_beam_scheduling_legacy_fallback,
               status.nof_ntn_beam_scheduling_sticky_kept,
               status.last_ntn_beam_scheduling_reason);
    fmt::print("NTN resource_weighting: evaluations={} weighted_beams={} qos_boosted={} legacy_fallback={} "
               "last_reason={}\n",
               status.nof_ntn_resource_weighting_evaluations,
               status.nof_ntn_resource_weighting_weighted_beams,
               status.nof_ntn_resource_weighting_qos_boosted_beams,
               status.nof_ntn_resource_weighting_legacy_fallback,
               status.last_ntn_resource_weighting_reason);
    fmt::print("NTN headroom: evaluations={} reserved_beams={} admission_allowed={} admission_blocked={} "
               "handover_protected={} last_reason={}\n",
               status.nof_ntn_headroom_evaluations,
               status.nof_ntn_headroom_reserved_beams,
               status.nof_ntn_headroom_admission_allowed,
               status.nof_ntn_headroom_admission_blocked,
               status.nof_ntn_headroom_handover_protected,
               status.last_ntn_headroom_reason);
    fmt::print("NTN release_allowed: requested={} scheduled={} skipped={}\n",
               status.nof_ntn_release_allowed_ues_requested,
               status.nof_ntn_release_allowed_ues_scheduled,
               status.nof_ntn_release_allowed_ues_skipped);
    fmt::print("NTN UE capability: supported={} unsupported={} unknown={} parse_failed={}\n",
               status.nof_ntn_capability_supported_ues,
               status.nof_ntn_capability_unsupported_ues,
               status.nof_ntn_capability_unknown_ues,
               status.nof_ntn_capability_parse_failed_ues);
    fmt::print("NTN UE capability profile: ngso={} gso={} both={} implicit_both={} blocked={}\n",
               status.nof_ntn_capability_ngso_ues,
               status.nof_ntn_capability_gso_ues,
               status.nof_ntn_capability_both_ues,
               status.nof_ntn_capability_implicit_both_ues,
               status.nof_ntn_capability_profile_blocked_ues);
    const srs_cu_cp::cu_cp_ntn_antenna_intent_snapshot antenna_intent =
        cu_cp.get_ntn_command_handler().get_current_ntn_antenna_intent_snapshot();
    unsigned nof_analog_access_active_ues = 0;
    for (const auto& intent : antenna_intent.analog_access_intents) {
      nof_analog_access_active_ues += intent.nof_access_active_ues;
    }
    unsigned nof_digital_service_ues  = 0;
    unsigned nof_digital_service_drbs = 0;
    for (const auto& intent : antenna_intent.digital_service_intents) {
      nof_digital_service_ues += intent.nof_ues;
      nof_digital_service_drbs += intent.nof_drbs;
    }
    fmt::print("NTN antenna intent: analog_access={} access_active_ues={} digital_service={} digital_ues={} digital_drbs={}\n",
               antenna_intent.analog_access_intents.size(),
               nof_analog_access_active_ues,
               antenna_intent.digital_service_intents.size(),
               nof_digital_service_ues,
               nof_digital_service_drbs);
    const srs_cu_cp::ntn_beam_service_resource_snapshot resource_snapshot =
        cu_cp.get_ntn_command_handler().get_current_ntn_beam_service_resource_snapshot();
    fmt::print("NTN resource manager: rnti_owned={} rnti_conflicts={} digital_slot_active={} sent={} applied={} rejected={} cleared={} cleared_by_du={} rollback={}\n",
               resource_snapshot.nof_access_rnti_owned,
               resource_snapshot.nof_access_rnti_conflicts,
               resource_snapshot.nof_digital_slot_active,
               resource_snapshot.nof_digital_slot_sent_to_du,
               resource_snapshot.nof_digital_slot_applied_by_du,
               resource_snapshot.nof_digital_slot_rejected_by_du,
               resource_snapshot.nof_digital_slot_cleared,
               resource_snapshot.nof_digital_slot_cleared_by_du,
               resource_snapshot.nof_digital_slot_rollback);
    fmt::print("NTN RNTI leases: reserved={} available={} sent={} applied={} rejected={} offered={} consumed={} initial_ul={} committed={} released={} expired={} conflicts={}\n",
               resource_snapshot.nof_rnti_leases_reserved,
               resource_snapshot.nof_rnti_leases_available,
               resource_snapshot.nof_rnti_leases_sent_to_du,
               resource_snapshot.nof_rnti_leases_applied_by_du,
               resource_snapshot.nof_rnti_leases_rejected_by_du,
               resource_snapshot.nof_rnti_leases_offered_in_rar,
               resource_snapshot.nof_rnti_leases_consumed_by_du,
               resource_snapshot.nof_rnti_leases_initial_ul_seen,
               resource_snapshot.nof_rnti_leases_committed,
               resource_snapshot.nof_rnti_leases_released,
               resource_snapshot.nof_rnti_leases_expired,
               resource_snapshot.nof_rnti_leases_conflict);
    fmt::print("NTN resource audit: generation={} queries={} accepted={} mismatches={} repairs={} failures={} "
               "rnti_incomplete={} ue_slot_incomplete={} reason={}\n",
               status.ntn_resource_audit_generation,
               status.nof_ntn_resource_audit_queries_sent,
               status.nof_ntn_resource_audit_responses_accepted,
               status.nof_ntn_resource_audit_mismatches,
               status.nof_ntn_resource_audit_repair_actions,
               status.nof_ntn_resource_audit_failures,
               status.nof_ntn_resource_audit_rnti_incomplete,
               status.nof_ntn_resource_audit_ue_slot_incomplete,
               status.last_ntn_resource_audit_reason);
    fmt::print("NTN resource repairs: queued={} sent={} applied={} failed={} retry_exhausted={} conflicts={}\n",
               resource_snapshot.nof_resource_repairs_queued,
               resource_snapshot.nof_resource_repairs_sent,
               resource_snapshot.nof_resource_repairs_applied,
               resource_snapshot.nof_resource_repairs_failed,
               resource_snapshot.nof_resource_repairs_retry_exhausted,
               resource_snapshot.nof_resource_repairs_blocked_conflict);
    fmt::print("NTN service pair resource audit: targets={} mismatches={} repairs={} skipped={} "
               "slot_intents={} repair_records={} last_reason={}\n",
               status.nof_ntn_service_pair_resource_audit_targets,
               status.nof_ntn_service_pair_resource_audit_mismatches,
               status.nof_ntn_service_pair_resource_audit_repairs,
               status.nof_ntn_service_pair_resource_audit_skipped,
               resource_snapshot.nof_service_pair_digital_slot_intents,
               resource_snapshot.nof_service_pair_resource_repairs,
               status.last_ntn_service_pair_resource_audit_reason);
    fmt::print("NTN SIB19 broadcast: desired={} sent={} applied={} rejected={} cleared={} stale={}\n",
               status.nof_sib19_broadcast_desired,
               status.nof_sib19_broadcast_sent_to_du,
               status.nof_sib19_broadcast_applied_by_du,
               status.nof_sib19_broadcast_rejected_by_du,
               status.nof_sib19_broadcast_cleared_by_du,
               status.nof_sib19_broadcast_stale_blocked);
    fmt::print("NTN resource domain: analog_cap_blocked={} digital_cap_blocked={} conflict_blocked={} active_reuse_groups={}\n",
               status.nof_resource_domain_analog_cap_blocked,
               status.nof_resource_domain_digital_cap_blocked,
               status.nof_resource_domain_conflict_blocked,
               status.nof_active_reuse_groups);
    fmt::print("NTN UEs: with_context={} active_switch_over_events={}\n",
               status.nof_ues_with_ntn_context,
               status.nof_active_switch_over_events);
  }
};

/// Application command to explain NTN readiness and likely next steps.
class ntn_diagnose_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;
  static constexpr unsigned         default_row_limit = 16;
  static constexpr unsigned         max_row_limit     = 256;

  static void add_access_diagnostics(std::vector<ntn_diagnose_entry>&                 entries,
                                     const srs_cu_cp::cu_cp_ntn_runtime_status&       status,
                                     const srs_cu_cp::ntn_beam_service_resource_snapshot& resources)
  {
    if (!status.enabled) {
      entries.push_back({"access", ntn_diagnose_status::blocker, "ntn_disabled", "enable_ntn_location_mobility"});
    }
    if (!status.satellite_state_available) {
      entries.push_back(
          {"access", ntn_diagnose_status::blocker, "no_satellite_state", "inject_or_enable_satellite_state"});
    }
    if (!status.assistance_valid) {
      entries.push_back({"access",
                         ntn_diagnose_status::blocker,
                         fmt::format("assistance_invalid:{}",
                                     ntn_assistance_invalid_reason_to_string(status.assistance_invalid_reason)),
                         "refresh_satellite_state_or_check_beam_table"});
    }
    if (status.nof_access_roundtrip_ready_beams == 0 && status.nof_paired_uplink_access_ready_beams == 0) {
      const bool has_downlink_visible_beam = status.nof_downlink_visible_beams != 0;
      const bool has_uplink_access_beam    = status.nof_uplink_access_ready_beams != 0;
      entries.push_back({"access",
                         ntn_diagnose_status::blocker,
                         has_downlink_visible_beam ? "downlink_only_without_ul_pair" : "no_downlink_visible_beam",
                         has_downlink_visible_beam && !has_uplink_access_beam
                             ? "check_uplink_access_resources_for_visible_beams"
                             : "check_access_beam_capacity_and_beam_visibility"});
    } else if (status.nof_active_analog_access_beams == 0) {
      entries.push_back({"access",
                         ntn_diagnose_status::warn,
                         "no_access_ready_beam",
                         "check_access_beam_capacity_and_beam_visibility"});
    }
    if (resources.nof_rnti_leases_sent_to_du > resources.nof_rnti_leases_applied_by_du ||
        resources.nof_rnti_leases_rejected_by_du > 0 || resources.nof_rnti_leases_conflict > 0) {
      entries.push_back({"access",
                         ntn_diagnose_status::warn,
                         "rnti_lease_apply_issue",
                         "check_du_resource_feedback_and_rnti_pool"});
    }
    if (status.nof_ntn_paired_access_contexts > 0 && status.nof_ntn_paired_access_responses == 0) {
      entries.push_back({"access",
                         ntn_diagnose_status::warn,
                         "paired_access_waiting_for_ul_response",
                         "check_paired_uplink_access_beam_and_rnti_lease"});
    }
    ntn_diagnose_add_ok_if_area_clean(entries, "access", "access_ready");
  }

  static void add_service_diagnostics(std::vector<ntn_diagnose_entry>&           entries,
                                      const srs_cu_cp::cu_cp_ntn_runtime_status& status)
  {
    const bool service_ready = status.nof_loaded_digital_service_beams > 0 || status.nof_active_loaded_beams > 0;
    if (!service_ready) {
      entries.push_back({"service",
                         ntn_diagnose_status::blocker,
                         "no_service_ready_beam",
                         "check_loaded_service_capacity_and_beam_visibility"});
    }
    if (status.nof_draining_beams > 0) {
      entries.push_back({"service",
                         ntn_diagnose_status::warn,
                         "draining_service_beams",
                         "wait_for_target_beam_or_check_switch_over_policy"});
    }
    if (status.nof_ntn_service_binding_pending_ues > 0 || status.nof_ntn_service_binding_blocked_ues > 0) {
      entries.push_back({"service",
                         ntn_diagnose_status::warn,
                         "service_binding_not_complete",
                         "inspect_ntn_ues_for_binding_reason"});
    }
    if (status.nof_ntn_paired_access_blocked > 0) {
      entries.push_back({"service",
                         ntn_diagnose_status::warn,
                         fmt::format("paired_access_service_blocked:{}", status.last_ntn_paired_access_reason),
                         "provide_bidirectional_service_or_dl_ul_service_pair"});
    } else if (status.nof_ntn_paired_access_responses > status.nof_ntn_service_bindings_from_paired_access) {
      entries.push_back({"service",
                         ntn_diagnose_status::warn,
                         "paired_access_waiting_for_service_binding",
                         "check_bidirectional_or_service_pair_target"});
    }
    if (status.nof_ntn_service_pair_blocked_ues > 0) {
      entries.push_back({"service",
                         ntn_diagnose_status::warn,
                         fmt::format("service_pair_blocked:{}", status.last_ntn_service_pair_reason),
                         "check_uplink_resource_pair_or_capacity"});
    }
    if (status.nof_ntn_admission_blocked_by_target_reservation > 0 ||
        status.nof_ntn_headroom_admission_blocked > 0) {
      entries.push_back({"service",
                         ntn_diagnose_status::warn,
                         "reserved_capacity_blocks_low_priority_demand",
                         "wait_for_move_or_raise_demand_priority"});
    }
    ntn_diagnose_add_ok_if_area_clean(entries, "service", "service_ready");
  }

  static void add_mobility_diagnostics(std::vector<ntn_diagnose_entry>&           entries,
                                       const srs_cu_cp::cu_cp_ntn_runtime_status& status)
  {
    if (status.nof_pre_service_relocations_pending > 0 || status.nof_pre_service_relocations_active > 0) {
      entries.push_back({"mobility",
                         ntn_diagnose_status::warn,
                         "pre_service_move_pending",
                         "wait_for_target_du_preparation"});
    }
    if (status.nof_connected_handovers_blocked > 0 || status.nof_connected_handovers_rollback > 0 ||
        status.nof_ntn_beam_hopping_ues_skipped > 0 ||
        status.nof_ntn_predictive_beam_hopping_ues_skipped > 0 ||
        status.nof_ntn_handover_preferred_ues_skipped > 0 ||
        status.nof_ntn_load_balancing_handover_skipped > 0) {
      entries.push_back({"mobility",
                         ntn_diagnose_status::warn,
                         "handover_blocked_or_skipped",
                         "inspect_target_beam_capacity_and_resource_state"});
    }
    if (status.nof_ntn_service_pair_handover_rolled_back > 0 ||
        status.nof_ntn_service_pair_handover_context_cleared > 0) {
      entries.push_back({"mobility",
                         ntn_diagnose_status::warn,
                         "service_pair_handover_completion_issue",
                         "check_target_downlink_and_uplink_resource_completion"});
    }
    if (status.nof_ntn_load_balancing_skipped_projected_capacity > 0 ||
        status.nof_ntn_load_balancing_skipped_cold_analog > 0 ||
        status.nof_ntn_preheat_skipped_by_capacity > 0 || status.nof_ntn_preheat_skipped_by_policy > 0) {
      entries.push_back({"mobility",
                         ntn_diagnose_status::warn,
                         "target_capacity_or_cold_beam",
                         "check_preheat_capacity_and_target_reservation"});
    }
    if (status.nof_ntn_handover_skipped_by_pair_cooldown > 0 ||
        status.nof_ntn_handover_skipped_by_preheat_ready_guard > 0 ||
        status.nof_ntn_preheat_demote_deferred_by_reservation > 0) {
      entries.push_back(
          {"mobility", ntn_diagnose_status::warn, "safety_guard_active", "wait_for_cooldown_or_ready_guard"});
    }
    ntn_diagnose_add_ok_if_area_clean(entries, "mobility", "no_blocked_moves");
  }

  static void add_paging_diagnostics(std::vector<ntn_diagnose_entry>&           entries,
                                     const srs_cu_cp::cu_cp_ntn_runtime_status& status)
  {
    if (status.nof_paging_recommendable_beams == 0) {
      const bool has_downlink_visible_beam       = status.nof_downlink_visible_beams != 0;
      const bool lacks_roundtrip_for_visible_beam =
          has_downlink_visible_beam && status.nof_access_roundtrip_ready_beams == 0 &&
          status.nof_paired_uplink_access_ready_beams == 0;
      entries.push_back({"paging",
                         ntn_diagnose_status::blocker,
                         lacks_roundtrip_for_visible_beam ? "downlink_only_without_ul_pair" : "no_pageable_beam",
                         lacks_roundtrip_for_visible_beam ? "enable_uplink_access_or_select_paired_access_beam"
                                                          : "check_beam_tac_and_current_service_area"});
    }
    if (status.nof_ntn_idle_paging_contexts_expired > 0 || status.nof_ntn_idle_paging_tac_fallbacks > 0 ||
        status.nof_ntn_idle_paging_skipped > 0) {
      entries.push_back({"paging",
                         ntn_diagnose_status::warn,
                         "idle_context_or_tac_fallback",
                         "inspect_idle_context_age_and_last_beam"});
    }
    if (status.nof_ntn_paired_access_contexts > 0) {
      entries.push_back({"paging",
                         ntn_diagnose_status::warn,
                         "paired_access_context_active",
                         "check_ntn_ues_for_dl_wake_and_ul_response_beams"});
    }
    ntn_diagnose_add_ok_if_area_clean(entries, "paging", "paging_ready");
  }

  static void add_resource_diagnostics(std::vector<ntn_diagnose_entry>&                 entries,
                                       const srs_cu_cp::cu_cp_ntn_runtime_status&       status,
                                       const srs_cu_cp::ntn_beam_service_resource_snapshot& resources)
  {
    if (status.nof_sib19_broadcast_desired > status.nof_sib19_broadcast_applied_by_du ||
        status.nof_sib19_broadcast_sent_to_du > status.nof_sib19_broadcast_applied_by_du ||
        status.nof_sib19_broadcast_rejected_by_du > 0 || status.nof_sib19_broadcast_stale_blocked > 0) {
      entries.push_back({"resources",
                         ntn_diagnose_status::warn,
                         "sib19_not_applied",
                         "ntn_repair apply sib19"});
    }
    if (resources.nof_rnti_leases_sent_to_du > resources.nof_rnti_leases_applied_by_du ||
        resources.nof_rnti_leases_rejected_by_du > 0 || resources.nof_rnti_leases_conflict > 0 ||
        resources.nof_digital_slot_sent_to_du > resources.nof_digital_slot_applied_by_du ||
        resources.nof_digital_slot_rejected_by_du > 0 || resources.nof_digital_slot_rollback > 0) {
      entries.push_back({"resources",
                         ntn_diagnose_status::warn,
                         "rnti_or_slot_apply_issue",
                         "ntn_repair apply resources"});
    }
    if (resources.nof_resource_repairs_failed > 0 || resources.nof_resource_repairs_retry_exhausted > 0 ||
        resources.nof_resource_repairs_blocked_conflict > 0) {
      entries.push_back({"resources",
                         ntn_diagnose_status::warn,
                         "resource_repair_failed",
                         "ntn_repair apply resources"});
    }
    if (status.nof_ntn_service_pair_resource_audit_skipped > 0) {
      entries.push_back({"resources",
                         ntn_diagnose_status::warn,
                         fmt::format("service_pair_ul_resource_repair_skipped:{}",
                                     status.last_ntn_service_pair_resource_audit_reason),
                         "ntn_repair apply resources"});
    }
    ntn_diagnose_add_ok_if_area_clean(entries, "resources", "resources_applied");
  }

  static std::vector<ntn_diagnose_entry> build_summary_entries(const std::vector<ntn_diagnose_entry>& entries)
  {
    static constexpr std::array<std::string_view, 5> areas = {"access", "service", "mobility", "paging", "resources"};
    std::vector<ntn_diagnose_entry>                  summary;

    for (std::string_view area : areas) {
      const ntn_diagnose_entry* best = nullptr;
      for (const auto& entry : entries) {
        if (entry.area != area) {
          continue;
        }
        if (best == nullptr || ntn_diagnose_status_rank(entry.status) > ntn_diagnose_status_rank(best->status)) {
          best = &entry;
        }
      }
      if (best != nullptr) {
        summary.push_back(*best);
      }
    }
    return summary;
  }

public:
  explicit ntn_diagnose_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  std::string_view get_name() const override { return "ntn_diagnose"; }

  std::string_view get_description() const override
  {
    return " [summary|access|service|mobility|paging|resources|all] [limit]: explain NTN readiness and next steps";
  }

  void execute(span<const std::string> args) override
  {
    if (args.size() > 2) {
      fmt::print("Invalid NTN diagnose command structure. Usage: ntn_diagnose "
                 "[summary|access|service|mobility|paging|resources|all] [limit]\n");
      return;
    }

    const std::string_view filter = args.empty() ? std::string_view{"summary"} : std::string_view{args[0]};
    if (!ntn_diagnose_is_valid_filter(filter)) {
      fmt::print("Invalid NTN diagnose filter. Usage: ntn_diagnose "
                 "[summary|access|service|mobility|paging|resources|all] [limit]\n");
      return;
    }

    unsigned row_limit = default_row_limit;
    if (args.size() == 2) {
      expected<unsigned, std::string> parsed_limit = app_services::parse_int<unsigned>(args[1]);
      if (!parsed_limit.has_value()) {
        fmt::print("Invalid NTN diagnose row limit.\n");
        return;
      }
      row_limit = parsed_limit.value();
      if (row_limit > max_row_limit) {
        fmt::print("NTN diagnose row limit capped to {} rows.\n", max_row_limit);
        row_limit = max_row_limit;
      }
    }

    const srs_cu_cp::cu_cp_ntn_runtime_status status =
        cu_cp.get_ntn_command_handler().get_current_ntn_runtime_status();
    const srs_cu_cp::ntn_beam_service_resource_snapshot resources =
        cu_cp.get_ntn_command_handler().get_current_ntn_beam_service_resource_snapshot();

    std::vector<ntn_diagnose_entry> entries;
    add_access_diagnostics(entries, status, resources);
    add_service_diagnostics(entries, status);
    add_mobility_diagnostics(entries, status);
    add_paging_diagnostics(entries, status);
    add_resource_diagnostics(entries, status, resources);

    std::vector<ntn_diagnose_entry> selected_entries;
    if (filter == "summary") {
      selected_entries = build_summary_entries(entries);
    } else {
      for (const auto& entry : entries) {
        if (ntn_diagnose_matches_filter(entry, filter)) {
          selected_entries.push_back(entry);
        }
      }
    }

    const unsigned nof_printed = std::min<unsigned>(row_limit, selected_entries.size());
    fmt::print("NTN diagnose: filter={} diagnostics={} limit={}\n", filter, selected_entries.size(), row_limit);
    for (unsigned i = 0; i != nof_printed; ++i) {
      const ntn_diagnose_entry& entry = selected_entries[i];
      fmt::print("area={} status={} reason={} next_step={}\n",
                 entry.area,
                 ntn_diagnose_status_to_string(entry.status),
                 entry.reason,
                 entry.next_step);
    }
    if (nof_printed < selected_entries.size()) {
      fmt::print("Showing {} of {} diagnostics. Increase [limit] to inspect more rows.\n",
                 nof_printed,
                 selected_entries.size());
    }
  }
};

/// Application command to run a controlled NTN repair dry-run or apply request.
class ntn_repair_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;
  static constexpr unsigned         default_row_limit = 16;
  static constexpr unsigned         max_row_limit     = 256;

  static void print_usage()
  {
    fmt::print("Usage: ntn_repair [dry-run|apply] [resources|sib19|all] [limit]\n");
  }

public:
  explicit ntn_repair_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  std::string_view get_name() const override { return "ntn_repair"; }

  std::string_view get_description() const override
  {
    return " [dry-run|apply] [resources|sib19|all] [limit]: preview or trigger controlled NTN repair";
  }

  void execute(span<const std::string> args) override
  {
    if (args.size() > 3) {
      fmt::print("Invalid NTN repair command structure. ");
      print_usage();
      return;
    }

    srs_cu_cp::ntn_repair_command command;
    command.mode  = srs_cu_cp::ntn_repair_mode::dry_run;
    command.scope = srs_cu_cp::ntn_repair_scope::all;
    command.limit = default_row_limit;

    unsigned arg_index = 0;
    if (arg_index < args.size()) {
      if (const std::optional<srs_cu_cp::ntn_repair_mode> mode = parse_ntn_repair_mode(args[arg_index]);
          mode.has_value()) {
        command.mode = mode.value();
        ++arg_index;
      }
    }
    if (arg_index < args.size()) {
      if (const std::optional<srs_cu_cp::ntn_repair_scope> scope = parse_ntn_repair_scope(args[arg_index]);
          scope.has_value()) {
        command.scope = scope.value();
        ++arg_index;
      } else {
        fmt::print("Invalid NTN repair argument. ");
        print_usage();
        return;
      }
    }
    if (arg_index < args.size()) {
      expected<unsigned, std::string> parsed_limit = app_services::parse_int<unsigned>(args[arg_index]);
      if (!parsed_limit.has_value()) {
        fmt::print("Invalid NTN repair row limit.\n");
        return;
      }
      command.limit = parsed_limit.value();
      if (command.limit > max_row_limit) {
        fmt::print("NTN repair row limit capped to {} rows.\n", max_row_limit);
        command.limit = max_row_limit;
      }
      ++arg_index;
    }
    if (arg_index != args.size()) {
      fmt::print("Invalid NTN repair argument. ");
      print_usage();
      return;
    }

    const srs_cu_cp::ntn_repair_response response =
        cu_cp.get_ntn_command_handler().handle_ntn_repair_command(command);
    fmt::print("NTN repair: mode={} scope={} accepted={} reason={} next_step={} "
               "audit_targets={} sib19_candidates={} existing_failed={} existing_blockers={}\n",
               ntn_repair_mode_to_string(command.mode),
               ntn_repair_scope_to_string(command.scope),
               response.accepted ? "yes" : "no",
               response.reason,
               response.next_step,
               response.audit_targets,
               response.sib19_candidates,
               response.existing_failed,
               response.existing_blockers);
  }
};

/// Application command to inspect CU-CP-side NTN assistance packaging input.
class ntn_assistance_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;
  static constexpr unsigned         max_row_limit = 1024;

public:
  explicit ntn_assistance_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  std::string_view get_name() const override { return "ntn_assistance"; }

  std::string_view get_description() const override
  {
    return " [summary|all] [limit]: show CU-CP NTN assistance packaging input";
  }

  void execute(span<const std::string> args) override
  {
    if (args.size() > 2) {
      fmt::print("Invalid NTN assistance command structure. Usage: ntn_assistance [summary|all] [limit]\n");
      return;
    }
    const std::string_view filter = args.empty() ? std::string_view{"summary"} : std::string_view{args[0]};
    if (filter != "summary" && filter != "all") {
      fmt::print("Invalid NTN assistance filter. Usage: ntn_assistance [summary|all] [limit]\n");
      return;
    }

    unsigned row_limit = 128;
    if (args.size() == 2) {
      expected<unsigned, std::string> parsed_limit = app_services::parse_int<unsigned>(args[1]);
      if (!parsed_limit.has_value()) {
        fmt::print("Invalid NTN assistance row limit.\n");
        return;
      }
      row_limit = std::min(parsed_limit.value(), max_row_limit);
    }

    const srs_cu_cp::ntn_assistance_snapshot snapshot =
        cu_cp.get_ntn_command_handler().get_current_ntn_assistance_snapshot();
    fmt::print("NTN assistance: {} invalid_reason={} beams={}\n",
               snapshot.valid ? "valid" : "invalid",
               ntn_assistance_invalid_reason_to_string(snapshot.invalid_reason),
               snapshot.beams.size());
    if (snapshot.satellite_ecef.has_value()) {
      fmt::print("satellite_ecef=({:.3f},{:.3f},{:.3f}) velocity=({:.3f},{:.3f},{:.3f})\n",
                 snapshot.satellite_ecef->position_x,
                 snapshot.satellite_ecef->position_y,
                 snapshot.satellite_ecef->position_z,
                 snapshot.satellite_ecef->velocity_vx,
                 snapshot.satellite_ecef->velocity_vy,
                 snapshot.satellite_ecef->velocity_vz);
    }
    if (filter == "summary") {
      return;
    }

    fmt::print("{:<18} {:<13} {:<12} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8}\n",
               "beam",
               "state",
               "nci",
               "lat",
               "lon",
               "alt",
               "ta_us",
               "koffset",
               "tserv");
    unsigned nof_printed = 0;
    for (const auto& beam : snapshot.beams) {
      if (nof_printed >= row_limit) {
        break;
      }
      const std::string ta_common =
          beam.ta_info.has_value() ? fmt::format("{:.3f}", beam.ta_info->ta_common) : std::string{"-"};
      const std::string t_service =
          beam.t_service.has_value() ? std::to_string(beam.t_service.value()) : std::string{"-"};
      fmt::print("{:<18} {:<13} {:#012x} {:>8.3f} {:>8.3f} {:>8.1f} {:>8} {:>8} {:>8}\n",
                 beam.beam_id,
                 ntn_assistance_beam_state_to_string(beam.state),
                 beam.nci.value(),
                 beam.reference_location.latitude,
                 beam.reference_location.longitude,
                 beam.reference_location.altitude,
                 ta_common,
                 beam.cell_specific_koffset,
                 t_service);
      ++nof_printed;
    }
    if (nof_printed < snapshot.beams.size()) {
      fmt::print("Showing {} of {} assistance beams. Increase [limit] to inspect more rows.\n",
                 nof_printed,
                 snapshot.beams.size());
    }
  }
};

/// Application command to inspect UE-level CU-CP NTN runtime state.
class ntn_ues_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;
  static constexpr unsigned         max_row_limit = 1024;

public:
  explicit ntn_ues_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  std::string_view get_name() const override { return "ntn_ues"; }

  std::string_view get_description() const override { return " [limit]: show UE-level CU-CP NTN runtime state"; }

  void execute(span<const std::string> args) override
  {
    if (args.size() > 1) {
      fmt::print("Invalid NTN UE command structure. Usage: ntn_ues [limit]\n");
      return;
    }

    unsigned row_limit = 128;
    if (args.size() == 1) {
      expected<unsigned, std::string> parsed_limit = app_services::parse_int<unsigned>(args[0]);
      if (!parsed_limit.has_value()) {
        fmt::print("Invalid NTN UE row limit.\n");
        return;
      }
      row_limit = std::min(parsed_limit.value(), max_row_limit);
    }

    const std::vector<srs_cu_cp::cu_cp_ntn_ue_status> ue_status =
        cu_cp.get_ntn_command_handler().get_current_ntn_ue_status();
    fmt::print("NTN UEs: with_context={}\n", ue_status.size());
    fmt::print("NTN UE state guide: access_only=signaling_only control_only=connected_without_service "
               "binding_pending=waiting_for_service_beam service_bound=using_service_beam\n");
    fmt::print("NTN UE movement guide: reloc=move_before_service conn_ho=connected_move "
               "loc_req=location_reporting_request core=core_location_requests\n");
    fmt::print("NTN UE paired access guide: dl_wake=downlink paging beam ul_response=paired uplink access beam "
               "service can use bidirectional beam or paired DL service plus UL resource beam\n");
    if (ue_status.empty()) {
      return;
    }

    fmt::print("{:<8} {:<6} {:<8} {:>4} {:<12} {:<18} {:<18} {:<16} {:<11} {:<18} {:<10} {:<9} {:<18} {:<24} {:<9} {:<18} {:<18} {:<18} {:<14} {:<16} {:<12} {:<6} {:<18} {:<18} {:<18} {:<5} {:<5} {:<12} {:>5}\n",
               "ue",
               "du",
               "rnti",
               "drbs",
               "serv_nci",
               "serv_beam",
               "cand_beam",
               "runtime",
               "capability",
               "scenario",
               "deploy",
               "prof",
               "cap_reason",
               "profile_reason",
               "analog",
               "access",
               "service",
               "binding",
               "rnti_owner",
               "slot_intent",
               "reloc",
               "tdu",
               "reloc_target",
               "conn_ho",
               "conn_target",
               "slot",
               "ho",
               "loc_req",
               "core");
    unsigned nof_printed = 0;
    for (const auto& ue : ue_status) {
      if (nof_printed >= row_limit) {
        break;
      }
      const std::string du_index = ue.du_index == srs_cu_cp::du_index_t::invalid
                                       ? "-"
                                       : std::to_string(du_index_to_uint(ue.du_index));
      const std::string serving_nci =
          ue.serving_nci.has_value() ? fmt::format("{:#x}", ue.serving_nci->value()) : std::string{"-"};
      const std::string serving_beam = ue.serving_beam_id.value_or("-");
      const std::string candidate_beam = ue.candidate_beam_id.value_or("-");
      const std::string rnti = fmt::format("{}", ue.rnti);
      const std::string target_du =
          ue.pre_service_relocation_target_du_index == srs_cu_cp::du_index_t::invalid
              ? "-"
              : std::to_string(du_index_to_uint(ue.pre_service_relocation_target_du_index));
      const std::string relocation_target = ue.pre_service_relocation_target_beam_id.value_or("-");
      const std::string relocation =
          ue.pre_service_relocation_state == "none"
              ? std::string{"none"}
              : fmt::format("{}:{}", ue.pre_service_relocation_state, ue.pre_service_relocation_reason);
      const std::string connected_handover =
          ue.connected_handover_state == "none"
              ? std::string{"none"}
              : fmt::format("{}:{}:{}",
                            ue.connected_handover_state,
                            ue.connected_handover_reason,
                            ue.connected_handover_target_resource_state);
      const std::string connected_target = ue.connected_handover_target_beam_id.value_or("-");
      const std::string access_layer =
          ue.access_layer_state == "none"
              ? std::string{"none"}
              : fmt::format("{}:{}", ue.access_layer_state, ue.access_analog_beam_id.value_or("-"));
      const std::string service_layer =
          ue.service_layer_state == "none"
              ? std::string{"none"}
              : fmt::format("{}:{}", ue.service_layer_state, ue.service_digital_beam_id.value_or("-"));
      const std::string binding_source =
          ue.service_binding_source == "none" ? std::string{"none"} : ue.service_binding_source;
      const std::string rnti_owner =
          ue.access_rnti_ownership_state == "none"
              ? std::string{"none"}
              : fmt::format("{}:{}", ue.access_rnti_ownership_state, ue.access_rnti_ownership_reason);
      const std::string slot_intent =
          ue.digital_slot_intent_state == "none"
              ? std::string{"none"}
              : fmt::format("{}:{}", ue.digital_slot_intent_state, ue.digital_slot_intent_reason);
      const std::string loc_req = ue.ntn_rrc_location_request_pending
                                      ? std::string{"pending"}
                                      : (ue.ntn_rrc_location_request_configured
                                             ? std::string{"on"}
                                             : (ue.ntn_rrc_location_request_desired
                                                    ? std::string{"desired"}
                                                    : fmt::format("skip:{}", ue.ntn_rrc_location_request_skipped_reason)));
      const std::string analog_release = ue.analog_access_released ? "released" : "-";
      const std::string profile_match = ue.ntn_capability_profile_match ? "match" : "blocked";
      fmt::print("{:<8} {:<6} {:<8} {:>4} {:<12} {:<18} {:<18} {:<16} {:<11} {:<18} {:<10} {:<9} {:<18} {:<24} {:<9} {:<18} {:<18} {:<18} {:<14} {:<16} {:<12} {:<6} {:<18} {:<18} {:<18} {:<5} {:<5} {:<12} {:>5}\n",
                 ue_index_to_uint(ue.ue_index),
                 du_index,
                 rnti,
                 ue.nof_drbs,
                 serving_nci,
                 serving_beam,
                 candidate_beam,
                 ue.ntn_runtime_state,
                 srs_cu_cp::to_string(ue.ntn_capability_state),
                 srs_cu_cp::to_string(ue.ntn_capability_scenario_support),
                 ue.ntn_capability_deployment_profile,
                 profile_match,
                 ue.ntn_capability_reason,
                 ue.ntn_capability_profile_reason,
                 analog_release,
                 access_layer,
                 service_layer,
                 binding_source,
                 rnti_owner,
                 slot_intent,
                 relocation,
                 target_du,
                 relocation_target,
                 connected_handover,
                 connected_target,
                 ue.has_ul_slot_request ? "yes" : "no",
                 ue.candidate_handover_triggered ? "yes" : "no",
                 loc_req,
                 ue.active_core_location_requests);
      if (ue.connected_handover_target_uplink_resource_beam_id.has_value()) {
        const std::string handover_ul_nci =
            ue.connected_handover_target_uplink_resource_nci.has_value()
                ? fmt::format("{:#x}", ue.connected_handover_target_uplink_resource_nci->value())
                : std::string{"-"};
        const std::string handover_ul_du =
            ue.connected_handover_target_uplink_resource_du_index == srs_cu_cp::du_index_t::invalid
                ? std::string{"-"}
                : std::to_string(du_index_to_uint(ue.connected_handover_target_uplink_resource_du_index));
        fmt::print("  service_pair_handover ue={} dl_target={} ul_resource={} ul_nci={} ul_du={} reason={} "
                   "handover_reason={}\n",
                   ue_index_to_uint(ue.ue_index),
                   ue.connected_handover_target_beam_id.value_or("-"),
                   ue.connected_handover_target_uplink_resource_beam_id.value(),
                   handover_ul_nci,
                   handover_ul_du,
                   ue.connected_handover_target_service_pair_reason,
                   ue.connected_handover_reason);
      }
      if (ue.paired_uplink_access_beam_id.has_value()) {
        const std::string paired_ul_nci = ue.paired_uplink_access_nci.has_value()
                                              ? fmt::format("{:#x}", ue.paired_uplink_access_nci->value())
                                              : std::string{"-"};
        const std::string paired_ul_du = ue.paired_uplink_access_du_index == srs_cu_cp::du_index_t::invalid
                                             ? std::string{"-"}
                                             : std::to_string(du_index_to_uint(ue.paired_uplink_access_du_index));
        fmt::print("  paired_access ue={} dl_wake={} ul_response={} ul_nci={} ul_du={} reason={} "
                   "binding_source={} service_reason={}\n",
                   ue_index_to_uint(ue.ue_index),
                   ue.last_downlink_wake_beam_id.value_or("-"),
                   ue.paired_uplink_access_beam_id.value(),
                   paired_ul_nci,
                   paired_ul_du,
                   ue.paired_access_reason,
                   ue.service_binding_source,
                   ue.service_layer_reason);
      }
      if (ue.service_uplink_resource_beam_id.has_value()) {
        const std::string service_ul_nci = ue.service_uplink_resource_nci.has_value()
                                               ? fmt::format("{:#x}", ue.service_uplink_resource_nci->value())
                                               : std::string{"-"};
        const std::string service_ul_du = ue.service_uplink_resource_du_index == srs_cu_cp::du_index_t::invalid
                                              ? std::string{"-"}
                                              : std::to_string(du_index_to_uint(ue.service_uplink_resource_du_index));
        fmt::print("  service_pair ue={} dl_service={} ul_resource={} ul_nci={} ul_du={} reason={} "
                   "service_reason={} slot_audit_target=paired_ul_resource\n",
                   ue_index_to_uint(ue.ue_index),
                   ue.service_downlink_beam_id.value_or(ue.service_digital_beam_id.value_or("-")),
                   ue.service_uplink_resource_beam_id.value(),
                   service_ul_nci,
                   service_ul_du,
                   ue.service_pair_reason,
                   ue.service_layer_reason);
      }
      ++nof_printed;
    }
    if (nof_printed < ue_status.size()) {
      fmt::print("Showing {} of {} NTN UE rows. Increase [limit] to inspect more rows.\n",
                 nof_printed,
                 ue_status.size());
    }
  }
};

/// Application command to inject an NTN satellite ECEF state into CU-CP mobility.
class ntn_satellite_state_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;

public:
  explicit ntn_satellite_state_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  // See interface for documentation.
  std::string_view get_name() const override { return "ntn_sat"; }

  // See interface for documentation.
  std::string_view get_description() const override
  {
    return " <ecef_x_m> <ecef_y_m> <ecef_z_m>: inject NTN satellite state";
  }

  // See interface for documentation.
  void execute(span<const std::string> args) override
  {
    if (args.size() != 3) {
      fmt::print("Invalid NTN satellite command structure. Usage: ntn_sat <ecef_x_m> <ecef_y_m> <ecef_z_m>\n");
      return;
    }

    expected<double, std::string> x = app_services::parse_double(args[0]);
    expected<double, std::string> y = app_services::parse_double(args[1]);
    expected<double, std::string> z = app_services::parse_double(args[2]);
    if (!x.has_value() || !y.has_value() || !z.has_value()) {
      fmt::print("Invalid NTN satellite ECEF state.\n");
      return;
    }
    if (!std::isfinite(x.value()) || !std::isfinite(y.value()) || !std::isfinite(z.value())) {
      fmt::print("Invalid NTN satellite ECEF state. Coordinates must be finite.\n");
      return;
    }

    ecef_coordinates_t satellite;
    satellite.position_x = x.value();
    satellite.position_y = y.value();
    satellite.position_z = z.value();

    inject_ntn_satellite_state(cu_cp, satellite);
  }
};

/// Application command to inject an NTN satellite geodetic state into CU-CP mobility.
class ntn_satellite_geo_state_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;

public:
  explicit ntn_satellite_geo_state_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  // See interface for documentation.
  std::string_view get_name() const override { return "ntn_sat_geo"; }

  // See interface for documentation.
  std::string_view get_description() const override
  {
    return " <lat_deg> <lon_deg> [alt_m]: inject NTN satellite state from geodetic coordinates";
  }

  // See interface for documentation.
  void execute(span<const std::string> args) override
  {
    if (args.size() != 2 && args.size() != 3) {
      fmt::print("Invalid NTN satellite geo command structure. Usage: ntn_sat_geo <lat_deg> <lon_deg> [alt_m]\n");
      return;
    }

    expected<double, std::string> latitude = app_services::parse_double(args[0]);
    expected<double, std::string> longitude = app_services::parse_double(args[1]);
    double altitude_m = 500000.0;
    if (args.size() == 3) {
      expected<double, std::string> altitude = app_services::parse_double(args[2]);
      if (!altitude.has_value()) {
        fmt::print("Invalid NTN satellite altitude.\n");
        return;
      }
      altitude_m = altitude.value();
    }
    if (!latitude.has_value() || !longitude.has_value()) {
      fmt::print("Invalid NTN satellite geodetic state.\n");
      return;
    }
    if (!std::isfinite(latitude.value()) || !std::isfinite(longitude.value()) || !std::isfinite(altitude_m) ||
        latitude.value() < -90.0 || latitude.value() > 90.0 || longitude.value() < -180.0 ||
        longitude.value() > 180.0 || altitude_m <= 0.0) {
      fmt::print("Invalid NTN satellite geodetic state. Use lat in [-90,90], lon in [-180,180], alt_m > 0.\n");
      return;
    }

    const ecef_coordinates_t satellite =
        make_ecef_from_geodetic(latitude.value(), longitude.value(), altitude_m);
    inject_ntn_satellite_state(cu_cp, satellite);
  }
};

} // namespace srsran
