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

#include "cell_scheduler.h"
#include "logging/scheduler_metrics_handler.h"
#include "ue_scheduling/ue_scheduler_impl.h"
#include <algorithm>
#include <map>

using namespace srsran;

namespace {

uint32_t compute_max_cyclic_gap(std::map<std::string, std::vector<uint32_t>>& opportunity_offsets, uint32_t cycle_slots)
{
  uint32_t max_gap = 0;
  for (auto& position_opportunities : opportunity_offsets) {
    std::vector<uint32_t>& offsets = position_opportunities.second;
    if (offsets.empty()) {
      // A position with no matched opportunity has no bounded cyclic gap. cycle + 1 is the smallest finite value that
      // reports this condition without introducing another optional field.
      max_gap = std::max(max_gap, cycle_slots + 1U);
      continue;
    }

    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    if (offsets.size() == 1) {
      max_gap = std::max(max_gap, cycle_slots);
      continue;
    }

    for (unsigned i = 1; i != offsets.size(); ++i) {
      max_gap = std::max(max_gap, offsets[i] - offsets[i - 1]);
    }
    max_gap = std::max(max_gap, cycle_slots - offsets.back() + offsets.front());
  }
  return max_gap;
}

} // namespace

cell_scheduler::cell_scheduler(const scheduler_expert_config&                  sched_cfg,
                               const sched_cell_configuration_request_message& msg,
                               const cell_configuration&                       cell_cfg_,
                               ue_scheduler&                                   ue_sched_,
                               cell_metrics_handler&                           metrics_handler) :
  cell_cfg(cell_cfg_),
  res_grid(cell_cfg),
  access_calendar_numerology(to_numerology_value(cell_cfg.scs_common)),
  access_calendar_minimum_lead_slots(std::max(res_grid.max_dl_slot_alloc_delay, res_grid.max_ul_slot_alloc_delay) + 1),
  event_logger(cell_cfg.cell_index, cell_cfg.pci),
  metrics(metrics_handler),
  result_logger(sched_cfg.log_broadcast_messages, cell_cfg.pci),
  logger(srslog::fetch_basic_logger("SCHED")),
  ssb_sch(cell_cfg),
  pdcch_sch(cell_cfg),
  si_sch(cell_cfg, pdcch_sch, msg),
  csi_sch(cell_cfg),
  ra_sch(sched_cfg.ra, cell_cfg, pdcch_sch, event_logger, metrics),
  prach_sch(cell_cfg),
  pucch_alloc(cell_cfg, sched_cfg.ue.max_pucchs_per_slot, sched_cfg.ue.max_ul_grants_per_slot),
  uci_alloc(pucch_alloc),
  pg_sch(sched_cfg, cell_cfg, pdcch_sch, msg)
{
  // Register new cell in the UE scheduler.
  ue_sched = ue_sched_.add_cell(ue_cell_scheduler_creation_request{
      msg.cell_index, &pdcch_sch, &pucch_alloc, &uci_alloc, &res_grid, &metrics, &event_logger});
}

ntn_access_calendar_response
cell_scheduler::handle_ntn_access_calendar_update(const ntn_access_calendar_request& request)
{
  // Preflight must remain ahead of lazy gate creation: it only inspects the immutable cell radio configuration.
  if (request.operation == ntn_access_calendar_operation::preflight) {
    return preflight_ntn_access_calendar(request);
  }

  scheduler_ntn_access_calendar_gate* gate = access_calendar_gate.load(std::memory_order_acquire);
  if (gate == nullptr) {
    if (request.operation != ntn_access_calendar_operation::prepare) {
      ntn_access_calendar_response response;
      response.state        = ntn_access_calendar_state::cleared;
      response.reason       = ntn_access_calendar_reject_reason::none;
      response.version      = request.version;
      response.content_hash = request.content_hash;
      return response;
    }
    std::lock_guard<std::mutex> lock(access_calendar_creation_mutex);
    gate = access_calendar_gate.load(std::memory_order_relaxed);
    if (gate == nullptr) {
      access_calendar_gate_owner = std::make_unique<scheduler_ntn_access_calendar_gate>(
          cell_cfg.cell_index, access_calendar_numerology, access_calendar_minimum_lead_slots);
      gate = access_calendar_gate_owner.get();
      access_calendar_gate.store(gate, std::memory_order_release);
    }
  }
  return gate->handle_update(request);
}

