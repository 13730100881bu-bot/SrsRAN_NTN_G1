@echo off
title srsRAN live NGAP
echo Streaming /tmp/gnb_live_ngap.pcap into Wireshark. Keep this window open while gNB runs.
wsl.exe -u root -- bash -lc "cat /tmp/gnb_live_ngap.pcap" | "C:\Program Files\Wireshark\Wireshark.exe" -k -i -
