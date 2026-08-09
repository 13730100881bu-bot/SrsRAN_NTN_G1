param(
  [string[]]$Scenario = @(
    "baseline_attach_ping",
    "ntn_runtime_sib19_visibility",
    "ntn_connected_mobility_sim",
    "ntn_paging_sim",
    "ntn_resource_repair_sim",
    "ntn_rnti_retirement_sim",
    "ntn_ue_slot_recovery_sim",
    "ntn_cli_observability_sim"
  ),
  [ValidateSet("live", "sim", "all")]
  [string]$Mode = "all",
  [switch]$Build,
  [int]$TimeoutSeconds = 900,
  [string]$OutputRoot = ""
)

$ErrorActionPreference = "Stop"

$SplitWin = $PSScriptRoot
$RepoWin = (Resolve-Path (Join-Path $SplitWin "..\..\..")).Path
$ScenarioFile = Join-Path $SplitWin "ntn_system_suite\scenarios.psd1"
$Timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
  $OutputRoot = Join-Path $SplitWin "log_exports\CUCP-075-$Timestamp"
}

function Convert-ToWslPath {
  param([Parameter(Mandatory = $true)][string]$Path)

  if (Test-Path -LiteralPath $Path) {
    $resolved = (Resolve-Path -LiteralPath $Path).Path
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

function Format-BashSingleQuoted {
  param([Parameter(Mandatory = $true)][string]$Value)

  return "'" + ($Value -replace "'", "'\''") + "'"
}

function Invoke-ProcessCapture {
  param(
    [Parameter(Mandatory = $true)][string]$FilePath,
    [Parameter(Mandatory = $true)][string[]]$ArgumentList,
    [Parameter(Mandatory = $true)][string]$StdoutPath,
    [Parameter(Mandatory = $true)][int]$TimeoutSeconds
  )

  $stderrPath = "$StdoutPath.stderr"
  $process = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -NoNewWindow -PassThru -RedirectStandardOutput $StdoutPath -RedirectStandardError $stderrPath
  if (-not $process.WaitForExit([Math]::Max(1, $TimeoutSeconds) * 1000)) {
    try { $process.Kill() } catch {}
    throw "process timed out after $TimeoutSeconds seconds: $FilePath $($ArgumentList -join ' ')"
  }
  if (Test-Path -LiteralPath $stderrPath) {
    $stderr = Get-Content -LiteralPath $stderrPath -Raw -ErrorAction SilentlyContinue
    if (-not [string]::IsNullOrWhiteSpace($stderr)) {
      Add-Content -LiteralPath $StdoutPath -Encoding UTF8 -Value ""
      Add-Content -LiteralPath $StdoutPath -Encoding UTF8 -Value "----- stderr -----"
      Add-Content -LiteralPath $StdoutPath -Encoding UTF8 -Value $stderr
    }
    Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
  }
  return $process.ExitCode
}

function Invoke-WslBash {
  param(
    [Parameter(Mandatory = $true)][string]$Command,
    [int]$TimeoutSeconds = 900
  )

  $startedAt = Get-Date
  $job = Start-Job -ScriptBlock {
    param([string]$InnerCommand)

    $output = & wsl.exe -e bash -lc $InnerCommand 2>&1
    [pscustomobject]@{
      ExitCode = $LASTEXITCODE
      Output   = ($output | Out-String)
    }
  } -ArgumentList $Command

  $completed = Wait-Job -Job $job -Timeout ([Math]::Max(1, $TimeoutSeconds))
  if ($null -eq $completed) {
    Stop-Job -Job $job -ErrorAction SilentlyContinue
    Remove-Job -Job $job -Force -ErrorAction SilentlyContinue
    Get-Process -Name wsl -ErrorAction SilentlyContinue |
      Where-Object { $_.StartTime -ge $startedAt } |
      Stop-Process -Force -ErrorAction SilentlyContinue
    throw "WSL command timed out after $TimeoutSeconds seconds: $Command"
  }

  $records = Receive-Job -Job $job
  $jobState = $job.State
  Remove-Job -Job $job -Force -ErrorAction SilentlyContinue
  if ($jobState -ne "Completed") {
    throw "WSL command job failed with state $jobState`: $Command"
  }
  $record = @($records)[-1]
  if ($record.ExitCode -ne 0) {
    $text = ([string]$record.Output).Trim()
    throw "WSL command failed with exit code $($record.ExitCode).`n$text"
  }
  return ([string]$record.Output).TrimEnd()
}

function Stop-SplitProcesses {
  Write-Host "cleanup: stopping srscucp/srscuup/srsdu/srsue/tcpdump"
  Invoke-WslBash -TimeoutSeconds 30 -Command "pkill -TERM srsue 2>/dev/null || true; pkill -TERM srsdu 2>/dev/null || true; pkill -TERM srscuup 2>/dev/null || true; pkill -TERM srscucp 2>/dev/null || true; pkill -TERM tcpdump 2>/dev/null || true; sleep 2; pkill -KILL srsue 2>/dev/null || true; pkill -KILL srsdu 2>/dev/null || true; pkill -KILL srscuup 2>/dev/null || true; pkill -KILL srscucp 2>/dev/null || true; pkill -KILL tcpdump 2>/dev/null || true" | Out-Null
}

function Get-Tshark {
  $default = "C:\Program Files\Wireshark\tshark.exe"
  if (Test-Path -LiteralPath $default) {
    return $default
  }
  $found = Get-Command tshark.exe -ErrorAction SilentlyContinue
  if ($null -ne $found) {
    return $found.Source
  }
  return ""
}

function New-ScenarioResult {
  param(
    [string]$Name,
    [string]$Kind,
    [string]$Status,
    [string]$Reason,
    [string]$ArtifactPath,
    [bool]$Required = $true
  )

  return [ordered]@{
    name     = $Name
    kind     = $Kind
    status   = $Status
    reason   = $Reason
    artifact = $ArtifactPath
    required = $Required
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
    $reason = ([string]$result.reason) -replace "\|", "/"
    $artifact = ([string]$result.artifact) -replace "\|", "/"
    $lines.Add("| $($result.name) | $($result.kind) | $($result.status) | $reason | $artifact |")
  }
  Set-Content -LiteralPath $Path -Encoding UTF8 -Value $lines
}

function Copy-LiveArtifacts {
  param([Parameter(Mandatory = $true)][string]$OutputDir)

  $export = Join-Path $OutputDir "live_artifacts"
  New-Item -ItemType Directory -Force -Path $export | Out-Null
  $items = @(
    "split_live_sctp_latest.pcap"
  )
  foreach ($item in $items) {
    $src = Join-Path $SplitWin $item
    if (Test-Path -LiteralPath $src) {
      Copy-Item -LiteralPath $src -Destination $export -Recurse -Force -ErrorAction SilentlyContinue
    }
  }

  $proofExport = Join-Path $export "nested_proof_exports"
  New-Item -ItemType Directory -Force -Path $proofExport | Out-Null
  $currentOutputRoot = (Resolve-Path -LiteralPath $OutputDir).Path
  $logExports = Join-Path $SplitWin "log_exports"
  if (Test-Path -LiteralPath $logExports) {
    foreach ($prefix in @("CUCP-058-*", "CUCP-057-*")) {
      $latestProof = Get-ChildItem -LiteralPath $logExports -Directory -Filter $prefix -ErrorAction SilentlyContinue |
        Where-Object { (Resolve-Path -LiteralPath $_.FullName).Path -ne $currentOutputRoot } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
      if ($null -ne $latestProof) {
        $destination = Join-Path $proofExport $latestProof.Name
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Get-ChildItem -LiteralPath $latestProof.FullName -File -ErrorAction SilentlyContinue |
          Copy-Item -Destination $destination -Force -ErrorAction SilentlyContinue
      }
    }
  }

  $exportWsl = Format-BashSingleQuoted -Value (Convert-ToWslPath $export)
  Invoke-WslBash -TimeoutSeconds 30 -Command "cp -f /tmp/split_cu_cp_stdout.log /tmp/split_cu_up_stdout.log /tmp/split_du_stdout.log /tmp/split_ue_stdout.log /tmp/split_ue_ping.log $exportWsl/ 2>/dev/null || true" | Out-Null
  return $export
}

function Get-AsciiFromBinaryFile {
  param([Parameter(Mandatory = $true)][string]$Path)

  if (-not (Test-Path -LiteralPath $Path)) {
    return ""
  }
  $bytes = [System.IO.File]::ReadAllBytes($Path)
  return [System.Text.Encoding]::ASCII.GetString($bytes)
}

function Get-LiveTextEvidence {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$OutputDir
  )

  $roots = New-Object System.Collections.Generic.List[string]
  $scenarioOut = Join-Path $OutputDir $Name
  if (Test-Path -LiteralPath $scenarioOut) {
    $roots.Add($scenarioOut)
  }

  $text = New-Object System.Text.StringBuilder
  foreach ($root in $roots) {
    Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
      Where-Object { $_.Extension -in @(".log", ".txt", ".md", ".out", ".json", ".yml", ".yaml") } |
      ForEach-Object {
        try {
          [void]$text.AppendLine((Get-Content -LiteralPath $_.FullName -Raw -ErrorAction Stop))
        } catch {}
      }
  }
  return $text.ToString()
}

function Test-TextEvidence {
  param(
    [Parameter(Mandatory = $true)][string]$Text,
    [Parameter(Mandatory = $true)][string[]]$Patterns
  )

  foreach ($pattern in $Patterns) {
    if ($Text -match $pattern) {
      return $true
    }
  }
  return $false
}

function Test-NoResidualProcesses {
  $residual = Invoke-WslBash -TimeoutSeconds 30 -Command "printf '__cucp075_process_start__\n'; pgrep -a srscucp || true; pgrep -a srscuup || true; pgrep -a srsdu || true; pgrep -a srsue || true; pgrep -a tcpdump || true; printf '__cucp075_process_end__\n'"
  $match = [regex]::Match([string]$residual, "__cucp075_process_start__\s*(?<body>.*?)\s*__cucp075_process_end__", [System.Text.RegularExpressions.RegexOptions]::Singleline)
  if ($match.Success) {
    $text = $match.Groups["body"].Value.Trim()
  } else {
    $text = ([string]$residual).Trim()
  }
  if ($text.Length -ne 0) {
    return New-ScenarioResult -Name "cleanup" -Kind "process" -Status "fail" -Reason $text -ArtifactPath ""
  }
  return New-ScenarioResult -Name "cleanup" -Kind "process" -Status "pass" -Reason "no residual split process" -ArtifactPath ""
}

function Invoke-LiveScenario {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][hashtable]$Definition,
    [Parameter(Mandatory = $true)][string]$OutputDir
  )

  $scriptPath = Join-Path $SplitWin $Definition.Command
  if (-not (Test-Path -LiteralPath $scriptPath)) {
    return New-ScenarioResult -Name $Name -Kind "live" -Status "fail" -Reason "script not found: $scriptPath" -ArtifactPath $OutputDir
  }

  $scenarioOut = Join-Path $OutputDir $Name
  New-Item -ItemType Directory -Force -Path $scenarioOut | Out-Null
  $stdoutPath = Join-Path $scenarioOut "stdout.log"
  $stampPath = Join-Path $scenarioOut "started_at.txt"
  $startedAt = Get-Date

  try {
    Set-Content -LiteralPath $stampPath -Encoding ASCII -Value $startedAt.ToString("o")
    Stop-SplitProcesses
    Remove-Item -LiteralPath (Join-Path $SplitWin "split_live_sctp_latest.pcap") -Force -ErrorAction SilentlyContinue
    $exitCode = Invoke-ProcessCapture -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $scriptPath) -StdoutPath $stdoutPath -TimeoutSeconds $TimeoutSeconds
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

