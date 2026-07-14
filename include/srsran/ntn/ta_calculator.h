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

#include "srsran/ran/ntn.h"
#include <optional>

namespace srsran {
namespace srs_ntn {

/// Physical constants used in NTN timing calculations.
namespace ntn_constants {

/// Speed of light in metres per microsecond.
constexpr double SPEED_OF_LIGHT_M_PER_US = 299.792458;

/// WGS-84 semi-major axis (metres).
constexpr double WGS84_A = 6378137.0;

/// WGS-84 flattening.
constexpr double WGS84_F = 1.0 / 298.257223563;

/// WGS-84 first eccentricity squared (= 2f - f²).
constexpr double WGS84_E2 = 2.0 * WGS84_F - WGS84_F * WGS84_F;

} // namespace ntn_constants

// ---------------------------------------------------------------------------
// Utility: geodetic → ECEF
// ---------------------------------------------------------------------------

/// Convert WGS-84 geodetic coordinates to ECEF Cartesian (metres).
///
/// \param geo   Geodetic position: latitude and longitude in degrees, altitude in metres.
/// \return      ECEF position in metres.  Velocity components are set to zero.
///
/// Conversion formulae (Bowring / standard closed-form):
///   N   = a / sqrt(1 - e² × sin²(φ))
///   X   = (N + h) × cos(φ) × cos(λ)
///   Y   = (N + h) × cos(φ) × sin(λ)
///   Z   = (N × (1 - e²) + h) × sin(φ)
ecef_coordinates_t geodetic_to_ecef(const geodetic_coordinates_t& geo);

// ---------------------------------------------------------------------------
// TA calculator
// ---------------------------------------------------------------------------

/// Detailed output of a per-beam TA computation.
struct ta_calc_result {
  ta_info_t ta;            ///< Fully populated ta_info_t, ready for SIB-19 encoding.
  double    slant_range_m; ///< Beam-centre-to-satellite distance (metres, informational).
  double    radial_vel_ms; ///< Radial velocity component (m/s, positive = receding).
};

/// Compute ta_info_t for one beam position given the satellite's current ECEF state.
///
/// Field derivations:
///
///   Δr  = r_sat - r_beam_ecef                    (range vector, metres)
///   R   = |Δr|                                   (slant range, metres)
///   V_r = (Δr · v_sat) / R                       (radial velocity, m/s)
///
///   ta_common               = 2R / c             (round-trip service-link delay, μs)
///   ta_common_drift         = 2·V_r / c          (rate of TA change, μs/s)
///   ta_common_drift_variant = 2·(|v|²-V_r²)/(R·c) (centripetal acceleration term, μs/s²)
///   ta_common_offset        = 2·R_feeder / c     (round-trip feeder-link delay, μs)
///                             or 0 if gateway_location is absent.
///
/// The drift-variant formula uses the centripetal approximation (omits gravitational
/// acceleration ≈ 8.4 m/s² at 500 km, introduces < 5 % error at the nadir worst-case).
///
/// ASN-1 range compliance is asserted via srsran_assert:
///   ta_common               ≤ 270 725 μs   (covers GEO at 35 786 km)
///   |ta_common_drift|       ≤ 51.46 μs/s   (covers LEO at 500 km, 7.6 km/s)
///   ta_common_drift_variant ≤ 0.579 μs/s²
///
/// \param beam_center       Geographic centre of the beam position (altitude treated as 0 m).
/// \param satellite         Satellite ECEF position (metres) and velocity (m/s).
/// \param gateway_location  Optional NTN-gateway WGS-84 position for feeder-link offset.
ta_calc_result compute_beam_ta(const geodetic_coordinates_t&            beam_center,
                               const ecef_coordinates_t&                satellite,
                               const std::optional<geodetic_coordinates_t>& gateway_location = std::nullopt);

} // namespace srs_ntn
} // namespace srsran
