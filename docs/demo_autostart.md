# MetaPuppet two-Pi demo autostart

## Current branch changes

This branch adds a single integrated dashboard on the camera Pi.

- `tools/mp_control.py` now proxies the motor Pi Flask API under `/api/motor/*`.
- The dashboard includes a Motor Pi panel for status, UDP enable/disable, jog, stop, calibrate, and basic PD config.
- Camera sender startup now reports missing sender binaries as JSON errors instead of crashing the Flask route.
- Network addresses are configurable through environment variables.
- `docs/systemd/` contains draft systemd units for the camera dashboard and motor Web UI.

This setup keeps the camera Pi as the single dashboard:

- Camera Pi: `mp_control.py` on port `7000`
- Motor Pi: `pi_motor_webui.py` on port `8080`
- PC/Unity: receives camera stream on `5004`
- Unity hand tracking: sends motor UDP to motor Pi on `5005`

## Recommended boot shape

Use `systemd` for both Pis. It is a better fit than a hand-written daemon because it restarts failed processes, starts after networking, and exposes logs through `journalctl`.

The camera Pi can have both addresses at the same time:

- `eth0`: fixed demo address, for the PC-to-camera wired link.
- `tailscale0`: Tailscale address, for remote debugging.

They are separate network interfaces. The dashboard binds to `0.0.0.0:7000`, so it is reachable from either address as long as firewall/routing allows it.

Recommended addressing:

- PC Ethernet: `192.168.50.1/24`
- Camera Pi `eth0`: `192.168.50.2/24`
- Motor Pi: Tailscale IP or stable LAN IP, for example `100.65.206.21`

## Camera Pi

First pull this branch onto the camera Pi. This is required until the branch is merged into `main`.

If the branch does not exist locally yet:

```sh
cd /home/pi/MetaPuppet
git fetch origin
git switch -c codex-integrated-demo-dashboard origin/codex-integrated-demo-dashboard
```

If the branch already exists locally:

```sh
cd /home/pi/MetaPuppet
git fetch origin
git switch codex-integrated-demo-dashboard
git pull
```

Confirm the expected commit is present:

```sh
git log -1 --oneline
```

Expected branch/commit:

```text
codex-integrated-demo-dashboard
1ed5dbe Add integrated camera and motor demo dashboard
```

Install Flask if needed:

```sh
python3 -m pip install --user Flask
```

Install and enable the service:

```sh
sudo cp docs/systemd/metapuppet-camera-control.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now metapuppet-camera-control.service
```

Set the motor Pi URL in `/etc/systemd/system/metapuppet-camera-control.service`:

```ini
Environment=MP_MOTOR_BASE_URL=http://100.65.206.21:8080
```

The service also reads `/home/pi/MetaPuppet/tools/metapuppet_pi.env`. Keep these values aligned with the wired demo network:

```sh
METAPUPPET_PC_HOST=192.168.50.1
MP_PI_IP=192.168.50.2
```

Then reload:

```sh
sudo systemctl daemon-reload
sudo systemctl restart metapuppet-camera-control.service
```

Open the integrated dashboard:

```text
http://stereocam.local:7000
```

If mDNS is unavailable, use one of these directly:

```text
http://192.168.50.2:7000
http://<camera-pi-tailscale-ip>:7000
```

## Motor Pi

Deploy `pi_motor_webui.py` from `afcfdd/MetaPuppet-motorPi`.

Install Flask if needed:

```sh
python3 -m pip install --user Flask
```

Install and enable the service:

```sh
sudo cp docs/systemd/metapuppet-motor-webui.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now metapuppet-motor-webui.service
```

## Health checks

Camera Pi:

```sh
curl http://127.0.0.1:7000/api/status
curl http://127.0.0.1:7000/api/motor/status
systemctl status metapuppet-camera-control.service
journalctl -u metapuppet-camera-control.service -f
```

Motor Pi:

```sh
curl http://127.0.0.1:8080/api/status
systemctl status metapuppet-motor-webui.service
journalctl -u metapuppet-motor-webui.service -f
```

## Demo power-on checklist

1. Power on PC, camera Pi, and motor Pi.
2. Wait for both Pis to join the network.
3. Open `http://stereocam.local:7000`.
4. Confirm Camera Health shows both cameras OK.
5. Confirm Motor Pi shows Motor OK and servo rows.
6. Press Camera Start.
7. Enable Motor UDP.
8. Start Unity demo.

The dashboard is intentionally tolerant of missing hardware: if the sender binary or motor Pi is unavailable, it reports the error instead of crashing the whole page.

## Tomorrow's first checks

1. On the camera Pi, pull or switch to `codex-integrated-demo-dashboard`.
2. Confirm the repo path is actually `/home/pi/MetaPuppet`.
3. Confirm the sender binary path:

```sh
ls -l /home/pi/MetaPuppet/build/mps_imt_rpicam_sbs_sender
```

If the binary lives elsewhere, set `MP_SENDER_BIN` in the camera systemd unit.

4. Confirm the wired demo IPs:

```sh
ip addr show eth0
ping -c 2 192.168.50.1
```

5. Confirm Tailscale and the dashboard can coexist:

```sh
ip addr show tailscale0
tailscale ip -4
curl http://127.0.0.1:7000/api/status
```

6. Confirm the integrated motor proxy:

```sh
curl http://127.0.0.1:7000/api/motor/status
```

7. From the PC browser, open both paths if available:

```text
http://192.168.50.2:7000
http://<camera-pi-tailscale-ip>:7000
```

## Likely next work

- Add a saved demo profile so Camera Start uses known-good defaults without re-entering fields.
- Add an optional `--autostart-demo` mode that starts camera streaming after boot once `eth0` and PC ping are healthy.
- Add a top-level "Demo Ready" indicator that combines camera health, Ethernet, sender running, motor reachable, and motor UDP state.
- Decide whether Motor UDP should default to on at boot or require an explicit dashboard click.
- Test the systemd capabilities on the real camera Pi; if `CAP_NET_ADMIN` is not enough for the network fix path, move static `eth0` setup fully into NetworkManager and make the dashboard observe rather than repair it.
