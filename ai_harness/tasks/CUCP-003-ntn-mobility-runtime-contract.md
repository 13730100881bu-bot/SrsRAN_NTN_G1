# CUCP-003: CU-CP NTN mobility runtime contract

## Goal

Verify and, if needed, complete the runtime contract from the accepted CU-CP NTN mobility configuration into the CU-CP runtime path.

This task must not introduce a new NTN config schema.

## Accepted schema

CUCP-002 confirmed the accepted CU-CP-facing NTN configuration path is:

- `mobility_config.ntn_location_mobility`

The accepted satellite state source values are:

- `manual`
- `circular_orbit`
- `tle`

A GEO-like scenario is represented through `circular_orbit`, not through a separate `orbit_type: geo` field.

## Existing runtime path

The expected runtime chain is:

`mobility_config.ntn_location_mobility`
-> `apps/units/o_cu_cp/cu_cp/cu_cp_config_translators.cpp`
-> `cu_cp_cfg.mobility.meas_manager_config.ntn_location_mobility`
-> `lib/cu_cp/cu_cp_impl.cpp`
-> `create_ntn_served_beam_scheduler(...)`
-> `create_ntn_satellite_state_updater(...)`
-> `handle_ntn_satellite_state_update(...)`
-> runtime served beam / beam status / CU-CP-side UE policy inputs.

This task verifies or strengthens this CU-CP runtime contract.

It must not implement DU/MAC scheduler, HARQ timing, TA scheduler, PRACH, PHY, RU/RF, ZMQ, or GIS behavior.

## Read first

- `AGENTS.md`
- `ai_harness/context/cucp_scope.md`
- `ai_harness/context/cucp_code_map.md`
- `ai_harness/context/ntn_cucp_spec_matrix.md`
- `ai_harness/context/allowed_paths.md`
- `ai_harness/audit/path_review_notes.md`
- `ai_harness/audit/rejected_or_quarantined_paths.txt`
- `ai_harness/tasks/CUCP-002-cucp-ntn-config-validation.md`

## First step before editing

Before modifying production code:

1. Inspect the current runtime path.
2. Confirm whether the planned tests can be implemented using existing public or internal CU-CP test seams.
3. List the exact files that must change.
4. If any required path is not allowed, stop and request human approval instead of editing it.

## Preferred implementation strategy

Prefer test-first changes.

Start with tests in:

- `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
- `tests/unittests/cu_cp/ntn_mobility/ntn_satellite_state_updater_test.cpp`

Do not modify production code unless a failing test proves a real runtime contract gap.

If production code is required, prefer the smallest change in already allowed CU-CP paths, such as:

- `lib/cu_cp/cu_cp_impl.cpp`
- `lib/cu_cp/ntn_mobility/ntn_satellite_state_updater.cpp`

Do not modify app-level o_cu_cp config tests in the first attempt unless the runtime contract cannot be proven without them.

## Forbidden areas

Do not modify:

- O-DU
- flexible_o_du
- DU
- DU-high
- DU manager
- MAC scheduler
- HARQ timing
- TA scheduler
- PRACH
- PHY
- lower PHY
- RU / RF / radio drivers
- ZMQ channel behavior
- GIS site behavior
- rejected or quarantined paths

## Required behavior

Implement or verify these behaviors:

1. NTN mobility disabled preserves terrestrial CU-CP runtime behavior.
2. Valid `circular_orbit` NTN mobility config reaches the intended CU-CP runtime consumer.
3. `circular_orbit` with a non-zero update period can drive runtime served-beam updates after the relevant CU-CP setup and timer progression.
4. `manual` source does not create an automatic orbit updater, but explicit `handle_ntn_satellite_state_update(...)` can still drive served-beam runtime state if supported by current code.
5. Valid `tle` source creates a satellite state updater and can push an initial or periodic satellite state if supported by current code.
6. Invalid `tle` returns a factory/config error and does not create a broken updater.
7. `update_period == 0` behavior is explicitly tested according to the existing implementation contract.
8. Runtime behavior must not depend on DU/MAC/PHY modifications.

## Required tests

Add or update tests for:

1. `circular_orbit_config_drives_runtime_served_beam_updates`
2. `manual_update_drives_runtime_served_beam_without_auto_updater`, if the test seam exists
3. `tle_source_creates_satellite_state_updater`, if TLE is supported
4. `invalid_tle_returns_factory_error`, if TLE parsing is in scope
5. `update_period_zero_contract`, according to current implementation behavior
6. NTN disabled runtime behavior remains unchanged

Test assertions should avoid overfitting to exact satellite positions when time-dependent orbit propagation is involved.
For TLE, prefer assertions on successful state generation, reasonable ECEF/radius range, callback/update count, and error handling.

## Boundary rule

If a required test or implementation path is outside the approved CU-CP harness scope:

1. Stop.
2. Do not edit that path.
3. Report the smallest required exception path.
4. Explain why it is needed.

## Validation commands

Always run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/guard_changed_paths.ps1 -Base ai/cucp-harness-base
```

If available, also run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/check_rejected_overlap.ps1 -Base ai/cucp-harness-base
```

Use a clean build dir if possible:

```powershell
$env:BUILD_DIR = "build/ai-clean"
```

Focused validation:

```powershell
cmake --build build/ai-clean --target cu_cp_test -j2
cmake --build build/ai-clean --target ntn_mobility_test -j2
ctest --test-dir build/ai-clean -R "cu_cp_ntn_mobility_test|ntn_satellite_state_updater" --output-on-failure
```

If bash and CMake are available and not prohibitively slow, also run:

```bash
bash ai_harness/scripts/run_cucp_tests.sh
```

## Done means

- No new config schema was introduced.
- The accepted `mobility_config.ntn_location_mobility` path is verified or completed.
- Tests are added or updated.
- Path guard passes.
- Rejected/quarantined overlap check passes.
- Focused build/test commands pass, or failures are explained with logs.
- Final response lists files changed, behavior changed, tests added or updated, validation commands run, results, known risks, and any requested exception.