function Test-LivePcapEvidence {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][hashtable]$Definition,
    [Parameter(Mandatory = $true)][string]$OutputDir
  )

  $tshark = Get-Tshark
  $pcap = Join-Path $SplitWin "split_live_sctp_latest.pcap"
  if (-not (Test-Path -LiteralPath $pcap)) {
    return New-ScenarioResult -Name "$Name.pcap" -Kind "pcap" -Status "fail" -Reason "pcap not found" -ArtifactPath $pcap
  }
  $stampPath = Join-Path (Join-Path $OutputDir $Name) "started_at.txt"
  if (Test-Path -LiteralPath $stampPath) {
    $startedAtText = Get-Content -LiteralPath $stampPath -Raw -ErrorAction SilentlyContinue
    $startedAt = [datetime]::MinValue
    if ([datetime]::TryParse($startedAtText, [ref]$startedAt)) {
      $pcapTime = (Get-Item -LiteralPath $pcap).LastWriteTime
      if ($pcapTime -lt $startedAt) {
        return New-ScenarioResult -Name "$Name.pcap" -Kind "pcap" -Status "fail" -Reason "pcap is older than scenario start" -ArtifactPath $pcap
      }
    }
  }

  $protocols = ""
  $pcapDetail = ""
  $pcapAscii = ""
  if (Test-Path -LiteralPath $pcap) {
    $pcapAscii = Get-AsciiFromBinaryFile -Path $pcap
  }
  if (-not [string]::IsNullOrWhiteSpace($tshark) -and (Test-Path -LiteralPath $pcap)) {
    $protocols = (& $tshark -r $pcap -q -z io,phs 2>$null | Out-String).ToLowerInvariant()
    $pcapDetail = (& $tshark -r $pcap -V 2>$null | Out-String)
  }

  $missing = New-Object System.Collections.Generic.List[string]
  $warnings = New-Object System.Collections.Generic.List[string]
  $textEvidence = Get-LiveTextEvidence -Name $Name -OutputDir $OutputDir
  foreach ($item in $Definition.Evidence) {
    switch ($item) {
      { $_ -in @("ngap", "e1ap", "f1ap", "nr-rrc", "gtp", "icmp") } {
        if ([string]::IsNullOrWhiteSpace($tshark)) {
          $warnings.Add("$item(tshark not found)")
        } elseif (-not $protocols.Contains($item)) {
          $missing.Add($item)
        }
      }
      "f1ap_coordination" {
        $combinedPcap = "$pcapAscii`n$pcapDetail"
        $hasMarkers = ($combinedPcap -match "SIB19U01" -and $combinedPcap -match "SIB19R01")
        $hasResourceCoordination = $combinedPcap -match "Resource\s+Coordination"
        if (-not ($hasMarkers -or $hasResourceCoordination)) {
          $missing.Add("f1ap_coordination")
        }
      }
      "ntn_state" {
        if (-not (Test-TextEvidence -Text $textEvidence -Patterns @("ntn_state", "NTN state"))) {
          $missing.Add("ntn_state")
        }
      }
      "ntn_beams" {
        if (-not (Test-TextEvidence -Text $textEvidence -Patterns @("ntn_beams", "NTN beams"))) {
          $missing.Add("ntn_beams")
        }
      }
      "sib19_applied" {
        if (-not (Test-TextEvidence -Text $textEvidence -Patterns @("sib19_applied", "SIB19.*appl", "appl.*SIB19"))) {
          $missing.Add("sib19_applied")
        }
      }
      "mac_si_rnti" {
        $combinedMacText = "$textEvidence`n$pcapAscii`n$pcapDetail"
        if (-not (Test-TextEvidence -Text $combinedMacText -Patterns @("SI-RNTI", "BCCH-DL-SCH", "MAC SI proof"))) {
          $missing.Add("mac_si_rnti")
        }
      }
    }
  }
  if ($missing.Count -ne 0) {
    return New-ScenarioResult -Name "$Name.pcap" -Kind "pcap" -Status "fail" -Reason "missing protocol evidence: $($missing -join ',')" -ArtifactPath $pcap
  }
  if ($warnings.Count -ne 0) {
    return New-ScenarioResult -Name "$Name.pcap" -Kind "pcap" -Status "warn" -Reason "pcap protocol checks skipped: $($warnings -join ',')" -ArtifactPath $pcap
  }
  return New-ScenarioResult -Name "$Name.pcap" -Kind "pcap" -Status "pass" -Reason "required pcap protocols found" -ArtifactPath $pcap
}

