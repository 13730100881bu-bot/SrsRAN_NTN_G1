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

#include "lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.h"
#include "fmt/format.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <set>

using namespace srsran;
using namespace srsran::srs_cu_cp;

namespace {

constexpr const char* catalog_hash        = "sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a";
constexpr const char* registry_hash       = "sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a";
constexpr const char* access_profile_hash = "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63";

constexpr std::chrono::microseconds access_slot{10000};
constexpr std::chrono::microseconds subvisit_duration{2500};
constexpr std::chrono::microseconds ssb_period{80000};
constexpr std::chrono::microseconds calendar_period{640000};

std::chrono::system_clock::time_point at_ms(int64_t milliseconds)
{
  return std::chrono::system_clock::time_point{std::chrono::milliseconds{milliseconds}};
}

ntn_onboard_position_plan_config make_config()
{
  ntn_onboard_position_plan_config config;
  config.enabled                            = true;
  config.satellite_id                       = "P01-S01";
  config.expected_catalog_id                = "global-land-l1-v1";
  config.expected_catalog_hash              = catalog_hash;
  config.expected_identity_registry_version = "mc-ntn-onboard-cell-registry-v1";
  config.expected_identity_registry_hash    = registry_hash;
  config.expected_access_profile_id         = "ntn-access-16a-64d-v1";
  config.expected_access_profile_hash       = access_profile_hash;
  config.onboard_cells[0]                   = {nr_cell_identity::create(0x123450001ULL).value(), 101};
  config.onboard_cells[1]                   = {nr_cell_identity::create(0x123450002ULL).value(), 202};
  return config;
}

ntn_versioned_position_plan make_plan(unsigned count)
{
  ntn_versioned_position_plan plan;
  plan.schema_version            = 2;
  plan.planning_run_id           = "planning-run-2026-07-15";
  plan.catalog_id                = "global-land-l1-v1";
  plan.catalog_hash              = catalog_hash;
  plan.identity_registry_version = "mc-ntn-onboard-cell-registry-v1";
  plan.identity_registry_hash    = registry_hash;
  plan.access_profile_id         = "ntn-access-16a-64d-v1";
  plan.access_profile_hash       = access_profile_hash;
  plan.satellite_id              = "P01-S01";
  plan.catalog_version           = 1;
  plan.schedule_version          = 7;
  plan.valid_from                = at_ms(640);
  plan.valid_until               = at_ms(64000);
  plan.activation_epoch          = at_ms(1920);
  plan.onboard_cells             = make_config().onboard_cells;
  for (unsigned i = 0; i != count; ++i) {
    plan.visible_l1_positions.push_back({fmt::format("G{:06}", i + 1),
                                         10.0 + static_cast<double>(i / 32) * 0.05,
                                         20.0 + static_cast<double>(i % 32) * 0.05,
                                         0x7f});
  }
  plan.content_hash = compute_ntn_position_plan_content_hash(plan);
  return plan;
}

const ntn_activated_position_plan& build_valid_plan(ntn_onboard_position_plan_controller& controller,
                                                    unsigned                              count = 256)
{
  EXPECT_TRUE(controller.submit(make_plan(count), at_ms(1280)).accepted);
  EXPECT_TRUE(controller.pending_plan().has_value());
  return *controller.pending_plan();
}

bool is_ssb_intent(const ntn_access_calendar_intent& intent)
{
  return intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
         intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar;
}

unsigned get_phase_index(const ntn_access_calendar_intent& intent, const ntn_onboard_position_plan_config& config)
{
  return static_cast<unsigned>((intent.start_time.count() / config.access_slot.count()) % config.access_phases.size());
}

uint16_t get_phase_port_mask(const ntn_access_calendar_intent& intent, const ntn_onboard_position_plan_config& config)
{
  const ntn_access_calendar_phase& phase = config.access_phases[get_phase_index(intent, config)];
  return intent.direction == ntn_access_calendar_direction::downlink ? phase.downlink_port_mask
                                                                     : phase.uplink_port_mask;
}

unsigned count_set_ports(uint16_t mask)
{
  unsigned count = 0;
  while (mask != 0) {
    count += mask & 1U;
    mask >>= 1U;
  }
  return count;
}

std::chrono::microseconds cyclic_delta(std::chrono::microseconds from, std::chrono::microseconds to)
{
  const int64_t delta = (to.count() - from.count() + calendar_period.count()) % calendar_period.count();
  return std::chrono::microseconds{delta};
}

} // namespace

