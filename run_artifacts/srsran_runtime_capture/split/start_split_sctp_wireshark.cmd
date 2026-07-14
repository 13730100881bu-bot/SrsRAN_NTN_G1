@echo off
set WIRESHARK=C:\Program Files\Wireshark\Wireshark.exe
where wsl.exe >nul 2>nul || (echo wsl.exe not found & pause & exit /b 1)
if not exist "%WIRESHARK%" (echo Wireshark not found at "%WIRESHARK%" & pause & exit /b 1)
echo Capturing WSL loopback SCTP traffic for NGAP, F1AP and E1AP.
echo In Wireshark use display filter: ngap || f1ap || e1ap || nr-rrc
wsl.exe -u root -- bash -lc "tcpdump -i lo -U -w - 'sctp'" | "%WIRESHARK%" -k -i -