function Invoke-FocusedBuild {
  param([Parameter(Mandatory = $true)][string[]]$Targets)

  if (-not $Build) {
    return "build skipped by default"
  }
  $targetText = ($Targets | Sort-Object -Unique) -join " "
  $repoWsl = Format-BashSingleQuoted -Value (Convert-ToWslPath $RepoWin)
  $cmd = "cd $repoWsl && cmake --build build/ai-clean --target $targetText -j8"
  return Invoke-WslBash -TimeoutSeconds $TimeoutSeconds -Command $cmd
}

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
    New-Item -ItemType Directory -Force -Path $scenarioOut | Out-Null
    if ($Build) {
      Invoke-FocusedBuild -Targets @($Definition.BuildTarget) | Set-Content -LiteralPath (Join-Path $scenarioOut "build.log") -Encoding UTF8
    }
    $repoWsl = Format-BashSingleQuoted -Value (Convert-ToWslPath $RepoWin)
    $regexValues = if ($Definition.ContainsKey("RegexGroups")) {
      @($Definition.RegexGroups)
    } else {
      @([string]$Definition.Regex)
    }
    $outputs = [System.Collections.Generic.List[string]]::new()
    foreach ($regexValue in $regexValues) {
      if ([string]::IsNullOrWhiteSpace([string]$regexValue)) {
        throw "Scenario '$Name' contains an empty CTest filter"
      }
      $regex = Format-BashSingleQuoted -Value ([string]$regexValue)
      $cmd = "cd $repoWsl && ctest --test-dir build/ai-clean -R $regex --output-on-failure --no-tests=error"
      $outputs.Add("=== ctest group: $regexValue ===")
      $outputs.Add([string](Invoke-WslBash -TimeoutSeconds $TimeoutSeconds -Command $cmd))
    }
    Set-Content -LiteralPath $stdoutPath -Encoding UTF8 -Value $outputs
    return New-ScenarioResult -Name $Name -Kind "sim" -Status "pass" -Reason "focused ctest groups passed ($($regexValues.Count))" -ArtifactPath $scenarioOut
  } catch {
    New-Item -ItemType Directory -Force -Path $scenarioOut | Out-Null
    Set-Content -LiteralPath $stdoutPath -Encoding UTF8 -Value $_.Exception.Message
    return New-ScenarioResult -Name $Name -Kind "sim" -Status "fail" -Reason $_.Exception.Message -ArtifactPath $scenarioOut
  }
}

