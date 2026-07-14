#pragma once

#include "srsran/cu_cp/ntn_service_switch_over.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

class ntn_service_switch_over_controller
{
public:
  bool apply_event(const ntn_service_switch_over_event& event);
  bool clear_event(uint64_t event_id);
  bool apply_manual_override(const ntn_manual_override_command& command);

  bool automatic_source_updates_allowed() const;
  bool blocks_new_demand_for_beam(const std::string& beam_id) const;
  bool blocks_new_demand_for_beam(const std::string& beam_id, nr_cell_identity nci) const;
  bool forces_drain_for_beam(const std::string& beam_id) const;
  bool forces_drain_for_beam(const std::string& beam_id, nr_cell_identity nci) const;

  ntn_manual_override_mode get_manual_override_mode() const { return manual_override_mode; }

  ntn_service_switch_over_snapshot
  build_snapshot(const std::vector<std::pair<std::string, nr_cell_identity>>& known_beams) const;

private:
  ntn_service_beam_policy policy_for_beam(const std::string& beam_id, std::optional<nr_cell_identity> nci) const;

  std::vector<ntn_service_switch_over_event> active_events;
  ntn_manual_override_mode                   manual_override_mode = ntn_manual_override_mode::none;
};

} // namespace srs_cu_cp
} // namespace srsran
