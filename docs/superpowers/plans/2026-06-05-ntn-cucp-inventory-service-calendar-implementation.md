# NTN CU-CP Inventory And Service Calendar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement CUCP-006 and CUCP-007 so CU-CP keeps a full NTN candidate inventory and derives SR/SRS slot request intent only from loaded beams with real demand.

**Architecture:** First decouple candidate selection from loaded-service limits in the CU-CP NTN selector/scheduler. Then keep placement/calendar behavior demand-driven, with empty candidates excluded from antenna slot and F1AP-CU slot request payloads.

**Tech Stack:** C++20, srsRAN CU-CP, gtest/ctest, PowerShell and bash harness scripts.

---

## File Map

- Modify `lib/cu_cp/ntn_mobility/ntn_served_beam_selector.h`
  - Add candidate-inventory naming wrappers while preserving old served-beam API compatibility.
- Modify `lib/cu_cp/ntn_mobility/ntn_served_beam_selector.cpp`
  - Stop truncating candidate inventory by loaded-service beam limits.
- Modify `lib/cu_cp/ntn_mobility/ntn_served_beam_scheduler.h`
  - Clarify that the cap applies to hopping window / loaded-service focus, not candidate inventory.
- Modify `lib/cu_cp/ntn_mobility/ntn_served_beam_scheduler.cpp`
  - Keep full candidates and apply `max_nof_served_beams` only to the hopping window.
- Modify `lib/cu_cp/ntn_mobility/ntn_beam_placement_planner.h`
  - Add loaded service calendar helper naming if current helpers are not explicit enough.
- Modify `lib/cu_cp/ntn_mobility/ntn_beam_placement_planner.cpp`
  - Keep empty candidates slot-free and make calendar extraction deterministic.
- Modify `lib/cu_cp/cu_cp_impl.cpp`
  - Preserve CU-CP F1AP slot request behavior using active-loaded/draining demand only.
- Modify CU-CP tests under `tests/unittests/cu_cp/ntn_mobility/` and `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`.
- Do not modify F1AP-CU production paths in the first pass. Reuse existing F1AP-CU contract tests unless a focused CUCP-007 test proves the current CU-side contract cannot express clear/update semantics.

## Task 1: CUCP-006 Red Tests For Full Candidate Inventory

**Files:**
- Modify: `tests/unittests/cu_cp/ntn_mobility/ntn_served_beam_scheduler_test.cpp`
- Modify: `tests/unittests/cu_cp/ntn_mobility/ntn_beam_placement_planner_test.cpp`

- [ ] **Step 1: Add failing scheduler test for uncapped candidates**

Add a test equivalent to:

```cpp
TEST(ntn_served_beam_scheduler, keeps_full_candidate_inventory_when_hopping_window_is_capped)
{
  ntn_served_beam_scheduler_config cfg;
  cfg.min_elevation_deg              = -90.0;
  cfg.max_nof_served_beams           = 2;
  cfg.served_beam_hopping_enabled    = false;
  cfg.served_beam_hopping_dwell_updates = 1;

  for (unsigned i = 0; i != 1000; ++i) {
    ntn_beam_position beam;
    beam.beam_id              = fmt::format("CN-BEAM-{:04}", i);
    beam.nci                  = nr_cell_identity::create(0x1000 + i).value();
    beam.center_latitude_deg  = 0.0;
    beam.center_longitude_deg = static_cast<double>(i) * 0.01;
    beam.coverage_radius_m    = 10000.0;
    beam.enabled              = true;
    cfg.beams.push_back(beam);
  }

  ntn_served_beam_scheduler scheduler(cfg);
  const auto schedule = scheduler.compute_schedule({0.0, 0.0, 7000000.0});

  ASSERT_EQ(schedule.candidates.size(), 1000U);
  EXPECT_EQ(schedule.beam_ids.size(), 2U);
  EXPECT_EQ(std::count_if(schedule.candidates.begin(),
                          schedule.candidates.end(),
                          [](const ntn_served_beam_candidate& c) { return c.in_hopping_window; }),
            2);
}
```

- [ ] **Step 2: Add failing selector test for uncapped candidate helper**

Add a test for the new helper signature:

```cpp
TEST(ntn_served_beam_selector, selects_full_candidate_inventory_without_loaded_beam_limit)
{
  std::vector<ntn_beam_position> beams;
  for (unsigned i = 0; i != 16; ++i) {
    ntn_beam_position beam;
    beam.beam_id              = fmt::format("CN-BEAM-{:04}", i);
    beam.nci                  = nr_cell_identity::create(0x2000 + i).value();
    beam.center_latitude_deg  = 0.0;
    beam.center_longitude_deg = static_cast<double>(i);
    beam.coverage_radius_m    = 10000.0;
    beam.enabled              = true;
    beams.push_back(beam);
  }

  const std::vector<ntn_served_beam_candidate> candidates =
      select_ntn_candidate_inventory_by_elevation(beams, {0.0, 0.0, 7000000.0}, -90.0);

  ASSERT_EQ(candidates.size(), beams.size());
}
```

Expected red result: compilation fails because `select_ntn_candidate_inventory_by_elevation` does not exist yet.

- [ ] **Step 3: Run red tests**

Run:

```bash
wsl bash -lc "cd /mnt/d/code/srsRAN_Project-main && cmake --build build/ai-clean --target ntn_mobility_test -j2 && ./build/ai-clean/tests/unittests/cu_cp/ntn_mobility/ntn_mobility_test --gtest_filter='ntn_served_beam_scheduler.*candidate*|ntn_served_beam_selector.*candidate*'"
```

Expected: at least one new test fails because old selector semantics still cap or old helper naming has no uncapped candidate inventory API.

## Task 2: CUCP-006 Implementation

**Files:**
- Modify: `lib/cu_cp/ntn_mobility/ntn_served_beam_selector.h`
- Modify: `lib/cu_cp/ntn_mobility/ntn_served_beam_selector.cpp`
- Modify: `lib/cu_cp/ntn_mobility/ntn_served_beam_scheduler.h`
- Modify: `lib/cu_cp/ntn_mobility/ntn_served_beam_scheduler.cpp`
- Modify: `lib/cu_cp/cu_cp_impl.cpp`

- [ ] **Step 1: Add uncapped candidate inventory helper**

In `ntn_served_beam_selector.h`, add:

```cpp
std::vector<ntn_served_beam_candidate>
select_ntn_candidate_inventory_by_elevation(const std::vector<ntn_beam_position>& beams,
                                            const ecef_coordinates_t&             satellite,
                                            double                                min_elevation_deg);
```

- [ ] **Step 2: Implement uncapped helper**

In `ntn_served_beam_selector.cpp`, move the filtering/sorting logic into `select_ntn_candidate_inventory_by_elevation` and remove loaded-limit truncation from that helper.

- [ ] **Step 3: Preserve old API as capped compatibility wrapper**

Keep `select_ntn_served_beam_candidates_by_elevation` but make it call the uncapped helper and then apply the old `max_nof_served_beams` cap. This preserves existing served-beam tests while the new helper becomes the only candidate-inventory API.

- [ ] **Step 4: Keep hopping window cap in scheduler**

In `ntn_served_beam_scheduler::compute_schedule`, call `select_ntn_candidate_inventory_by_elevation(...)` and use `cfg.max_nof_served_beams` only for `window_size`.

- [ ] **Step 5: Run CUCP-006 green tests**

Run:

```bash
wsl bash -lc "cd /mnt/d/code/srsRAN_Project-main && cmake --build build/ai-clean --target ntn_mobility_test -j2 && ./build/ai-clean/tests/unittests/cu_cp/ntn_mobility/ntn_mobility_test"
```

Expected: `ntn_mobility_test` passes.

## Task 3: CUCP-007 Red Tests For Demand-Driven Calendar

