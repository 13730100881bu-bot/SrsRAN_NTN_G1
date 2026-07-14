$ErrorActionPreference = "Stop"

$SplitWin = $PSScriptRoot
$RepoWin = (Resolve-Path (Join-Path $SplitWin "..\..\..")).Path
$PcapPath = Join-Path $SplitWin "split_live_sctp_latest.pcap"
$DefaultTshark = "C:\Program Files\Wireshark\tshark.exe"
$Timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$ExportDirWin = Join-Path $SplitWin "log_exports\CUCP-058-$Timestamp"
$MacPcapExportWin = Join-Path $ExportDirWin "split_du_ntn_sib19_proof_mac.pcap"
$MacPcapSourceWsl = "/tmp/split_du_ntn_sib19_proof_mac.pcap"

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

function Normalize-ProofText {
  param([AllowNull()][object]$Text)

  if ($null -eq $Text) {
    return ""
  }
  return ($Text | Out-String)
}

function Assert-Contains {
  param(
    [Parameter(Mandatory = $true)][object]$Text,
    [Parameter(Mandatory = $true)][string]$Needle,
    [Parameter(Mandatory = $true)][string]$Label
  )

  $haystack = Normalize-ProofText $Text
  if (-not $haystack.Contains($Needle)) {
    throw "Missing proof: $Label. Expected to find: $Needle"
  }
}

function Assert-AnyContains {
  param(
    [Parameter(Mandatory = $true)][object]$Text,
    [Parameter(Mandatory = $true)][string[]]$Needles,
    [Parameter(Mandatory = $true)][string]$Label
  )

  $haystack = Normalize-ProofText $Text
  foreach ($needle in $Needles) {
    if ($haystack.Contains($needle)) {
      return
    }
  }
  throw "Missing proof: $Label. Expected one of: $($Needles -join ', ')"
}

function Test-TextContains {
  param(
    [AllowNull()][object]$Text,
    [Parameter(Mandatory = $true)][string]$Needle
  )

  return (Normalize-ProofText $Text).Contains($Needle)
}

function Test-TextContainsAny {
  param(
    [AllowNull()][object]$Text,
    [Parameter(Mandatory = $true)][string[]]$Needles
  )

  $haystack = Normalize-ProofText $Text
  foreach ($needle in $Needles) {
    if ($haystack.Contains($needle)) {
      return $true
    }
  }
  return $false
}

function Get-ProofText {
  $output = Invoke-WslBash "cat /tmp/split_cu_cp_stdout.log /tmp/split_cu_cp_ntn_sib19_proof.log /tmp/split_du_stdout.log /tmp/split_du_ntn_sib19_proof.log 2>/dev/null || true"
  return ($output | Out-String)
}

function Wait-ForProofText {
  param(
    [Parameter(Mandatory = $true)][string[]]$Needles,
    [int]$TimeoutSeconds = 60
  )

  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  $lastText = ""
  while ((Get-Date) -lt $deadline) {
    $lastText = Get-ProofText
    $allPresent = $true
    foreach ($needle in $Needles) {
      if (-not $lastText.Contains($needle)) {
        $allPresent = $false
        break
      }
    }
    if ($allPresent) {
      return $lastText
    }
    Start-Sleep -Seconds 2
  }
  return $lastText
}

function Stop-SplitProcesses {
  Invoke-WslBash "pkill -TERM srsue 2>/dev/null || true; pkill -TERM srsdu 2>/dev/null || true; pkill -TERM srscuup 2>/dev/null || true; pkill -TERM srscucp 2>/dev/null || true; pkill -TERM tcpdump 2>/dev/null || true; sleep 3; pkill -KILL srsue 2>/dev/null || true; pkill -KILL srsdu 2>/dev/null || true; pkill -KILL srscuup 2>/dev/null || true; pkill -KILL srscucp 2>/dev/null || true; pkill -KILL tcpdump 2>/dev/null || true" | Out-Null
}

function Assert-NoResidualSplitProcesses {
  $residual = Invoke-WslBash "pgrep -a srscucp || true; pgrep -a srscuup || true; pgrep -a srsdu || true; pgrep -a srsue || true; pgrep -a tcpdump || true"
  $residualText = ($residual | Out-String).Trim()
  if ($residualText.Length -ne 0) {
    throw "Residual split demo process found:`n$residualText"
  }
}

function Copy-ProofArtifacts {
  param([Parameter(Mandatory = $true)][string]$ExportDirWsl)

  Invoke-WslBash "mkdir -p '$ExportDirWsl'; cp -f /tmp/split_cu_cp_stdout.log /tmp/split_cu_cp_ntn_sib19_proof.log /tmp/split_du_stdout.log /tmp/split_du_ntn_sib19_proof.log /tmp/split_ue_stdout.log /tmp/ue_20_cucp_quiet.log /tmp/split_ue_ping.log /tmp/run_split_ntn_sib19_ue_visibility_stack.out '$ExportDirWsl/' 2>/dev/null || true; cp -f '$MacPcapSourceWsl' '$ExportDirWsl/split_du_ntn_sib19_proof_mac.pcap' 2>/dev/null || true" | Out-Null
}

