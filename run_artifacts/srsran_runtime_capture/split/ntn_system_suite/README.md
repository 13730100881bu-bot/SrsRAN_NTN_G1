# NTN System Test Suite

This suite runs two kinds of checks:

1. Live end-to-end checks with Open5GS, srsUE, CU-CP, CU-UP, and DU.
2. Simulated control-plane checks with existing CTest fake AMF/DU/UE coverage.

The live checks prove that the split stack can attach a UE, create a PDU session, pass GTP-U traffic, and expose pcap/log evidence. The simulated checks cover deeper NTN control-plane behavior that current srsUE cannot drive directly, such as paging policy, connected mobility, service-pair behavior, resource audit/repair, and NRPPa.

## Common Commands

Before running live checks, stop any unrelated local `srscucp`, `srscuup`, `srsdu`, `srsue`, or `tcpdump` experiment in the same WSL instance. The existing split scripts and this wrapper clean those process names so the captured pcap is closed and repeatable.

Run the default suite:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1
```

Run live checks only:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Mode live
```

Run simulated control-plane checks only:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Mode sim
```

Run one simulated scenario:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Mode sim -Scenario ntn_cli_observability_sim
```

Run with focused builds first:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Mode sim -Build
```

Use a shorter command timeout:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Mode sim -TimeoutSeconds 300
```

## Scenarios

The scenario registry lives in:

```text
D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\ntn_system_suite\scenarios.psd1
```

The v1 registry includes:

- `baseline_attach_ping`: live Open5GS + srsUE + split attach, PDU session, and ping.
- `ntn_runtime_sib19_visibility`: live NTN SIB19/F1AP/MAC visibility proof through the existing split script.
- `ntn_connected_mobility_sim`: simulated connected mobility coverage through focused CTest.
- `ntn_paging_sim`: simulated NTN paging coverage through focused CTest.
- `ntn_resource_repair_sim`: simulated RNTI/SR/SRS resource audit and repair coverage.
- `ntn_nrppa_sim`: simulated NRPPa and positioning transport coverage.
- `ntn_cli_observability_sim`: simulated CLI observability coverage.

## Output

Each run writes a directory under:

```text
D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\log_exports\
```

The main report is:

```text
ntn_system_suite_summary.md
```

Each row has:

- `Scenario`: scenario name or suite helper check.
- `Kind`: `live`, `sim`, `pcap`, `artifact`, or `process`.
- `Status`: `pass`, `fail`, or `warn`.
- `Reason`: concise command result or failure reason.
- `Artifact`: output directory, log path, or pcap path.

## Interpretation

- `live` means Open5GS and srsUE were used through an existing split script.
- `sim` means CTest drove existing fake AMF/DU/UE control-plane tests.
- `pcap` means `tshark` checked captured protocol evidence. Missing `tshark` is reported as `warn`, not `fail`.
- `artifact` means the suite copied live pcap/log artifacts when live scenarios were selected.
- `process` means the suite verified that no split demo process was left running.

The suite returns exit code `1` when any scenario or helper row has status `fail`. `warn` rows are visible in the summary but do not fail the command.

This suite does not claim RF/PHY validation and does not replace a commercial UE simulator.
