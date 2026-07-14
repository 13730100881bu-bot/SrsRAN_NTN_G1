#!/bin/bash
# Full E2E 5G stack: Open5GS + UERANSIM gNB + UERANSIM UE
# Run inside WSL Ubuntu-22.04
# Prerequisites: run build_ueransim.sh first

set -e

SRSRAN=/mnt/d/code/srsRAN_Project-main
CONFIGS=$SRSRAN/configs
UERANSIM=/opt/UERANSIM/build

# Verify UERANSIM binaries exist
if [ ! -f "$UERANSIM/nr-gnb" ] || [ ! -f "$UERANSIM/nr-ue" ]; then
    echo "ERROR: UERANSIM not built. Run: bash $CONFIGS/build_ueransim.sh"
    exit 1
fi

cleanup() {
    echo ""
    echo "=== Cleaning up ==="
    [ -n "$GNB_PID" ] && kill "$GNB_PID" 2>/dev/null || true
    [ -n "$UE_PID"  ] && kill "$UE_PID"  2>/dev/null || true
    sudo pkill -f open5gs 2>/dev/null || true
    echo "Done."
}
trap cleanup EXIT INT TERM

echo "=== [1/6] Starting MongoDB ==="
sudo mongod --dbpath /var/lib/mongodb --logpath /var/log/mongodb/mongod.log --fork 2>/dev/null || true
sleep 1

echo "=== [2/6] Starting Open5GS core ==="
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

echo "=== [3/6] Setup UPF TUN interface ==="
sudo ip tuntap add name ogstun mode tun 2>/dev/null || true
sudo ip addr add 10.45.0.1/16 dev ogstun   2>/dev/null || true
sudo ip link set ogstun up
sudo sysctl -w net.ipv4.ip_forward=1 > /dev/null
sudo iptables -t nat -A POSTROUTING -s 10.45.0.0/16 ! -o ogstun -j MASQUERADE 2>/dev/null || true

echo "=== [4/6] Registering subscriber in Open5GS ==="
bash "$CONFIGS/register_subscriber.sh"

echo "=== [5/6] Starting UERANSIM gNB ==="
sudo "$UERANSIM/nr-gnb" -c "$CONFIGS/ueransim_gnb.yaml" > /tmp/ueransim_gnb.log 2>&1 &
GNB_PID=$!
echo "nr-gnb PID: $GNB_PID"
sleep 3

# Check gNB is still running
if ! kill -0 "$GNB_PID" 2>/dev/null; then
    echo "ERROR: nr-gnb crashed! Check /tmp/ueransim_gnb.log"
    cat /tmp/ueransim_gnb.log
    exit 1
fi
echo "nr-gnb running. Log: /tmp/ueransim_gnb.log"

echo "=== [6/6] Starting UERANSIM UE ==="
sudo "$UERANSIM/nr-ue" -c "$CONFIGS/ueransim_ue.yaml" > /tmp/ueransim_ue.log 2>&1 &
UE_PID=$!
echo "nr-ue PID: $UE_PID"
sleep 5

# Check UE is still running
if ! kill -0 "$UE_PID" 2>/dev/null; then
    echo "ERROR: nr-ue crashed! Check /tmp/ueransim_ue.log"
    cat /tmp/ueransim_ue.log
    exit 1
fi

echo ""
echo "=========================================="
echo " Full 5G stack is running!"
echo ""
echo " gNB log : /tmp/ueransim_gnb.log"
echo " UE  log : /tmp/ueransim_ue.log"
echo ""
echo " Wait ~5s then test connectivity:"
echo "   # Ping via UE TUN interface (uesimtun0)"
echo "   ping -I uesimtun0 -c3 10.45.0.1"
echo ""
echo "   # Or run ping via UERANSIM's built-in tool:"
echo "   sudo $UERANSIM/nr-binder $UE_PID ping 10.45.0.1"
echo ""
echo " Check attached interfaces:"
echo "   ip addr show uesimtun0"
echo ""
echo " Stop: Ctrl+C"
echo "=========================================="

wait $GNB_PID $UE_PID