New-Item -ItemType Directory -Path $ExportDirWin -Force | Out-Null

$RepoWsl = Convert-ToWslPath $RepoWin
$SplitWsl = Convert-ToWslPath $SplitWin
$PcapWsl = Convert-ToWslPath $PcapPath
$ExportDirWsl = Convert-ToWslPath $ExportDirWin
$CuCpConfigWsl = Convert-ToWslPath (Join-Path $SplitWin "cu_cp_split_ntn_sib19_proof.yml")
$DuConfigWsl = Convert-ToWslPath (Join-Path $SplitWin "du_zmq_split_ntn_sib19_proof.yml")

$Tshark = $DefaultTshark
if (-not (Test-Path $Tshark)) {
  $found = Get-Command tshark.exe -ErrorAction SilentlyContinue
  if ($null -eq $found) {
    throw "tshark not found. Install Wireshark or add tshark.exe to PATH."
  }
  $Tshark = $found.Source
}

$StdinWin = Join-Path $env:TEMP "split_cu_cp_ntn_sib19_ue_visibility.stdin"
Set-Content -Path $StdinWin -Encoding ASCII -NoNewline -Value "sleep 35;ntn_sat_geo 31.2304 121.4737 500000;sleep 2;ntn_state;ntn_assistance all 4;ntn_beams all 4;sleep 70;ntn_state;ntn_beams all 4;"
$StdinWsl = Convert-ToWslPath $StdinWin

$summaryLines = New-Object System.Collections.Generic.List[string]
$summaryLines.Add("# CUCP-058 NTN SIB19 UE Visibility Proof")
$summaryLines.Add("")
$summaryLines.Add("- Generated: $(Get-Date -Format o)")
$summaryLines.Add("- Scope: air-interface visibility via DU MAC SI-RNTI pcap, not UE SIB19 decode.")
$summaryLines.Add("- UE config: $RepoWin\run_artifacts\srsran_runtime_capture\ue_20_cucp_quiet.conf")
$summaryLines.Add("- UE SIB19 decode: not claimed; current srsUE config is RRC release 15.")

