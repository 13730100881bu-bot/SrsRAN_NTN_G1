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

#include "srsran/ofh/ofh_uplane_rx_symbol_notifier.h"
#include "srsran/phy/support/shared_resource_grid.h"

namespace srsran {
namespace ofh {
namespace testing {

/// User-Plane received symbol notifier spy.
class uplane_rx_symbol_notifier_spy : public uplane_rx_symbol_notifier
{
  bool                                                                    new_uplink_symbol_function_called  = false;
  bool                                                                    new_prach_function_called          = false;
  bool                                                                    new_verified_prach_function_called = false;
  static_vector<verified_prach_uplane_context, MAX_NOF_SUPPORTED_EAXC> last_verified_contexts;

public:
  // See interface for documentation.
  void on_new_uplink_symbol(const uplane_rx_symbol_context& context, shared_resource_grid grid, bool is_valid) override
  {
    new_uplink_symbol_function_called = true;
  }

  // See interface for documentation.
  void on_new_prach_window_data(const prach_buffer_context& context, shared_prach_buffer buffer) override
  {
    new_prach_function_called = true;
  }

  // See interface for documentation.
  void on_new_prach_window_data(const prach_buffer_context&                 context,
                                shared_prach_buffer                         buffer,
                                span<const verified_prach_uplane_context>   verified_contexts) override
  {
    new_prach_function_called          = true;
    new_verified_prach_function_called = true;
    last_verified_contexts.assign(verified_contexts.begin(), verified_contexts.end());
  }

  /// Returns true if on_new_uplink_symbol function has been called, otherwise false.
  bool has_new_uplink_symbol_function_been_called() const { return new_uplink_symbol_function_called; }

  /// Returns true if on_new_prach_window_data function has been called, otherwise false.
  bool has_new_prach_function_been_called() const { return new_prach_function_called; }

  /// Returns true if verified PRACH context was supplied with the PRACH buffer.
  bool has_new_verified_prach_function_been_called() const { return new_verified_prach_function_called; }

  span<const verified_prach_uplane_context> get_last_verified_contexts() const { return last_verified_contexts; }
};

} // namespace testing
} // namespace ofh
} // namespace srsran
