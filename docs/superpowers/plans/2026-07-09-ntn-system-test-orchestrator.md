# CUCP-075 NTN System Test Orchestrator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a repeatable NTN system test orchestrator that runs live split tests with Open5GS and srsUE, runs simulated control-plane tests with existing fake AMF/DU/UE test doubles, collects pcap/log/runtime evidence, and writes a single readable report.

**Architecture:** The first version is script-first. It does not create a new UE or core-network protocol stack; it reuses Open5GS, srsUE, the split CU-CP/CU-UP/DU scripts, existing NTN proof scripts, and focused CTest filters that already simulate AMF/DU/UE behavior. A PowerShell orchestrator coordinates scenarios, exports artifacts, runs `tshark` checks, and produces one Markdown summary.

**Tech Stack:** PowerShell on Windows, WSL bash for Linux processes, Open5GS, srsUE, srsRAN split apps, CTest, `tshark`, existing `run_artifacts/srsran_runtime_capture/split` assets.

---

## Scope Boundaries

- Do not modify DU, MAC scheduler, PHY, PRACH, TA, HARQ, RF, or ZMQ code.
- Do not build a full custom UE simulator in this stage.
- Do not build a full custom core-network simulator in this stage.
- Reuse Open5GS and srsUE for live end-to-end coverage.
- Reuse focused CTest tests for simulated AMF/DU/UE control-plane coverage.
- Keep the output easy to read: "live end-to-end", "simulated control-plane", "pcap evidence", "runtime evidence", and "failed stage".

## Planned Files

- Create: `run_artifacts/srsran_runtime_capture/split/run_ntn_system_suite.ps1`
  - Main operator command.
  - Accepts scenario selection, output directory, timeout, and optional build flag.
  - Starts and stops live split processes through existing scripts.
  - Runs focused CTest filters for simulated NTN control-plane scenarios.
  - Collects logs, pcap files, and summary reports.

- Create: `run_artifacts/srsran_runtime_capture/split/ntn_system_suite/scenarios.psd1`
  - Declarative scenario registry.
  - Maps scenario names to type (`live_script`, `ctest_filter`, `pcap_check`, `artifact_only`), command, expected artifacts, and readable purpose.

- Create: `run_artifacts/srsran_runtime_capture/split/ntn_system_suite/README.md`
  - Explains how to run the suite, how to read the report, and what is live versus simulated.

- Modify: none in C++ for v1.
  - If implementation discovers that a scenario cannot be observed because CU-CP lacks a CLI/log counter, stop and propose a separate CUCP task instead of editing behavior.

---

### Task 1: Add Scenario Registry

**Files:**
- Create: `run_artifacts/srsran_runtime_capture/split/ntn_system_suite/scenarios.psd1`

- [ ] **Step 1: Create the scenario directory**

Run:

```powershell
New-Item -ItemType Directory -Force -Path D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\ntn_system_suite
```

Expected: directory exists.

- [ ] **Step 2: Add the scenario registry**

Create `run_artifacts/srsran_runtime_capture/split/ntn_system_suite/scenarios.psd1` with this content:

```powershell
@{
  baseline_attach_ping = @{
    Type        = "live_script"
    Purpose     = "Open5GS + srsUE + split CU-CP/CU-UP/DU attach, PDU session, and GTP-U ping"
    Command     = "run_split_demo.ps1"
    Evidence    = @("ngap", "e1ap", "f1ap", "nr-rrc", "gtp", "icmp")
    Required    = $true
  }

  ntn_runtime_sib19_visibility = @{
    Type        = "live_script"
    Purpose     = "NTN runtime, SIB19 CU-CP to DU apply, F1AP coordination pcap, and DU MAC SI-RNTI visibility"
    Command     = "run_split_ntn_sib19_ue_visibility_proof.ps1"
    Evidence    = @("ntn_state", "ntn_beams", "sib19_applied", "f1ap_coordination", "mac_si_rnti")
    Required    = $true
  }

  ntn_connected_mobility_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Simulated AMF/DU/UE coverage for NTN handover, beam hopping, predictive mobility, load balancing, and service-pair lifecycle"
    BuildTarget = "cu_cp_test"
    Regex       = "cu_cp_ntn_mobility_test.*(handover|beam_hopping|predictive|load_balancing|service_pair|paired_access)"
    Required    = $true
  }

  ntn_paging_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Simulated paging coverage for NTN beam-derived TAC, idle/inactive context, direction-aware access, and paired access"
    BuildTarget = "cu_cp_test"
    Regex       = "cu_cp.*paging|cu_cp_ntn_mobility_test.*(paging|inactive|paired_access)"
    Required    = $true
  }

  ntn_resource_repair_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Simulated RNTI, SR/SRS, service-pair uplink resource audit, and repair coverage"
    BuildTarget = "ntn_mobility_test"
    Regex       = "ntn_beam_service_resource_manager.*service_pair|cu_cp_ntn_mobility_test.*(resource|repair|audit|slot)"
    Required    = $true
  }

  ntn_nrppa_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Simulated NRPPa transport, TRP information, positioning information, measurement, activation, and assistance-control coverage"
    BuildTarget = "cu_cp_test"
    Regex       = "nrppa.*|cu_cp.*nrppa|f1ap_cu.*positioning"
    Required    = $false
  }

  ntn_cli_observability_sim = @{
    Type        = "ctest_filter"
    Purpose     = "CLI coverage for ntn_state, ntn_beams, ntn_ues, ntn_diagnose, and ntn_repair"
    BuildTarget = "cu_cp_unit_config_test"
    Regex       = "cu_cp_unit_config.*ntn_(state|beams|ues|diagnose|repair)"
    Required    = $true
  }
}
```

