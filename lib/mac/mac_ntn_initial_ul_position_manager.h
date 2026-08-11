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
 * the LICENSE file in the top-level directory of this distribution.
 *
 */

#pragma once

#include "srsran/mac/mac_cell_rach_handler.h"
#include "srsran/mac/mac_manager.h"
#include "srsran/mac/mac_ntn_initial_ul_position.h"
#include "srsran/ofh/transmitter/ofh_uplink_request_handler.h"
#include "srsran/phy/support/prach_buffer_context.h"
#include "srsran/ran/prach/verified_prach_rx_context.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace srsran {

/// Bounded correlation store between one PRACH detection and the later Msg3 UL-CCCH.
class mac_ntn_initial_ul_position_manager : public ofh::prach_beam_context_provider
{
public:
  explicit mac_ntn_initial_ul_position_manager(mac_ntn_rx_mapping_config config = {})
  {
    update_rx_mapping(std::move(config));
  }

  void update_rx_mapping(mac_ntn_rx_mapping_config config)
  {
    std::lock_guard<std::mutex> lock(mutex);
    mapping       = std::move(config);
    mapping_valid = validate_mapping(mapping);
    pending.clear();
  }

  bool is_rx_mapping_valid() const
  {
    std::lock_guard<std::mutex> lock(mutex);
    return mapping_valid;
  }

  bool has_ofh_rx_mapping() const
  {
    std::lock_guard<std::mutex> lock(mutex);
    return mapping_valid && std::any_of(mapping.entries.begin(), mapping.entries.end(), [](const auto& entry) {
             return entry.backend == mac_ntn_rx_backend::ofh;
           });
  }

  std::optional<float> rx_port_attribution_margin_db() const
  {
    std::lock_guard<std::mutex> lock(mutex);
    return mapping_valid ? std::optional<float>{mapping.unique_margin_db} : std::nullopt;
  }

