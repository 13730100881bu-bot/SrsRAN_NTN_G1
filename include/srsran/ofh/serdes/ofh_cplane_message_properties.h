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

#include "srsran/ofh/compression/compression_params.h"
#include "srsran/ofh/serdes/ofh_message_properties.h"
#include "srsran/ran/cyclic_prefix.h"
#include "srsran/ran/slot_point.h"
#include <cstddef>
#include <optional>
#include <string>

namespace srsran {
namespace ofh {

/// Open Fronthaul Control-Plane section fields common to section types 0, 1, 3 and 5.
struct cplane_common_section_0_1_3_5_fields {
  /// Section identifier.
  uint16_t section_id;
  /// Starting PRB of data section.
  uint16_t prb_start;
  /// Number of contiguous PRBs per data section.
  uint16_t nof_prb;
  /// Resource element mask.
  uint16_t re_mask;
  /// Number of symbols.
  uint8_t nof_symbols;
};

/// Open Fronthaul Control-Plane DL/UL radio channel section fields.
struct cplane_dl_ul_radio_channel_section_fields {
  cplane_common_section_0_1_3_5_fields common_fields;
};

/// Open Fronthaul Control-Plane idle/guard period section fields.
struct cplane_idle_guard_period_section_fields {
  cplane_common_section_0_1_3_5_fields common_fields;
};

/// Open Fronthaul Control-Plane PRACH/mixed-numerology section fields.
struct cplane_prach_mixed_num_section_fields {
  cplane_common_section_0_1_3_5_fields common_fields;
  /// Optional 15-bit beam identifier. When absent, BeamId is encoded as zero.
  std::optional<uint16_t> beam_id;
  /// The fequency offset with respect to the carrier center frequency in steps of one half the subcarrier spacings,
  /// see O-RAN.WG4.CUS, 7.5.3.11.
  int frequency_offset;
};

/// Maximum value of the 15-bit Open Fronthaul BeamId field.
constexpr uint16_t MAX_CPLANE_BEAM_ID = 0x7fff;

/// Maximum length of opaque position, calendar and receive-mapping identifiers carried in local PRACH context.
constexpr size_t MAX_PRACH_CONTEXT_IDENTIFIER_LENGTH = 256;

/// Local metadata that binds one PRACH eAxC to the access calendar that requested it.
///
/// This metadata is not serialized into an Open Fronthaul packet. Only \c beam_id is placed in the type-3 message;
/// the remaining fields are retained locally and matched against the eAxC of received U-Plane packets.
struct prach_beam_context {
  /// Beam identifier sent in the type-3 Control-Plane message.
  uint16_t beam_id = 0;
  /// Cell-local logical uplink beam port.
  uint16_t logical_port_id = 0;
  /// Opaque earth-fixed position identifier.
  std::string position_id;
  /// Access-calendar schedule version.
  uint64_t schedule_version = 0;
  /// Opaque access-calendar hash.
  std::string calendar_hash;
  /// Generation of the logical-port-to-device mapping.
  uint64_t mapping_generation = 0;
  /// Opaque hash of the logical-port-to-device mapping.
  std::string mapping_hash;