TEST(ntn_access_calendar_audit, full_capacity_calendar_meets_port_ssb_prach_and_pairing_limits)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const auto&                          plan = build_valid_plan(controller);

  const auto audit =
      controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, plan.access_calendar);

  EXPECT_TRUE(audit.accepted);
  EXPECT_EQ(audit.nof_l1_positions, 256U);
  EXPECT_EQ(plan.access_calendar.size(), 2560U);
  EXPECT_EQ(audit.nof_calendar_intents, 2560U);
  EXPECT_EQ(audit.nof_ssb_intents, 2048U);
  EXPECT_EQ(audit.nof_prach_ro_intents, 256U);
  EXPECT_EQ(audit.nof_prach_ul_beam_intents, 256U);
  EXPECT_LE(audit.max_used_analog_ports_per_cell, 16U);
  EXPECT_LE(audit.max_used_analog_ports_per_satellite, 32U);
  EXPECT_EQ(audit.max_ssb_interval, std::chrono::milliseconds{80});
  EXPECT_EQ(audit.max_prach_interval, std::chrono::milliseconds{640});
  EXPECT_EQ(audit.prach_ro_without_beam, 0U);
  EXPECT_EQ(audit.prach_beam_without_ro, 0U);
  EXPECT_EQ(audit.duplicate_prach_ros, 0U);
  EXPECT_EQ(audit.duplicate_prach_ul_beams, 0U);
  EXPECT_EQ(audit.invalid_direction_intents, 0U);
  EXPECT_EQ(audit.invalid_phase_ports, 0U);
  EXPECT_EQ(audit.rar_placement_mismatches, 0U);
  EXPECT_EQ(audit.resource_conflicts, 0U);

  struct per_position_counts {
    unsigned ssb      = 0;
    unsigned prach_ro = 0;
    unsigned ul_beam  = 0;
  };
  std::map<std::string, per_position_counts> counts;
  for (const ntn_access_calendar_intent& intent : plan.access_calendar) {
    per_position_counts& position = counts[intent.position_id];
    if (is_ssb_intent(intent)) {
      ++position.ssb;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      ++position.prach_ro;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ul_beam) {
      ++position.ul_beam;
    }
  }
  ASSERT_EQ(counts.size(), 256U);
  for (const auto& [position_id, count] : counts) {
    SCOPED_TRACE(position_id);
    EXPECT_EQ(count.ssb, 8U);
    EXPECT_EQ(count.prach_ro, 1U);
    EXPECT_EQ(count.ul_beam, 1U);
  }
}

TEST(ntn_access_calendar_audit, opaque_nci_order_deterministically_selects_even_and_odd_access_slots)
{
  const ntn_onboard_position_plan_config config = make_config();
  ntn_onboard_position_plan_controller   controller(config);
  const auto&                            plan = build_valid_plan(controller);
  const nr_cell_identity                 smaller_nci =
      std::min(plan.cell_positions[0].identity.nci, plan.cell_positions[1].identity.nci);
  const nr_cell_identity larger_nci =
      std::max(plan.cell_positions[0].identity.nci, plan.cell_positions[1].identity.nci);

  for (const ntn_access_calendar_intent& intent : plan.access_calendar) {
    SCOPED_TRACE(intent.position_id);
    const unsigned slot = static_cast<unsigned>(intent.start_time.count() / access_slot.count());
    if (intent.nci == smaller_nci) {
      EXPECT_EQ(slot % 2U, 0U);
    } else {
      ASSERT_EQ(intent.nci, larger_nci);
      EXPECT_EQ(slot % 2U, 1U);
    }
  }
}

