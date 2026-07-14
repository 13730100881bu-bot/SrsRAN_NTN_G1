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

#include "sib_pdu_assembler.h"
#include "srsran/srslog/srslog.h"
#include "srsran/support/units.h"
#include <algorithm>
#include <deque>

using namespace srsran;

// Max SI Message PDU size. This value is implementation-defined.
static constexpr unsigned MAX_BCCH_DL_SCH_PDU_SIZE = 2048;

// Payload of zeros sent to when an error occurs.
static const std::vector<uint8_t> zeros_payload(MAX_BCCH_DL_SCH_PDU_SIZE, 0);

sib_pdu_assembler::sib_pdu_assembler(const mac_cell_sys_info_config& req) : logger(srslog::fetch_basic_logger("MAC"))
{
  // Version starts at 0.
  last_cfg_buffers.version  = 0;
  last_cfg_buffers.sib1_len = units::bytes{0};
  save_buffers(0, req);
  current_buffers = last_cfg_buffers;

  message_ext_handler = create_si_message_extension_handler(req);
}

void sib_pdu_assembler::handle_si_change_request(const mac_cell_sys_info_config& req)
{
  // Save new buffers that have changed.
  srsran_assert(last_cfg_buffers.version != req.si_sched_cfg.version,
                "Version of the last SI message update is the same as the new one");
  save_buffers(req.si_sched_cfg.version, req);

  // Forward new version and buffers to SIB assembler RT path.
  pending.write_and_commit(last_cfg_buffers);
}

bool sib_pdu_assembler::enqueue_si_message_pdu_updates(const mac_cell_sys_info_pdu_update& req)
{
  if (message_ext_handler) {
    return message_ext_handler->enqueue_si_pdu_updates(req);
  }
  return false;
}

static std::shared_ptr<const std::vector<uint8_t>> make_linear_buffer(const byte_buffer& pdu)
{
  // Note: We overallocate the SI message buffer to account for padding.
  // Note: After this point, resizing the vector is not possible as it would invalidate the spans passed to lower
  // layers.
  auto vec = std::make_shared<std::vector<uint8_t>>(MAX_BCCH_DL_SCH_PDU_SIZE, 0);
  copy_segments(pdu, span<uint8_t>(vec->data(), vec->size()));
  return vec;
}

void sib_pdu_assembler::save_buffers(si_version_type si_version, const mac_cell_sys_info_config& req)
{
  // Note: In case the SIB1/SI message does not change, the comparison between the respective byte_buffers should be
  // fast (as they will point to the same memory location). Avoid at all costs the operator== for the stored vectors
  // as they are overdimensioned to account for padding.

  // Check if SIB1 has changed.
  if (last_si_cfg.sib1 != req.sib1) {
    last_si_cfg.sib1             = req.sib1.copy();
    last_cfg_buffers.sib1_len    = units::bytes{static_cast<unsigned>(req.sib1.length())};
    last_cfg_buffers.sib1_buffer = make_linear_buffer(req.sib1);
  }

  // Check if SI messages have changed.
  last_cfg_buffers.si_msg_buffers.resize(req.si_messages.size());
  for (unsigned i = 0, e = req.si_messages.size(); i != e; ++i) {
    if (last_si_cfg.si_messages.size() <= i) {
      last_si_cfg.si_messages.resize(i + 1);
    }
    if (req.si_messages[i] != last_si_cfg.si_messages[i]) {
      // Resize according to the number of message segments in the new SI message.
      last_si_cfg.si_messages[i].resize(req.si_messages[i].size());
      // Check if the request contains a segmented SI message.
      if (req.si_messages[i].size() > 1) {
        const auto& new_si_cfg = req.si_messages[i];

        // Copy the last configuration request (perform a shallow copy of each segment).
        auto& last_si_cfg_buf = last_si_cfg.si_messages[i];
        last_si_cfg_buf.resize(new_si_cfg.size());
        for (unsigned i_segment = 0, nof_segments = new_si_cfg.size(); i_segment != nof_segments; ++i_segment) {
          last_si_cfg_buf[i_segment] = new_si_cfg[i_segment].copy();
        }

        // Set the SI message size.
        size_t si_msg_len = new_si_cfg.front().length();
        srsran_assert(std::all_of(last_si_cfg_buf.begin(),
                                  last_si_cfg_buf.end(),
                                  [si_msg_len](const byte_buffer& si_msg) { return si_msg.length() == si_msg_len; }),
                      "All segments of an SI message must have the same length.");
        last_cfg_buffers.si_msg_buffers[i].first = units::bytes{static_cast<unsigned>(si_msg_len)};

        // Copy the contents of the request into a linear buffer (deep copy each segment).
        auto& si_messsage_buf =
            last_cfg_buffers.si_msg_buffers[i].second.emplace<segmented_sib_list<bcch_dl_sch_buffer>>();
        for (unsigned i_segment = 0, nof_segments = new_si_cfg.size(); i_segment != nof_segments; ++i_segment) {
          si_messsage_buf.append_segment(make_linear_buffer(new_si_cfg[i_segment]));
        }

      } else {
        // Do the same for a configuration request carrying a single SI message segment.
        auto&       last_si_cfg_buf               = last_si_cfg.si_messages[i].front();
        const auto& new_si_cfg                    = req.si_messages[i].front();
        last_si_cfg_buf                           = new_si_cfg.copy();
        last_cfg_buffers.si_msg_buffers[i].first  = units::bytes{static_cast<unsigned>(new_si_cfg.length())};
        last_cfg_buffers.si_msg_buffers[i].second = make_linear_buffer(new_si_cfg);
      }
    }
  }

  last_cfg_buffers.version = si_version;
}

