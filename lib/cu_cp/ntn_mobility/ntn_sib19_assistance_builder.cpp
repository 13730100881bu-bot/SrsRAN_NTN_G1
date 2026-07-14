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

#include "ntn_sib19_assistance_builder.h"
#include "srsran/adt/byte_buffer.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>

using namespace srsran;
using namespace srs_cu_cp;
using namespace asn1::rrc_nr;

namespace {

asn1::dyn_octstring encode_reference_location(const geodetic_coordinates_t& loc)
{
  const auto lat_int = static_cast<uint32_t>(std::lround(std::abs(loc.latitude) * (1u << 23) / 90.0));
  const auto lon_int = static_cast<int32_t>(std::lround(loc.longitude * (1u << 24) / 360.0));

  const std::array<uint8_t, 11> bytes = {{
      0x03u,
      static_cast<uint8_t>((loc.latitude < 0.0 ? 0x80u : 0x00u) | ((lat_int >> 16u) & 0x7fu)),
      static_cast<uint8_t>((lat_int >> 8u) & 0xffu),
      static_cast<uint8_t>(lat_int & 0xffu),
      static_cast<uint8_t>((static_cast<uint32_t>(lon_int) >> 16u) & 0xffu),
      static_cast<uint8_t>((static_cast<uint32_t>(lon_int) >> 8u) & 0xffu),
      static_cast<uint8_t>(static_cast<uint32_t>(lon_int) & 0xffu),
      0x00u,
      0x00u,
      0x00u,
      0x00u,
  }};

  auto buffer = byte_buffer::create(bytes.begin(), bytes.end());
  srsran_assert(buffer.has_value(), "Failed to allocate SIB19 reference location buffer");
  return asn1::dyn_octstring{std::move(buffer.value())};
}

expected<int64_t, std::string> scale_to_integer(double value, double scale, int64_t min_value, int64_t max_value, const char* name)
{
  if (!std::isfinite(value)) {
    return make_unexpected(fmt::format("{} is not finite", name));
  }
  const double scaled = value / scale;
  if (scaled < static_cast<double>(min_value) || scaled > static_cast<double>(max_value)) {
    return make_unexpected(fmt::format("{} is outside ASN.1 range", name));
  }
  return static_cast<int64_t>(scaled);
}

expected<ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_e_, std::string> make_ul_sync_validity_enum(unsigned seconds)
{
  ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_e_ value;
  switch (seconds) {
    case 5:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s5;
      break;
    case 10:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s10;
      break;
    case 15:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s15;
      break;
    case 20:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s20;
      break;
    case 25:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s25;
      break;
    case 30:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s30;
      break;
    case 35:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s35;
      break;
    case 40:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s40;
      break;
    case 45:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s45;
      break;
    case 50:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s50;
      break;
    case 55:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s55;
      break;
    case 60:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s60;
      break;
    case 120:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s120;
      break;
    case 180:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s180;
      break;
    case 240:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s240;
      break;
    case 900:
      value.value = ntn_cfg_r17_s::ntn_ul_sync_validity_dur_r17_opts::s900;
      break;
    default:
      return make_unexpected(fmt::format("ntn-UlSyncValidityDuration-r17={} is unsupported", seconds));
  }
  return value;
}

expected<void, std::string> fill_position_velocity(ntn_cfg_r17_s& cfg, const ecef_coordinates_t& satellite)
{
  auto x = scale_to_integer(satellite.position_x, 1.3, -33554432, 33554431, "positionX-r17");
  if (!x.has_value()) {
    return make_unexpected(x.error());
  }
  auto y = scale_to_integer(satellite.position_y, 1.3, -33554432, 33554431, "positionY-r17");
  if (!y.has_value()) {
    return make_unexpected(y.error());
  }
  auto z = scale_to_integer(satellite.position_z, 1.3, -33554432, 33554431, "positionZ-r17");
  if (!z.has_value()) {
    return make_unexpected(z.error());
  }
  auto vx = scale_to_integer(satellite.velocity_vx, 0.06, -131072, 131071, "velocityVX-r17");
  if (!vx.has_value()) {
    return make_unexpected(vx.error());
  }
  auto vy = scale_to_integer(satellite.velocity_vy, 0.06, -131072, 131071, "velocityVY-r17");
  if (!vy.has_value()) {
    return make_unexpected(vy.error());
  }
  auto vz = scale_to_integer(satellite.velocity_vz, 0.06, -131072, 131071, "velocityVZ-r17");
  if (!vz.has_value()) {
    return make_unexpected(vz.error());
  }

  cfg.ephemeris_info_r17_present = true;
  position_velocity_r17_s& pv     = cfg.ephemeris_info_r17.set_position_velocity_r17();
  pv.position_x_r17               = static_cast<int32_t>(x.value());
  pv.position_y_r17               = static_cast<int32_t>(y.value());
  pv.position_z_r17               = static_cast<int32_t>(z.value());
  pv.velocity_vx_r17              = static_cast<int32_t>(vx.value());
  pv.velocity_vy_r17              = static_cast<int32_t>(vy.value());
  pv.velocity_vz_r17              = static_cast<int32_t>(vz.value());
  return {};
}

expected<void, std::string> fill_ta_info(ntn_cfg_r17_s& cfg, const ta_info_t& src)
{
  const double common_ta = src.ta_common + src.ta_common_offset;
  auto ta_common = scale_to_integer(common_ta, 0.004072, 0, 66485757, "ta-Common-r17");
  if (!ta_common.has_value()) {
    return make_unexpected(ta_common.error());
  }
  auto drift = scale_to_integer(src.ta_common_drift, 0.0002, -257303, 257303, "ta-CommonDrift-r17");
  if (!drift.has_value()) {
    return make_unexpected(drift.error());
  }
  auto drift_variant =
      scale_to_integer(src.ta_common_drift_variant, 0.00002, 0, 28949, "ta-CommonDriftVariant-r17");
  if (!drift_variant.has_value()) {
    return make_unexpected(drift_variant.error());
  }

  cfg.ta_info_r17_present       = true;
  ta_info_r17_s& dst            = cfg.ta_info_r17;
  dst.ta_common_drift_r17_present         = true;
  dst.ta_common_drift_variant_r17_present = true;
  dst.ta_common_r17                       = static_cast<uint32_t>(ta_common.value());
  dst.ta_common_drift_r17                 = static_cast<int32_t>(drift.value());
  dst.ta_common_drift_variant_r17         = static_cast<uint16_t>(drift_variant.value());
  return {};
}

} // namespace