if (-not (Test-Path -LiteralPath $ScenarioFile)) {
  throw "Scenario registry not found: $ScenarioFile"
}

$registry = Import-PowerShellDataFile -LiteralPath $ScenarioFile
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

$results = New-Object System.Collections.Generic.List[object]
foreach ($name in $Scenario) {
  if (-not $registry.ContainsKey($name)) {
    $results.Add((New-ScenarioResult -Name $name -Kind "unknown" -Status "fail" -Reason "scenario not registered" -ArtifactPath $OutputRoot -Required $true))
    continue
  }

  $definition = $registry[$name]
  $type = [string]$definition.Type
  $required = [bool]$definition.Required
  if ($Mode -eq "live" -and $type -ne "live_script") {
    continue
  }
  if ($Mode -eq "sim" -and $type -ne "ctest_filter") {
    continue
  }

  Write-Host "Running scenario: $name ($($definition.Purpose))"
  if ($type -eq "live_script") {
    $liveResult = Invoke-LiveScenario -Name $name -Definition $definition -OutputDir $OutputRoot
    $liveResult.required = $required
    $results.Add($liveResult)
    if ($liveResult.status -eq "pass") {
      $pcapResult = Test-LivePcapEvidence -Name $name -Definition $definition -OutputDir $OutputRoot
      $pcapResult.required = $required
      $results.Add($pcapResult)
    } else {
      $results.Add((New-ScenarioResult -Name "$name.pcap" -Kind "pcap" -Status "warn" -Reason "skipped because live scenario failed" -ArtifactPath $OutputRoot -Required $false))
    }
  } elseif ($type -eq "ctest_filter") {
    $ctestResult = Invoke-CtestScenario -Name $name -Definition $definition -OutputDir $OutputRoot
    $ctestResult.required = $required
    $results.Add($ctestResult)
  } else {
    $results.Add((New-ScenarioResult -Name $name -Kind $type -Status "warn" -Reason "scenario type not executable in v1" -ArtifactPath $OutputRoot -Required $required))
  }
}

