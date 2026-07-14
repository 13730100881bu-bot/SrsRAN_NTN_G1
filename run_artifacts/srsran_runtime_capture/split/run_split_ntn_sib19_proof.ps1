$ErrorActionPreference = "Stop"

$SplitWin = $PSScriptRoot
$RepoWin = (Resolve-Path (Join-Path $SplitWin "..\..\..")).Path
$PcapPath = Join-Path $SplitWin "split_live_sctp_latest.pcap"
$DefaultTshark = "C:\Program Files\Wireshark\tshark.exe"

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

$RepoWsl = Convert-ToWslPath $RepoWin
$SplitWsl = Convert-ToWslPath $SplitWin
$PcapWsl = Convert-ToWslPath $PcapPath
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

$StdinWin = Join-Path $env:TEMP "split_cu_cp_ntn_sib19_proof.stdin"
Set-Content -Path $StdinWin -Encoding ASCII -NoNewline -Value "sleep 18;ntn_sat_geo 31.2304 121.4737 500000;sleep 2;ntn_state;ntn_assistance all 4;ntn_beams all 4;sleep 5;ntn_state;ntn_beams all 4;"
$StdinWsl = Convert-ToWslPath $StdinWin

try {
  Write-Host "[1/5] Cleaning previous split demo processes and ensuring Open5GS is active..."
  Invoke-WslBash "timeout 75s bash -lc 'pkill -TERM srsue 2>/dev/null || true; pkill -TERM srsdu 2>/dev/null || true; pkill -TERM srscuup 2>/dev/null || true; pkill -TERM srscucp 2>/dev/null || true; pkill -TERM tcpdump 2>/dev/null || true; sleep 1; pkill -KILL srsue 2>/dev/null || true; pkill -KILL srsdu 2>/dev/null || true; pkill -KILL srscuup 2>/dev/null || true; pkill -KILL srscucp 2>/dev/null || true; pkill -KILL tcpdump 2>/dev/null || true; systemctl start open5gs-udrd open5gs-udmd open5gs-ausfd open5gs-nrfd open5gs-amfd open5gs-smfd open5gs-upfd open5gs-pcfd open5gs-nssfd open5gs-bsfd || true; for i in {1..60}; do ss -a -n -p | grep -q 127.0.0.1:38412 && exit 0; sleep 1; done; systemctl is-active open5gs-amfd open5gs-smfd open5gs-upfd open5gs-nrfd open5gs-ausfd open5gs-udmd open5gs-udrd open5gs-pcfd open5gs-nssfd open5gs-bsfd || true; ss -a -n -p | grep 38412 || true; exit 1'"

  Write-Host "[2/5] Starting split stack with NTN SIB19 proof configs..."
  $stackCommand = @"
set -euo pipefail
repo='$RepoWsl'
split_dir='$SplitWsl'
rm -f /tmp/split_cu_cp_ntn_sib19_proof.log /tmp/split_du_ntn_sib19_proof.log
rm -f /tmp/run_split_ntn_sib19_stack.out
cd "`$repo"
SPLIT_CU_CP_CONFIG='$CuCpConfigWsl' \
SPLIT_DU_CONFIG='$DuConfigWsl' \
SPLIT_CU_CP_STDIN_SCRIPT='$StdinWsl' \
SPLIT_LIVE_PCAP='$PcapWsl' \
  timeout 90s "`$split_dir/run_split_stack.sh" >/tmp/run_split_ntn_sib19_stack.out 2>&1 || true
cat /tmp/run_split_ntn_sib19_stack.out
"@
  $stackOutput = Invoke-WslBash $stackCommand
  Write-Host ($stackOutput | Out-String)

  Write-Host "[3/5] Waiting for CU-CP CLI injection and SIB19 apply result..."
  Start-Sleep -Seconds 30
  $proofOutput = Invoke-WslBash "cat /tmp/split_cu_cp_stdout.log /tmp/split_cu_cp_ntn_sib19_proof.log /tmp/split_du_stdout.log /tmp/split_du_ntn_sib19_proof.log 2>/dev/null || true"
  $proofText = ($proofOutput | Out-String)
  Write-Host $proofText

  Assert-Contains $proofText "NTN satellite state accepted" "manual NTN satellite injection accepted"
  Assert-Contains $proofText "NTN state: enabled=yes satellite=yes assistance=valid invalid_reason=none" "NTN runtime has valid assistance"
  Assert-Contains $proofText "NTN SIB19 broadcast:" "ntn_state prints SIB19 broadcast counters"
  Assert-Contains $proofText "applied=1" "CU-CP records at least one SIB19 apply result"
  Assert-Contains $proofText "SPLIT-DIGI-0001" "proof beam id is visible"
  Assert-Contains $proofText "applied_by_du" "ntn_beams records DU-applied SIB19 state"
  Assert-Contains $proofText "sib19_hash" "ntn_beams prints packed SIB19 hash column"
  Assert-Contains $proofText "SIB19 broadcast finished" "CU-CP F1AP procedure completed SIB19 broadcast"
  Assert-Contains $proofText "Enqueued dynamic SI PDU update" "DU/MAC accepted dynamic SI PDU update"

  Write-Host "[4/5] Checking F1AP pcap for SIB19 update/result containers..."
  Invoke-WslBash "pkill -TERM tcpdump 2>/dev/null || true; sleep 1; pkill -KILL tcpdump 2>/dev/null || true"
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

  Write-Host "[5/5] Stopping split demo processes..."
  Invoke-WslBash "pkill -TERM srsue 2>/dev/null || true; pkill -TERM srsdu 2>/dev/null || true; pkill -TERM srscuup 2>/dev/null || true; pkill -TERM srscucp 2>/dev/null || true; pkill -TERM tcpdump 2>/dev/null || true; sleep 1; pkill -KILL srsue 2>/dev/null || true; pkill -KILL srsdu 2>/dev/null || true; pkill -KILL srscuup 2>/dev/null || true; pkill -KILL srscucp 2>/dev/null || true; pkill -KILL tcpdump 2>/dev/null || true"
  $residual = Invoke-WslBash "pgrep -a srscucp || true; pgrep -a srscuup || true; pgrep -a srsdu || true; pgrep -a srsue || true; pgrep -a tcpdump || true"
  $residualText = ($residual | Out-String).Trim()
  if ($residualText.Length -ne 0) {
    throw "Residual split demo process found:`n$residualText"
  }

  Write-Host "CUCP-057 SIB19 proof passed."
  Write-Host "PCAP: $PcapPath"
  Write-Host "Useful Wireshark display filter: f1ap || nr-rrc"
} finally {
  try {
    Invoke-WslBash "pkill -TERM srsue 2>/dev/null || true; pkill -TERM srsdu 2>/dev/null || true; pkill -TERM srscuup 2>/dev/null || true; pkill -TERM srscucp 2>/dev/null || true; pkill -TERM tcpdump 2>/dev/null || true; sleep 1; pkill -KILL srsue 2>/dev/null || true; pkill -KILL srsdu 2>/dev/null || true; pkill -KILL srscuup 2>/dev/null || true; pkill -KILL srscucp 2>/dev/null || true; pkill -KILL tcpdump 2>/dev/null || true" | Out-Null
  } catch {
  }
  Remove-Item -Path $StdinWin -Force -ErrorAction SilentlyContinue
}