- [ ] **Step 3: Verify the registry parses**

Run:

```powershell
powershell -NoProfile -Command "Import-PowerShellDataFile D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\ntn_system_suite\scenarios.psd1 | Out-String"
```

Expected: scenario names are printed without parse errors.

---

### Task 2: Add Orchestrator Skeleton

**Files:**
- Create: `run_artifacts/srsran_runtime_capture/split/run_ntn_system_suite.ps1`

- [ ] **Step 1: Add the script parameters and shared helpers**

Create `run_artifacts/srsran_runtime_capture/split/run_ntn_system_suite.ps1` with these top-level pieces:

```powershell
$ErrorActionPreference = "Stop"

param(
  [string[]]$Scenario = @("baseline_attach_ping", "ntn_runtime_sib19_visibility", "ntn_connected_mobility_sim", "ntn_paging_sim", "ntn_resource_repair_sim", "ntn_cli_observability_sim"),
  [ValidateSet("live", "sim", "all")]
  [string]$Mode = "all",
  [switch]$Build,
  [int]$TimeoutSeconds = 900,
  [string]$OutputRoot = ""
)

$SplitWin = $PSScriptRoot
$RepoWin = (Resolve-Path (Join-Path $SplitWin "..\..\..")).Path
$ScenarioFile = Join-Path $SplitWin "ntn_system_suite\scenarios.psd1"
$Timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
  $OutputRoot = Join-Path $SplitWin "log_exports\CUCP-075-$Timestamp"
}

function Convert-ToWslPath {
  param([Parameter(Mandatory = $true)][string]$Path)
  if (Test-Path $Path) {
    $resolved = (Resolve-Path $Path).Path
  } else {
    $resolved = [System.IO.Path]::GetFullPath($Path)
  }
  if ($resolved -notmatch "^([A-Za-z]):\\(.*)$") {
    throw "Cannot convert path to WSL form: $resolved"
  }
  $drive = $Matches[1].ToLowerInvariant()
  $tail = $Matches[2] -replace "\\", "/"
  return "/mnt/$drive/$tail"
}

function Invoke-WslBash {
  param([Parameter(Mandatory = $true)][string]$Command)
  $output = & wsl.exe -e bash -lc $Command
  $exitCode = $LASTEXITCODE
  if ($exitCode -ne 0) {
    $text = ($output | Out-String).Trim()
    throw "WSL command failed with exit code $exitCode.`n$text"
  }
  return $output
}

function Stop-SplitProcesses {
  Invoke-WslBash "pkill -TERM srsue 2>/dev/null || true; pkill -TERM srsdu 2>/dev/null || true; pkill -TERM srscuup 2>/dev/null || true; pkill -TERM srscucp 2>/dev/null || true; pkill -TERM tcpdump 2>/dev/null || true; sleep 2; pkill -KILL srsue 2>/dev/null || true; pkill -KILL srsdu 2>/dev/null || true; pkill -KILL srscuup 2>/dev/null || true; pkill -KILL srscucp 2>/dev/null || true; pkill -KILL tcpdump 2>/dev/null || true" | Out-Null
}