span<const uint8_t> sib_pdu_assembler::encode_si_pdu(slot_point sl_tx, const sib_information& si_info)
{
  const unsigned tbs = si_info.pdsch_cfg.codewords[0].tb_size_bytes;
  srsran_assert(tbs <= MAX_BCCH_DL_SCH_PDU_SIZE, "BCCH-DL-SCH is too long. Revisit constant");

  if (si_info.version != current_buffers.version) {
    // Current SI message version is too old. Fetch new version from shared buffer.
    current_buffers = pending.read();
    if (current_buffers.version != si_info.version) {
      // Versions do not match.
      logger.error("SI message version mismatch. Expected: {}, got: {}", si_info.version, current_buffers.version);
      // We force the version to avoid more than one warning.
      current_buffers.version = si_info.version;
    }
  }

  if (si_info.si_indicator == sib_information::si_indicator_type::sib1) {
    if (current_buffers.sib1_len.value() > tbs) {
      logger.warning("Failed to encode SIB1 PDSCH. Cause: PDSCH TB size {} is smaller than the SIB1 length {}",
                     tbs,
                     current_buffers.sib1_len);
      return span<const uint8_t>{zeros_payload}.first(tbs);
    }
    return span<const uint8_t>(current_buffers.sib1_buffer->data(), tbs);
  }

  srsran_assert(si_info.si_msg_index.has_value(), "Invalid SI message index");
  const unsigned idx = si_info.si_msg_index.value();
  if (idx >= current_buffers.si_msg_buffers.size()) {
    logger.error("Failed to encode SI-message in PDSCH. Cause: SI message index {} does not exist", idx);
    return span<const uint8_t>{zeros_payload}.first(tbs);
  }

  if (message_ext_handler) {
    auto si_pdu = message_ext_handler->get_pdu(sl_tx, si_info);
    if (!si_pdu.empty()) {
      return si_pdu;
    }
  }

  if (current_buffers.si_msg_buffers[idx].first.value() > tbs) {
    logger.warning(
        "Failed to encode SI-message {} PDSCH. Cause: PDSCH TB size {} is smaller than the SI-message length {}",
        idx,
        tbs,
        current_buffers.si_msg_buffers[idx].first.value());
    return span<const uint8_t>{zeros_payload}.first(tbs);
  }

  // If the message is segmented, return the current segment.
  if (auto* segmented_msg =
          std::get_if<segmented_sib_list<bcch_dl_sch_buffer>>(&current_buffers.si_msg_buffers[idx].second)) {
    // If the SI message has not been scheduled previously within the repetition period, and has been previously
    // transmitted, advance to the next message segment.
    if (!si_info.is_repetition && (si_info.nof_txs > 0)) {
      segmented_msg->advance_current_segment();
    }
    return span<const uint8_t>(segmented_msg->get_current_segment()->data(), tbs);
  }

  return span<const uint8_t>(std::get<bcch_dl_sch_buffer>(current_buffers.si_msg_buffers[idx].second)->data(), tbs);
}

#ifndef SRSRAN_HAS_ENTERPRISE_NTN

namespace {

// Maximum number of dynamic SI update records kept in the matching table.
static constexpr size_t MAX_DYNAMIC_SI_PDU_UPDATES = 256;

// Maximum number of linearized SI PDU buffers retained to keep spans handed to lower layers valid.
static constexpr size_t MAX_RETAINED_DYNAMIC_SI_PDUS = 4096;

class generic_si_message_extension_handler final : public sib_pdu_assembler::message_handler
{
public:
  explicit generic_si_message_extension_handler(srslog::basic_logger& logger_) : logger(logger_) {}

  si_version_type update(si_version_type si_version, const byte_buffer& pdu) override
  {
    // Dynamic SI PDU replacement does not change the SI scheduling version advertised in SIB1.
    (void)pdu;
    return si_version;
  }

