# NTN Initial UL Position Consumer

## Purpose

CUCP-048 connects the existing onboard position-plan audit to the first RRC
Setup admission decision. CU-CP can now consume one private position
observation for an Initial UL attempt and compare it with the active onboard
plan, the live DU cell route and the installed software access calendar.

The feature is opt-in and has three modes:

| Mode | Initial UL processing |
|---|---|
| `disabled` | Skip position lookup and preserve the existing admission path. This is the default. |
| `audit` | Consume and check an available observation, update counters and continue normal admission for every result. |
| `strict` | Continue normal admission only after the observation passes every check. CU-CP startup fails if no ready source was injected. |

The mode is configured with
`ntn_onboard_position_plan.initial_ul_position_validation`. The observation
source itself implements the CU-CP private extension contract in
`include/srsran/cu_cp/ntn_initial_ul_position_observation.h` and is injected
through `cu_cp_configuration`; it is not loaded from YAML and is not carried by
a standard protocol message.

## Observation and lookup key

One observation contains:

- a non-zero, source-session-unique `observation_id`;
- DU, DU cell, C-RNTI and DU connection generation;
- `position_id`, NCI and PCI;
- catalog and schedule versions;
- source and calendar hashes;
- the PRACH occasion time and cell-local uplink port.

CU-CP assigns the receive time when the record enters the store. Lookup uses
the exact tuple:

```text
DU + DU cell + C-RNTI + DU connection generation
```

The store is thread-safe, keeps at most 1,024 records and accepts a record for
one second. It never overwrites a record to make room. Two live records for one
lookup key are ambiguous. A consumed observation cannot authorize another
attempt, and reuse of its `observation_id` during the one-second lifetime is
reported as replay.

The PRACH event time uses the same system-clock basis as CU-CP. It must not be
in the future and must be less than one second old when the Initial UL request
is checked. A newly received copy of an old event therefore cannot regain
authorization after its store entry expires.

Neither the pending records nor the accepted UE context are written to the
position-plan state file.

## Admission sequence

For an onboard Initial UL attempt, CU-CP performs the following sequence before
the existing access-ownership and capacity checks:

1. Read the UE's current DU, DU cell and C-RNTI and attach the current DU
   connection generation.
2. Take exactly one observation using the complete lookup key.
3. Require a ready runtime mapping backed by an active, valid and DU-applied
   software calendar.
4. Match satellite, catalog, schedule and both hashes with the active plan.
5. Match `position_id`, NCI and PCI with the position owner in that plan.
6. Match the observation with the UE's current DU cell and connection
   generation.
7. Match the PRACH time with the planned opportunity and the uplink port with
   its paired UL-beam intent.
8. Run the existing RNTI ownership, resource-capacity and ordinary CU-CP
   admission checks.

In `strict` mode, any failure in steps 1-7 returns RRC Reject before an access
ownership record is created. When all checks and ordinary admission succeed,
CU-CP keeps a temporary UE context containing the position, stable cell
identity, schedule, calendar hash and DU generation. `audit` mode never creates
this trusted temporary context.

## Lifetime rules

- A pending plan never authorizes Initial UL.
- A failed update leaves the old active plan available for checks.
- Activation replaces the authorization baseline as one operation and removes
  observations and temporary contexts tied to the previous plan.
- DU disconnection removes that DU's records and contexts. A reconnect uses a
  new connection generation.
- Restart removes all records and contexts. Runtime mapping must first complete
  its existing DU reconciliation.
- Initial Context Setup completion, UE removal and RRC Setup failure remove the
  temporary UE context.
- An active plan with no assigned positions is a deny-all Initial UL baseline
  in `strict` mode.
- RRC reestablishment, resume and handover continue to use their existing
  admission paths.

## Read-only status

The existing `ntn_state` command reports:

- validation mode and source state;
- source authority and pending-record count;
- active temporary UE contexts;
- accepted, rejected, audited, expired and replayed counters;
- the most recent machine-readable result.

Stable reasons include `source_unavailable`, `observation_missing`,
`observation_expired`, `ambiguous_observation`, `replayed_observation`,
`ue_identity_mismatch`, `du_generation_mismatch` and
`active_plan_mismatch`. Detailed active-plan checks retain their existing
reasons, such as `schedule_version_mismatch`, `calendar_hash_mismatch`,
`position_not_assigned_to_cell`, `prach_occasion_not_scheduled` and
`resource_port_mismatch`.

## Scope

CUCP-048 implements the CU-CP consumer and test injection boundary. It does not
change F1AP, DU, MAC, PHY, RU/RF, Web/GIS or generated ASN.1. The current
application has no production lower-layer observation producer, so `strict`
mode is intentionally unavailable until a source is injected before startup.
The next cross-layer task can connect that source without changing this
consumer contract.

`applied` keeps its established meaning: the matching software access calendar
is installed. Hardware beam steering and radio transmission are separate
device-level functions.

## Main code areas

- Store and authorizer:
  `lib/cu_cp/ntn_mobility/ntn_initial_ul_position_authorizer.*`
- Injected source contract:
  `include/srsran/cu_cp/ntn_initial_ul_position_observation.h`
- RRC Setup integration, lifecycle and status:
  `lib/cu_cp/cu_cp_impl.*`
- Configuration and read-only command:
  `include/srsran/cu_cp/cu_cp_configuration.h`,
  `include/srsran/cu_cp/cu_cp_command_handler.h`,
  `apps/units/o_cu_cp/cu_cp/`
- Focused tests:
  `tests/unittests/cu_cp/ntn_mobility/ntn_initial_ul_position_authorizer_test.cpp`,
  `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`, and
  `tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp`

## Validation results

The CUCP-048 focused validation completed on 2026-08-08:

- all requested build targets completed: `ntn_mobility_test`, `cu_cp_test`,
  `cu_cp_unit_config_test` and `srsran_cu_cp`;
- the bounded store, provider contract, authorizer and active-plan audit group
  passed 20/20;
- the direct CU-CP Initial UL, lifecycle and default-off group passed 12/12;
- the configuration and read-only status group passed 8/8;
- the simulated `ntn_cli_observability_sim` suite passed 22/22 and reported no
  residual split process;
- `git diff --check` passed.

The full F1AP, DU, MAC and RF suites were not repeated because this task did not
change their interfaces or implementations. The system scenario exercised the
read-only command path; the lower-layer position producer remains the next
separate integration task.