function Get-Tshark {
  $default = "C:\Program Files\Wireshark\tshark.exe"
  if (Test-Path $default) {
    return $default
  }
  $found = Get-Command tshark.exe -ErrorAction SilentlyContinue
  if ($null -ne $found) {
    return $found.Source
  }
  return ""
}
```

- [ ] **Step 2: Add report helpers**

Add these functions below the shared helpers:

```powershell
function New-ScenarioResult {
  param(
    [string]$Name,
    [string]$Kind,
    [string]$Status,
    [string]$Reason,
    [string]$ArtifactPath
  )
  return [ordered]@{
    name     = $Name
    kind     = $Kind
    status   = $Status
    reason   = $Reason
    artifact = $ArtifactPath
  }
}

function Write-Summary {
  param(
    [Parameter(Mandatory = $true)][object[]]$Results,
    [Parameter(Mandatory = $true)][string]$Path
  )

  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add("# CUCP-075 NTN System Test Suite Summary")
  $lines.Add("")
  $lines.Add("- Generated: $(Get-Date -Format o)")
  $lines.Add("- Scope: live Open5GS/srsUE split checks plus simulated CU-CP/F1AP/NGAP CTest checks.")
  $lines.Add("- Boundary: this does not claim a full custom UE simulator or RF/PHY validation.")
  $lines.Add("")
  $lines.Add("| Scenario | Kind | Status | Reason | Artifact |")
  $lines.Add("| --- | --- | --- | --- | --- |")
  foreach ($result in $Results) {
    $lines.Add("| $($result.name) | $($result.kind) | $($result.status) | $($result.reason -replace '\|','/') | $($result.artifact) |")
  }
  Set-Content -Path $Path -Encoding UTF8 -Value $lines
}
```

- [ ] **Step 3: Verify script syntax**

Run:

```powershell
powershell -NoProfile -Command "$null = [System.Management.Automation.PSParser]::Tokenize((Get-Content -Raw D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1), [ref]$null); 'syntax-ok'"
```

Expected: `syntax-ok`.

---

### Task 3: Implement Live Scenario Runner

**Files:**
- Modify: `run_artifacts/srsran_runtime_capture/split/run_ntn_system_suite.ps1`

- [ ] **Step 1: Add live scenario execution**

Add this function:

```powershell
function Invoke-LiveScenario {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][hashtable]$Definition,
    [Parameter(Mandatory = $true)][string]$OutputDir
  )

  $scriptPath = Join-Path $SplitWin $Definition.Command
  if (-not (Test-Path $scriptPath)) {
    return New-ScenarioResult -Name $Name -Kind "live" -Status "fail" -Reason "script not found: $scriptPath" -ArtifactPath $OutputDir
  }

  $scenarioOut = Join-Path $OutputDir $Name
  New-Item -ItemType Directory -Force -Path $scenarioOut | Out-Null
  $stdoutPath = Join-Path $scenarioOut "stdout.log"

  try {
    Stop-SplitProcesses
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath *> $stdoutPath
    $exitCode = $LASTEXITCODE
    Stop-SplitProcesses
    if ($exitCode -ne 0) {
      return New-ScenarioResult -Name $Name -Kind "live" -Status "fail" -Reason "script exit code $exitCode" -ArtifactPath $scenarioOut
    }
    return New-ScenarioResult -Name $Name -Kind "live" -Status "pass" -Reason "live script completed" -ArtifactPath $scenarioOut
  } catch {
    try { Stop-SplitProcesses } catch {}
    return New-ScenarioResult -Name $Name -Kind "live" -Status "fail" -Reason $_.Exception.Message -ArtifactPath $scenarioOut
  }
}
```

- [ ] **Step 2: Add live pcap evidence check**

Add this function:

```powershell
function Test-LivePcapEvidence {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][hashtable]$Definition,
    [Parameter(Mandatory = $true)][string]$OutputDir
  )

  $tshark = Get-Tshark
  if ([string]::IsNullOrWhiteSpace($tshark)) {
    return New-ScenarioResult -Name "$Name.pcap" -Kind "pcap" -Status "warn" -Reason "tshark not found" -ArtifactPath ""
  }

  $pcap = Join-Path $SplitWin "split_live_sctp_latest.pcap"
  if (-not (Test-Path $pcap)) {
    return New-ScenarioResult -Name "$Name.pcap" -Kind "pcap" -Status "fail" -Reason "pcap not found" -ArtifactPath $pcap
  }

  $protocols = (& $tshark -r $pcap -q -z io,phs 2>$null | Out-String).ToLowerInvariant()
  $missing = New-Object System.Collections.Generic.List[string]
  foreach ($item in $Definition.Evidence) {
    if ($item -in @("ngap", "e1ap", "f1ap", "nr-rrc", "gtp", "icmp") -and -not $protocols.Contains($item)) {
      $missing.Add($item)
    }
  }
  if ($missing.Count -ne 0) {
    return New-ScenarioResult -Name "$Name.pcap" -Kind "pcap" -Status "fail" -Reason "missing protocol evidence: $($missing -join ',')" -ArtifactPath $pcap
  }
  return New-ScenarioResult -Name "$Name.pcap" -Kind "pcap" -Status "pass" -Reason "required pcap protocols found" -ArtifactPath $pcap
}
```

- [ ] **Step 3: Run the baseline live scenario only**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Scenario baseline_attach_ping -Mode live
```

