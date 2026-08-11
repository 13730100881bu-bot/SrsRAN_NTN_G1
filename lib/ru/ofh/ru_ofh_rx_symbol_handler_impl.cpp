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

#include "ru_ofh_rx_symbol_handler_impl.h"
#include "srsran/phy/support/shared_resource_grid.h"
#include <algorithm>
#include <array>
#include <optional>

using namespace srsran;

void ru_ofh_rx_symbol_handler_impl::on_new_uplink_symbol(const ofh::uplane_rx_symbol_context& context,
                                                         shared_resource_grid                 grid,
                                                         bool                                 is_valid)
{
  ru_uplink_rx_symbol_context ru_context;
  ru_context.sector    = context.sector;
  ru_context.slot      = context.slot;
  ru_context.symbol_id = context.symbol;

  notifier.on_new_uplink_symbol(ru_context, grid, is_valid);
}

void ru_ofh_rx_symbol_handler_impl::on_new_prach_window_data(const prach_buffer_context& context,
                                                             shared_prach_buffer         buffer)
{
  prach_buffer_context legacy_context = context;
  legacy_context.verified_rx_contexts.reset();
  notifier.on_new_prach_window_data(legacy_context, std::move(buffer));
}

static std::optional<verified_prach_rx_context_list>
convert_verified_ofh_context(const prach_buffer_context&                     prach_context,
                             span<const ofh::verified_prach_uplane_context>   ofh_contexts)
{
  if (ofh_contexts.empty() || ofh_contexts.size() != prach_context.ports.size()) {
    return std::nullopt;
  }

  std::array<bool, MAX_PORTS>       seen_buffer_ports{};
  verified_prach_rx_context_list converted;
  for (const auto& item : ofh_contexts) {
    if (item.buffer_port >= prach_context.ports.size() || seen_buffer_ports[item.buffer_port] ||
        item.eaxc >= MAX_VERIFIED_PRACH_EAXC_ID_VALUE || !ofh::is_valid_prach_beam_context(item.context) ||
        std::count_if(ofh_contexts.begin(), ofh_contexts.end(), [&item](const auto& candidate) {
          return candidate.eaxc == item.eaxc;
        }) != 1) {
      return std::nullopt;
    }

    seen_buffer_ports[item.buffer_port] = true;
    verified_prach_rx_context value;
    value.authority          = prach_rx_context_authority::ofh_beam_id_verified;
    value.buffer_port        = item.buffer_port;
    value.logical_port_id    = item.context.logical_port_id;
    value.ofh_prach_eaxc     = static_cast<uint16_t>(item.eaxc);
    value.ofh_beam_id        = item.context.beam_id;
    value.position_id        = item.context.position_id;
    value.schedule_version   = item.context.schedule_version;
    value.calendar_hash      = item.context.calendar_hash;
    value.mapping_generation = item.context.mapping_generation;
    value.mapping_hash       = item.context.mapping_hash;
    if (!is_valid_verified_prach_rx_context(value)) {
      return std::nullopt;
    }
    converted.push_back(std::move(value));
  }

  for (unsigned port = 0; port != prach_context.ports.size(); ++port) {
    if (!seen_buffer_ports[port]) {
      return std::nullopt;
    }
  }
  return converted;
}

void ru_ofh_rx_symbol_handler_impl::on_new_prach_window_data(
    const prach_buffer_context&                     context,
    shared_prach_buffer                             buffer,
    span<const ofh::verified_prach_uplane_context> verified_contexts)
{
  auto converted = convert_verified_ofh_context(context, verified_contexts);
  if (!converted) {
    prach_buffer_context unverified_context = context;
    unverified_context.verified_rx_contexts.reset();
    notifier.on_new_prach_window_data(unverified_context, std::move(buffer));
    return;
  }
  notifier.on_new_prach_window_data(context, std::move(buffer), *converted);
}