ntn_sib19_assistance_snapshot
srsran::srs_cu_cp::build_ntn_sib19_assistance_snapshot(const ntn_sib19_assistance_request& request)
{
  ntn_sib19_assistance_snapshot snapshot;
  snapshot.valid           = request.assistance.valid;
  snapshot.invalid_reason  = request.assistance.invalid_reason;
  snapshot.satellite_epoch = request.assistance.satellite_epoch;

  if (!request.assistance.valid || !request.assistance.satellite_ecef.has_value()) {
    snapshot.valid = false;
    if (snapshot.invalid_reason == ntn_assistance_invalid_reason::none) {
      snapshot.invalid_reason = ntn_assistance_invalid_reason::no_satellite_state;
    }
    return snapshot;
  }

  const unsigned max_entries =
      request.max_entries == 0 ? std::numeric_limits<unsigned>::max() : request.max_entries;
  snapshot.entries.reserve(std::min<unsigned>(request.assistance.beams.size(), max_entries));
  std::map<std::string, ecef_coordinates_t> satellite_ecef_by_id;
  for (const ntn_satellite_state& satellite : request.assistance.satellite_states) {
    satellite_ecef_by_id[satellite.satellite_id] = satellite.ecef;
  }

  for (const ntn_assistance_beam_snapshot& beam : request.assistance.beams) {
    if (snapshot.entries.size() >= max_entries) {
      break;
    }
    ntn_sib19_assistance_entry entry;
    entry.beam_id            = beam.beam_id;
    entry.nci                = beam.nci;
    entry.state              = beam.state;
    entry.reference_location = beam.reference_location;
    entry.serving_satellite_id = beam.serving_satellite_id;
    const auto satellite_it = satellite_ecef_by_id.find(beam.serving_satellite_id);
    entry.satellite_ecef     =
        satellite_it != satellite_ecef_by_id.end() ? std::optional<ecef_coordinates_t>{satellite_it->second}
                                                   : request.assistance.satellite_ecef;
    entry.satellite_epoch    = request.assistance.satellite_epoch;
    entry.epoch_time         = request.epoch_time;
    entry.ta_info            = beam.ta_info;
    if (beam.cell_specific_koffset > 0) {
      entry.cell_specific_koffset = beam.cell_specific_koffset;
    }
    entry.k_mac              = beam.k_mac;
    entry.ul_sync_validity_s = beam.ul_sync_validity_s;
    entry.t_service          = beam.t_service;
    snapshot.entries.push_back(std::move(entry));
  }

  return snapshot;
}