  std::optional<static_vector<ofh::prach_eaxc_beam_context, ofh::MAX_NOF_SUPPORTED_EAXC>>
  get_prach_beam_context(const prach_buffer_context& context) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!mapping_valid || !context.calendar_position_valid || context.calendar_schedule_version == 0 ||
        context.sector >= MAX_NOF_DU_CELLS) {
      return std::nullopt;
    }

    const calendar_record* calendar = find_calendar_by_version(context.calendar_schedule_version);
    if (calendar == nullptr) {
      return std::nullopt;
    }
    const calendar_cell_match cell = find_calendar_cell(*calendar,
                                                        to_du_cell_index(static_cast<uint16_t>(context.sector)),
                                                        context.calendar_position_valid,
                                                        context.calendar_cycle_index,
                                                        context.occasion_offset_us);
    if (cell.cell == nullptr) {
      return std::nullopt;
    }

    static_vector<ofh::prach_eaxc_beam_context, ofh::MAX_NOF_SUPPORTED_EAXC> result;
    for (const mac_ntn_rx_port_mapping& entry : mapping.entries) {
      if (entry.nci != cell.cell->nci || entry.backend != mac_ntn_rx_backend::ofh) {
        continue;
      }

      const mac_ntn_access_calendar_intent* matched_intent = nullptr;
      for (const mac_ntn_access_calendar_intent& intent : cell.cell->intents) {
        if (intent.direction != mac_ntn_access_calendar_direction::uplink ||
            intent.purpose != mac_ntn_access_calendar_purpose::prach_ul_beam ||
            intent.port_id != entry.cell_local_port) {
          continue;
        }
        const uint64_t start = static_cast<uint64_t>(intent.start_time.count());
        const uint64_t end   = start + static_cast<uint64_t>(intent.duration.count());
        if (cell.occasion_offset_us < start || cell.occasion_offset_us >= end) {
          continue;
        }
        if (matched_intent != nullptr && matched_intent->position_id != intent.position_id) {
          return std::nullopt;
        }
        matched_intent = &intent;
      }
      if (matched_intent == nullptr) {
        continue;
      }
      const bool duplicate_eaxc = entry.prach_eaxc.has_value() &&
                                  std::any_of(result.begin(), result.end(), [&](const auto& item) {
                                    return item.eaxc == entry.prach_eaxc.value();
                                  });
      if (!entry.prach_eaxc.has_value() || !entry.beam_id.has_value() || result.full() || duplicate_eaxc) {
        return std::nullopt;
      }

      ofh::prach_eaxc_beam_context& item = result.emplace_back();
      item.eaxc                              = entry.prach_eaxc.value();
      item.context.beam_id                   = entry.beam_id.value();
      item.context.logical_port_id           = entry.cell_local_port;
      item.context.position_id               = matched_intent->position_id;
      item.context.schedule_version          = calendar->request.schedule_version;
      item.context.calendar_hash             = calendar->request.calendar_hash;
      item.context.mapping_generation        = mapping.version;
      item.context.mapping_hash              = mapping.hash;
    }

    return result.empty() ? std::nullopt : std::optional{std::move(result)};
  }

  void handle_calendar_result(const mac_ntn_access_calendar_update& request,
                              const mac_ntn_access_calendar_result& result)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!result.accepted()) {
      return;
    }

    if (request.operation == mac_ntn_access_calendar_operation::prepare ||
        (request.operation == mac_ntn_access_calendar_operation::query &&
         result.status == mac_ntn_access_calendar_status::ready)) {
      // A preparing result has not yet been consumed by both cell slot threads and cannot be used to predict the
      // calendar identity of a future PRACH request. Store only a fully armed snapshot.
      if (result.status != mac_ntn_access_calendar_status::ready) {
        return;
      }
      if (!result.effective_activation_slot.valid()) {
        return;
      }
      calendar_record record;
      record.request         = request;
      record.activation_slot = result.effective_activation_slot;
      staged                 = std::move(record);
      return;
    }

    const auto matches = [&request](const std::optional<calendar_record>& record) {
      return record.has_value() && record->request.schedule_version == request.schedule_version &&
             record->request.calendar_hash == request.calendar_hash;
    };
    if (request.operation == mac_ntn_access_calendar_operation::query &&
        result.status == mac_ntn_access_calendar_status::applied) {
      if (matches(active)) {
        // Repeated applied queries are idempotent and must not discard an observation that is waiting for Msg3.
      } else if (matches(staged)) {
        if (active.has_value()) {
          previous_active = active;
        }
        active = staged;
        staged.reset();
      } else if (result.effective_activation_slot.valid()) {
        if (active.has_value()) {
          previous_active = active;
        }
        calendar_record record;
        record.request         = request;
        record.activation_slot = result.effective_activation_slot;
        active                 = std::move(record);
      } else {
        return;
      }
      retain_pending_for_active_calendar();
    } else if (request.operation == mac_ntn_access_calendar_operation::clear &&
               result.status == mac_ntn_access_calendar_status::cleared) {
      if (matches(staged)) {
        staged.reset();
      }
      if (matches(active)) {
        if (previous_active.has_value() &&
            std::chrono::system_clock::now() < previous_active->request.valid_until) {
          active = previous_active;
        } else {
          active.reset();
        }
        previous_active.reset();
      } else if (matches(previous_active)) {
        previous_active.reset();
      }
      retain_pending_for_active_calendar();
    }
  }

  void record_initial_prach(du_cell_index_t                                     cell_index,
                            slot_point                                          prach_slot,
                            rnti_t                                              c_rnti,
                            uint32_t                                            rnti_generation,
                            bool                                                calendar_position_valid,
                            uint64_t                                            calendar_schedule_version,
                            uint64_t                                            calendar_cycle_index,
                            uint32_t                                            occasion_offset_us,
                            mac_rach_indication::rx_port_attribution_status     port_status,
                            std::optional<unsigned>                             strongest_rx_port,
                            std::optional<float>                                strongest_to_second_margin_db,
                            std::shared_ptr<const verified_prach_rx_context>    verified_context = nullptr,
                            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
  {
    std::lock_guard<std::mutex> lock(mutex);
    prune(now);
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const pending_record& item) {
                    return item.record.cell_index == cell_index && item.record.c_rnti == c_rnti &&
                           item.record.rnti_generation != rnti_generation;
                  }),
                  pending.end());

    // The manager is always present in MAC. Without an installed NTN calendar, preserve the terrestrial path and do
    // not create bookkeeping records for ordinary PRACH detections.
    if (!active.has_value() && !staged.has_value()) {
      return;
    }

    mac_ntn_initial_ul_position_record record;
    record.observation_id  = allocate_observation_id();
    record.cell_index      = cell_index;
    record.c_rnti          = c_rnti;
    record.rnti_generation = rnti_generation;
    record.prach_slot      = prach_slot;
    record.receive_port_margin_db = strongest_to_second_margin_db.value_or(0.0F);

    if (rnti_generation == 0) {
      record.reason = "rnti_generation_mismatch";
      store(std::move(record), now);
      return;
    }

    const calendar_record* calendar =
        calendar_position_valid ? find_calendar_by_version(calendar_schedule_version) : nullptr;
    const calendar_cell_match cell = calendar == nullptr
                                         ? calendar_cell_match{}
                                         : find_calendar_cell(*calendar,
                                                              cell_index,
                                                              calendar_position_valid,
                                                              calendar_cycle_index,
                                                              occasion_offset_us);
    if (cell.cell == nullptr) {
      record.reason = calendar_position_valid && calendar_schedule_version != 0 ? "calendar_position_mismatch"
                                                                                : "calendar_position_unavailable";
      store(std::move(record), now);
      return;
    }
    record.nci                 = cell.cell->nci;
    record.pci                 = cell.cell->pci;
    record.satellite_id        = calendar->request.satellite_id;
    record.catalog_version     = calendar->request.catalog_version;
    record.schedule_version    = calendar->request.schedule_version;
    record.source_content_hash = calendar->request.source_content_hash;
    record.calendar_hash       = calendar->request.calendar_hash;
    record.calendar_cycle_index = cell.calendar_cycle_index;
    record.cycle_slot_offset   = cell.cycle_slot_offset;
    record.occasion_offset_us  = cell.occasion_offset_us;

    std::vector<const mac_ntn_access_calendar_intent*> candidates;
    for (const mac_ntn_access_calendar_intent& intent : cell.cell->intents) {
      if (intent.direction != mac_ntn_access_calendar_direction::uplink ||
          intent.purpose != mac_ntn_access_calendar_purpose::prach_ul_beam) {
        continue;
      }
      const uint64_t start = static_cast<uint64_t>(intent.start_time.count());
      const uint64_t end   = start + static_cast<uint64_t>(intent.duration.count());
      if (cell.occasion_offset_us >= start && cell.occasion_offset_us < end) {
        candidates.push_back(&intent);
      }
    }
    if (candidates.empty()) {
      record.reason = "receive_port_unavailable";
      store(std::move(record), now);
      return;
    }

    if (port_status == mac_rach_indication::rx_port_attribution_status::ambiguous) {
      record.reason = "ambiguous_receive_position";
      store(std::move(record), now);
      return;
    }

    const mac_ntn_access_calendar_intent* selected = nullptr;
    mac_ntn_initial_ul_position_authority verified_authority = mac_ntn_initial_ul_position_authority::none;
    const mac_ntn_rx_port_mapping*        selected_mapping   = nullptr;
    const char*                           attribution_failure = nullptr;
    if (port_status == mac_rach_indication::rx_port_attribution_status::unique && strongest_rx_port.has_value()) {
      if (strongest_rx_port.value() > std::numeric_limits<uint16_t>::max()) {
        attribution_failure = "receive_port_unavailable";
      } else {
        record.physical_rx_port = static_cast<uint16_t>(strongest_rx_port.value());
        selected_mapping        = find_mapping(cell.cell->nci, record.physical_rx_port);
        if (selected_mapping != nullptr) {
          std::vector<const mac_ntn_access_calendar_intent*> mapped_candidates;
          std::copy_if(candidates.begin(),
                       candidates.end(),
                       std::back_inserter(mapped_candidates),
                       [selected_mapping](const auto* intent) {
                         return intent->port_id == selected_mapping->cell_local_port;
                       });
          const auto unique_mapped_candidate =
              mapped_candidates.empty() ? std::optional<const mac_ntn_access_calendar_intent*>{}
                                        : unique_candidate(mapped_candidates);
          if (unique_mapped_candidate.has_value()) {
            const mac_ntn_access_calendar_intent* mapped_intent = unique_mapped_candidate.value();
            if (selected_mapping->backend == mac_ntn_rx_backend::sdr) {
              selected            = mapped_intent;
              verified_authority  = mac_ntn_initial_ul_position_authority::sdr_rx_port_verified;
            } else if (selected_mapping->backend == mac_ntn_rx_backend::ofh) {
              if (verified_context == nullptr) {
                attribution_failure = "ofh_beam_capability_unavailable";
              } else if (matches_ofh_context(*selected_mapping, *mapped_intent, *verified_context, *calendar)) {
                selected           = mapped_intent;
                verified_authority = mac_ntn_initial_ul_position_authority::ofh_beam_id_verified;
              } else {
                attribution_failure = "rx_mapping_mismatch";
              }
            }
          } else {
            attribution_failure = "rx_mapping_mismatch";
          }
        } else {
          attribution_failure = "rx_mapping_mismatch";
        }
      }
    } else if (port_status == mac_rach_indication::rx_port_attribution_status::unique) {
      attribution_failure = "receive_port_unavailable";
    }

    if (selected == nullptr) {
      // A measured physical port is authoritative input. If it cannot be resolved through the configured mapping,
      // do not silently replace that failed check with a calendar-only attribution. Audit mode will still continue
      // RRC admission using the explicit reason, while strict mode fails closed.
      if (port_status == mac_rach_indication::rx_port_attribution_status::unique) {
        record.reason = attribution_failure != nullptr ? attribution_failure : "rx_mapping_mismatch";
        store(std::move(record), now);
        return;
      }
      if (port_status != mac_rach_indication::rx_port_attribution_status::unavailable) {
        record.reason = "receive_port_unavailable";
        store(std::move(record), now);
        return;
      }
      const auto unique_calendar_candidate = unique_candidate(candidates);
      if (!unique_calendar_candidate.has_value()) {
        record.reason = "ambiguous_receive_position";
        store(std::move(record), now);
        return;
      }
      selected           = unique_calendar_candidate.value();
      verified_authority = mac_ntn_initial_ul_position_authority::software_attributed;
    }

    record.position_id     = selected->position_id;
    record.cell_local_port = selected->port_id;
    record.authority       = verified_authority;
    if (verified_authority == mac_ntn_initial_ul_position_authority::sdr_rx_port_verified ||
        verified_authority == mac_ntn_initial_ul_position_authority::ofh_beam_id_verified) {
      record.mapping_version = mapping.version;
      record.mapping_hash    = mapping.hash;
    }
    if (verified_authority == mac_ntn_initial_ul_position_authority::ofh_beam_id_verified) {
      record.prach_eaxc = verified_context->ofh_prach_eaxc;
      record.beam_id    = verified_context->ofh_beam_id;
    }

    store(std::move(record), now);
  }

  std::optional<mac_ntn_initial_ul_position_record>
  take_for_ul_ccch(du_cell_index_t cell_index,
                   rnti_t          c_rnti,
                   std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
  {
    std::lock_guard<std::mutex> lock(mutex);
    prune(now);
    auto first = pending.end();
    size_t count = 0;
    for (auto it = pending.begin(); it != pending.end(); ++it) {
      if (it->record.cell_index == cell_index && it->record.c_rnti == c_rnti) {
        if (first == pending.end()) {
          first = it;
        }
        ++count;
      }
    }
    if (count == 0) {
      return std::nullopt;
    }
    if (count != 1) {
      mac_ntn_initial_ul_position_record ambiguous = first->record;
      ambiguous.authority = mac_ntn_initial_ul_position_authority::none;
      ambiguous.reason    = "ambiguous_receive_position";
      pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const pending_record& item) {
                      return item.record.cell_index == cell_index && item.record.c_rnti == c_rnti;
                    }),
                    pending.end());
      return ambiguous;
    }
    mac_ntn_initial_ul_position_record result = std::move(first->record);
    pending.erase(first);
    return result;
  }

  void invalidate_cell(du_cell_index_t cell_index)
  {
    std::lock_guard<std::mutex> lock(mutex);
    pending.erase(std::remove_if(pending.begin(), pending.end(), [cell_index](const pending_record& item) {
                    return item.record.cell_index == cell_index;
                  }),
                  pending.end());
  }