TEST(ntn_access_calendar_audit, full_capacity_slots_use_four_subvisits_and_the_bound_three_phase_port_split)
{
  const ntn_onboard_position_plan_config config = make_config();
  ntn_onboard_position_plan_controller   controller(config);
  const auto&                            plan = build_valid_plan(controller);

  ASSERT_EQ(config.access_phases.size(), 3U);
  EXPECT_EQ(count_set_ports(config.access_phases[0].downlink_port_mask), 11U);
  EXPECT_EQ(count_set_ports(config.access_phases[0].uplink_port_mask), 5U);
  EXPECT_EQ(count_set_ports(config.access_phases[1].downlink_port_mask), 11U);
  EXPECT_EQ(count_set_ports(config.access_phases[1].uplink_port_mask), 5U);
  EXPECT_EQ(count_set_ports(config.access_phases[2].downlink_port_mask), 10U);
  EXPECT_EQ(count_set_ports(config.access_phases[2].uplink_port_mask), 6U);
  EXPECT_EQ(config.access_phases[0].digital_downlink_capacity, 43U);
  EXPECT_EQ(config.access_phases[0].digital_uplink_capacity, 21U);
  EXPECT_EQ(config.access_phases[1].digital_downlink_capacity, 43U);
  EXPECT_EQ(config.access_phases[1].digital_uplink_capacity, 21U);
  EXPECT_EQ(config.access_phases[2].digital_downlink_capacity, 42U);
  EXPECT_EQ(config.access_phases[2].digital_uplink_capacity, 22U);

  std::map<std::pair<uint64_t, unsigned>, std::set<std::chrono::microseconds>> used_subvisits;
  std::array<uint16_t, 3>                                                      used_downlink_ports{};
  std::array<uint16_t, 3>                                                      used_uplink_ports{};
  for (const ntn_access_calendar_intent& intent : plan.access_calendar) {
    SCOPED_TRACE(intent.position_id);
    EXPECT_EQ(intent.duration, subvisit_duration);
    const unsigned                  slot = static_cast<unsigned>(intent.start_time.count() / access_slot.count());
    const std::chrono::microseconds offset{intent.start_time.count() % access_slot.count()};
    EXPECT_TRUE(offset == std::chrono::microseconds{0} || offset == std::chrono::microseconds{2500} ||
                offset == std::chrono::microseconds{5000} || offset == std::chrono::microseconds{7500});
    used_subvisits[{intent.nci.value(), slot}].insert(offset);

    if (intent.port_id == ntn_access_calendar_intent::no_resource_port) {
      ASSERT_EQ(intent.purpose, ntn_access_calendar_purpose::prach_ro);
      continue;
    }
    ASSERT_LT(intent.port_id, config.max_analog_ports_per_cell);
    EXPECT_NE(get_phase_port_mask(intent, config) & (uint16_t{1} << intent.port_id), 0U);
    if (intent.direction == ntn_access_calendar_direction::downlink) {
      used_downlink_ports[get_phase_index(intent, config)] |= uint16_t{1} << intent.port_id;
    } else {
      used_uplink_ports[get_phase_index(intent, config)] |= uint16_t{1} << intent.port_id;
    }
  }

  const std::set<std::chrono::microseconds> expected_offsets{std::chrono::microseconds{0},
                                                             std::chrono::microseconds{2500},
                                                             std::chrono::microseconds{5000},
                                                             std::chrono::microseconds{7500}};
  ASSERT_FALSE(used_subvisits.empty());
  for (const auto& [cell_slot, offsets] : used_subvisits) {
    SCOPED_TRACE(fmt::format("nci={} slot={}", cell_slot.first, cell_slot.second));
    EXPECT_EQ(offsets, expected_offsets);
  }
  for (unsigned phase = 0; phase != config.access_phases.size(); ++phase) {
    SCOPED_TRACE(phase);
    EXPECT_EQ(used_downlink_ports[phase], config.access_phases[phase].downlink_port_mask);
    EXPECT_EQ(used_uplink_ports[phase], config.access_phases[phase].uplink_port_mask);
  }
}

TEST(ntn_access_calendar_audit, every_position_has_an_80_ms_ssb_gap_including_the_640_ms_wrap)
{
  ntn_onboard_position_plan_controller                          controller(make_config());
  const auto&                                                   plan = build_valid_plan(controller);
  std::map<std::string, std::vector<std::chrono::microseconds>> ssb_offsets;
  for (const ntn_access_calendar_intent& intent : plan.access_calendar) {
    if (is_ssb_intent(intent)) {
      ssb_offsets[intent.position_id].push_back(intent.start_time);
    }
  }

  ASSERT_EQ(ssb_offsets.size(), 256U);
  for (auto& [position_id, offsets] : ssb_offsets) {
    SCOPED_TRACE(position_id);
    std::sort(offsets.begin(), offsets.end());
    ASSERT_EQ(offsets.size(), 8U);
    for (size_t i = 1; i != offsets.size(); ++i) {
      EXPECT_EQ(offsets[i] - offsets[i - 1], ssb_period);
    }
    EXPECT_EQ(calendar_period - offsets.back() + offsets.front(), ssb_period);
  }
}