ntn_access_calendar_response
cell_scheduler::preflight_ntn_access_calendar(const ntn_access_calendar_request& request) const
{
  ntn_access_calendar_response response;
  response.version                   = request.version;
  response.content_hash              = request.content_hash;
  response.effective_activation_slot = request.activation_slot;
  response.minimum_lead_slots        = access_calendar_minimum_lead_slots;
  response.preflight.performed       = true;
  response.preflight.numerology      = access_calendar_numerology;

  auto reject = [&response](ntn_access_calendar_reject_reason reason) {
    response.state            = ntn_access_calendar_state::rejected;
    response.reason           = reason;
    response.preflight.passed = false;
    return response;
  };

  if (request.cell_index != cell_cfg.cell_index) {
    return reject(ntn_access_calendar_reject_reason::cell_not_configured);
  }
  if (!request.activation_slot.valid() || request.activation_slot.numerology() != access_calendar_numerology) {
    return reject(ntn_access_calendar_reject_reason::invalid_activation_slot);
  }
  if (request.cycle_slots == 0 || request.cycle_slots > MAX_NTN_ACCESS_CALENDAR_CYCLE_SLOTS) {
    return reject(ntn_access_calendar_reject_reason::invalid_cycle);
  }

  std::vector<bool> ssb_opportunities(request.cycle_slots);
  std::vector<bool> prach_opportunities(request.cycle_slots);
  for (uint32_t offset = 0; offset != request.cycle_slots; ++offset) {
    const slot_point sl         = request.activation_slot + offset;
    ssb_opportunities[offset]   = ssb_sch.has_ssb_opportunity(sl);
    prach_opportunities[offset] = prach_sch.has_prach_opportunity(sl);
  }

  std::map<std::string, std::vector<uint32_t>> ssb_matches_by_position;
  std::map<std::string, std::vector<uint32_t>> prach_matches_by_position;
  bool                                         invalid_expectation = false;

  for (const ntn_access_calendar_expectation& expectation : request.expectations) {
    const bool purpose_valid = expectation.purpose == ntn_access_calendar_purpose::ssb ||
                               expectation.purpose == ntn_access_calendar_purpose::prach;
    if (purpose_valid) {
      uint32_t& expected_count = expectation.purpose == ntn_access_calendar_purpose::ssb
                                     ? response.preflight.expected_ssb
                                     : response.preflight.expected_prach;
      ++expected_count;
    }
    const bool window_valid = expectation.nof_slots != 0 && expectation.start_slot_offset < request.cycle_slots &&
                              expectation.nof_slots <= request.cycle_slots - expectation.start_slot_offset;
    if (expectation.position_id.empty() || !purpose_valid || !window_valid) {
      if (purpose_valid && !expectation.position_id.empty()) {
        auto& matches_by_position = expectation.purpose == ntn_access_calendar_purpose::ssb ? ssb_matches_by_position
                                                                                            : prach_matches_by_position;
        matches_by_position[expectation.position_id];
      }
      invalid_expectation = true;
      if (!response.preflight.first_unmatched_present) {
        response.preflight.first_unmatched_present           = true;
        response.preflight.first_unmatched_position_id       = expectation.position_id;
        response.preflight.first_unmatched_purpose           = expectation.purpose;
        response.preflight.first_unmatched_start_slot_offset = expectation.start_slot_offset;
        response.preflight.first_unmatched_nof_slots         = expectation.nof_slots;
      }
      continue;
    }

    const std::vector<bool>& opportunities =
        expectation.purpose == ntn_access_calendar_purpose::ssb ? ssb_opportunities : prach_opportunities;
    std::map<std::string, std::vector<uint32_t>>& matches_by_position =
        expectation.purpose == ntn_access_calendar_purpose::ssb ? ssb_matches_by_position : prach_matches_by_position;
    std::vector<uint32_t>& matched_offsets = matches_by_position[expectation.position_id];

    uint32_t& matched_count = expectation.purpose == ntn_access_calendar_purpose::ssb
                                  ? response.preflight.matched_ssb
                                  : response.preflight.matched_prach;

    const auto opportunity_it = std::find(opportunities.begin() + expectation.start_slot_offset,
                                          opportunities.begin() + expectation.start_slot_offset + expectation.nof_slots,
                                          true);
    if (opportunity_it != opportunities.begin() + expectation.start_slot_offset + expectation.nof_slots) {
      ++matched_count;
      matched_offsets.push_back(static_cast<uint32_t>(std::distance(opportunities.begin(), opportunity_it)));
      continue;
    }

    if (!response.preflight.first_unmatched_present) {
      response.preflight.first_unmatched_present           = true;
      response.preflight.first_unmatched_position_id       = expectation.position_id;
      response.preflight.first_unmatched_purpose           = expectation.purpose;
      response.preflight.first_unmatched_start_slot_offset = expectation.start_slot_offset;
      response.preflight.first_unmatched_nof_slots         = expectation.nof_slots;
    }
  }

  response.preflight.max_ssb_gap_slots   = compute_max_cyclic_gap(ssb_matches_by_position, request.cycle_slots);
  response.preflight.max_prach_gap_slots = compute_max_cyclic_gap(prach_matches_by_position, request.cycle_slots);
  response.preflight.passed = !invalid_expectation &&
                              response.preflight.matched_ssb == response.preflight.expected_ssb &&
                              response.preflight.matched_prach == response.preflight.expected_prach;
  response.state = response.preflight.passed ? ntn_access_calendar_state::ready : ntn_access_calendar_state::rejected;
  response.reason = response.preflight.passed
                        ? ntn_access_calendar_reject_reason::none
                        : (invalid_expectation ? ntn_access_calendar_reject_reason::invalid_window
                                               : ntn_access_calendar_reject_reason::static_opportunity_missing);
  return response;
}