**Files:**
- Modify: `tests/unittests/cu_cp/ntn_mobility/ntn_beam_placement_planner_test.cpp`
- Modify: `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
- Do not modify F1AP-CU tests in this task unless the CU-CP tests expose a missing CU-side clear/update contract.

- [ ] **Step 1: Add loaded service calendar helper test**

Add this helper test:

```cpp
TEST(ntn_beam_placement_plan_helpers, extracts_loaded_service_calendar_beams_with_demand_only)
{
  ntn_beam_placement_plan plan;
  plan.assignments = {
      make_assignment("CN-BEAM-0001", ntn_beam_assignment_state::candidate, du_index_t::min, 70.0, 0, 0, true),
      make_assignment("CN-BEAM-0002", ntn_beam_assignment_state::active_loaded, du_index_t::min, 65.0, 1, 1, true),
      make_assignment("CN-BEAM-0003", ntn_beam_assignment_state::draining, du_index_t::min, 60.0, 1, 0, false),
      make_assignment("CN-BEAM-0004", ntn_beam_assignment_state::inactive, du_index_t::invalid, 0.0, 0, 0, false)};

  const std::vector<std::string> loaded_beam_ids = get_loaded_service_calendar_ntn_beam_ids(
      plan,
      {{"CN-BEAM-0001", 70.0, true},
       {"CN-BEAM-0002", 65.0, true},
       {"CN-BEAM-0003", 60.0, false},
       {"CN-BEAM-0004", 0.0, false}});

  EXPECT_EQ(loaded_beam_ids, std::vector<std::string>({"CN-BEAM-0002", "CN-BEAM-0003"}));
}
```

Expected red result: compilation fails because `get_loaded_service_calendar_ntn_beam_ids` does not exist yet.

- [ ] **Step 2: Add no-retransmit or clear behavior test**

Use existing CU-CP NTN test doubles to prove that an empty candidate with a valid DU still has no slot request. If the current tests already cover that setup path, add the assertion to the existing NTN slot-request test rather than creating a duplicate fixture.

- [ ] **Step 3: Run red tests**

Run:

```bash
wsl bash -lc "cd /mnt/d/code/srsRAN_Project-main && cmake --build build/ai-clean --target cu_cp_test -j2 && ./build/ai-clean/tests/unittests/cu_cp/cu_cp_test --gtest_filter='cu_cp_ntn_mobility_test.*slot*|cu_cp_ntn_mobility_test.*calendar*'"
```

Expected: new CUCP-007 assertions fail until the loaded calendar helper / CU-CP retransmit guard is implemented or tightened.

## Task 4: CUCP-007 Implementation And Validation

**Files:**
- Modify: `lib/cu_cp/ntn_mobility/ntn_beam_placement_planner.h`
- Modify: `lib/cu_cp/ntn_mobility/ntn_beam_placement_planner.cpp`
- Modify: `lib/cu_cp/cu_cp_impl.cpp`
- Modify exact F1AP-CU exception paths only if required by failing F1AP-CU contract tests.

- [ ] **Step 1: Add loaded service calendar helper**

Add:

```cpp
std::vector<std::string>
get_loaded_service_calendar_ntn_beam_ids(const ntn_beam_placement_plan&                plan,
                                         const std::vector<ntn_served_beam_candidate>& visible_beams);
```

This helper must return only service assignments that have user demand and preserve visible-candidate order.

- [ ] **Step 2: Use helper for CU-CP slot request contract**

In `cu_cp_impl.cpp`, keep F1AP slot request generation restricted to assignments where:

```cpp
assignment.state == ntn_beam_assignment_state::active_loaded ||
assignment.state == ntn_beam_assignment_state::draining
```

and all slot periods are non-zero.

- [ ] **Step 3: Keep empty candidates slot-free**

Ensure placement assigns all slot fields to zero before scheduling and only fills them for service assignments with demand.

- [ ] **Step 4: Run focused validation**

Run:

```bash
wsl bash -lc "cd /mnt/d/code/srsRAN_Project-main && cmake --build build/ai-clean --target ntn_mobility_test cu_cp_test -j2"
wsl bash -lc "cd /mnt/d/code/srsRAN_Project-main && ./build/ai-clean/tests/unittests/cu_cp/ntn_mobility/ntn_mobility_test"
wsl bash -lc "cd /mnt/d/code/srsRAN_Project-main && ./build/ai-clean/tests/unittests/cu_cp/cu_cp_test --gtest_filter=cu_cp_ntn_mobility_test.*"
```

- [ ] **Step 5: Run harness and required validation**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/guard_changed_paths.ps1 -Base ai/cucp-harness-base -TaskFile ai_harness/tasks/CUCP-006-candidate-inventory.md
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/guard_changed_paths.ps1 -Base ai/cucp-harness-base -TaskFile ai_harness/tasks/CUCP-007-demand-driven-service-calendar.md
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/check_rejected_overlap.ps1 -Base ai/cucp-harness-base
```

Then run:

```bash
wsl bash -lc "cd /mnt/d/code/srsRAN_Project-main && BUILD_DIR=build/ai-clean bash ai_harness/scripts/configure_build.sh"
wsl bash -lc "cd /mnt/d/code/srsRAN_Project-main && BUILD_DIR=build/ai-clean bash ai_harness/scripts/build.sh"
wsl bash -lc "cd /mnt/d/code/srsRAN_Project-main && BUILD_DIR=build/ai-clean bash ai_harness/scripts/run_cucp_tests.sh"
```

Expected: path guards pass, rejected overlap passes, focused tests pass, and CU-CP validation passes. If full build/test is delegated to subagents, the main agent must still record the final pass/fail output.
