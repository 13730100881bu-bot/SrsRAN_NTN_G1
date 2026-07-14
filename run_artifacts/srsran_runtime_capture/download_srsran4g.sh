#!/usr/bin/env bash
set -u
cd /mnt/d/code || exit 1
rm -rf srsRAN_4G_23_11 srsRAN_4G_23_11.tar.gz
for tag in release_23_11 23.11 v23.11; do
  echo "trying ${tag}"
  url="https://github.com/srsran/srsRAN_4G/archive/refs/tags/${tag}.tar.gz"
  if curl -L --http1.1 --retry 2 --connect-timeout 20 --max-time 180 -o srsRAN_4G_23_11.tar.gz "$url"; then
    if tar -tzf srsRAN_4G_23_11.tar.gz >/dev/null 2>&1; then
      echo "OK ${tag}"
      tar -xzf srsRAN_4G_23_11.tar.gz
      mv srsRAN_4G-* srsRAN_4G_23_11
      exit 0
    fi
  fi
  rm -f srsRAN_4G_23_11.tar.gz
 done
exit 1