Expected:

- The script starts Open5GS/srsUE/split through `run_split_demo.ps1`.
- It writes an output directory under `run_artifacts\srsran_runtime_capture\split\log_exports\CUCP-075-*`.
- Summary marks `baseline_attach_ping` as `pass` if the existing script succeeds.
- If the live stack fails, summary marks the failed stage and exports logs.

---

### Task 4: Implement Simulated Control-Plane Runner

**Files:**
- Modify: `run_artifacts/srsran_runtime_capture/split/run_ntn_system_suite.ps1`

- [ ] **Step 1: Add focused build helper**

Add this function:

```powershell
function Invoke-FocusedBuild {
  param([Parameter(Mandatory = $true)][string[]]$Targets)

  if (-not $Build) {
    return "build skipped by default"
  }
  $targetText = ($Targets | Sort-Object -Unique) -join " "
  $cmd = "cd /mnt/d/code/srsRAN_Project-main && cmake --build build/ai-clean --target $targetText -j8"
  $output = Invoke-WslBash $cmd
  return ($output | Out-String)
}
```

- [ ] **Step 2: Add CTest scenario execution**

Add this function:

```powershell
function Invoke-CtestScenario {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][hashtable]$Definition,
    [Parameter(Mandatory = $true)][string]$OutputDir
  )

  $scenarioOut = Join-Path $OutputDir $Name
  New-Item -ItemType Directory -Force -Path $scenarioOut | Out-Null
  $stdoutPath = Join-Path $scenarioOut "ctest.log"

  try {
    if ($Build) {
      Invoke-FocusedBuild -Targets @($Definition.BuildTarget) | Set-Content -Path (Join-Path $scenarioOut "build.log") -Encoding UTF8
    }
    $regex = $Definition.Regex.Replace("'", "'\''")
    $cmd = "cd /mnt/d/code/srsRAN_Project-main && ctest --test-dir build/ai-clean -R '$regex' --output-on-failure"
    $output = Invoke-WslBash $cmd
    Set-Content -Path $stdoutPath -Encoding UTF8 -Value ($output | Out-String)
    return New-ScenarioResult -Name $Name -Kind "sim" -Status "pass" -Reason "focused ctest passed" -ArtifactPath $scenarioOut
  } catch {
    Set-Content -Path $stdoutPath -Encoding UTF8 -Value $_.Exception.Message
    return New-ScenarioResult -Name $Name -Kind "sim" -Status "fail" -Reason $_.Exception.Message -ArtifactPath $scenarioOut
  }
}
```

- [ ] **Step 3: Run one simulated scenario**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Scenario ntn_cli_observability_sim -Mode sim
```

Expected:

- No live split processes start.
- `ctest` runs a focused `cu_cp_unit_config.*ntn_(state|beams|ues|diagnose|repair)` filter.
- The summary records `pass` or the exact CTest failure.

---

### Task 5: Wire Main Scenario Loop

**Files:**
- Modify: `run_artifacts/srsran_runtime_capture/split/run_ntn_system_suite.ps1`

- [ ] **Step 1: Add registry loading and mode filtering**

Add this main block at the end of the script:

```powershell
if (-not (Test-Path $ScenarioFile)) {
  throw "Scenario registry not found: $ScenarioFile"
}

$registry = Import-PowerShellDataFile $ScenarioFile
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

