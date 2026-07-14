@echo off
title srsRAN live E1AP
echo Streaming /tmp/gnb_live_e1ap.pcap into Wireshark. Keep this window open while gNB runs.
wsl.exe -u root -- bash -lc "cat /tmp/gnb_live_e1ap.pcap" | "C:\Program Files\Wireshark\Wireshark.exe" -k -i -
