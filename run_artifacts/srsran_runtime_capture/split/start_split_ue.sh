#!/usr/bin/env bash
set -euo pipefail

repo=/mnt/d/code/srsRAN_Project-main
ue_bin=/mnt/d/code/srsRAN_4G_23_11/build/srsue/src/srsue
ue_cfg="$repo/run_artifacts/srsran_runtime_capture/ue_20_cucp_quiet.conf"
rt_mode="${SPLIT_RT_MODE:-radio}"

case "$rt_mode" in
  radio | all | none)
    ;;
  *)
    echo "Invalid SPLIT_RT_MODE='$rt_mode'. Expected radio, all, or none." >&2
    exit 2
    ;;
esac

ip netns add ue1 2>/dev/null || true
ip netns exec ue1 ip link set lo up 2>/dev/null || true

pkill -TERM srsue 2>/dev/null || true
rm -f /tmp/split_ue_stdout.log /tmp/split_ue.log

rt_prefix=""
if command -v chrt >/dev/null 2>&1 && chrt -f 20 true >/dev/null 2>&1; then
  case "$rt_mode" in
    radio | all)
      rt_prefix="chrt -f 20"
      ;;
    none)
      ;;
  esac
fi
echo "UE realtime mode: $rt_mode (${rt_prefix:-normal})"

setsid -f bash -c "cd /mnt/d/code/srsRAN_4G_23_11/build/srsue/src && $rt_prefix '$ue_bin' '$ue_cfg' >/tmp/split_ue_stdout.log 2>&1"
sleep 12

pgrep -af "srsue|srsdu|srscucp|srscuup" || true
echo "--- SCTP ---"
ss -a -n -p | grep -E "38412|38462|38472|srscucp|srscuup|srsdu" || true
echo "--- UE ---"
tail -n 120 /tmp/split_ue_stdout.log /tmp/split_ue.log 2>/dev/null || true
echo "--- DU ---"
tail -n 120 /tmp/split_du_stdout.log /tmp/split_du.log 2>/dev/null || true
echo "--- CUCP ---"
grep -aE "F1|Initial|RRC|UE|PDU|E1|NG|Setup|ERROR|fail|reject|connect" /tmp/split_cu_cp.log /tmp/split_cu_cp_stdout.log 2>/dev/null | tail -n 120 || true
