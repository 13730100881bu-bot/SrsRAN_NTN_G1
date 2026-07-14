$ErrorActionPreference = "Continue"

$SplitWin = $PSScriptRoot
$SplitWsl = "/mnt/d/code/srsRAN_Project-main/run_artifacts/srsran_runtime_capture/split"
$PcapPath = Join-Path $SplitWin "split_live_sctp_latest.pcap"
$Wireshark = "C:\Program Files\Wireshark\Wireshark.exe"
$Tshark = "C:\Program Files\Wireshark\tshark.exe"

Write-Host "[1/5] Cleaning old split demo processes and ensuring Open5GS is active..."
Stop-Process -Name Wireshark -Force -ErrorAction SilentlyContinue
wsl.exe -e bash -lc "timeout 75s bash -lc 'pkill -TERM srsue 2>/dev/null || true; pkill -TERM srsdu 2>/dev/null || true; pkill -TERM srscuup 2>/dev/null || true; pkill -TERM srscucp 2>/dev/null || true; pkill -TERM tcpdump 2>/dev/null || true; sleep 2; systemctl start open5gs-udrd open5gs-udmd open5gs-ausfd open5gs-nrfd open5gs-amfd open5gs-smfd open5gs-upfd open5gs-pcfd open5gs-nssfd open5gs-bsfd || true; for i in {1..60}; do ss -a -n -p | grep -q 127.0.0.1:38412 && exit 0; sleep 1; done; systemctl is-active open5gs-amfd open5gs-smfd open5gs-upfd open5gs-nrfd open5gs-ausfd open5gs-udmd open5gs-udrd open5gs-pcfd open5gs-nssfd open5gs-bsfd || true; ss -a -n -p | grep 38412 || true' || true"

Write-Host "[2/5] Starting split CU-CP / CU-UP / DU and SCTP file capture..."
wsl.exe -e bash -lc "timeout 45s $SplitWsl/run_split_stack.sh >/tmp/run_split_stack_last.out 2>&1 || true"

Write-Host "[3/5] Waiting for NG/F1/E1 setup, then starting UE..."
Start-Sleep -Seconds 35
wsl.exe -e bash -lc "timeout 45s $SplitWsl/start_split_ue.sh >/tmp/start_split_ue_last.out 2>&1 || true"
Start-Sleep -Seconds 20

Write-Host "Sending short UE data probe through tun_srsue..."
wsl.exe -e bash -lc "timeout 45s $SplitWsl/probe_split_ue_data.sh || true"
Start-Sleep -Seconds 20

Write-Host "[4/5] Current process and pcap status..."
wsl.exe -e bash -lc "pgrep -a srscucp || true; pgrep -a srscuup || true; pgrep -a srsdu || true; pgrep -a srsue || true; pgrep -a tcpdump || true; ls -lh '$SplitWsl/split_live_sctp_latest.pcap' || true"

Write-Host "Stopping split demo processes so the pcap is closed cleanly..."
wsl.exe -e bash -lc "timeout 10s bash -lc 'pkill -TERM srsue 2>/dev/null || true; pkill -TERM srsdu 2>/dev/null || true; pkill -TERM srscuup 2>/dev/null || true; pkill -TERM srscucp 2>/dev/null || true; pkill -TERM tcpdump 2>/dev/null || true; sleep 1; pkill -KILL srsue 2>/dev/null || true; pkill -KILL srsdu 2>/dev/null || true; pkill -KILL srscuup 2>/dev/null || true; pkill -KILL srscucp 2>/dev/null || true; pkill -KILL tcpdump 2>/dev/null || true' || true"

Write-Host "[5/5] Decoded NGAP/F1AP/E1AP summary from saved pcap:"
if (Test-Path $Tshark) {
  & $Tshark -r $PcapPath -Y "ngap || f1ap || e1ap" -T fields -e frame.number -e _ws.col.Protocol -e _ws.col.Info 2>$null | Select-Object -First 80
} else {
  Write-Host "tshark not found at $Tshark"
}

if ($env:SRSRAN_OPEN_WIRESHARK -eq "1" -and (Test-Path $Wireshark)) {
  Write-Host "Opening one Wireshark window for: $PcapPath"
  Start-Process -FilePath $Wireshark -ArgumentList "`"$PcapPath`""
} else {
  Write-Host "Wireshark not opened. To open it automatically, set SRSRAN_OPEN_WIRESHARK=1."
}

Write-Host "PCAP: $PcapPath"