if ($results.Count -eq 0) {
  $results.Add((New-ScenarioResult -Name "selection" -Kind "suite" -Status "warn" -Reason "no scenarios selected for mode $Mode" -ArtifactPath $OutputRoot))
}

try {
  $hasLive = $false
  foreach ($result in $results) {
    if ($result.kind -eq "live" -or $result.kind -eq "pcap") {
      $hasLive = $true
    }
  }
  if ($hasLive) {
    $artifactDir = Copy-LiveArtifacts -OutputDir $OutputRoot
    $results.Add((New-ScenarioResult -Name "artifact_export" -Kind "artifact" -Status "pass" -Reason "artifacts copied" -ArtifactPath $artifactDir))
  } else {
    $results.Add((New-ScenarioResult -Name "artifact_export" -Kind "artifact" -Status "warn" -Reason "no live scenarios selected" -ArtifactPath $OutputRoot))
  }
} catch {
  $results.Add((New-ScenarioResult -Name "artifact_export" -Kind "artifact" -Status "warn" -Reason $_.Exception.Message -ArtifactPath $OutputRoot))
}

try {
  $results.Add((Test-NoResidualProcesses))
} catch {
  $results.Add((New-ScenarioResult -Name "cleanup" -Kind "process" -Status "warn" -Reason $_.Exception.Message -ArtifactPath ""))
}

$summaryPath = Join-Path $OutputRoot "ntn_system_suite_summary.md"
Write-Summary -Results $results.ToArray() -Path $summaryPath
Write-Host "Summary: $summaryPath"

$failed = $false
foreach ($result in $results) {
  if ($result.status -eq "fail" -and ($result.required -or $result.kind -eq "unknown")) {
    $failed = $true
  }
}
if ($failed) {
  exit 1
}
