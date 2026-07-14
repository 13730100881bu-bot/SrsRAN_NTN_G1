#!/bin/bash
# One-shot E2E test: Open5GS + srsRAN gNB (ZMQ) + srsUE
# Run inside WSL Ubuntu-22.04

set -e
SRSRAN=/mnt/d/code/srsRAN_Project-main
CONFIGS=$SRSRAN/configs

echo "=== [1/5] Starting MongoDB ==="
sudo mongod --dbpath /var/lib/mongodb --logpath /var/log/mongodb/mongod.log --fork 2>/dev/null || true
sleep 1

echo "=== [2/5] Starting Open5GS core ==="
sudo open5gs-nrfd  -D
sudo open5gs-scpd  -D
sleep 2
sudo open5gs-amfd  -D
sudo open5gs-smfd  -D
sudo open5gs-upfd  -D
sudo open5gs-ausfd -D
sudo open5gs-udmd  -D
sudo open5gs-udrd  -D
sudo open5gs-pcfd  -D
sudo open5gs-nssfd -D
sudo open5gs-bsfd  -D
sleep 3
echo "Open5GS started."

echo "=== [3/5] Setup UPF TUN ==="
sudo ip tuntap add name ogstun mode tun 2>/dev/null || true
sudo ip addr add 10.45.0.1/16 dev ogstun 2>/dev/null || true
sudo ip link set ogstun up
sudo sysctl -w net.ipv4.ip_forward=1 > /dev/null
sudo iptables -t nat -A POSTROUTING -s 10.45.0.0/16 ! -o ogstun -j MASQUERADE 2>/dev/null || true

echo "=== [4/5] Starting gNB (ZMQ) ==="
sudo $SRSRAN/build/apps/gnb/gnb -c $CONFIGS/gnb_zmq.yml &
GNB_PID=$!
echo "gNB PID: $GNB_PID"
sleep 4

echo "=== [5/5] Starting srsUE ==="
sudo ip netns add ue1 2>/dev/null || true
sudo srsue $CONFIGS/ue_zmq.conf &
UE_PID=$!
echo "srsUE PID: $UE_PID"

echo ""
echo "=============================="
echo " System started. Wait ~10s for UE to attach."
echo " Then test with:"
echo "   sudo ip netns exec ue1 ping -c3 10.45.0.1"
echo ""
echo " Stop all: kill $GNB_PID $UE_PID && sudo pkill open5gs"
echo "=============================="

wait