private:
  static constexpr size_t max_pending_records = 1024U;
  static constexpr std::chrono::milliseconds record_ttl{1000};

  struct calendar_record {
    mac_ntn_access_calendar_update request;
    slot_point                     activation_slot;
  };

  struct pending_record {
    mac_ntn_initial_ul_position_record record;
    std::chrono::steady_clock::time_point created_at;
  };

  struct calendar_cell_match {
    const mac_ntn_access_calendar_cell* cell = nullptr;
    uint64_t calendar_cycle_index = 0;
    uint32_t cycle_slot_offset = 0;
    uint32_t occasion_offset_us = 0;
  };

  static bool validate_mapping(const mac_ntn_rx_mapping_config& config)
  {
    if (!config.enabled) {
      return false;
    }
    if (config.version == 0 || config.hash.empty() || config.hash.size() > 128 || !std::isfinite(config.unique_margin_db) ||
        config.unique_margin_db < 0.0F || config.unique_margin_db > 60.0F || config.entries.empty() ||
        config.entries.size() > max_mapping_entries) {
      return false;
    }
    std::set<std::pair<nr_cell_identity, uint16_t>> logical_keys;
    std::set<std::pair<nr_cell_identity, uint16_t>> physical_keys;
    std::set<std::pair<nr_cell_identity, uint16_t>> eaxc_keys;
    std::set<std::pair<nr_cell_identity, uint16_t>> beam_keys;
    std::map<nr_cell_identity, unsigned>             entries_per_cell;
    for (const mac_ntn_rx_port_mapping& entry : config.entries) {
      if (entry.backend == mac_ntn_rx_backend::invalid ||
          entry.cell_local_port == std::numeric_limits<uint16_t>::max() ||
          entry.physical_rx_port > max_physical_rx_port ||
          ++entries_per_cell[entry.nci] > max_mapping_entries_per_cell ||
          !logical_keys.emplace(entry.nci, entry.cell_local_port).second ||
          !physical_keys.emplace(entry.nci, entry.physical_rx_port).second) {
        return false;
      }
      if (entry.backend == mac_ntn_rx_backend::ofh) {
        if (!entry.prach_eaxc.has_value() || !entry.beam_id.has_value() ||
            entry.prach_eaxc.value() >= ofh::MAX_SUPPORTED_EAXC_ID_VALUE ||
            entry.beam_id.value() > 0x7fffU || !eaxc_keys.emplace(entry.nci, entry.prach_eaxc.value()).second ||
            !beam_keys.emplace(entry.nci, entry.beam_id.value()).second) {
          return false;
        }
      } else if (entry.prach_eaxc.has_value() || entry.beam_id.has_value()) {
        return false;
      }
    }
    return true;
  }

  calendar_cell_match find_calendar_cell(const calendar_record& calendar,
                                         du_cell_index_t        cell_index,
                                         bool                   calendar_position_valid,
                                         uint64_t               calendar_cycle_index,
                                         uint32_t               occasion_offset_us) const
  {
    calendar_cell_match result;
    if (!calendar_position_valid || !calendar.activation_slot.valid()) {
      return result;
    }
    const int64_t cycle_duration_us = calendar.request.cycle_duration.count();
    if (cycle_duration_us <= 0 || occasion_offset_us >= static_cast<uint64_t>(cycle_duration_us)) {
      return result;
    }
    const uint32_t slots_per_ms = 1U << calendar.activation_slot.numerology();
    if (slots_per_ms == 0) {
      return result;
    }
    result.calendar_cycle_index = calendar_cycle_index;
    result.cycle_slot_offset =
        static_cast<uint32_t>((static_cast<uint64_t>(occasion_offset_us) * slots_per_ms) / 1000U);
    result.occasion_offset_us = occasion_offset_us;
    const auto cell = std::find_if(calendar.request.cells.begin(), calendar.request.cells.end(), [cell_index](const auto& item) {
      return item.cell_index == cell_index;
    });
    if (cell != calendar.request.cells.end()) {
      result.cell = &*cell;
    }
    return result;
  }

  const calendar_record* find_calendar_by_version(uint64_t schedule_version) const
  {
    if (schedule_version == 0) {
      return nullptr;
    }
    if (active.has_value() && active->request.schedule_version == schedule_version) {
      return &*active;
    }
    if (staged.has_value() && staged->request.schedule_version == schedule_version) {
      return &*staged;
    }
    if (previous_active.has_value() && previous_active->request.schedule_version == schedule_version) {
      return &*previous_active;
    }
    return nullptr;
  }

  static std::optional<const mac_ntn_access_calendar_intent*>
  unique_candidate(const std::vector<const mac_ntn_access_calendar_intent*>& candidates)
  {
    const mac_ntn_access_calendar_intent* first = candidates.front();
    for (const auto* candidate : candidates) {
      if (candidate->position_id != first->position_id || candidate->port_id != first->port_id) {
        return std::nullopt;
      }
    }
    return first;
  }

  const mac_ntn_rx_port_mapping*
  find_mapping(nr_cell_identity nci, uint16_t physical_port) const
  {
    if (!mapping_valid) {
      return nullptr;
    }
    const auto found = std::find_if(mapping.entries.begin(), mapping.entries.end(), [&](const auto& entry) {
      return entry.nci == nci && entry.physical_rx_port == physical_port;
    });
    return found == mapping.entries.end() ? nullptr : &*found;
  }

  bool matches_ofh_context(const mac_ntn_rx_port_mapping&            rx_mapping,
                           const mac_ntn_access_calendar_intent&      intent,
                           const verified_prach_rx_context&           context,
                           const calendar_record&                     calendar) const
  {
    return context.authority == prach_rx_context_authority::ofh_beam_id_verified &&
           is_valid_verified_prach_rx_context(context) && context.logical_port_id == intent.port_id &&
           rx_mapping.prach_eaxc == context.ofh_prach_eaxc && rx_mapping.beam_id == context.ofh_beam_id &&
           context.position_id == intent.position_id && context.schedule_version == calendar.request.schedule_version &&
           context.calendar_hash == calendar.request.calendar_hash && context.mapping_generation == mapping.version &&
           context.mapping_hash == mapping.hash;
  }

  uint64_t allocate_observation_id()
  {
    if (next_observation_id == 0) {
      return 0;
    }
    const uint64_t result = next_observation_id;
    next_observation_id = next_observation_id == std::numeric_limits<uint64_t>::max() ? 0 : next_observation_id + 1;
    return result;
  }

  void store(mac_ntn_initial_ul_position_record record, std::chrono::steady_clock::time_point now)
  {
    if (record.observation_id == 0 || pending.size() >= max_pending_records) {
      return;
    }
    pending.push_back({std::move(record), now});
  }

  void prune(std::chrono::steady_clock::time_point now)
  {
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const pending_record& item) {
                    return now - item.created_at >= record_ttl;
                  }),
                  pending.end());
  }

  void retain_pending_for_active_calendar()
  {
    if (!active.has_value()) {
      pending.clear();
      return;
    }
    pending.erase(std::remove_if(pending.begin(),
                                 pending.end(),
                                 [&](const pending_record& item) {
                                   return item.record.schedule_version != active->request.schedule_version ||
                                          item.record.calendar_hash != active->request.calendar_hash;
                                 }),
                  pending.end());
  }

  mutable std::mutex mutex;
  static constexpr size_t   max_mapping_entries          = 1024U;
  static constexpr unsigned max_mapping_entries_per_cell = 16U;
  static constexpr uint16_t max_physical_rx_port         = 254U;
  mac_ntn_rx_mapping_config mapping;
  bool                      mapping_valid = false;
  std::optional<calendar_record> staged;
  std::optional<calendar_record> active;
  std::optional<calendar_record> previous_active;
  std::deque<pending_record>     pending;
  uint64_t                       next_observation_id = 1;
};

} // namespace srsran
