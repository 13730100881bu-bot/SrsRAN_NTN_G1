#!/bin/bash
# Build UERANSIM in WSL Ubuntu-22.04
# Run this script ONCE to install and build UERANSIM
set -e

UERANSIM_DIR=/opt/UERANSIM

echo "=== Installing UERANSIM dependencies ==="
sudo apt-get update -qq
sudo apt-get install -y \
    make gcc g++ libsctp-dev lksctp-tools iproute2 \
    cmake git curl wget

echo "=== Cloning UERANSIM ==="
if [ -d "$UERANSIM_DIR" ]; then
    echo "Already cloned at $UERANSIM_DIR, pulling latest..."
    cd "$UERANSIM_DIR" && sudo git pull
else
    sudo git clone https://github.com/aligungr/UERANSIM "$UERANSIM_DIR"
fi

echo "=== Building UERANSIM ==="
cd "$UERANSIM_DIR"
sudo cmake -B build -DCMAKE_BUILD_TYPE=Release .
sudo cmake --build build --parallel $(nproc)

echo ""
echo "=== UERANSIM build complete ==="
ls -la "$UERANSIM_DIR/build/nr-gnb" "$UERANSIM_DIR/build/nr-ue"
echo ""
echo "Binaries are at:"
echo "  $UERANSIM_DIR/build/nr-gnb"
echo "  $UERANSIM_DIR/build/nr-ue"
