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

#include "ntn_ue_capability_gate.h"
#include "srsran/asn1/rrc_nr/ue_cap.h"
#include "srsran/asn1/rrc_nr/ul_dcch_msg_ies.h"

using namespace srsran;
using namespace srsran::srs_cu_cp;

const char* srsran::srs_cu_cp::to_string(ntn_ue_capability_state state)
{
  switch (state) {
    case ntn_ue_capability_state::unknown:
      return "unknown";
    case ntn_ue_capability_state::supported:
      return "supported";
    case ntn_ue_capability_state::unsupported:
      return "unsupported";
    case ntn_ue_capability_state::parse_failed:
      return "parse_failed";
  }
  return "unknown";
}

const char* srsran::srs_cu_cp::to_string(ntn_ue_capability_scenario_support scenario)
{
  switch (scenario) {
    case ntn_ue_capability_scenario_support::absent:
      return "absent";
    case ntn_ue_capability_scenario_support::ngso:
      return "ngso";
    case ntn_ue_capability_scenario_support::gso:
      return "gso";
    case ntn_ue_capability_scenario_support::both:
      return "both";
    case ntn_ue_capability_scenario_support::implicit_both:
      return "implicit_both";
  }
  return "absent";
}

static void mark_profile_matched(ntn_ue_capability_summary& summary)
{
  summary.matched_deployment_profile = true;
  summary.profile_block_reason       = "profile_matched";
}

static void update_leo_ngso_profile_match(ntn_ue_capability_summary& summary,
                                          const asn1::rrc_nr::ue_nr_cap_v1700_s& v1700)
{
  if (!v1700.ntn_scenario_support_r17_present) {
    summary.scenario_support = ntn_ue_capability_scenario_support::implicit_both;
    summary.ntn_scenario     = to_string(summary.scenario_support);
    mark_profile_matched(summary);
    return;
  }

  switch (v1700.ntn_scenario_support_r17.value) {
    case asn1::rrc_nr::ue_nr_cap_v1700_s::ntn_scenario_support_r17_opts::ngso:
      summary.scenario_support = ntn_ue_capability_scenario_support::ngso;
      summary.ntn_scenario     = v1700.ntn_scenario_support_r17.to_string();
      mark_profile_matched(summary);
      return;
    case asn1::rrc_nr::ue_nr_cap_v1700_s::ntn_scenario_support_r17_opts::gso:
      summary.scenario_support           = ntn_ue_capability_scenario_support::gso;
      summary.ntn_scenario               = v1700.ntn_scenario_support_r17.to_string();
      summary.matched_deployment_profile = false;
      summary.profile_block_reason       = "scenario_mismatch";
      return;
    default:
      summary.scenario_support           = ntn_ue_capability_scenario_support::absent;
      summary.ntn_scenario               = "none";
      summary.matched_deployment_profile = false;
      summary.profile_block_reason       = "scenario_mismatch";
      return;
  }
}

static const asn1::rrc_nr::ue_nr_cap_v1700_s* get_v1700_extension(const asn1::rrc_nr::ue_nr_cap_s& cap)
{
  if (!cap.non_crit_ext_present) {
    return nullptr;
  }

  const auto& v1530 = cap.non_crit_ext;
  if (!v1530.non_crit_ext_present) {
    return nullptr;
  }
  const auto& v1540 = v1530.non_crit_ext;
  if (!v1540.non_crit_ext_present) {
    return nullptr;
  }
  const auto& v1550 = v1540.non_crit_ext;
  if (!v1550.non_crit_ext_present) {
    return nullptr;
  }
  const auto& v1560 = v1550.non_crit_ext;
  if (!v1560.non_crit_ext_present) {
    return nullptr;
  }
  const auto& v1570 = v1560.non_crit_ext;
  if (!v1570.non_crit_ext_present) {
    return nullptr;
  }
  const auto& v1610 = v1570.non_crit_ext;
  if (!v1610.non_crit_ext_present) {
    return nullptr;
  }
  const auto& v1640 = v1610.non_crit_ext;
  if (!v1640.non_crit_ext_present) {
    return nullptr;
  }
  const auto& v1650 = v1640.non_crit_ext;
  if (!v1650.non_crit_ext_present) {
    return nullptr;
  }
  const auto& v1690 = v1650.non_crit_ext;
  if (!v1690.non_crit_ext_present) {
    return nullptr;
  }
  return &v1690.non_crit_ext;
}

ntn_ue_capability_summary
srsran::srs_cu_cp::evaluate_ntn_ue_capability(const byte_buffer& packed_ue_capability_rat_container_list)
{
  ntn_ue_capability_summary summary;
  if (packed_ue_capability_rat_container_list.empty()) {
    return summary;
  }

  asn1::rrc_nr::ue_cap_rat_container_list_l list;
  asn1::cbit_ref bref({packed_ue_capability_rat_container_list.begin(), packed_ue_capability_rat_container_list.end()});
  if (asn1::unpack_dyn_seq_of(list, bref, 0, 8) != asn1::SRSASN_SUCCESS) {
    summary.state  = ntn_ue_capability_state::parse_failed;
    summary.reason = "parse_failed";
    summary.profile_block_reason = summary.reason;
    return summary;
  }

  summary.parsed = true;
  for (const asn1::rrc_nr::ue_cap_rat_container_s& container : list) {
    if (container.rat_type.value != asn1::rrc_nr::rat_type_opts::nr) {
      continue;
    }

    summary.has_nr_container = true;
    asn1::rrc_nr::ue_nr_cap_s nr_cap;
    asn1::cbit_ref nr_bref({container.ue_cap_rat_container.begin(), container.ue_cap_rat_container.end()});
    if (nr_cap.unpack(nr_bref) != asn1::SRSASN_SUCCESS) {
      summary.state  = ntn_ue_capability_state::parse_failed;
      summary.reason = "parse_failed";
      summary.profile_block_reason = summary.reason;
      return summary;
    }

    const asn1::rrc_nr::ue_nr_cap_v1700_s* v1700 = get_v1700_extension(nr_cap);
    if (v1700 == nullptr || !v1700->non_terrestrial_network_r17_present) {
      summary.state  = ntn_ue_capability_state::unsupported;
      summary.reason = "missing_non_terrestrial_network_r17";
      summary.profile_block_reason = summary.reason;
      return summary;
    }

    summary.state                        = ntn_ue_capability_state::supported;
    summary.reason                       = "supported";
    summary.non_terrestrial_network_r17  = true;
    summary.ntn_scenario_support_r17     = v1700->ntn_scenario_support_r17_present;
    summary.ntn_parameters_r17           = v1700->ntn_params_r17_present;
    update_leo_ngso_profile_match(summary, *v1700);
    return summary;
  }

  summary.state  = ntn_ue_capability_state::unsupported;
  summary.reason = "no_nr_container";
  summary.profile_block_reason = summary.reason;
  return summary;
}
