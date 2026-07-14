$ErrorActionPreference = "Stop"

$SplitWin = $PSScriptRoot
$RepoWin = (Resolve-Path (Join-Path $SplitWin "..\..\..")).Path
$PcapPath = Join-Path $SplitWin "split_live_sctp_latest.pcap"
$DefaultTshark = "C:\Program Files\Wireshark\tshark.exe"

function Convert-ToWslPath {
  param([Parameter(Mandatory = $true)][string]$Path)

  $resolved = (Resolve-Path $Path).Path
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

function Assert-Contains {
  param(
    [Parameter(Mandatory = $true)][string]$Text,
    [Parameter(Mandatory = $true)][string]$Needle,
    [Parameter(Mandatory = $true)][string]$Label
  )

  if ($Text -notlike "*$Needle*") {
    throw "Missing proof: $Label. Expected to find: $Needle"
  }
}

function Assert-MinCount {
  param(
    [Parameter(Mandatory = $true)][int]$Actual,
    [Parameter(Mandatory = $true)][int]$Expected,
    [Parameter(Mandatory = $true)][string]$Label
  )

  if ($Actual -lt $Expected) {
    throw "Missing proof: $Label. Expected at least $Expected, got $Actual"
  }
}

$RepoWsl = Convert-ToWslPath $RepoWin
$SplitWsl = Convert-ToWslPath $SplitWin

$Tshark = $DefaultTshark
if (-not (Test-Path $Tshark)) {
  $found = Get-Command tshark.exe -ErrorAction SilentlyContinue
  if ($null -eq $found) {
    throw "tshark not found. Install Wireshark or add tshark.exe to PATH."
  }
  $Tshark = $found.Source
}

Write-Host "[1/4] Capturing CU-CP NTN runtime proof..."
$runtimeCommand = @"
set -euo pipefail
repo='$RepoWsl'
split_dir='$SplitWsl'
pkill -TERM srscucp 2>/dev/null || true
sleep 1
rm -f /tmp/split_cu_cp_ntn_proof.log /tmp/split_cu_cp_ntn_proof_stdout.log
rm -f /tmp/split_cu_cp_ntn_proof_*.pcap
cd "`$repo"
(printf '%s\n' 'sleep 2;ntn_sat_geo 31.2304 121.4737 500000;sleep 1;ntn_state;ntn_assistance all 4;ntn_beams all 4;sleep 1'; tail -f /dev/null) |
  timeout 15s ./build/ai-clean/apps/cu_cp/srscucp -c "`$split_dir/cu_cp_split_ntn_proof.yml" >/tmp/split_cu_cp_ntn_proof_stdout.log 2>&1 || true
cat /tmp/split_cu_cp_ntn_proof_stdout.log
"@
$runtimeOutput = Invoke-WslBash $runtimeCommand
$runtimeText = ($runtimeOutput | Out-String)
Write-Host $runtimeText

Assert-Contains $runtimeText "NTN satellite state accepted" "manual NTN satellite injection accepted"
Assert-Contains $runtimeText "NTN state: enabled=yes satellite=yes assistance=valid invalid_reason=none" "NTN runtime is enabled with valid assistance"
Assert-Contains $runtimeText "NTN beams: total=1" "split NTN beam table loaded"
Assert-Contains $runtimeText "NTN assistance: valid invalid_reason=none beams=1" "assistance snapshot has one beam"
Assert-Contains $runtimeText "SPLIT-DIGI-0001" "proof beam id is visible"
Assert-Contains $runtimeText "0x000066c000" "proof beam NCI matches live DU cell"

$pcapProofPassed = $false
$lastPcapProofError = $null
for ($attempt = 1; $attempt -le 2 -and -not $pcapProofPassed; ++$attempt) {
  Write-Host "[2/4] Running standard split demo to generate NGAP/F1AP/E1AP/RRC/GTP-U pcap (attempt $attempt/2)..."
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $SplitWin "run_split_demo.ps1")
  if (-not (Test-Path $PcapPath)) {
    $lastPcapProofError = "PCAP was not created: $PcapPath"
    continue
  }

  try {
    Write-Host "[3/4] Checking pcap protocol hierarchy and key procedures..."
    $protocolHierarchy = (& $Tshark -r $PcapPath -q -z io,phs 2>$null | Out-String)
    foreach ($protocol in @("ngap", "e1ap", "f1ap", "gtp", "icmp")) {
      Assert-Contains $protocolHierarchy.ToLowerInvariant() $protocol "pcap protocol $protocol"
    }

    $decodedLines = @(& $Tshark -r $PcapPath -Y "ngap || f1ap || e1ap || nr-rrc || gtp || icmp" -T fields -e frame.number -e _ws.col.Protocol -e _ws.col.Info 2>$null)
    $decodedText = ($decodedLines | Out-String)
    Assert-Contains $decodedText "F1AP/NR RRC" "decoded RRC carried over F1AP"
    foreach ($message in @(
        "NGSetupRequest",
        "NGSetupResponse",
        "GNB-CU-UP-E1SetupRequest",
        "GNB-CU-UP-E1SetupResponse",
        "F1SetupRequest",
        "F1SetupResponse",
        "RRC Setup Request",
        "RRC Setup",
        "InitialUEMessage",
        "InitialContextSetupRequest",
        "InitialContextSetupResponse",
        "UE Capability Enquiry",
        "UE Capability Information",
        "PDUSessionResourceSetupRequest",
        "BearerContextSetupRequest",
        "BearerContextSetupResponse",
        "UEContextModificationRequest",
        "UEContextModificationResponse",
        "RRC Reconfiguration",
        "RRC Reconfiguration Complete",
        "PDUSessionResourceSetupResponse",
        "Echo (ping) request",
        "Echo (ping) reply"
      )) {
      Assert-Contains $decodedText $message "pcap message $message"
    }

    $pingRequests = @(& $Tshark -r $PcapPath -Y "gtp && icmp.type == 8" -T fields -e frame.number 2>$null)
    $pingReplies = @(& $Tshark -r $PcapPath -Y "gtp && icmp.type == 0" -T fields -e frame.number 2>$null)
    Assert-MinCount $pingRequests.Count 5 "GTP-U ping requests"
    Assert-MinCount $pingReplies.Count 5 "GTP-U ping replies"
    $pcapProofPassed = $true
  } catch {
    $lastPcapProofError = $_.Exception.Message
    if ($attempt -lt 2) {
      Write-Warning "Split pcap proof failed on attempt ${attempt}: $lastPcapProofError. Retrying once."
    }
  }
}
if (-not $pcapProofPassed) {
  throw $lastPcapProofError
}

Write-Host "[4/4] Ensuring split demo processes are stopped..."
Invoke-WslBash "pkill -TERM srsue 2>/dev/null || true; pkill -TERM srsdu 2>/dev/null || true; pkill -TERM srscuup 2>/dev/null || true; pkill -TERM srscucp 2>/dev/null || true; pkill -TERM tcpdump 2>/dev/null || true; sleep 1; pkill -KILL srsue 2>/dev/null || true; pkill -KILL srsdu 2>/dev/null || true; pkill -KILL srscuup 2>/dev/null || true; pkill -KILL srscucp 2>/dev/null || true; pkill -KILL tcpdump 2>/dev/null || true"
$residual = Invoke-WslBash "pgrep -a srscucp || true; pgrep -a srscuup || true; pgrep -a srsdu || true; pgrep -a srsue || true; pgrep -a tcpdump || true"
$residualText = ($residual | Out-String).Trim()
if ($residualText.Length -ne 0) {
  throw "Residual split demo process found:`n$residualText"
}

Write-Host "CUCP-043 proof passed."
Write-Host "PCAP: $PcapPath"
Write-Host "Useful Wireshark display filter: ngap || f1ap || e1ap || nr-rrc || gtp || icmp"