void cell_scheduler::handle_si_update_request(const si_scheduling_update_request& msg)
{
  si_sch.handle_si_update_request(msg);
}

void cell_scheduler::handle_slice_reconfiguration_request(const du_cell_slice_reconfig_request& slice_reconf_req)
{
  ue_sched->handle_slice_reconfiguration_request(slice_reconf_req);
}

void cell_scheduler::handle_crc_indication(const ul_crc_indication& crc_ind)
{
  bool has_msg3_crcs = std::any_of(
      crc_ind.crcs.begin(), crc_ind.crcs.end(), [](const auto& pdu) { return pdu.ue_index == INVALID_DU_UE_INDEX; });

  if (has_msg3_crcs) {
    ul_crc_indication msg3_crcs{}, ue_crcs{};
    msg3_crcs.sl_rx      = crc_ind.sl_rx;
    msg3_crcs.cell_index = crc_ind.cell_index;
    ue_crcs.sl_rx        = crc_ind.sl_rx;
    ue_crcs.cell_index   = crc_ind.cell_index;
    for (const ul_crc_pdu_indication& crc_pdu : crc_ind.crcs) {
      if (crc_pdu.ue_index == INVALID_DU_UE_INDEX) {
        msg3_crcs.crcs.push_back(crc_pdu);
      } else {
        ue_crcs.crcs.push_back(crc_pdu);
      }
    }
    // Forward CRC to Msg3 HARQs that has no ueId yet associated.
    ra_sch.handle_crc_indication(msg3_crcs);
    // Forward remaining CRCs to UE scheduler.
    ue_sched->get_feedback_handler().handle_crc_indication(ue_crcs);
  } else {
    ue_sched->get_feedback_handler().handle_crc_indication(crc_ind);
  }
}

