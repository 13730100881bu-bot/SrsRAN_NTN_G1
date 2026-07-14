@echo off
title WSL SCTP live capture to Wireshark
REM Captures WSL loopback SCTP packets. In current monolithic gNB this is NGAP; in split CU/DU it would also include F1AP/E1AP SCTP.
echo Starting WSL tcpdump on lo for SCTP and piping into Wireshark...
echo Close Wireshark or this window to stop the live capture.
wsl.exe -u root -- bash -lc "tcpdump -i lo -U -w - 'sctp' 2>/tmp/wsl_sctp_tcpdump_latest.log" | "C:\Program Files\Wireshark\Wireshark.exe" -k -i -
