#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WAIT_SECS="${1:-35}"

GNB_LOG="/tmp/gnb_ra.log"
GNB_CONSOLE="/tmp/gnb_console_multibeam.log"
UE_LOG="/tmp/ue.log"
UE_CONSOLE="/tmp/srsue_console_multibeam.log"
UE_WRAPPER_PID="/tmp/srsue_wrapper.pid"

stop_stack() {
  if [[ -f "${UE_WRAPPER_PID}" ]]; then
    wrapper_pid="$(cat "${UE_WRAPPER_PID}" 2>/dev/null || true)"
    if [[ -n "${wrapper_pid}" ]]; then
      kill "${wrapper_pid}" 2>/dev/null || true
      pkill -P "${wrapper_pid}" 2>/dev/null || true
    fi
    rm -f "${UE_WRAPPER_PID}"
  fi

  sudo pkill -x srsue 2>/dev/null || true
  sudo pkill -x gnb 2>/dev/null || true
  sleep 2
}

start_gnb() {
  cd "${ROOT_DIR}"
  sudo nohup ./build/apps/gnb/gnb \
    -c configs/gnb_zmq.yml \
    -c configs/gnb_zmq_10mhz.yml \
    -c configs/gnb_zmq_ports209x.yml \
    -c configs/gnb_zmq_prach_cfg1.yml \
    -c configs/gnb_zmq_multibeam.yml \
    -c configs/gnb_zmq_wait_pdu.yml \
    -c configs/gnb_zmq_ra_debug.yml \
    >"${GNB_CONSOLE}" 2>&1 &
}

start_ue() {
  cd "${ROOT_DIR}"
  # srsUE stops when stdin reaches EOF. Keep stdin open for unattended WSL runs.
  (tail -f /dev/null | sudo ./build_srsue_zmq/build/srsue/src/srsue configs/ue_zmq_209x.conf \
    >"${UE_CONSOLE}" 2>&1) &
  echo "$!" >"${UE_WRAPPER_PID}"
}

print_summary() {
  echo "--- processes ---"
  pgrep -a gnb || true
  pgrep -a srsue || true

  echo "--- UE console ---"
  tail -n 100 "${UE_CONSOLE}" 2>/dev/null || true

  echo "--- UE key log ---"
  grep -nE 'Random Access|RRC|PDU Session|Registration|Security|reconfiguration|Setup|Reject|ERROR|RLF|max RETX|SIB1|Cell search|MIB|NAS' \
    "${UE_LOG}" 2>/dev/null | tail -160 || true

  echo "--- gNB key log ---"
  grep -nE 'SSB:|ssbIdx|SIB1|Could not|No valid|PRACH|RACH|RRC|TC-RNTI|C-RNTI|UE|Random Access' \
    "${GNB_LOG}" 2>/dev/null | tail -220 || true
}

main() {
  stop_stack
  rm -f "${GNB_LOG}" "${GNB_CONSOLE}" "${UE_LOG}" "${UE_CONSOLE}"

  start_gnb
  sleep 5
  start_ue
  sleep "${WAIT_SECS}"
  print_summary
}

main "$@"
