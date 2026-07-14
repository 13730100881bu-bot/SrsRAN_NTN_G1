@echo off
title srsRAN live F1AP_RRC_inside
echo Streaming /tmp/gnb_live_f1ap.pcap into Wireshark. Keep this window open while gNB runs.
wsl.exe -u root -- bash -lc "cat /tmp/gnb_live_f1ap.pcap" | "C:\Program Files\Wireshark\Wireshark.exe" -k -i -