$results = New-Object System.Collections.Generic.List[object]
foreach ($name in $Scenario) {
  if (-not $registry.ContainsKey($name)) {
    $results.Add((New-ScenarioResult -Name $name -Kind "unknown" -Status "fail" -Reason "scenario not registered" -ArtifactPath $OutputRoot))
    continue
  }

  $definition = $registry[$name]
  $type = [string]$definition.Type
  if ($Mode -eq "live" -and $type -ne "live_script") {
    continue
  }
  if ($Mode -eq "sim" -and $type -ne "ctest_filter") {
    continue
  }

  Write-Host "Running scenario: $name ($($definition.Purpose))"
  if ($type -eq "live_script") {
    $liveResult = Invoke-LiveScenario -Name $name -Definition $definition -OutputDir $OutputRoot
    $results.Add($liveResult)
    $results.Add((Test-LivePcapEvidence -Name $name -Definition $definition -OutputDir $OutputRoot))
  } elseif ($type -eq "ctest_filter") {
    $results.Add((Invoke-CtestScenario -Name $name -Definition $definition -OutputDir $OutputRoot))
  } else {
    $results.Add((New-ScenarioResult -Name $name -Kind $type -Status "warn" -Reason "scenario type not executable in v1" -ArtifactPath $OutputRoot))
  }
}

$summaryPath = Join-Path $OutputRoot "ntn_system_suite_summary.md"
Write-Summary -Results $results.ToArray() -Path $summaryPath
Write-Host "Summary: $summaryPath"

$failedRequired = $false
foreach ($result in $results) {
  if ($result.status -eq "fail") {
    $failedRequired = $true
  }
}
if ($failedRequired) {
  exit 1
}
```

- [ ] **Step 2: Run all simulated scenarios**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Mode sim
```

Expected:

- The script runs focused CTest filters only.
- Summary contains rows for NTN connected mobility, paging, resource repair, NRPPa if selected, and CLI observability.
- No split live processes are left running.

---

### Task 6: Add Artifact Export and Process Cleanup Checks

**Files:**
- Modify: `run_artifacts/srsran_runtime_capture/split/run_ntn_system_suite.ps1`

- [ ] **Step 1: Add artifact copy helper**

Add this function:

```powershell
function Copy-LiveArtifacts {
  param([Parameter(Mandatory = $true)][string]$OutputDir)

  $export = Join-Path $OutputDir "live_artifacts"
  New-Item -ItemType Directory -Force -Path $export | Out-Null
  $files = @(
    "split_live_sctp_latest.pcap",
    "log_exports"
  )
  foreach ($item in $files) {
    $src = Join-Path $SplitWin $item
    if (Test-Path $src) {
      Copy-Item -Path $src -Destination $export -Recurse -Force -ErrorAction SilentlyContinue
    }
  }

  $exportWsl = Convert-ToWslPath $export
  Invoke-WslBash "cp -f /tmp/split_cu_cp_stdout.log /tmp/split_cu_up_stdout.log /tmp/split_du_stdout.log /tmp/split_ue_stdout.log /tmp/split_ue_ping.log '$exportWsl/' 2>/dev/null || true" | Out-Null
  return $export
}
```

- [ ] **Step 2: Add residual process assertion**

Add this function:

```powershell
function Test-NoResidualProcesses {
  $residual = Invoke-WslBash "pgrep -a srscucp || true; pgrep -a srscuup || true; pgrep -a srsdu || true; pgrep -a srsue || true; pgrep -a tcpdump || true"
  $text = ($residual | Out-String).Trim()
  if ($text.Length -ne 0) {
    return New-ScenarioResult -Name "cleanup" -Kind "process" -Status "fail" -Reason $text -ArtifactPath ""
  }
  return New-ScenarioResult -Name "cleanup" -Kind "process" -Status "pass" -Reason "no residual split process" -ArtifactPath ""
}
```

- [ ] **Step 3: Call cleanup checks before writing summary**

Before `Write-Summary`, add:

```powershell
try {
  $artifactDir = Copy-LiveArtifacts -OutputDir $OutputRoot
  $results.Add((New-ScenarioResult -Name "artifact_export" -Kind "artifact" -Status "pass" -Reason "artifacts copied" -ArtifactPath $artifactDir))
} catch {
  $results.Add((New-ScenarioResult -Name "artifact_export" -Kind "artifact" -Status "warn" -Reason $_.Exception.Message -ArtifactPath $OutputRoot))
}
$results.Add((Test-NoResidualProcesses))
```