TEST(ntn_access_calendar_audit, missing_one_ssb_visit_reports_interval_over_80_ms)
{
  ntn_onboard_position_plan_controller    controller(make_config());
  const auto&                             plan        = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents     = plan.access_calendar;
  const std::string                       position_id = plan.cell_positions[0].assigned_l1_ids.front();
  const auto removed = std::find_if(intents.begin(), intents.end(), [&position_id](const auto& intent) {
    return intent.position_id == position_id && (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
                                                 intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar);
  });
  ASSERT_NE(removed, intents.end());
  intents.erase(removed);

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::ssb_deadline_miss);
  EXPECT_GT(audit.max_ssb_interval, std::chrono::milliseconds{80});
}

TEST(ntn_access_calendar_audit, missing_prach_opportunity_reports_interval_over_640_ms)
{
  ntn_onboard_position_plan_controller    controller(make_config());
  const auto&                             plan        = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents     = plan.access_calendar;
  const std::string                       position_id = plan.cell_positions[0].assigned_l1_ids.front();
  intents.erase(std::remove_if(intents.begin(),
                               intents.end(),
                               [&position_id](const auto& intent) {
                                 return intent.position_id == position_id &&
                                        (intent.purpose == ntn_access_calendar_purpose::prach_ro ||
                                         intent.purpose == ntn_access_calendar_purpose::prach_ul_beam);
                               }),
                intents.end());

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::prach_deadline_miss);
  EXPECT_GT(audit.max_prach_interval, std::chrono::milliseconds{640});
}

TEST(ntn_access_calendar_audit, advertised_prach_ro_without_ul_beam_is_rejected)
{
  ntn_onboard_position_plan_controller    controller(make_config());
  const auto&                             plan    = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  const auto removed = std::find_if(intents.begin(), intents.end(), [](const auto& intent) {
    return intent.purpose == ntn_access_calendar_purpose::prach_ul_beam;
  });
  ASSERT_NE(removed, intents.end());
  intents.erase(removed);

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::prach_ro_without_beam);
  EXPECT_EQ(audit.prach_ro_without_beam, 1U);
  EXPECT_EQ(audit.prach_beam_without_ro, 0U);
}

TEST(ntn_access_calendar_audit, duplicate_prach_ro_is_rejected_before_deadline_checks)
{
  ntn_onboard_position_plan_controller    controller(make_config());
  const auto&                             plan    = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  const auto                              ro = std::find_if(intents.begin(), intents.end(), [](const auto& intent) {
    return intent.purpose == ntn_access_calendar_purpose::prach_ro;
  });
  ASSERT_NE(ro, intents.end());
  intents.push_back(*ro);

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::duplicate_prach_ro);
  EXPECT_EQ(audit.duplicate_prach_ros, 1U);
}

TEST(ntn_access_calendar_audit, duplicate_prach_ul_beam_is_rejected_before_resource_conflict_checks)
{
  ntn_onboard_position_plan_controller    controller(make_config());
  const auto&                             plan    = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  const auto                              beam = std::find_if(intents.begin(), intents.end(), [](const auto& intent) {
    return intent.purpose == ntn_access_calendar_purpose::prach_ul_beam;
  });
  ASSERT_NE(beam, intents.end());
  intents.push_back(*beam);

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::duplicate_prach_ul_beam);
  EXPECT_EQ(audit.duplicate_prach_ul_beams, 1U);
}

