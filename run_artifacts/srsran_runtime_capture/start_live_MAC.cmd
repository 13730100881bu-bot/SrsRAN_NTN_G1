@echo off
title srsRAN live MAC
echo Streaming /tmp/gnb_live_mac.pcap into Wireshark. Close this window after stopping gNB.
wsl.exe -u root -- bash -lc "cat /tmp/gnb_live_mac.pcap" | "C:\Program Files\Wireshark\Wireshark.exe" -k -i -