- [ ] **Step 4: Run baseline and confirm cleanup**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Scenario baseline_attach_ping -Mode live
```

Expected:

- `cleanup` row is `pass`.
- `artifact_export` row is `pass` or `warn` with a clear reason.
- No `srscucp`, `srscuup`, `srsdu`, `srsue`, or `tcpdump` process remains.

---

### Task 7: Add User Documentation

**Files:**
- Create: `run_artifacts/srsran_runtime_capture/split/ntn_system_suite/README.md`

- [ ] **Step 1: Add README**

Create `run_artifacts/srsran_runtime_capture/split/ntn_system_suite/README.md`:

```markdown
# NTN System Test Suite

This suite runs two kinds of checks:

1. Live end-to-end checks with Open5GS, srsUE, CU-CP, CU-UP, and DU.
2. Simulated control-plane checks with existing CTest fake AMF/DU/UE coverage.

The live checks prove that the split stack can attach a UE, create a PDU session, pass GTP-U traffic, and expose pcap/log evidence. The simulated checks cover deeper NTN control-plane behavior that current srsUE cannot drive directly, such as paging policy, connected mobility, service-pair behavior, resource audit/repair, and NRPPa.

## Common Commands

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

Run with focused builds first:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Mode sim -Build
```

## Output

Each run writes a directory under:

```text
D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\log_exports\
```

The main report is:

```text
ntn_system_suite_summary.md
```

## Interpretation

- `live` means Open5GS and srsUE were used.
- `sim` means CTest drove existing fake AMF/DU/UE control-plane tests.
- `pcap` means `tshark` checked captured protocol evidence.
- `process` means the suite verified that no split demo process was left running.

This suite does not claim RF/PHY validation and does not replace a commercial UE simulator.
```

- [ ] **Step 2: Verify the README paths are valid**

Run:

```powershell
Test-Path D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\ntn_system_suite\README.md
```

Expected: `True`.

---

### Task 8: Focused Validation and Handoff

**Files:**
- Validate:
  - `run_artifacts/srsran_runtime_capture/split/run_ntn_system_suite.ps1`
  - `run_artifacts/srsran_runtime_capture/split/ntn_system_suite/scenarios.psd1`
  - `run_artifacts/srsran_runtime_capture/split/ntn_system_suite/README.md`

- [ ] **Step 1: Run PowerShell syntax and registry checks**

Run:

```powershell
powershell -NoProfile -Command "$errors = $null; [System.Management.Automation.PSParser]::Tokenize((Get-Content -Raw D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1), [ref]$errors) > $null; if ($errors) { $errors; exit 1 } else { 'syntax-ok' }"
powershell -NoProfile -Command "Import-PowerShellDataFile D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\ntn_system_suite\scenarios.psd1 | Out-String"
```

Expected:

- `syntax-ok`
- Scenario table loads.

- [ ] **Step 2: Run simulated suite**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Mode sim
```

Expected:

- Focused CTest checks run.
- Summary report is generated.
- Failed tests are reported with exact CTest output.

- [ ] **Step 3: Run live baseline only**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Scenario baseline_attach_ping -Mode live
```

Expected:

- Open5GS/srsUE/split stack path runs through the existing script.
- The suite exports logs and pcap.
- Cleanup check passes.

- [ ] **Step 4: Run live NTN SIB19 visibility scenario**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File D:\code\srsRAN_Project-main\run_artifacts\srsran_runtime_capture\split\run_ntn_system_suite.ps1 -Scenario ntn_runtime_sib19_visibility -Mode live
```

Expected:

- Existing NTN SIB19 proof script runs.
- Summary contains SIB19 runtime, F1AP coordination, and MAC SI-RNTI evidence.
- If srsUE attach fails, report clearly states the failing stage and does not claim UE SIB19 decode.

- [ ] **Step 5: Run git checks without staging unrelated files**

Run:

```powershell
git diff --check -- run_artifacts/srsran_runtime_capture/split/run_ntn_system_suite.ps1 run_artifacts/srsran_runtime_capture/split/ntn_system_suite/scenarios.psd1 run_artifacts/srsran_runtime_capture/split/ntn_system_suite/README.md
git status --short
```

Expected:

- `git diff --check` has no output.
- `git status --short` shows only expected CUCP-075 files as newly touched by this task, plus pre-existing dirty files that must not be staged.

---

## Self-Review

- Spec coverage: live UE/core/gNB, NTN runtime, pcap evidence, simulated AMF/DU/UE control-plane behavior, cleanup, and reports are covered by tasks.
- Reserved-word scan: the plan contains no unfinished marker words or unspecified implementation steps.
- Type consistency: scenario keys in `scenarios.psd1` match script parameter defaults and task commands.
- Scope check: v1 is intentionally script-first and does not implement a custom UE/core protocol stack.
