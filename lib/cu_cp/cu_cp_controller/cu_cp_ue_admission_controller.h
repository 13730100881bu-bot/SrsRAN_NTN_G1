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

#include "srsran/ran/plmn_identity.h"

namespace srsran {
namespace srs_cu_cp {

enum class cu_cp_admission_request_type { initial_access, reestablishment, handover };

class cu_cp_ue_admission_controller
{
public:
  virtual ~cu_cp_ue_admission_controller() = default;

  /// Determines whether the CU-CP is in a condition to accept new UEs.
  virtual bool request_ue_setup() const = 0;

  /// Determines whether the CU-CP is in a condition to accept a UE request of the given class.
  virtual bool request_ue_setup(cu_cp_admission_request_type request_type,
                                unsigned                    additional_ues  = 0,
                                unsigned                    additional_drbs = 0) const = 0;

  /// Enable or disable the admission of new UEs.
  virtual void set_ue_admission_enabled(bool enabled) = 0;

  /// Returns true if UE admission is administratively enabled.
  virtual bool is_ue_admission_enabled() const = 0;
};

} // namespace srs_cu_cp
} // namespace srsran