try {
  Write-Host "[1/8] Cleaning previous split demo processes and ensuring Open5GS is active..."
  Stop-SplitProcesses
  Invoke-WslBash "timeout 75s bash -lc 'systemctl start open5gs-udrd open5gs-udmd open5gs-ausfd open5gs-nrfd open5gs-amfd open5gs-smfd open5gs-upfd open5gs-pcfd open5gs-nssfd open5gs-bsfd || true; for i in {1..60}; do ss -a -n -p | grep -q 127.0.0.1:38412 && exit 0; sleep 1; done; systemctl is-active open5gs-amfd open5gs-smfd open5gs-upfd open5gs-nrfd open5gs-ausfd open5gs-udmd open5gs-udrd open5gs-pcfd open5gs-nssfd open5gs-bsfd || true; ss -a -n -p | grep 38412 || true; exit 1'" | Out-Null

  Write-Host "[2/8] Starting split stack with NTN SIB19 proof configs..."
  $stackCommand = @"
set -euo pipefail
repo='$RepoWsl'
split_dir='$SplitWsl'
rm -f /tmp/split_cu_cp_ntn_sib19_proof.log /tmp/split_du_ntn_sib19_proof.log
rm -f /tmp/split_ue_stdout.log /tmp/ue_20_cucp_quiet.log /tmp/split_ue_ping.log
rm -f '$MacPcapSourceWsl'
rm -f /tmp/run_split_ntn_sib19_ue_visibility_stack.out
cd "`$repo"
SPLIT_CU_CP_CONFIG='$CuCpConfigWsl' \
SPLIT_DU_CONFIG='$DuConfigWsl' \
SPLIT_CU_CP_STDIN_SCRIPT='$StdinWsl' \
SPLIT_LIVE_PCAP='$PcapWsl' \
  timeout 120s "`$split_dir/run_split_stack.sh" >/tmp/run_split_ntn_sib19_ue_visibility_stack.out 2>&1 || true
cat /tmp/run_split_ntn_sib19_ue_visibility_stack.out
"@
  $stackOutput = Invoke-WslBash $stackCommand
  Write-Host ($stackOutput | Out-String)

  Write-Host "[3/8] Waiting for CU-CP SIB19 runtime and DU apply proof..."
  $proofText = Wait-ForProofText -Needles @("NTN satellite state accepted", "NTN SIB19 broadcast:", "applied=1", "applied_by_du", "sib19_hash", "Enqueued dynamic SI PDU update") -TimeoutSeconds 100
  Start-Sleep -Seconds 2
  $proofText = "$proofText`n$(Get-ProofText)"
  $proofPreview = Invoke-WslBash "grep -aE 'NTN SIB19 broadcast|NTN state:|sib19|SIB19|applied_by_du|Enqueued dynamic SI|Encoding dynamic SI|Failed to encode dynamic' /tmp/split_cu_cp_stdout.log /tmp/split_cu_cp_ntn_sib19_proof.log /tmp/split_du_stdout.log /tmp/split_du_ntn_sib19_proof.log 2>/dev/null | tail -n 80 || true"
  Write-Host ($proofPreview | Out-String)

  Assert-Contains $proofText "NTN satellite state accepted" "manual NTN satellite injection accepted"
  Assert-Contains $proofText "NTN state: enabled=yes satellite=yes assistance=valid invalid_reason=none" "NTN runtime has valid assistance"
  Assert-Contains $proofText "NTN SIB19 broadcast:" "ntn_state prints SIB19 broadcast counters"
  Assert-Contains $proofText "applied=1" "CU-CP records at least one SIB19 apply result"
  Assert-Contains $proofText "SPLIT-DIGI-0001" "proof beam id is visible"
  Assert-Contains $proofText "applied_by_du" "ntn_beams records DU-applied SIB19 state"
  Assert-Contains $proofText "sib19_hash" "ntn_beams prints packed SIB19 hash column"
  Assert-Contains $proofText "SIB19 broadcast finished" "CU-CP F1AP procedure completed SIB19 broadcast"
  Assert-Contains $proofText "Enqueued dynamic SI PDU update" "DU/MAC accepted dynamic SI PDU update"
  $summaryLines.Add("- CU-CP/DU SIB19 apply: PASS")

  Write-Host "[4/8] Starting srsUE and probing user-plane data..."
  $ueOutput = Invoke-WslBash "timeout 70s '$SplitWsl/start_split_ue.sh' >/tmp/start_split_ue_ntn_sib19_visibility.out 2>&1 || true; cat /tmp/start_split_ue_ntn_sib19_visibility.out"
  Write-Host ($ueOutput | Out-String)
  Start-Sleep -Seconds 20
  $pingOutput = Invoke-WslBash "timeout 60s '$SplitWsl/probe_split_ue_data.sh' || true"
  Write-Host ($pingOutput | Out-String)
  Start-Sleep -Seconds 20

  Write-Host "[5/8] Stopping split demo processes and copying artifacts..."
  Stop-SplitProcesses
  Copy-ProofArtifacts -ExportDirWsl $ExportDirWsl
  Assert-NoResidualSplitProcesses

  $ueText = (Invoke-WslBash "cat '$ExportDirWsl/split_ue_stdout.log' '$ExportDirWsl/ue_20_cucp_quiet.log' '$ExportDirWsl/split_ue_ping.log' 2>/dev/null || true" | Out-String)
  $cuCpAfterUeText = (Invoke-WslBash "cat '$ExportDirWsl/split_cu_cp_stdout.log' '$ExportDirWsl/split_cu_cp_ntn_sib19_proof.log' 2>/dev/null || true" | Out-String)
  $duAfterUeText = (Invoke-WslBash "cat '$ExportDirWsl/split_du_stdout.log' '$ExportDirWsl/split_du_ntn_sib19_proof.log' 2>/dev/null || true" | Out-String)
  if ($duAfterUeText -like "*Failed to encode dynamic SI-message*") {
    throw "Missing proof: dynamic SIB19 fitted the scheduled SI PDSCH TB. See $ExportDirWin"
  }
  Assert-Contains $duAfterUeText "Encoding dynamic SI-message 0 sib=19" "DU/MAC encoded the dynamic SIB19 SI PDU"
  $ueSib1 = Test-TextContainsAny $ueText @("SIB1 received", "SIB1 acquired successfully")
  $ueRegistered = Test-TextContains $ueText "Handling Registration Accept"
  $uePdu = Test-TextContains $ueText "PDU Session Establishment successful"
  $uePing = Test-TextContains $ueText "5 packets transmitted, 5 received, 0% packet loss"
  $cuCpPdu = Test-TextContains $cuCpAfterUeText "PDU Session Resource Setup Routine"
  if ($ueSib1 -and $ueRegistered -and $uePdu -and $uePing -and $cuCpPdu) {
    $summaryLines.Add("- UE SIB1/RRC/NAS/PDU/ping: PASS")
  } else {
    $ueFailureStage = "unknown"
    if (-not $ueSib1) {
      $ueFailureStage = "before SIB1 acquisition"
    } elseif (-not $ueRegistered) {
      $ueFailureStage = "after SIB1, before NAS Registration Accept"
    } elseif (-not $uePdu) {
      $ueFailureStage = "after NAS registration, before PDU Session Establishment"
    } elseif (-not $uePing) {
      $ueFailureStage = "after PDU Session Establishment, before successful ping"
    } elseif (-not $cuCpPdu) {
      $ueFailureStage = "UE reported PDU path but CU-CP PDU setup evidence was not found"
    }
    $summaryLines.Add("- UE SIB1/RRC/NAS/PDU/ping: FAIL at $ueFailureStage. UE-side Rel-17 SIB19 decode is not expected or claimed; see exported logs.")
    Write-Warning "UE attach/data proof did not complete: $ueFailureStage. Continuing with F1AP/MAC SIB19 visibility checks."
  }

  Write-Host "[6/8] Checking F1AP pcap for SIB19 update/result containers..."
  if (-not (Test-Path $PcapPath)) {
    throw "PCAP was not created: $PcapPath"
  }
  $protocolHierarchy = (& $Tshark -r $PcapPath -q -z io,phs 2>$null | Out-String)
  Assert-Contains $protocolHierarchy.ToLowerInvariant() "f1ap" "pcap protocol f1ap"
  $f1apLines = (& $Tshark -r $PcapPath -Y "f1ap" -T fields -e frame.number -e _ws.col.Protocol -e _ws.col.Info 2>$null | Out-String)
  if (($f1apLines -notlike "*Resource Coordination*") -and
      ($f1apLines -notlike "*ResourceCoordination*") -and
      ($f1apLines -notlike "*GNBDUResourceCoordination*")) {
    throw "Missing proof: F1AP resource coordination is decoded."
  }
  $pcapAscii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($PcapPath))
  Assert-Contains $pcapAscii "SIB19U01" "F1AP pcap contains SIB19 update container"
  Assert-Contains $pcapAscii "SIB19R01" "F1AP pcap contains SIB19 result container"
  $summaryLines.Add("- F1AP SIB19 coordination pcap: PASS")

  Write-Host "[7/8] Checking DU MAC pcap for downlink SI-RNTI / BCCH-DL-SCH visibility..."
  if (-not (Test-Path $MacPcapExportWin)) {
    throw "MAC pcap was not exported: $MacPcapExportWin"
  }
  $macPcapLength = (Get-Item -LiteralPath $MacPcapExportWin).Length
  if ($macPcapLength -le 24) {
    throw "MAC pcap contains only the pcap header: $MacPcapExportWin"
  }
  $macSummary = (& $Tshark -r $MacPcapExportWin -T fields -e frame.number -e _ws.col.Protocol -e _ws.col.Info 2>$null | Out-String)
  $macVerbose = (& $Tshark -r $MacPcapExportWin -V 2>$null | Select-Object -First 500 | Out-String)
  $macEvidence = "$macSummary`n$macVerbose"
  Assert-AnyContains $macEvidence @("SI-RNTI", "RNTI Type: SI", "RNTI: 65535", "rnti: 65535", "0xffff", "BCCH-DL-SCH") "DU MAC pcap contains downlink SI-RNTI or BCCH-DL-SCH evidence"
  $summaryLines.Add("- DU MAC SI-RNTI pcap: PASS ($macPcapLength bytes; dynamic SIB19 encoding logged by DU/MAC)")

  Write-Host "[8/8] Writing proof summary..."
  $summaryPath = Join-Path $ExportDirWin "sib19_ue_visibility_summary.md"
  $summaryLines.Add("- SCTP/F1AP pcap: $PcapPath")
  $summaryLines.Add("- DU MAC pcap: $MacPcapExportWin")
  $summaryLines.Add("- Logs: $ExportDirWin")
  $summaryLines.Add("")
  $summaryLines.Add("## Notes")
  $summaryLines.Add("- This proof verifies SIB19 reaches CU-CP runtime, F1AP coordination, DU dynamic SI apply, and DU MAC SI-RNTI broadcast capture.")
  $summaryLines.Add("- It deliberately does not claim Rel-17 NR SIB19 decoding by the current external srsUE.")
  Set-Content -Path $summaryPath -Encoding UTF8 -Value $summaryLines

  Write-Host "CUCP-058 SIB19 UE visibility proof completed."
  Write-Host "Summary: $summaryPath"
  Write-Host "SCTP/F1AP PCAP: $PcapPath"
  Write-Host "DU MAC PCAP: $MacPcapExportWin"
  Write-Host "Useful Wireshark display filters: f1ap || nr-rrc, mac-nr"
} finally {
  try {
    Stop-SplitProcesses
  } catch {
  }
  try {
    Copy-ProofArtifacts -ExportDirWsl $ExportDirWsl
  } catch {
  }
  Remove-Item -Path $StdinWin -Force -ErrorAction SilentlyContinue
}
