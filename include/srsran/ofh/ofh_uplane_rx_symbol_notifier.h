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

#include "srsran/adt/span.h"
#include "srsran/ofh/serdes/ofh_cplane_message_properties.h"
#include "srsran/phy/support/shared_prach_buffer.h"
#include "srsran/ran/slot_point.h"
#include <utility>

namespace srsran {
struct prach_buffer_context;
class shared_resource_grid;

namespace ofh {

/// User-Plane reception symbol context.
struct uplane_rx_symbol_context {
  /// Slot point.
  slot_point slot;
  /// Symbol identifier.
  unsigned symbol;
  /// Sector.
  unsigned sector;
};

/// Open Fronthaul User-Plane symbol reception notifier.
class uplane_rx_symbol_notifier
{
public:
  virtual ~uplane_rx_symbol_notifier() = default;

  /// \brief Notifies the completion of an OFDM symbol for a given context.
  ///
  /// \param[in] context Notification context.
  /// \param[in] grid    Resource grid that belongs to the context.
  virtual void
  on_new_uplink_symbol(const uplane_rx_symbol_context& context, shared_resource_grid grid, bool is_valid) = 0;

  /// \brief Notifies the completion of a PRACH window.
  /// \param[in] context PRACH context.
  /// \param[in] buffer  PRACH buffer.
  virtual void on_new_prach_window_data(const prach_buffer_context& context, shared_prach_buffer buffer) = 0;

  /// \brief Notifies a completed PRACH window with verified per-buffer-port eAxC and beam/calendar context.
  ///
  /// The default implementation preserves compatibility by forwarding only the PRACH buffer to the legacy callback.
  /// Consumers that require receive attribution override this overload and must treat the supplied span as valid only
  /// for the duration of the call.
  virtual void on_new_prach_window_data(
      const prach_buffer_context&                 context,
      shared_prach_buffer                         buffer,
      span<const verified_prach_uplane_context>)
  {
    on_new_prach_window_data(context, std::move(buffer));
  }
};

} // namespace ofh
} // namespace srsran