void cell_scheduler::run_slot(slot_point sl_tx)
{
  // Mark the start of the slot.
  auto slot_start_tp = std::chrono::high_resolution_clock::now();

  // Consume a prepared calendar before any common scheduler starts filling current or future resource-grid slots.
  scheduler_ntn_access_calendar_gate* calendar_gate = access_calendar_gate.load(std::memory_order_acquire);
  if (calendar_gate != nullptr) {
    calendar_gate->slot_indication(sl_tx);
  }

  // If there are skipped slots, handle them. Otherwise, the cell grid and cached results are not correctly cleared.
  if (SRSRAN_LIKELY(res_grid.slot_tx().valid())) {
    while (SRSRAN_UNLIKELY(res_grid.slot_tx() + 1 != sl_tx)) {
      slot_point skipped_slot = res_grid.slot_tx() + 1;
      logger.info("cell={}: Detected skipped slot={}.", fmt::underlying(cell_cfg.cell_index), skipped_slot);
      reset_resource_grid(skipped_slot);
    }
  } else {
    if (SRSRAN_UNLIKELY(not active)) {
      // Implicitly activate cell on slot_indication.
      start();
    }
  }

  // > Start with clearing old allocations from the grid.
  reset_resource_grid(sl_tx);

  // > SSB scheduling.
  ssb_sch.run_slot(res_grid, sl_tx);

  // > Schedule CSI-RS.
  csi_sch.run_slot(res_grid[0]);

  // > Schedule SIB1 and SI-message signalling.
  si_sch.run_slot(res_grid);

  // > Schedule PRACH PDUs.
  prach_sch.run_slot(res_grid);

  // > Schedule RARs and Msg3.
  ra_sch.run_slot(res_grid);

  // > Schedule Paging.
  pg_sch.run_slot(res_grid);

  // > Schedule UE DL and UL data.
  ue_sched->run_slot(sl_tx);

  // Static SSB and PRACH opportunities are always prefilled so an aborted pending plan can immediately fall back to
  // the previous plan. Suppress only the current scheduler result before it is handed to MAC/PHY. The corresponding
  // resource-grid reservation remains conservative and no unauthorized PDU can reach RF.
  if (calendar_gate != nullptr && not calendar_gate->is_allowed(sl_tx, ntn_access_calendar_purpose::ssb)) {
    res_grid[0].result.dl.bc.ssb_info.clear();
  }
  if (calendar_gate != nullptr && not calendar_gate->is_allowed(sl_tx, ntn_access_calendar_purpose::prach)) {
    res_grid[0].result.ul.prachs.clear();
  }

  // > Mark stop of the slot processing
  auto slot_stop_tp = std::chrono::high_resolution_clock::now();
  auto slot_dur     = std::chrono::duration_cast<std::chrono::microseconds>(slot_stop_tp - slot_start_tp);

  // > Log processed events.
  event_logger.log();

  // > Log the scheduler results.
  result_logger.on_scheduler_result(last_result(), slot_dur);

  // > Push the scheduler results to the metrics handler.
  metrics.push_result(sl_tx, last_result(), slot_dur);
}

void cell_scheduler::handle_error_indication(slot_point sl_tx, scheduler_slot_handler::error_outcome event)
{
  ue_sched->handle_error_indication(sl_tx, event);
}

void cell_scheduler::reset_resource_grid(slot_point sl_tx)
{
  // Reset cell resource grid.
  res_grid.slot_indication(sl_tx);

  // Reset PDCCH slot context.
  pdcch_sch.slot_indication(sl_tx);

  // Reset PUCCH slot context.
  pucch_alloc.slot_indication(sl_tx);

  // Reset UCI slot context.
  uci_alloc.slot_indication(sl_tx);
}

void cell_scheduler::start()
{
  if (active) {
    return;
  }
  active = true;
  logger.info("cell={}: Cell scheduling was activated.", fmt::underlying(cell_cfg.cell_index));

  ue_sched->start();
}

void cell_scheduler::stop()
{
  // From this point onwards, no slot indications are expected until the cell is reenabled.

  if (not active) {
    // Do nothing.
    return;
  }
  active = false;
  logger.info("cell={}: Cell scheduling was deactivated.", fmt::underlying(cell_cfg.cell_index));

  // Stop sub-schedulers.
  ssb_sch.stop();
  si_sch.stop();
  prach_sch.stop();
  ra_sch.stop();
  pg_sch.stop();
  ue_sched->stop();

  // Reset resource grid and sub-allocators.
  res_grid.stop();
  pdcch_sch.stop();
  pucch_alloc.stop();
  uci_alloc.stop();

  // Report last metrics.
  metrics.handle_cell_deactivation();
}
