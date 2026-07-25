#!/usr/bin/env bash
set -euo pipefail

repo=/mnt/d/code/srsRAN_Project-main
split_dir="$repo/run_artifacts/srsran_runtime_capture/split"

cu_cp_config="${SPLIT_CU_CP_CONFIG:-$split_dir/cu_cp_split_cucp.yml}"
cu_up_config="${SPLIT_CU_UP_CONFIG:-$split_dir/cu_up_split_cucp.yml}"
du_config="${SPLIT_DU_CONFIG:-$split_dir/du_zmq_split_cucp.yml}"
pcap_path="${SPLIT_LIVE_PCAP:-$split_dir/split_live_sctp_latest.pcap}"
cu_cp_stdin_script="${SPLIT_CU_CP_STDIN_SCRIPT:-}"
wait_retries="${SPLIT_LISTENER_WAIT_RETRIES:-40}"
rt_mode="${SPLIT_RT_MODE:-radio}"

case "$rt_mode" in
  radio | all | none)
    ;;
  *)
    echo "Invalid SPLIT_RT_MODE='$rt_mode'. Expected radio, all, or none." >&2
    exit 2
    ;;
esac

wait_for_sctp_listener() {
  local addr="$1"
  local port="$2"
  local label="$3"

  for _ in $(seq 1 "$wait_retries"); do
    if ss -S -l -n 2>/dev/null | grep -q "${addr}:${port}"; then
      echo "$label listener ready at ${addr}:${port}"
      return 0
    fi
    sleep 1
  done

  echo "Timed out waiting for $label listener at ${addr}:${port}" >&2
  ss -S -l -n 2>/dev/null || true
  return 1
}

cu_cp_stdin_pipeline() {
  if [[ -n "$cu_cp_stdin_script" ]]; then
    cat "$cu_cp_stdin_script"
  fi
  tail -f /dev/null
}

pkill -TERM srsue 2>/dev/null || true
pkill -TERM srsdu 2>/dev/null || true
pkill -TERM srscuup 2>/dev/null || true
pkill -TERM srscucp 2>/dev/null || true
pkill -TERM tcpdump 2>/dev/null || true
sleep 2

rm -f /tmp/split_cu_cp.log /tmp/split_cu_up.log /tmp/split_du.log
rm -f /tmp/split_cu_cp_stdout.log /tmp/split_cu_up_stdout.log /tmp/split_du_stdout.log
rm -f /tmp/split_sctp_file.log /tmp/split_cu_cp_*.pcap /tmp/split_cu_up_*.pcap /tmp/split_du_*.pcap
rm -f "$pcap_path"

# Giving every split process the same FIFO priority can delay F1 RRC setup beyond the RA contention timer.
control_plane_rt_prefix=""
radio_rt_prefix=""
if command -v chrt >/dev/null 2>&1 && chrt -f 20 true >/dev/null 2>&1; then
  case "$rt_mode" in
    radio)
      radio_rt_prefix="chrt -f 20"
      ;;
    all)
      control_plane_rt_prefix="chrt -f 20"
      radio_rt_prefix="chrt -f 20"
      ;;
    none)
      ;;
  esac
fi
echo "Split realtime mode: $rt_mode (control-plane='${control_plane_rt_prefix:-normal}', radio='${radio_rt_prefix:-normal}')"

setsid -f bash -c "tcpdump -i lo -U -w '$pcap_path' 'sctp or udp port 2152' >/tmp/split_sctp_file.log 2>&1"

cd "$repo"
export cu_cp_stdin_script
setsid -f bash -c "$(declare -f cu_cp_stdin_pipeline); cu_cp_stdin_pipeline | $control_plane_rt_prefix ./build/ai-clean/apps/cu_cp/srscucp -c '$cu_cp_config' >/tmp/split_cu_cp_stdout.log 2>&1"
wait_for_sctp_listener "127.0.20.1" "38462" "CU-CP E1AP"
wait_for_sctp_listener "127.0.10.1" "38472" "CU-CP F1AP"

setsid -f bash -c "tail -f /dev/null | $control_plane_rt_prefix ./build/ai-clean/apps/cu_up/srscuup -c '$cu_up_config' >/tmp/split_cu_up_stdout.log 2>&1"
sleep 2
setsid -f bash -c "tail -f /dev/null | $radio_rt_prefix ./build/ai-clean/apps/du/srsdu -c '$du_config' >/tmp/split_du_stdout.log 2>&1"
sleep 5

pgrep -af "srscucp|srscuup|srsdu|tcpdump -i lo" || true
echo "--- CUCP LOG ---"
tail -n 80 /tmp/split_cu_cp_stdout.log /tmp/split_cu_cp.log 2>/dev/null || true
echo "--- CUUP LOG ---"
tail -n 80 /tmp/split_cu_up_stdout.log /tmp/split_cu_up.log 2>/dev/null || true
echo "--- DU LOG ---"
tail -n 100 /tmp/split_du_stdout.log /tmp/split_du.log 2>/dev/null || true