  bool enqueue_si_pdu_updates(const mac_cell_sys_info_pdu_update& pdu_update_req) override
  {
    if (pdu_update_req.si_messages.empty()) {
      logger.warning("Discarding dynamic SI PDU update. Cause: No SI messages were provided");
      return false;
    }

    if (pdu_update_req.si_messages.size() > 1 &&
        (!pdu_update_req.si_slot_period.has_value() || pdu_update_req.si_slot_period.value() == 0)) {
      logger.warning("Discarding dynamic SI PDU update. Cause: si_slot_period is required for multi-PDU updates");
      return false;
    }

    dynamic_si_pdu_update update;
    update.si_msg_idx     = pdu_update_req.si_msg_idx;
    update.sib_idx        = pdu_update_req.sib_idx;
    update.start_slot     = pdu_update_req.slot;
    update.si_slot_period = pdu_update_req.si_slot_period;
    update.pdus.reserve(pdu_update_req.si_messages.size());

    for (const byte_buffer& si_msg : pdu_update_req.si_messages) {
      auto pdu = dynamic_si_pdu{make_linear_buffer(si_msg), static_cast<unsigned>(si_msg.length())};
      retained_pdus.push_back(pdu.buffer);
      update.pdus.push_back(std::move(pdu));
    }

    while (retained_pdus.size() > MAX_RETAINED_DYNAMIC_SI_PDUS) {
      retained_pdus.pop_front();
    }

    control_snapshot.updates.push_back(std::move(update));
    if (control_snapshot.updates.size() > MAX_DYNAMIC_SI_PDU_UPDATES) {
      control_snapshot.updates.erase(control_snapshot.updates.begin(),
                                     control_snapshot.updates.end() - MAX_DYNAMIC_SI_PDU_UPDATES);
    }
    pending_snapshot.write_and_commit(control_snapshot);

    logger.debug("Enqueued dynamic SI PDU update si_msg={} sib={} slot={} nof_pdus={}",
                 pdu_update_req.si_msg_idx,
                 pdu_update_req.sib_idx,
                 pdu_update_req.slot,
                 pdu_update_req.si_messages.size());

    return true;
  }

  span<const uint8_t> get_pdu(slot_point sl_tx, const sib_information& si_info) override
  {
    if (si_info.si_indicator != sib_information::si_indicator_type::other_si || !si_info.si_msg_index.has_value()) {
      return {};
    }

    const auto& snapshot = pending_snapshot.read();
    const auto* selected = select_pdu(snapshot, sl_tx, si_info.si_msg_index.value());
    if (selected == nullptr) {
      return {};
    }

    const unsigned tbs = si_info.pdsch_cfg.codewords[0].tb_size_bytes;
    if (selected->length > tbs) {
      logger.warning("Failed to encode dynamic SI-message {} PDSCH. Cause: PDSCH TB size {} is smaller than the "
                     "SI-message length {}",
                     si_info.si_msg_index.value(),
                     tbs,
                     selected->length);
      return span<const uint8_t>{zeros_payload}.first(tbs);
    }

    return span<const uint8_t>(selected->buffer->data(), tbs);
  }

private:
  using bcch_dl_sch_buffer = std::shared_ptr<const std::vector<uint8_t>>;

  struct dynamic_si_pdu {
    bcch_dl_sch_buffer buffer;
    unsigned           length = 0;
  };

  struct dynamic_si_pdu_update {
    unsigned                         si_msg_idx = 0;
    uint8_t                          sib_idx    = 0;
    slot_point                       start_slot;
    std::optional<unsigned>          si_slot_period;
    std::vector<dynamic_si_pdu>      pdus;
  };

  struct dynamic_si_pdu_snapshot {
    std::vector<dynamic_si_pdu_update> updates;
  };

  static const dynamic_si_pdu* select_pdu(const dynamic_si_pdu_snapshot& snapshot,
                                          slot_point                     sl_tx,
                                          unsigned                       si_msg_idx)
  {
    const dynamic_si_pdu* selected = nullptr;

    for (const dynamic_si_pdu_update& update : snapshot.updates) {
      if (update.si_msg_idx != si_msg_idx || update.pdus.empty() || sl_tx < update.start_slot) {
        continue;
      }

      unsigned pdu_idx = 0;
      if (update.pdus.size() > 1) {
        if (!update.si_slot_period.has_value()) {
          continue;
        }

        const slot_difference slot_offset = sl_tx - update.start_slot;
        if (slot_offset < 0 || (slot_offset % static_cast<slot_difference>(*update.si_slot_period)) != 0) {
          continue;
        }

        pdu_idx = static_cast<unsigned>(slot_offset / static_cast<slot_difference>(*update.si_slot_period));
        if (pdu_idx >= update.pdus.size()) {
          continue;
        }
      }

      selected = &update.pdus[pdu_idx];
    }

    return selected;
  }

  srslog::basic_logger&                         logger;
  dynamic_si_pdu_snapshot                       control_snapshot;
  lockfree_triple_buffer<dynamic_si_pdu_snapshot> pending_snapshot;
  std::deque<bcch_dl_sch_buffer>                retained_pdus;
};

} // namespace

std::unique_ptr<sib_pdu_assembler::message_handler>
srsran::create_si_message_extension_handler(const mac_cell_sys_info_config& /*req*/)
{
  return std::make_unique<generic_si_message_extension_handler>(srslog::fetch_basic_logger("MAC"));
}

#endif // SRSRAN_HAS_ENTERPRISE_NTN