TEST(ntn_access_calendar_audit, prach_ul_beam_without_an_exact_ro_is_rejected_as_orphaned)
{
  ntn_onboard_position_plan_controller    controller(make_config());
  const auto&                             plan    = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  const auto                              ro = std::find_if(intents.begin(), intents.end(), [](const auto& intent) {
    return intent.purpose == ntn_access_calendar_purpose::prach_ro;
  });
  ASSERT_NE(ro, intents.end());
  intents.erase(ro);

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::prach_beam_without_ro);
  EXPECT_EQ(audit.prach_beam_without_ro, 1U);
  EXPECT_EQ(audit.prach_ro_without_beam, 0U);
}

TEST(ntn_access_calendar_audit, overlapping_port_time_resource_is_rejected)
{
  ntn_onboard_position_plan_controller    controller(make_config());
  const auto&                             plan    = build_valid_plan(controller, 64);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  const auto                              first = std::find_if(intents.begin(), intents.end(), [](const auto& intent) {
    return intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging;
  });
  ASSERT_NE(first, intents.end());
  const auto second = std::find_if(std::next(first), intents.end(), [&first](const auto& intent) {
    return intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging && intent.nci == first->nci &&
           intent.start_time == first->start_time && intent.port_id != first->port_id;
  });
  ASSERT_NE(second, intents.end());
  second->port_id = first->port_id;

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::resource_conflict);
  EXPECT_EQ(audit.resource_conflicts, 1U);
}

TEST(ntn_access_calendar_audit, wrong_direction_is_rejected_for_ssb_ro_and_ul_beam)
{
  ntn_onboard_position_plan_controller             controller(make_config());
  const auto&                                      plan = build_valid_plan(controller, 4);
  const std::array<ntn_access_calendar_purpose, 3> purposes{ntn_access_calendar_purpose::ssb_sib_paging,
                                                            ntn_access_calendar_purpose::prach_ro,
                                                            ntn_access_calendar_purpose::prach_ul_beam};

  for (ntn_access_calendar_purpose purpose : purposes) {
    SCOPED_TRACE(static_cast<unsigned>(purpose));
    std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
    const auto                              intent  = std::find_if(
        intents.begin(), intents.end(), [purpose](const auto& candidate) { return candidate.purpose == purpose; });
    ASSERT_NE(intent, intents.end());
    intent->direction = intent->direction == ntn_access_calendar_direction::downlink
                            ? ntn_access_calendar_direction::uplink
                            : ntn_access_calendar_direction::downlink;

    const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);
    EXPECT_FALSE(audit.accepted);
    EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::invalid_calendar_direction);
    EXPECT_EQ(audit.invalid_direction_intents, 1U);
  }
}

TEST(ntn_access_calendar_audit, unknown_calendar_purpose_is_rejected)
{
  ntn_onboard_position_plan_controller    controller(make_config());
  const auto&                             plan    = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  ASSERT_FALSE(intents.empty());
  intents.front().purpose = static_cast<ntn_access_calendar_purpose>(255);

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::invalid_calendar_position);
}

TEST(ntn_access_calendar_audit, off_grid_start_or_non_subvisit_duration_is_rejected)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const auto&                          plan = build_valid_plan(controller, 4);

  {
    std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
    ASSERT_FALSE(intents.empty());
    intents.front().start_time += std::chrono::microseconds{1};
    const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);
    EXPECT_FALSE(audit.accepted);
    EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::invalid_calendar_timing);
  }
  {
    std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
    ASSERT_FALSE(intents.empty());
    intents.front().duration -= std::chrono::microseconds{1};
    const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);
    EXPECT_FALSE(audit.accepted);
    EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::invalid_calendar_timing);
  }
}

TEST(ntn_access_calendar_audit, unsupported_local_access_profile_is_rejected_before_intent_audit)
{
  ntn_onboard_position_plan_controller valid_controller(make_config());
  const auto&                          plan           = build_valid_plan(valid_controller, 4);
  ntn_onboard_position_plan_config     drifted_config = make_config();
  ++drifted_config.subvisits_per_access_slot;
  ntn_onboard_position_plan_controller drifted_controller(drifted_config);

  const auto audit =
      drifted_controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, plan.access_calendar);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::invalid_calendar_profile);
}

