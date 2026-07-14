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

#include "ngap_dl_ran_status_transfer_procedure.h"
#include "srsran/asn1/ngap/ngap_pdu_contents.h"
#include "srsran/support/async/coroutine.h"

using namespace srsran;
using namespace srsran::srs_cu_cp;
using namespace asn1::ngap;

namespace {

/// Short timeout: this is part of the inter-CU HO critical path, the AMF should be sending the
/// DLRANStatusTransfer right after the HandoverRequestAcknowledge.
constexpr std::chrono::milliseconds dl_ran_status_transfer_timeout{1000};

class ngap_dl_status_transfer_procedure_impl
{
public:
  ngap_dl_status_transfer_procedure_impl(ue_index_t                   ue_index_,
                                         ngap_ue_transaction_manager& ev_mng_,
                                         timer_factory                timers_,
                                         ngap_ue_logger&              logger_) :
    ue_index(ue_index_), ev_mng(ev_mng_), timers(timers_), logger(logger_)
  {
    (void)ue_index;
    (void)timers;
  }

  void operator()(coro_context<async_task<expected<ngap_dl_ran_status_transfer>>>& ctx)
  {
    CORO_BEGIN(ctx);

    transaction.subscribe_to(ev_mng.dl_ran_status_transfer_outcome, dl_ran_status_transfer_timeout);
    CORO_AWAIT(transaction);

    if (transaction.successful()) {
      // The MVP does not propagate per-DRB PDCP COUNT into the bearer modification (the source
      // routine sends an empty UL list). We return an empty result so the execution routine can
      // proceed to HandoverNotify without altering the data path.
      logger.log_info("DLRANStatusTransfer received (status not propagated to CU-UP in MVP)");
      ngap_dl_ran_status_transfer result;
      result.ue_index = ue_index;
      CORO_EARLY_RETURN(result);
    }

    if (transaction.timeout_expired()) {
      logger.log_info("DLRANStatusTransfer not received within {}ms, continuing without PDCP SN sync",
                      dl_ran_status_transfer_timeout.count());
    } else {
      logger.log_info("DLRANStatusTransfer transaction cancelled");
    }
    CORO_RETURN(make_unexpected(default_error_t{}));
  }

private:
  ue_index_t                   ue_index;
  ngap_ue_transaction_manager& ev_mng;
  timer_factory                timers;
  ngap_ue_logger&              logger;

  protocol_transaction_outcome_observer<asn1::ngap::dl_ran_status_transfer_s> transaction;
};

} // namespace

async_task<expected<ngap_dl_ran_status_transfer>>
srsran::srs_cu_cp::start_ngap_dl_status_transfer_procedure(ue_index_t                   ue_index,
                                                           ngap_ue_transaction_manager& ev_mng,
                                                           timer_factory                timers,
                                                           ngap_ue_logger&              logger)
{
  return launch_async<ngap_dl_status_transfer_procedure_impl>(ue_index, ev_mng, timers, logger);
}
