#!/usr/bin/env bash
set -euo pipefail
out=/mnt/d/code/srsRAN_Project-main/run_artifacts/srsran_runtime_capture
mkdir -p "$out"
cp -f /tmp/gnb_20_cucp_ngap.pcap "$out/" 2>/dev/null || true
grep -aE 'CU-CP|CU-CP-F1|NGAP|RRC|InitialULRRCMessageTransfer|DLRRCMessageTransfer|InitialUE|Registration|Context|Security|PDU|ue=0|RRC Setup|F1Setup' /tmp/gnb_20_cucp.log > "$out/cucp_only_flow.log" 2>/dev/null || true
grep -aE 'Cell Selection|SIB1|Random Access|RRC Connected|rrcSetup|Registration|PDU|Failed|ERROR|WARN' /tmp/ue_20_official.log > "$out/ue_key_flow.log" 2>/dev/null || true
cp -f /tmp/gnb_20_cucp.log "$out/gnb_20_cucp.log" 2>/dev/null || true
ls -lh "$out"/cucp_only_flow.log "$out"/ue_key_flow.log "$out"/gnb_20_cucp_ngap.pcap 2>/dev/null || true