TEST(ntn_access_calendar_audit, direction_port_outside_the_current_phase_mask_is_rejected)
{
  const ntn_onboard_position_plan_config           config = make_config();
  ntn_onboard_position_plan_controller             controller(config);
  const auto&                                      plan = build_valid_plan(controller, 4);
  const std::array<ntn_access_calendar_purpose, 2> purposes{ntn_access_calendar_purpose::ssb_sib_paging,
                                                            ntn_access_calendar_purpose::prach_ul_beam};

  for (ntn_access_calendar_purpose purpose : purposes) {
    SCOPED_TRACE(static_cast<unsigned>(purpose));
    std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
    const auto                              intent  = std::find_if(
        intents.begin(), intents.end(), [purpose](const auto& candidate) { return candidate.purpose == purpose; });
    ASSERT_NE(intent, intents.end());
    const uint16_t allowed_mask    = get_phase_port_mask(*intent, config);
    unsigned       disallowed_port = config.max_analog_ports_per_cell;
    for (unsigned port = 0; port != config.max_analog_ports_per_cell; ++port) {
      if ((allowed_mask & (uint16_t{1} << port)) == 0) {
        disallowed_port = port;
        break;
      }
    }
    ASSERT_LT(disallowed_port, config.max_analog_ports_per_cell);
    intent->port_id = static_cast<uint16_t>(disallowed_port);

    const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);
    EXPECT_FALSE(audit.accepted);
    EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::invalid_access_phase_port);
    EXPECT_EQ(audit.invalid_phase_ports, 1U);
  }
}

TEST(ntn_access_calendar_audit, rar_is_coalesced_with_the_first_cyclic_ssb_after_each_ro)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const auto&                          plan = build_valid_plan(controller);

  std::map<std::string, const ntn_access_calendar_intent*>              ro_by_position;
  std::map<std::string, std::vector<const ntn_access_calendar_intent*>> ssb_by_position;
  for (const ntn_access_calendar_intent& intent : plan.access_calendar) {
    if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      ASSERT_TRUE(ro_by_position.emplace(intent.position_id, &intent).second);
    } else if (is_ssb_intent(intent)) {
      ssb_by_position[intent.position_id].push_back(&intent);
    }
  }

  ASSERT_EQ(ro_by_position.size(), 256U);
  ASSERT_EQ(ssb_by_position.size(), 256U);
  for (const auto& [position_id, ro] : ro_by_position) {
    SCOPED_TRACE(position_id);
    const auto& ssbs = ssb_by_position.at(position_id);
    ASSERT_EQ(ssbs.size(), 8U);
    const ntn_access_calendar_intent* next_ssb   = nullptr;
    std::chrono::microseconds         next_delta = calendar_period;
    unsigned                          rar_count  = 0;
    for (const ntn_access_calendar_intent* ssb : ssbs) {
      rar_count += ssb->purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar;
      const std::chrono::microseconds delta = cyclic_delta(ro->start_time, ssb->start_time);
      if (delta.count() != 0 && delta < next_delta) {
        next_delta = delta;
        next_ssb   = ssb;
      }
    }
    ASSERT_NE(next_ssb, nullptr);
    EXPECT_EQ(rar_count, 1U);
    EXPECT_EQ(next_ssb->purpose, ntn_access_calendar_purpose::ssb_sib_paging_rar);
  }

  EXPECT_EQ(plan.calendar_audit.rar_placement_mismatches, 0U);
  EXPECT_TRUE(plan.calendar_audit.accepted);
}

TEST(ntn_access_calendar_audit, rar_on_a_later_ssb_instead_of_the_next_cyclic_ssb_is_rejected)
{
  ntn_onboard_position_plan_controller    controller(make_config());
  const auto&                             plan    = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  const auto                              rar = std::find_if(intents.begin(), intents.end(), [](const auto& intent) {
    return intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar;
  });
  ASSERT_NE(rar, intents.end());
  const auto later_ssb = std::find_if(intents.begin(), intents.end(), [&rar](const auto& intent) {
    return intent.position_id == rar->position_id && intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging &&
           cyclic_delta(rar->start_time, intent.start_time) > ssb_period;
  });
  ASSERT_NE(later_ssb, intents.end());
  rar->purpose       = ntn_access_calendar_purpose::ssb_sib_paging;
  later_ssb->purpose = ntn_access_calendar_purpose::ssb_sib_paging_rar;

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::invalid_rar_placement);
  EXPECT_EQ(audit.rar_placement_mismatches, 1U);
}
