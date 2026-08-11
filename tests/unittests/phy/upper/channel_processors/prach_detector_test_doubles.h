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

#include "srsran/phy/upper/channel_processors/prach_detector.h"
#include <optional>

namespace srsran {

class prach_detector_spy : public prach_detector
{
  bool                         detect_method_been_called = false;
  unsigned                     detect_method_call_count  = 0;
  std::optional<configuration> last_configuration;

public:
  prach_detection_result detect(const prach_buffer& input, const configuration& config) override
  {
    detect_method_been_called = true;
    ++detect_method_call_count;
    last_configuration        = config;

    return {};
  }

  bool has_detect_method_been_called() const { return detect_method_been_called; }

  unsigned get_detect_method_call_count() const { return detect_method_call_count; }

  const std::optional<configuration>& get_last_configuration() const { return last_configuration; }
};

} // namespace srsran
