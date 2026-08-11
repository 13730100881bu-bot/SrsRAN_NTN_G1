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

#include "srsran/adt/static_vector.h"
#include "srsran/ran/phy_time_unit.h"
#include "srsran/ran/prach/prach_constants.h"
#include <cstdint>
#include <limits>

namespace srsran {

/// Describes a PRACH detection result.
struct prach_detection_result {
  /// Describes whether a detected preamble can be attributed to one receive port.
  enum class rx_port_attribution_status : uint8_t {
    /// Port attribution was not requested or the per-port measurements are not usable.
    unavailable,
    /// The strongest receive port exceeds the second strongest port by the configured margin.
    unique,
    /// More than one receive port has a plausible measurement for the detected preamble.
    ambiguous
  };

  /// Describes the optional receive-port attribution of a detected preamble.
  struct rx_port_attribution {
    /// Invalid receive-port index used when the attribution is unavailable.
    static constexpr unsigned invalid_port_index = std::numeric_limits<unsigned>::max();

    /// Attribution classification.
    rx_port_attribution_status status = rx_port_attribution_status::unavailable;
    /// Zero-based index of the strongest port in the detector input.
    unsigned strongest_port_index = invalid_port_index;
    /// Power difference between the strongest and second strongest ports, in dB.
    float strongest_to_second_margin_dB = 0.0F;
  };

  /// Describes the detection of a single preamble.
  struct preamble_indication {
    /// Index of the detected preamble. Possible values are {0, ..., 63}.
    unsigned preamble_index;
    /// Timing advance between the observed arrival time (for the considered UE) and the reference uplink time.
    phy_time_unit time_advance;
    /// Detection metric normalized with respect to the detection threshold.
    float detection_metric;
    /// Preamble received power in normalized dB units.
    float preamble_power_dB;
    /// Optional attribution to the strongest receive port. Unavailable by default.
    rx_port_attribution port_attribution;
  };

  /// Average RSSI value in normalized dB units.
  float rssi_dB;
  /// \brief Detector time resolution.
  ///
  /// This is equal to the PRACH subcarrier spacing divided by the DFT size of the detector.
  phy_time_unit time_resolution;
  /// \brief Detector maximum time in advance.
  ///
  /// This is equal to the minimum value among \f$N_{CP}^{RA}\f$ and \f$N_{CS}\f$ if \f$N_{CS}\f$ is not zero.
  /// Otherwise, it is equal to \f$N_{CP}^{RA}\f$.
  phy_time_unit time_advance_max;
  /// List of detected preambles.
  static_vector<preamble_indication, prach_constants::MAX_NUM_PREAMBLES> preambles;
};

} // namespace srsran
