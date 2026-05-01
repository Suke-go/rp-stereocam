#!/usr/bin/env bash
set -euo pipefail

target_user="${1:-${SUDO_USER:-$USER}}"

if [[ "${target_user}" == "root" || -z "${target_user}" ]]; then
  echo "usage: $0 <normal-user>" >&2
  exit 1
fi

if ! id "${target_user}" >/dev/null 2>&1; then
  echo "user not found: ${target_user}" >&2
  exit 1
fi

groups_to_add=()
for group in video render gpio i2c spi plugdev; do
  if getent group "${group}" >/dev/null 2>&1; then
    groups_to_add+=("${group}")
  fi
done

if ((${#groups_to_add[@]} > 0)); then
  sudo usermod -aG "$(IFS=,; echo "${groups_to_add[*]}")" "${target_user}"
fi

sudo install -d -m 0755 /etc/security/limits.d
sudo tee /etc/security/limits.d/99-metapuppet-lowlatency.conf >/dev/null <<'LIMITS'
@video   - rtprio 20
@video   - nice -10
@render  - rtprio 20
@render  - nice -10
LIMITS

if command -v loginctl >/dev/null 2>&1; then
  sudo loginctl enable-linger "${target_user}" || true
fi

echo "runtime permissions configured for ${target_user}"
echo "groups: ${groups_to_add[*]:-none}"
echo "log out and back in, or reboot, for group membership to apply"