expected<asn1::rrc_nr::sib19_r17_s, std::string>
srsran::srs_cu_cp::make_asn1_rrc_sib19_from_ntn_assistance(const ntn_sib19_assistance_entry& entry)
{
  if (!entry.valid) {
    return make_unexpected("SIB19 assistance entry is invalid");
  }
  if (!entry.satellite_ecef.has_value()) {
    return make_unexpected("SIB19 assistance entry has no satellite ephemeris");
  }

  sib19_r17_s sib19;
  sib19.ref_location_r17 = encode_reference_location(entry.reference_location);

  if (entry.t_service.has_value()) {
    if (entry.t_service.value() > 549755813887ULL) {
      return make_unexpected("t-Service-r17 is outside ASN.1 range");
    }
    sib19.t_service_r17_present = true;
    sib19.t_service_r17         = entry.t_service.value();
  }

  sib19.ntn_cfg_r17_present = true;
  ntn_cfg_r17_s& cfg        = sib19.ntn_cfg_r17;

  if (entry.epoch_time.has_value()) {
    if (entry.epoch_time->sfn > 1023 || entry.epoch_time->subframe_number > 9) {
      return make_unexpected("epochTime-r17 is outside ASN.1 range");
    }
    cfg.epoch_time_r17_present          = true;
    cfg.epoch_time_r17.sfn_r17          = static_cast<uint16_t>(entry.epoch_time->sfn);
    cfg.epoch_time_r17.sub_frame_nr_r17 = static_cast<uint8_t>(entry.epoch_time->subframe_number);
  }

  if (entry.cell_specific_koffset.has_value()) {
    if (entry.cell_specific_koffset.value() == 0 || entry.cell_specific_koffset.value() > 1023) {
      return make_unexpected("cellSpecificKoffset-r17 is outside ASN.1 range");
    }
    cfg.cell_specific_koffset_r17_present = true;
    cfg.cell_specific_koffset_r17         = static_cast<uint16_t>(entry.cell_specific_koffset.value());
  }

  if (entry.k_mac.has_value()) {
    if (entry.k_mac.value() == 0 || entry.k_mac.value() > 512) {
      return make_unexpected("kmac-r17 is outside ASN.1 range");
    }
    cfg.kmac_r17_present = true;
    cfg.kmac_r17         = static_cast<uint16_t>(entry.k_mac.value());
  }

  if (entry.ul_sync_validity_s.has_value()) {
    auto ul_sync = make_ul_sync_validity_enum(entry.ul_sync_validity_s.value());
    if (!ul_sync.has_value()) {
      return make_unexpected(ul_sync.error());
    }
    cfg.ntn_ul_sync_validity_dur_r17_present = true;
    cfg.ntn_ul_sync_validity_dur_r17         = ul_sync.value();
  }

  if (entry.ta_info.has_value()) {
    auto ta_result = fill_ta_info(cfg, entry.ta_info.value());
    if (!ta_result.has_value()) {
      return make_unexpected(ta_result.error());
    }
  }

  auto ephemeris_result = fill_position_velocity(cfg, entry.satellite_ecef.value());
  if (!ephemeris_result.has_value()) {
    return make_unexpected(ephemeris_result.error());
  }

  return sib19;
}
