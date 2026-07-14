#include "ntn_service_switch_over_controller.h"
#include <algorithm>
#include <optional>

using namespace srsran;
using namespace srs_cu_cp;

static bool event_affects_beam(const ntn_service_switch_over_event& event, const std::string& beam_id)
{
  return std::find(event.affected_beam_ids.begin(), event.affected_beam_ids.end(), beam_id) !=
         event.affected_beam_ids.end();
}

static bool event_affects_nci(const ntn_service_switch_over_event& event, nr_cell_identity nci)
{
  return std::any_of(event.affected_ncis.begin(), event.affected_ncis.end(), [nci](nr_cell_identity affected_nci) {
    return affected_nci.value() == nci.value();
  });
}

static ntn_service_beam_policy beam_policy_for_event(const ntn_service_switch_over_event& event)
{
  if (event.type == ntn_service_switch_over_type::soft) {
    return ntn_service_beam_policy::prepare;
  }
  switch (event.policy) {
    case ntn_service_switch_over_policy::prepare:
      return ntn_service_beam_policy::block_new_demand;
    case ntn_service_switch_over_policy::drain:
    case ntn_service_switch_over_policy::handover_preferred:
      return ntn_service_beam_policy::drain;
    case ntn_service_switch_over_policy::release_allowed:
      return ntn_service_beam_policy::release_allowed;
  }
  return ntn_service_beam_policy::drain;
}

static unsigned beam_policy_rank(ntn_service_beam_policy policy)
{
  switch (policy) {
    case ntn_service_beam_policy::normal:
      return 0;
    case ntn_service_beam_policy::prepare:
      return 1;
    case ntn_service_beam_policy::block_new_demand:
      return 2;
    case ntn_service_beam_policy::drain:
      return 3;
    case ntn_service_beam_policy::release_allowed:
      return 4;
  }
  return 0;
}

bool ntn_service_switch_over_controller::apply_event(const ntn_service_switch_over_event& event)
{
  if (event.event_id == 0 || (event.affected_beam_ids.empty() && event.affected_ncis.empty())) {
    return false;
  }

  clear_event(event.event_id);
  active_events.push_back(event);
  return true;
}

bool ntn_service_switch_over_controller::clear_event(uint64_t event_id)
{
  const auto old_size = active_events.size();
  active_events.erase(std::remove_if(active_events.begin(),
                                     active_events.end(),
                                     [event_id](const ntn_service_switch_over_event& event) {
                                       return event.event_id == event_id;
                                     }),
                      active_events.end());
  return active_events.size() != old_size;
}

bool ntn_service_switch_over_controller::apply_manual_override(const ntn_manual_override_command& command)
{
  switch (command.mode) {
    case ntn_manual_override_mode::none:
    case ntn_manual_override_mode::freeze_current_state:
    case ntn_manual_override_mode::replace_service_state:
      manual_override_mode = command.mode;
      return true;
    case ntn_manual_override_mode::restore_automatic_source:
      manual_override_mode = ntn_manual_override_mode::none;
      return true;
  }
  return false;
}

bool ntn_service_switch_over_controller::automatic_source_updates_allowed() const
{
  return manual_override_mode != ntn_manual_override_mode::freeze_current_state &&
         manual_override_mode != ntn_manual_override_mode::replace_service_state;
}

ntn_service_beam_policy ntn_service_switch_over_controller::policy_for_beam(const std::string&                 beam_id,
                                                                            std::optional<nr_cell_identity> nci) const
{
  ntn_service_beam_policy policy = ntn_service_beam_policy::normal;
  for (const auto& event : active_events) {
    if (!event_affects_beam(event, beam_id) && (!nci.has_value() || !event_affects_nci(event, nci.value()))) {
      continue;
    }
    const ntn_service_beam_policy event_policy = beam_policy_for_event(event);
    if (beam_policy_rank(event_policy) > beam_policy_rank(policy)) {
      policy = event_policy;
    }
  }
  return policy;
}

bool ntn_service_switch_over_controller::blocks_new_demand_for_beam(const std::string& beam_id) const
{
  const ntn_service_beam_policy policy = policy_for_beam(beam_id, std::nullopt);
  return policy == ntn_service_beam_policy::drain || policy == ntn_service_beam_policy::release_allowed ||
         policy == ntn_service_beam_policy::block_new_demand;
}

bool ntn_service_switch_over_controller::blocks_new_demand_for_beam(const std::string& beam_id,
                                                                    nr_cell_identity   nci) const
{
  const ntn_service_beam_policy policy = policy_for_beam(beam_id, nci);
  return policy == ntn_service_beam_policy::drain || policy == ntn_service_beam_policy::release_allowed ||
         policy == ntn_service_beam_policy::block_new_demand;
}

bool ntn_service_switch_over_controller::forces_drain_for_beam(const std::string& beam_id) const
{
  const ntn_service_beam_policy policy = policy_for_beam(beam_id, std::nullopt);
  return policy == ntn_service_beam_policy::drain || policy == ntn_service_beam_policy::release_allowed;
}

bool ntn_service_switch_over_controller::forces_drain_for_beam(const std::string& beam_id, nr_cell_identity nci) const
{
  const ntn_service_beam_policy policy = policy_for_beam(beam_id, nci);
  return policy == ntn_service_beam_policy::drain || policy == ntn_service_beam_policy::release_allowed;
}

ntn_service_switch_over_snapshot ntn_service_switch_over_controller::build_snapshot(
    const std::vector<std::pair<std::string, nr_cell_identity>>& known_beams) const
{
  ntn_service_switch_over_snapshot snapshot;
  snapshot.manual_override_mode             = manual_override_mode;
  snapshot.automatic_source_updates_allowed = automatic_source_updates_allowed();
  snapshot.active_events                    = active_events;
  snapshot.beams.reserve(known_beams.size());
  for (const auto& beam : known_beams) {
    snapshot.beams.push_back({beam.first, beam.second, policy_for_beam(beam.first, beam.second)});
  }
  return snapshot;
}