  bool operator==(const prach_beam_context& rhs) const
  {
    return beam_id == rhs.beam_id && logical_port_id == rhs.logical_port_id && position_id == rhs.position_id &&
           schedule_version == rhs.schedule_version && calendar_hash == rhs.calendar_hash &&
           mapping_generation == rhs.mapping_generation && mapping_hash == rhs.mapping_hash;
  }
};

/// Returns true when the local PRACH beam context is bounded and identifies a versioned mapping.
inline bool is_valid_prach_beam_context(const prach_beam_context& context)
{
  return context.beam_id <= MAX_CPLANE_BEAM_ID && !context.position_id.empty() &&
         context.position_id.size() <= MAX_PRACH_CONTEXT_IDENTIFIER_LENGTH && context.schedule_version != 0 &&
         !context.calendar_hash.empty() && context.calendar_hash.size() <= MAX_PRACH_CONTEXT_IDENTIFIER_LENGTH &&
         context.mapping_generation != 0 && !context.mapping_hash.empty() &&
         context.mapping_hash.size() <= MAX_PRACH_CONTEXT_IDENTIFIER_LENGTH;
}

/// Local access-calendar metadata for one PRACH eAxC.
struct prach_eaxc_beam_context {
  /// U-Plane eAxC expected for this context.
  unsigned eaxc = 0;
  /// Beam and access-calendar identity associated with the eAxC.
  prach_beam_context context;
};

/// Verified local association between one received PRACH buffer port and its eAxC/calendar context.
struct verified_prach_uplane_context {
  /// U-Plane eAxC on which IQ data was received.
  unsigned eaxc = 0;
  /// Port index in the PRACH buffer.
  unsigned buffer_port = 0;
  /// Beam and access-calendar identity matched to this eAxC.
  prach_beam_context context;
};

/// Open Fronthaul Control-Plane radio application header.
struct cplane_radio_application_header {
  /// Data direction.
  data_direction direction;
  /// Filter index.
  filter_index_type filter_index;
  /// Start symbol identifier.
  uint8_t start_symbol;
  /// Slot point.
  slot_point slot;
};

/// Open Fronthaul Control-Plane section type 1 parameters.
struct cplane_section_type1_parameters {
  /// Control-Plane radio application header.
  cplane_radio_application_header radio_hdr;
  /// Control-Plane DL/UL radio channel section fields.
  cplane_dl_ul_radio_channel_section_fields section_fields;
  /// Compression parameters.
  ru_compression_params compr_params;
};

/// FFT/iFFT size being used for IQ data processing, part of frameStructure field.
enum class cplane_fft_size : uint8_t {
  fft_noop = 0,
  fft_128  = 0x07,
  fft_256  = 0x08,
  fft_512  = 0x09,
  fft_1024 = 0x0a,
  fft_2048 = 0x0b,
  fft_4096 = 0x0c,
  fft_1536 = 0x0d,
  fft_3072 = 0x0e
};

/// Converts and returns the given FFT size into an integer.
constexpr unsigned to_value(cplane_fft_size fft)
{
  return static_cast<unsigned>(fft);
}

/// Subcarrier spacing defined in defined in O-RAN.WG4.CUS, Table 7.5.2.13-3 (part of frameStructure field).
enum class cplane_scs : uint8_t {
  kHz15   = 0,
  kHz30   = 0x01,
  kHz60   = 0x02,
  kHz120  = 0x03,
  kHz1_25 = 0x0c,
  kHz5    = 0x0e,
  reserved
};

/// Converts and returns the given cplane scs into an integer.
constexpr unsigned to_value(cplane_scs scs)
{
  return static_cast<unsigned>(scs);
}

/// Open Fronthaul Control-Plane section type 0 parameters.
struct cplane_section_type0_parameters {
  /// Control-Plane radio application header.
  cplane_radio_application_header radio_hdr;
  /// Control-Plane idle/guard period section fields.
  cplane_idle_guard_period_section_fields section_fields;
  /// Subcarrier spacing.
  subcarrier_spacing scs;
  /// Cyclic prefix.
  cyclic_prefix cp;
  /// FFT/iFFT size being used for all IQ data processing related to this message.
  cplane_fft_size fft_size;
  /// Time offset from the start of the slot to the start of cyclic prefix, see O-RAN.WG4.CUS, 7.5.2.12.
  uint16_t time_offset;
};

/// Open Fronthaul Control-Plane section type 3 parameters.
struct cplane_section_type3_parameters {
  /// Control-Plane radio application header.
  cplane_radio_application_header radio_hdr;
  /// Control-Plane PRACH/mixed-numerology section fields.
  cplane_prach_mixed_num_section_fields section_fields;
  /// Compression parameters.
  ru_compression_params compr_params;
  /// Subcarrier spacing.
  cplane_scs scs;
  /// FFT/iFFT size being used for all IQ data processing related to this message.
  cplane_fft_size fft_size;
  /// Cyclic prefix length.
  unsigned cpLength;
  /// Time offset from the start of the slot to the start of cyclic prefix, see O-RAN.WG4.CUS, 7.5.2.12.
  /// For the PRACH Control-Plane message the value may refer to the start of cyclic prefix or the start of PRACH
  /// preamble depending on \c cpLength parameter value, see O-RAN.WG4.CUS, 4.4.3
  uint16_t time_offset;
};

} // namespace ofh
} // namespace srsran
