#!/usr/bin/env bash
set -uo pipefail

rm -f /tmp/split_ue_ping.log

for _ in $(seq 1 25); do
  if ip netns exec ue1 ip addr show tun_srsue 2>/dev/null | grep -q 'inet '; then
    break
  fi
  sleep 1
done

ip netns exec ue1 ip addr show tun_srsue 2>/dev/null || true

if ip netns exec ue1 ip addr show tun_srsue 2>/dev/null | grep -q 'inet '; then
  ip netns exec ue1 ping -I tun_srsue -c 5 -W 2 10.45.0.1 >/tmp/split_ue_ping.log 2>&1 || true
else
  echo "tun_srsue has no IPv4 address" >/tmp/split_ue_ping.log
fi

cat /tmp/split_ue_ping.log
