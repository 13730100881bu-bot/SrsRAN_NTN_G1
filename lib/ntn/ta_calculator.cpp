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

#include "srsran/ntn/ta_calculator.h"
#include "srsran/support/srsran_assert.h"
#include <cmath>

using namespace srsran;
using namespace srsran::srs_ntn;
using namespace srsran::srs_ntn::ntn_constants;

// ---------------------------------------------------------------------------
// ASN-1 encoding bounds (reproduced from asn1_ntn_config_helpers.cpp scale factors)
//   ta_common_r17              uint32  0..66 485 757  step 0.004072 μs → max 270 725 μs
//   ta_common_drift_r17        int32   -257 303..257 303  step 0.0002 μs/s → ±51.46 μs/s
//   ta_common_drift_variant_r17 uint16 0..28 949  step 0.00002 μs/s² → max 0.5790 μs/s²
// ---------------------------------------------------------------------------
static constexpr double MAX_TA_COMMON_US        = 66485757.0 * 0.004072; // ≈ 270 725 μs
static constexpr double MAX_TA_DRIFT_ABS_US_S   = 257303.0  * 0.0002;   // ≈ 51.46 μs/s
static constexpr double MAX_TA_DRIFT_VAR_US_S2  = 28949.0   * 0.00002;  // ≈ 0.5790 μs/s²

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// ECEF Cartesian point (position only, no velocity).
struct ecef_point {
  double x, y, z;
};

/// Convert WGS-84 geodetic to ECEF (internal, no velocity).
static ecef_point geo_to_ecef_point(const geodetic_coordinates_t& geo)
{
  const double lat_rad = geo.latitude * (M_PI / 180.0);
  const double lon_rad = geo.longitude * (M_PI / 180.0);
  const double sin_lat = std::sin(lat_rad);
  const double cos_lat = std::cos(lat_rad);

  // Prime vertical radius of curvature.
  const double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sin_lat * sin_lat);
  const double h = geo.altitude; // metres above ellipsoid

  return {(N + h) * cos_lat * std::cos(lon_rad),
          (N + h) * cos_lat * std::sin(lon_rad),
          (N * (1.0 - WGS84_E2) + h) * sin_lat};
}

/// Compute the slant range (metres) between two ECEF points.
static double ecef_range(const ecef_point& a, const ecef_point& b)
{
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  const double dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ecef_coordinates_t srsran::srs_ntn::geodetic_to_ecef(const geodetic_coordinates_t& geo)
{
  const auto p = geo_to_ecef_point(geo);
  ecef_coordinates_t result{};
  result.position_x  = p.x;
  result.position_y  = p.y;
  result.position_z  = p.z;
  result.velocity_vx = 0.0;
  result.velocity_vy = 0.0;
  result.velocity_vz = 0.0;
  return result;
}

ta_calc_result srsran::srs_ntn::compute_beam_ta(const geodetic_coordinates_t&            beam_center,
                                                const ecef_coordinates_t&                sat,
                                                const std::optional<geodetic_coordinates_t>& gateway_location)
{
  // ----- Service-link geometry -----
  // Beam centre is on the ground (altitude = 0).
  const ecef_point beam = geo_to_ecef_point({beam_center.latitude, beam_center.longitude, 0.0});

  const double dx = sat.position_x - beam.x;
  const double dy = sat.position_y - beam.y;
  const double dz = sat.position_z - beam.z;
  const double R  = std::sqrt(dx * dx + dy * dy + dz * dz);
  srsran_assert(R > 0.0, "Satellite and beam centre are at the same ECEF point");

  // Radial velocity: projection of satellite velocity onto the range unit vector.
  // Positive value means the satellite is receding (range increasing → TA increasing).
  const double V_r = (dx * sat.velocity_vx + dy * sat.velocity_vy + dz * sat.velocity_vz) / R;

  // Speed squared for drift-variant calculation.
  const double v2 = sat.velocity_vx * sat.velocity_vx +
                    sat.velocity_vy * sat.velocity_vy +
                    sat.velocity_vz * sat.velocity_vz;

  // ----- TA fields (units: μs, μs/s, μs/s²) -----

  // Round-trip service-link propagation delay.
  const double ta_common = 2.0 * R / SPEED_OF_LIGHT_M_PER_US;

  // Rate of change: d(ta_common)/dt = 2 × V_r / c.
  const double ta_drift = 2.0 * V_r / SPEED_OF_LIGHT_M_PER_US;

  // Second derivative: centripetal term dominates for LEO.
  // d²(R)/dt² ≈ (|v|² - V_r²) / R   (tangential acceleration / range)
  const double ta_drift_variant = 2.0 * (v2 - V_r * V_r) / (R * SPEED_OF_LIGHT_M_PER_US);

  // ----- Feeder-link offset (constant for a given satellite position) -----
  double ta_offset = 0.0;
  if (gateway_location.has_value()) {
    // One-way feeder-link delay × 2 (gateway → satellite → gateway round trip).
    // This models the fixed end-to-end delay seen by the UE relative to the gNB at the gateway.
    const ecef_point gw = geo_to_ecef_point(gateway_location.value());
    const ecef_point sp = {sat.position_x, sat.position_y, sat.position_z};
    ta_offset = 2.0 * ecef_range(gw, sp) / SPEED_OF_LIGHT_M_PER_US;
  }

  // ----- Range checks against ASN-1 encoding bounds -----
  srsran_assert(ta_common >= 0.0 && ta_common <= MAX_TA_COMMON_US,
                "ta_common={:.2f} μs out of ASN-1 range [0, {:.0f}] μs",
                ta_common, MAX_TA_COMMON_US);
  srsran_assert(std::abs(ta_drift) <= MAX_TA_DRIFT_ABS_US_S,
                "ta_common_drift={:.4f} μs/s out of ASN-1 range ±{:.2f} μs/s",
                ta_drift, MAX_TA_DRIFT_ABS_US_S);
  srsran_assert(ta_drift_variant >= 0.0 && ta_drift_variant <= MAX_TA_DRIFT_VAR_US_S2,
                "ta_common_drift_variant={:.6f} μs/s² out of ASN-1 range [0, {:.4f}] μs/s²",
                ta_drift_variant, MAX_TA_DRIFT_VAR_US_S2);

  ta_calc_result result;
  result.slant_range_m       = R;
  result.radial_vel_ms       = V_r;
  result.ta.ta_common        = ta_common;
  result.ta.ta_common_drift  = ta_drift;
  result.ta.ta_common_drift_variant = ta_drift_variant;
  result.ta.ta_common_offset = ta_offset;

  return result;
}
