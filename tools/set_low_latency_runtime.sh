#!/usr/bin/env bash
set -euo pipefail

for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  [[ -e "${governor}" ]] || continue
  echo performance | sudo tee "${governor}" >/dev/null
done

if command -v vcgencmd >/dev/null 2>&1; then
  vcgencmd get_throttled || true
  vcgencmd measure_temp || true
fi

echo "runtime tuning applied for this boot"
