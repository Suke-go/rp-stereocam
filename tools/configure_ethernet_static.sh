#!/usr/bin/env bash
set -euo pipefail

connection_name="${1:-metapuppet-eth}"
interface_name="${2:-eth0}"
pi_cidr="${3:-192.168.50.2/24}"
gateway="${4:-}"

if ! command -v nmcli >/dev/null 2>&1; then
  echo "nmcli not found; configure Ethernet manually or install NetworkManager." >&2
  exit 1
fi

if nmcli -t -f NAME connection show | grep -Fxq "${connection_name}"; then
  sudo nmcli connection modify "${connection_name}" \
    connection.interface-name "${interface_name}" \
    ipv4.method manual \
    ipv4.addresses "${pi_cidr}" \
    ipv6.method disabled
else
  sudo nmcli connection add type ethernet \
    con-name "${connection_name}" \
    ifname "${interface_name}" \
    ipv4.method manual \
    ipv4.addresses "${pi_cidr}" \
    ipv6.method disabled
fi

if [[ -n "${gateway}" ]]; then
  sudo nmcli connection modify "${connection_name}" ipv4.gateway "${gateway}"
else
  sudo nmcli connection modify "${connection_name}" ipv4.gateway ""
fi

sudo nmcli connection up "${connection_name}"

echo "Ethernet configured: ${interface_name} ${pi_cidr}"
echo "For direct PC link, set the PC Ethernet adapter to 192.168.50.1/24 and use METAPUPPET_PC_HOST=192.168.50.1."
