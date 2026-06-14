#!/usr/bin/env python3
"""
MetaPuppet Pi Control Server
Usage : python3 mp_control.py
Browser: http://stereocam.local:7000

pip install flask   (一度だけ)
"""

import base64
import json
import subprocess
import threading
import time
from pathlib import Path

try:
    from flask import Flask, Response, jsonify, request
except ImportError:
    print("Flask が必要です: pip install flask")
    raise

SENDER_BIN = Path.home() / "MetaPuppet/build/mps_imt_rpicam_sbs_sender"
LOG_FILE   = Path("/tmp/mp_sender.log")

# Network config (edit if your addresses differ)
PI_IP  = "192.168.137.2"
PC_IP  = "192.168.137.1"
ETH_IF = "eth0"

app = Flask(__name__)
_proc: subprocess.Popen | None = None
_proc_lock = threading.Lock()
_last_params: dict = {}
_snapshot_lock = threading.Lock()
_net_status: dict = {}        # filled by watchdog
_watchdog_enabled = True


# ── Network helpers ───────────────────────────────────────────────────────────

def _run(args: list, timeout: int = 6) -> tuple[bool, str]:
    """Run command, return (success, output)."""
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return r.returncode == 0, (r.stdout + r.stderr).strip()
    except Exception as e:
        return False, str(e)

def eth0_has_ip() -> bool:
    _, out = _run(["ip", "addr", "show", ETH_IF])
    return PI_IP in out

def fix_eth0() -> str:
    if eth0_has_ip():
        return "already ok"
    _run(["sudo", "ip", "link", "set", ETH_IF, "up"])
    ok, out = _run(["sudo", "ip", "addr", "add", f"{PI_IP}/24", "dev", ETH_IF])
    if not ok and "exists" not in out:
        return f"failed: {out}"
    return "fixed"

def ping_pc() -> bool:
    ok, _ = _run(["ping", "-c", "2", "-W", "1", PC_IP])
    return ok


# ── Watchdog thread ───────────────────────────────────────────────────────────

def _watchdog():
    """Every 10 s: ensure eth0 IP, ping PC, restart sender if needed."""
    global _net_status
    while True:
        try:
            eth_ok  = eth0_has_ip()
            if not eth_ok:
                fix_eth0()
                eth_ok = eth0_has_ip()

            ping_ok = ping_pc()
            sender_ok = proc_running()

            if not sender_ok and _last_params and _watchdog_enabled:
                start_sender(_last_params)
                sender_ok = proc_running()

            _net_status = {
                "eth0":   "ok" if eth_ok  else "missing",
                "ping":   ping_ok,
                "sender": sender_ok,
                "ts":     int(time.time()),
            }
        except Exception:
            pass
        time.sleep(10)

threading.Thread(target=_watchdog, daemon=True).start()


# ── Sender process management ─────────────────────────────────────────────────

def proc_running() -> bool:
    global _proc
    with _proc_lock:
        if _proc is None:
            return False
        if _proc.poll() is not None:
            _proc = None
            return False
        return True

def start_sender(p: dict) -> int:
    global _proc, _last_params
    _last_params = p
    args = [
        str(SENDER_BIN),
        p.get("host", "192.168.137.1"),
        str(p.get("port", 5004)),
        str(p.get("eye_w", 640)),
        str(p.get("eye_h", 360)),
        str(p.get("fps", 30)),
        str(p.get("left_cam", 0)),
        str(p.get("right_cam", 1)),
        str(p.get("left_xform", 0)),
        str(p.get("right_xform", 0)),
    ]
    with _proc_lock:
        if _proc and _proc.poll() is None:
            _proc.terminate()
            try:    _proc.wait(timeout=3)
            except: _proc.kill()
        LOG_FILE.write_text("")
        _proc = subprocess.Popen(args,
                                 stdout=LOG_FILE.open("w"),
                                 stderr=subprocess.STDOUT)
        return _proc.pid

def stop_sender():
    global _proc
    with _proc_lock:
        if _proc:
            _proc.terminate()
            try:    _proc.wait(timeout=3)
            except: _proc.kill()
            _proc = None


# ── API ───────────────────────────────────────────────────────────────────────

@app.get("/api/status")
def api_status():
    temp = load = ""
    mem  = {}
    try:
        temp = subprocess.check_output(
            ["vcgencmd", "measure_temp"], stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        pass
    try:
        parts = subprocess.check_output(["free", "-m"]).decode().split("\n")[1].split()
        mem = {"used": int(parts[2]), "total": int(parts[1])}
    except Exception:
        pass
    try:
        load = " ".join(Path("/proc/loadavg").read_text().split()[:3])
    except Exception:
        pass
    return jsonify({
        "running": proc_running(),
        "pid":     _proc.pid if proc_running() else None,
        "temp":    temp,
        "mem":     mem,
        "load":    load,
    })

@app.post("/api/start")
def api_start():
    pid = start_sender(request.get_json(force=True) or {})
    return jsonify({"ok": True, "pid": pid})

@app.post("/api/stop")
def api_stop():
    stop_sender()
    return jsonify({"ok": True})

@app.get("/api/cameras")
def api_cameras():
    """List detected cameras and do a quick health check on each."""
    # 1. List cameras
    list_out = ""
    try:
        r = subprocess.run(
            ["rpicam-vid", "--list-cameras"],
            capture_output=True, text=True, timeout=6
        )
        list_out = (r.stdout + r.stderr).strip()
    except Exception as e:
        list_out = f"rpicam-vid --list-cameras failed: {e}"

    # 2. Health check each camera (1 frame, short timeout)
    health = {}
    was_running = proc_running()
    if was_running:
        stop_sender()
        time.sleep(0.4)

    for cam in (0, 1):
        path = f"/tmp/mp_camcheck{cam}.yuv"
        try:
            r = subprocess.run(
                ["rpicam-vid",
                 "--camera", str(cam),
                 "--timeout", "1500",
                 "--frames", "1",
                 "--codec", "yuv420",
                 "--output", path,
                 "--nopreview"],
                capture_output=True, text=True, timeout=8
            )
            ok = Path(path).exists() and Path(path).stat().st_size > 0
            health[f"cam{cam}"] = {
                "ok": ok,
                "msg": "OK" if ok else "no output",
                "stderr": (r.stderr or "").strip()[-200:],
            }
        except Exception as e:
            health[f"cam{cam}"] = {"ok": False, "msg": str(e), "stderr": ""}

    if was_running and _last_params:
        time.sleep(0.2)
        start_sender(_last_params)

    return jsonify({"list": list_out, "health": health})

# ── Network autocheck / fix ───────────────────────────────────────────────────

@app.get("/api/net/autocheck")
def api_net_autocheck():
    eth_ok  = eth0_has_ip()
    ping_ok = ping_pc()
    _, ifout = _run(["ip", "-br", "addr", "show", ETH_IF])
    return jsonify({
        "eth0_ok":  eth_ok,
        "eth0_ip":  ifout,
        "ping_ok":  ping_ok,
        "sender":   proc_running(),
        "status":   _net_status,
    })

@app.post("/api/net/fix_eth0")
def api_net_fix_eth0():
    result = fix_eth0()
    return jsonify({"result": result, "eth0_ok": eth0_has_ip()})

@app.post("/api/net/fix_all")
def api_net_fix_all():
    """Fix eth0 → restart sender → report."""
    eth_result = fix_eth0()
    time.sleep(0.5)
    ping_ok = ping_pc()
    sender_ok = proc_running()
    if not sender_ok and _last_params:
        start_sender(_last_params)
        time.sleep(1)
        sender_ok = proc_running()
    return jsonify({
        "eth0":   eth_result,
        "ping":   ping_ok,
        "sender": sender_ok,
    })

@app.get("/api/net/eth0_detail")
def api_net_eth0_detail():
    """Full eth0 status: link state, IP, carrier."""
    _, link  = _run(["ip", "-s", "link", "show", ETH_IF])
    _, addr  = _run(["ip", "addr", "show", ETH_IF])
    _, conns = _run(["nmcli", "-t", "con", "show", "--active"])
    return jsonify({"link": link, "addr": addr, "nmcli_active": conns})

@app.post("/api/net/setup_static")
def api_net_setup_static():
    """Permanently set eth0 static IP via nmcli (survives reboot)."""
    # Find the connection name for eth0
    _, out = _run(["nmcli", "-t", "-f", "NAME,DEVICE", "con", "show"])
    con_name = None
    for line in out.splitlines():
        if ETH_IF in line:
            con_name = line.split(":")[0]
            break

    if not con_name:
        # Create a new connection
        ok, out = _run([
            "sudo", "nmcli", "con", "add",
            "type", "ethernet", "ifname", ETH_IF,
            "con-name", "mp-eth0-static",
            "ipv4.method", "manual",
            "ipv4.addresses", f"{PI_IP}/24",
            "ipv4.gateway", "",
            "ipv6.method", "disabled",
        ])
        con_name = "mp-eth0-static"
    else:
        ok, out = _run([
            "sudo", "nmcli", "con", "mod", con_name,
            "ipv4.method", "manual",
            "ipv4.addresses", f"{PI_IP}/24",
            "ipv4.gateway", "",
            "ipv6.method", "ignore",
        ])

    if not ok:
        return jsonify({"ok": False, "detail": out})

    # Bring up
    _, up_out = _run(["sudo", "nmcli", "con", "up", con_name])
    time.sleep(1)

    ping_ok = ping_pc()
    return jsonify({
        "ok": True,
        "con_name": con_name,
        "eth0_ok": eth0_has_ip(),
        "ping_ok": ping_ok,
        "detail": up_out,
    })


# ── WiFi management ──────────────────────────────────────────────────────────

def _get_all_ips() -> list[dict]:
    """Return list of {iface, ip} for all active interfaces."""
    _, out = _run(["ip", "-br", "addr", "show"])
    results = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] == "UP":
            iface = parts[0]
            for addr in parts[2:]:
                ip = addr.split("/")[0]
                if ":" not in ip:  # skip IPv6
                    results.append({"iface": iface, "ip": ip})
    return results


@app.get("/api/wifi/status")
def api_wifi_status():
    _, wlan_addr = _run(["ip", "-br", "addr", "show", "wlan0"])
    wlan_ip = None
    if "UP" in wlan_addr:
        parts = wlan_addr.split()
        for p in parts[2:]:
            ip = p.split("/")[0]
            if ":" not in ip:
                wlan_ip = ip
                break
    _, ssid_out = _run(["nmcli", "-t", "-f", "ACTIVE,SSID", "dev", "wifi"])
    ssid = None
    for line in ssid_out.splitlines():
        if line.startswith("yes:"):
            ssid = line.split(":", 1)[1]
            break
    return jsonify({
        "wlan_ip": wlan_ip,
        "ssid": ssid,
        "all_ips": _get_all_ips(),
    })


@app.get("/api/wifi/scan")
def api_wifi_scan():
    """Scan for available WiFi networks (rescan first)."""
    _run(["nmcli", "dev", "wifi", "rescan"], timeout=8)
    _, out = _run(["nmcli", "-f", "SSID,SIGNAL,SECURITY", "dev", "wifi", "list"], timeout=8)
    return jsonify({"output": out})


@app.post("/api/wifi/connect")
def api_wifi_connect():
    """Connect to a WiFi network. Body: {ssid, password}"""
    body = request.get_json(force=True, silent=True) or {}
    ssid = body.get("ssid", "")
    pw   = body.get("password", "")
    if not ssid:
        return jsonify({"ok": False, "error": "ssid required"}), 400

    args = ["sudo", "nmcli", "dev", "wifi", "connect", ssid]
    if pw:
        args += ["password", pw]
    ok, out = _run(args, timeout=30)
    time.sleep(2)

    status = api_wifi_status().get_json()
    return jsonify({"ok": ok, "detail": out, **status})


@app.post("/api/wifi/disconnect")
def api_wifi_disconnect():
    _run(["sudo", "nmcli", "dev", "disconnect", "wlan0"])
    return jsonify({"ok": True})


# ── Network diagnostics ───────────────────────────────────────────────────────

_NET_CMDS: dict[str, list[str]] = {
    "tcpdump":   ["sudo", "timeout", "6", "tcpdump", "-i", "eth0", "-n",
                  "-l", "udp", "port", "5004", "-c", "40"],
    "ifinfo":    ["ip", "-br", "addr", "show"],
    "route":     ["ip", "route"],
    "ss_udp":    ["ss", "-ulnp"],
    "ping":      ["ping", "-c", "4", "-W", "1", "192.168.137.1"],
    "send_test": ["bash", "-c",
                  "echo -n 'MPTEST' | nc -u -w1 192.168.137.1 5004 && echo 'sent OK'"],
    "neigh":     ["ip", "neigh", "show"],
}


@app.post("/api/net/send_burst")
def api_net_send_burst():
    """Send 120 UDP packets to PC:5004 — use while Unity is in Play mode to test FW/DLL."""
    import socket as _sock
    host = request.get_json(force=True).get("host", PC_IP) if request.content_length else PC_IP
    port = 5004
    sent = 0
    try:
        s = _sock.socket(_sock.AF_INET, _sock.SOCK_DGRAM)
        # Send 120 raw UDP packets (2 seconds worth at 60fps).
        # They won't have IMT magic, so DLL will count them as drops,
        # but packets_received will still increment — enough to confirm delivery.
        payload = b"MPUDPTEST" * 16   # 144 bytes
        for _ in range(120):
            s.sendto(payload, (host, port))
            sent += 1
        s.close()
        return jsonify({"ok": True, "sent": sent, "host": host, "port": port})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e), "sent": sent})

@app.get("/api/net/<cmd>")
def api_net(cmd: str):
    if cmd not in _NET_CMDS:
        return jsonify({"error": "unknown"}), 400
    args = _NET_CMDS[cmd]

    # tcpdump streams line-by-line via SSE
    if cmd == "tcpdump":
        def generate():
            try:
                p = subprocess.Popen(args, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
                for line in iter(p.stdout.readline, ""):
                    yield f"data: {json.dumps(line.rstrip())}\n\n"
                p.wait()
            except Exception as e:
                yield f"data: {json.dumps(f'[error] {e}')}\n\n"
            yield "data: \"--- done ---\"\n\n"
        return Response(generate(), mimetype="text/event-stream",
                        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})

    # others: run and return
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=10)
        return jsonify({"output": (r.stdout + r.stderr).strip()})
    except Exception as e:
        return jsonify({"error": str(e)})


@app.get("/api/snapshot")
def api_snapshot():
    """Briefly stop sender, grab one JPEG per camera, restart sender."""
    if not _snapshot_lock.acquire(blocking=False):
        return jsonify({"error": "snapshot in progress"}), 429

    try:
        was_running = proc_running()
        if was_running:
            stop_sender()
            time.sleep(0.4)   # wait for libcamera to release cameras

        images = {}
        # Capture each camera sequentially (libcamera won't allow parallel access)
        for cam in (0, 1):
            path = f"/tmp/mp_preview{cam}.jpg"
            try:
                subprocess.run(
                    ["rpicam-still",
                     "--camera", str(cam),
                     "--output", path,
                     "--timeout", "300",
                     "--width", "320", "--height", "240",
                     "--nopreview"],
                    timeout=6, capture_output=True
                )
                with open(path, "rb") as f:
                    images[f"cam{cam}"] = (
                        "data:image/jpeg;base64,"
                        + base64.b64encode(f.read()).decode()
                    )
            except Exception as e:
                images[f"cam{cam}"] = None
                images[f"cam{cam}_err"] = str(e)

        if was_running and _last_params:
            time.sleep(0.2)
            start_sender(_last_params)

        return jsonify(images)
    finally:
        _snapshot_lock.release()


@app.get("/api/log/stream")
def api_log_stream():
    """Server-Sent Events — streams sender log lines in real-time."""
    def generate():
        f = None
        while True:
            if f is None:
                if LOG_FILE.exists():
                    f = LOG_FILE.open("r")
                    f.seek(0, 2)   # start from tail
                else:
                    yield ": heartbeat\n\n"
                    time.sleep(0.5)
                    continue
            line = f.readline()
            if line:
                yield f"data: {json.dumps(line.rstrip())}\n\n"
            else:
                yield ": heartbeat\n\n"
                time.sleep(0.1)
    return Response(
        generate(),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


# ── Frontend HTML ─────────────────────────────────────────────────────────────

_HTML = r"""<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MetaPuppet Pi Control</title>
<style>
*{box-sizing:border-box}
body{font-family:monospace;background:#0e0e0e;color:#ccc;margin:0;padding:16px;font-size:13px}
h1{color:#3af;margin:0 0 14px;font-size:17px;letter-spacing:1px}
.card{background:#161616;border:1px solid #2a2a2a;border-radius:8px;padding:14px;margin-bottom:12px}
.row{display:flex;gap:8px;align-items:center;margin:5px 0;flex-wrap:wrap}
label{color:#888;min-width:78px}
input[type=number],input[type=text],select{
  background:#1c1c1c;border:1px solid #3a3a3a;color:#ddd;
  padding:4px 7px;border-radius:4px;font-family:monospace}
input[type=number]{width:72px}
input[type=text]{width:150px}
select{width:128px}
button{padding:6px 14px;border-radius:4px;border:none;cursor:pointer;font-family:monospace;font-weight:bold;font-size:12px}
.btn-start{background:#0b5c30;color:#4f4}
.btn-stop{background:#5c0b0b;color:#f44}
.btn-restart{background:#5c3500;color:#fa4}
.btn-sm{background:#252525;color:#aaa}
.badge{padding:3px 10px;border-radius:12px;font-weight:bold;display:inline-block;font-size:12px}
.badge.running{background:#0b3d1f;color:#4f4;border:1px solid #2a6b3a}
.badge.stopped{background:#222;color:#888;border:1px solid #333}
.sysinfo{display:flex;gap:16px;color:#666;font-size:11px;flex-wrap:wrap}
.log{background:#000;border:1px solid #222;border-radius:4px;
     padding:8px;height:240px;overflow-y:auto;font-size:11px;
     color:#0d0;white-space:pre-wrap;word-break:break-all;line-height:1.4}
.sec{color:#666;font-size:10px;text-transform:uppercase;letter-spacing:1px;margin:10px 0 4px}
.cmd{color:#555;font-size:10px;margin-top:6px;word-break:break-all}
</style>
</head>
<body>
<h1>MetaPuppet Pi Control</h1>

<div class="card">
  <div class="row">
    <span id="badge" class="badge stopped">● Stopped</span>
    <span id="pid" style="color:#555;font-size:11px"></span>
  </div>
  <div class="sysinfo" style="margin-top:6px">
    <span id="temp"></span><span id="mem"></span><span id="load"></span>
  </div>
</div>

<div class="card">
  <div class="sec">送信先</div>
  <div class="row">
    <label>Target IP</label>
    <input type="text" id="host" value="192.168.137.1" style="width:130px">
    <select id="host_preset" onchange="if(this.value){$('host').value=this.value;updatePreview();}" style="width:110px">
      <option value="">-- 検出IP --</option>
    </select>
    <button class="btn-sm" onclick="detectIps()" style="font-size:11px">🔍 IP検出</button>
    <label style="margin-left:6px">Port</label>
    <input type="number" id="port" value="5004" style="width:62px">
  </div>

  <div class="sec">解像度 / FPS</div>
  <div class="row">
    <label>解像度</label>
    <select id="res_preset" onchange="applyResPreset()">
      <option value="640,360">640×360  (低負荷・高FPS)</option>
      <option value="1280,720">1280×720  (HD)</option>
      <option value="1920,1080">1920×1080  (Full HD)</option>
      <option value="2304,1296">2304×1296  (IMX708 高解像度)</option>
      <option value="custom">カスタム…</option>
    </select>
  </div>
  <div class="row" id="custom-res-row" style="display:none">
    <label>W × H</label>
    <input type="number" id="eye_w" value="640">
    <span style="color:#555">×</span>
    <input type="number" id="eye_h" value="360">
  </div>
  <div class="row">
    <label>FPS</label>
    <input type="number" id="fps" value="30" style="width:55px">
    <span style="color:#555;font-size:11px" id="fps-hint"></span>
  </div>

  <div class="sec">カメラ割り当て / Transform</div>
  <div class="row">
    <label>Left</label>
    <select id="left_cam"><option value="0">cam0</option><option value="1">cam1</option></select>
    <select id="left_xform">
      <option value="0">None</option><option value="1">Flip H</option>
      <option value="2">Flip V</option><option value="3">Rotate 180°</option>
    </select>
  </div>
  <div class="row">
    <label>Right</label>
    <select id="right_cam"><option value="0">cam0</option><option value="1" selected>cam1</option></select>
    <select id="right_xform">
      <option value="0">None</option><option value="1">Flip H</option>
      <option value="2">Flip V</option><option value="3">Rotate 180°</option>
    </select>
  </div>

  <div class="cmd" id="cmd-preview"></div>

  <div class="row" style="margin-top:10px;gap:6px">
    <button class="btn-start"   onclick="ctrlStart()">▶ Start</button>
    <button class="btn-stop"    onclick="ctrlStop()">■ Stop</button>
    <button class="btn-restart" onclick="ctrlRestart()">↺ Restart</button>
  </div>

  <div class="sec">プリセット</div>
  <div class="row" style="gap:5px">
    <button class="btn-sm" onclick="setPreset(640,360,30,0)">標準 30fps</button>
    <button class="btn-sm" onclick="setPreset(640,360,60,0)">60fps</button>
    <button class="btn-sm" onclick="setPreset(640,360,90,0)">90fps</button>
    <button class="btn-sm" onclick="setXform(3)">180°反転</button>
    <button class="btn-sm" onclick="setXform(1)">水平反転</button>
    <button class="btn-sm" onclick="setXform(0)">リセット</button>
  </div>
</div>

<div class="card">
  <div class="row" style="margin-bottom:6px">
    <span class="sec" style="margin:0">Camera Health</span>
    <button class="btn-sm" onclick="checkCameras()" id="health-btn" style="margin-left:auto;font-size:11px">🔍 Check</button>
  </div>
  <div id="cam-list" style="color:#555;font-size:10px;white-space:pre-wrap;margin-bottom:6px"></div>
  <div style="display:flex;gap:12px;flex-wrap:wrap">
    <div id="health0" style="font-size:12px;padding:6px 10px;background:#1a1a1a;border-radius:4px;border:1px solid #2a2a2a">cam0 —</div>
    <div id="health1" style="font-size:12px;padding:6px 10px;background:#1a1a1a;border-radius:4px;border:1px solid #2a2a2a">cam1 —</div>
  </div>
</div>

<div class="card">
  <div class="sec" style="margin-top:0">WiFi</div>
  <div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:6px">
    <div id="wifi-ssid" class="badge stopped" style="font-size:11px">WiFi —</div>
    <div id="wifi-ip"   style="color:#666;font-size:11px;padding:3px 0">wlan0 —</div>
  </div>
  <div class="row" style="gap:5px;flex-wrap:wrap">
    <input type="text" id="wifi_ssid" placeholder="SSID" style="width:130px">
    <input type="text" id="wifi_pw"   placeholder="パスワード" style="width:110px">
    <button class="btn-sm" onclick="wifiConnect()" style="background:#1a2a3a;color:#7af;border:1px solid #2a3a5a">接続</button>
    <button class="btn-sm" onclick="wifiScan()">スキャン</button>
    <button class="btn-sm" onclick="wifiDisconnect()">切断</button>
  </div>
  <div id="wifi-log" style="color:#555;font-size:10px;margin-top:4px;white-space:pre-wrap;max-height:80px;overflow-y:auto"></div>
</div>

<div class="card">
  <div class="row" style="margin-bottom:8px;align-items:center">
    <span class="sec" style="margin:0">Network (Ethernet)</span>
    <button onclick="netAutoCheck()" style="margin-left:auto;background:#1a3a5c;color:#7af;border:none;border-radius:4px;padding:5px 12px;cursor:pointer;font-family:monospace;font-size:12px">⟳ Auto Check</button>
    <button onclick="netFixAll()"   style="margin-left:6px;background:#3a1a1a;color:#f77;border:none;border-radius:4px;padding:5px 12px;cursor:pointer;font-family:monospace;font-size:12px">🔧 Fix All</button>
  </div>

  <!-- Status badges -->
  <div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px">
    <div id="net-eth0"   class="badge stopped" style="font-size:11px">eth0 —</div>
    <div id="net-ping"   class="badge stopped" style="font-size:11px">Ping PC —</div>
    <div id="net-sender" class="badge stopped" style="font-size:11px">Sender —</div>
  </div>

  <!-- Manual tools -->
  <div class="row" style="gap:5px;flex-wrap:wrap">
    <button class="btn-sm" onclick="setupStatic()" style="background:#1a2a1a;color:#4f4;border:1px solid #2a4a2a">📌 Static IP 永続設定</button>
    <button class="btn-sm" onclick="eth0Detail()">eth0 詳細</button>
    <button class="btn-sm" onclick="netRun('tcpdump')">tcpdump 6s</button>
    <button class="btn-sm" onclick="netFetch('ping')">Ping PC</button>
    <button class="btn-sm" onclick="netFetch('send_test')">Test Send 1pkt</button>
    <button class="btn-sm" onclick="netFetch('ss_udp')">UDP Sockets</button>
    <button class="btn-sm" onclick="netFetch('neigh')">ARP Table</button>
    <button class="btn-sm" onclick="sendBurst()" style="background:#1a1a2a;color:#8af;border:1px solid #2a2a4a">▶ UDP Burst→ターゲット:5004</button>
  </div>
  <div class="log" id="net-log" style="height:150px;margin-top:6px"></div>
</div>

<div class="card">
  <div class="row" style="margin-bottom:6px">
    <span class="sec" style="margin:0">Camera Preview</span>
    <button class="btn-sm" onclick="takeSnapshot()" id="snap-btn" style="margin-left:auto;font-size:11px">📷 Snapshot</button>
  </div>
  <div id="snap-status" style="color:#666;font-size:11px;margin-bottom:6px"></div>
  <div style="display:flex;gap:8px;flex-wrap:wrap">
    <div>
      <div style="color:#555;font-size:10px;margin-bottom:3px">cam0 (Left)</div>
      <img id="prev0" style="width:320px;height:180px;background:#111;border:1px solid #222;border-radius:4px;object-fit:contain;display:block">
    </div>
    <div>
      <div style="color:#555;font-size:10px;margin-bottom:3px">cam1 (Right)</div>
      <img id="prev1" style="width:320px;height:180px;background:#111;border:1px solid #222;border-radius:4px;object-fit:contain;display:block">
    </div>
  </div>
</div>

<div class="card">
  <div class="row" style="margin-bottom:4px">
    <span class="sec" style="margin:0">Sender Log</span>
    <button class="btn-sm" onclick="clearLog()" style="margin-left:auto;font-size:10px">Clear</button>
  </div>
  <div class="log" id="log"></div>
</div>

<script>
const $=id=>document.getElementById(id);

// Resolution presets: [w, h, maxFps, hint]
const RES_PRESETS={
  '640,360':  [640,  360,  120, '最大120fps'],
  '1280,720': [1280, 720,  60,  '最大60fps'],
  '1920,1080':[1920, 1080, 30,  '最大30fps'],
  '2304,1296':[2304, 1296, 14,  '最大14fps・高負荷'],
};

function applyResPreset(){
  const v=$('res_preset').value;
  const row=$('custom-res-row');
  if(v==='custom'){row.style.display='flex';return;}
  row.style.display='none';
  const [w,h,maxFps,hint]=RES_PRESETS[v];
  $('eye_w').value=w; $('eye_h').value=h;
  if(+$('fps').value>maxFps) $('fps').value=maxFps;
  $('fps-hint').textContent=hint;
  updatePreview();
}

function params(){
  return{
    host:$('host').value, port:+$('port').value,
    eye_w:+$('eye_w').value, eye_h:+$('eye_h').value, fps:+$('fps').value,
    left_cam:+$('left_cam').value, right_cam:+$('right_cam').value,
    left_xform:+$('left_xform').value, right_xform:+$('right_xform').value
  };
}
function updatePreview(){
  const p=params();
  $('cmd-preview').textContent=
    `$ mps_imt_rpicam_sbs_sender ${p.host} ${p.port} ${p.eye_w} ${p.eye_h} ${p.fps} ${p.left_cam} ${p.right_cam} ${p.left_xform} ${p.right_xform}`;
}
['fps','eye_w','eye_h','host','port','left_cam','right_cam','left_xform','right_xform']
  .forEach(id=>{const el=$(id);if(el)el.addEventListener('input',updatePreview);});
applyResPreset(); updatePreview();

async function ctrlStart(){
  await fetch('/api/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(params())});
}
async function ctrlStop(){await fetch('/api/stop',{method:'POST'});}
async function ctrlRestart(){await ctrlStop();setTimeout(ctrlStart,600);}

function setXform(x){$('left_xform').value=x;$('right_xform').value=x;updatePreview();}

async function checkCameras(){
  const btn=$('health-btn');
  btn.disabled=true; btn.textContent='⏳ Checking…';
  $('cam-list').textContent=''; $('health0').textContent='cam0 …'; $('health1').textContent='cam1 …';
  try{
    const d=await(await fetch('/api/cameras')).json();
    $('cam-list').textContent=d.list||'';
    [0,1].forEach(i=>{
      const h=d.health[`cam${i}`];
      const el=$(`health${i}`);
      const box=$('health'+i);
      if(h.ok){
        box.textContent=`cam${i} ✓ OK`;
        box.style.borderColor='#2a6b3a'; box.style.color='#4f4';
      } else {
        box.textContent=`cam${i} ✗ ${h.msg}`;
        box.style.borderColor='#6b2a2a'; box.style.color='#f44';
        if(h.stderr) box.title=h.stderr;
      }
    });
  }catch(e){$('cam-list').textContent='Error: '+e;}
  btn.disabled=false; btn.textContent='🔍 Check';
}

// Status polling
async function poll(){
  try{
    const d=await(await fetch('/api/status')).json();
    const b=$('badge');
    if(d.running){b.textContent='● Running';b.className='badge running';$('pid').textContent='PID '+d.pid;}
    else{b.textContent='● Stopped';b.className='badge stopped';$('pid').textContent='';}
    $('temp').textContent=d.temp||'';
    $('mem').textContent=d.mem&&d.mem.total?`Mem ${d.mem.used}/${d.mem.total} MB`:'';
    $('load').textContent=d.load?`Load ${d.load}`:'';
  }catch(e){}
}
setInterval(poll,2000);poll();

// Log SSE
const logEl=$('log');
const es=new EventSource('/api/log/stream');
es.onmessage=e=>{
  const line=JSON.parse(e.data);
  logEl.textContent+=line+'\n';
  logEl.scrollTop=logEl.scrollHeight;
};
function clearLog(){logEl.textContent='';}

// Network diagnostics
let _netEs = null;
function netLog(msg){ const el=$('net-log'); el.textContent+=msg+'\n'; el.scrollTop=el.scrollHeight; }
function netClear(){ $('net-log').textContent=''; }

function applyNetStatus(d){
  function badge(id, ok, label){
    const el=$(id); el.textContent=label;
    el.className='badge '+(ok?'running':'stopped');
  }
  badge('net-eth0',   d.eth0_ok!==false, 'eth0 '+(d.eth0_ok ? '✓ '+PI_IP : '✗ missing'));
  badge('net-ping',   d.ping_ok,         'Ping PC '+(d.ping_ok ? '✓' : '✗'));
  badge('net-sender', d.sender,          'Sender '+(d.sender ? '✓' : '✗'));
}

async function netAutoCheck(){
  netClear(); netLog('> autocheck…');
  try{
    const d=await(await fetch('/api/net/autocheck')).json();
    netLog('eth0 : '+(d.eth0_ok ? '✓ ok' : '✗ missing')+' → '+d.eth0_ip);
    netLog('ping : '+(d.ping_ok ? '✓ reachable' : '✗ unreachable'));
    netLog('sender: '+(d.sender ? '✓ running' : '✗ stopped'));
    applyNetStatus(d);
  }catch(e){netLog('[error] '+e);}
}

async function netFixAll(){
  netClear(); netLog('> fix all…');
  try{
    const d=await(await fetch('/api/net/fix_all',{method:'POST'})).json();
    netLog('eth0  : '+d.eth0);
    netLog('ping  : '+(d.ping ? '✓' : '✗'));
    netLog('sender: '+(d.sender ? '✓ running' : '✗ failed to start'));
    applyNetStatus({eth0_ok: d.eth0!=='failed', ping_ok: d.ping, sender: d.sender});
  }catch(e){netLog('[error] '+e);}
}

const PI_IP = '192.168.137.2';

async function setupStatic(){
  netClear(); netLog('> nmcli static IP 永続設定中…');
  try{
    const d=await(await fetch('/api/net/setup_static',{method:'POST'})).json();
    if(d.ok){
      netLog('✓ 設定完了: '+d.con_name);
      netLog('eth0: '+(d.eth0_ok?'✓ '+PI_IP:'✗ 未取得'));
      netLog('ping: '+(d.ping_ok?'✓':'✗'));
      netLog(d.detail||'');
      applyNetStatus({eth0_ok:d.eth0_ok, ping_ok:d.ping_ok, sender:false});
    }else{
      netLog('✗ 失敗: '+d.detail);
    }
  }catch(e){netLog('[error] '+e);}
}

async function eth0Detail(){
  netClear(); netLog('> eth0 詳細情報…');
  try{
    const d=await(await fetch('/api/net/eth0_detail')).json();
    netLog('=== link ===\n'+d.link);
    netLog('=== addr ===\n'+d.addr);
    netLog('=== nmcli active ===\n'+d.nmcli_active);
  }catch(e){netLog('[error] '+e);}
}

// Auto-check on load and every 15s
setInterval(()=>fetch('/api/net/autocheck').then(r=>r.json()).then(applyNetStatus).catch(()=>{}), 15000);
setTimeout(()=>fetch('/api/net/autocheck').then(r=>r.json()).then(applyNetStatus).catch(()=>{}), 1000);
// WiFi status on load and every 30s
wifiStatus();
setInterval(wifiStatus, 30000);

function netRun(cmd, label){
  if(_netEs){ _netEs.close(); _netEs=null; }
  netClear(); netLog('> '+cmd);
  _netEs = new EventSource('/api/net/'+cmd);
  _netEs.onmessage = e => {
    const line = JSON.parse(e.data);
    if(line==='--- done ---'){ _netEs.close(); _netEs=null; }
    else netLog(line);
  };
  _netEs.onerror = () => { netLog('[SSE error]'); _netEs.close(); _netEs=null; };
}

async function netFetch(cmd){
  netClear(); netLog('> '+cmd);
  try{
    const d = await(await fetch('/api/net/'+cmd)).json();
    netLog(d.output || d.error || JSON.stringify(d));
  }catch(e){ netLog('[error] '+e); }
}

async function sendBurst(){
  const host=$('host').value||'192.168.137.1';
  netClear(); netLog(`> UDP Burst: 120パケットを ${host}:5004 へ送信中…\n(Unity Play中でpkts_receivedが増えればFW/DLL正常)`);
  try{
    const d=await(await fetch('/api/net/send_burst',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({host})})).json();
    if(d.ok) netLog(`✓ sent ${d.sent} packets to ${d.host}:${d.port}`);
    else netLog('[error] '+d.error);
  }catch(e){netLog('[error] '+e);}
}

// WiFi
async function wifiStatus(){
  try{
    const d=await(await fetch('/api/wifi/status')).json();
    const ssidEl=$('wifi-ssid'), ipEl=$('wifi-ip'), preset=$('host_preset');
    if(d.ssid){
      ssidEl.className='badge running'; ssidEl.textContent='WiFi: '+d.ssid;
    }else{
      ssidEl.className='badge stopped'; ssidEl.textContent='WiFi: 未接続';
    }
    ipEl.textContent = d.wlan_ip ? 'wlan0: '+d.wlan_ip : 'wlan0: —';
    // Populate IP preset dropdown
    if(d.all_ips && d.all_ips.length){
      preset.innerHTML='<option value="">-- 検出IP --</option>';
      d.all_ips.forEach(e=>{
        preset.innerHTML+=`<option value="${e.ip}">${e.iface}: ${e.ip}</option>`;
      });
    }
    if(d.wlan_ip) $('wifi-log').textContent='wlan0: '+d.wlan_ip+(d.ssid?' ('+d.ssid+')':'');
  }catch{}
}

async function detectIps(){
  $('wifi-log').textContent='検出中…';
  await wifiStatus();
}

async function wifiScan(){
  $('wifi-log').textContent='スキャン中…';
  try{
    const d=await(await fetch('/api/wifi/scan')).json();
    $('wifi-log').textContent=d.output||d.error||'(empty)';
  }catch(e){$('wifi-log').textContent='[error] '+e;}
}

async function wifiConnect(){
  const ssid=$('wifi_ssid').value.trim(), pw=$('wifi_pw').value;
  if(!ssid){alert('SSIDを入力してください');return;}
  $('wifi-log').textContent='接続中: '+ssid+'…';
  try{
    const d=await(await fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,password:pw})})).json();
    $('wifi-log').textContent=(d.ok?'✓ 接続成功':'✗ 接続失敗')+'\n'+d.detail;
    if(d.wlan_ip){
      $('wifi-ssid').className='badge running'; $('wifi-ssid').textContent='WiFi: '+d.ssid;
      $('wifi-ip').textContent='wlan0: '+d.wlan_ip;
    }
    wifiStatus();
  }catch(e){$('wifi-log').textContent='[error] '+e;}
}

async function wifiDisconnect(){
  await fetch('/api/wifi/disconnect',{method:'POST'});
  $('wifi-ssid').className='badge stopped'; $('wifi-ssid').textContent='WiFi: 未接続';
  $('wifi-ip').textContent='wlan0: —';
  $('wifi-log').textContent='切断しました';
}

async function takeSnapshot(){
  const btn=$('snap-btn'), st=$('snap-status');
  btn.disabled=true; btn.textContent='⏳ Capturing…';
  st.textContent='sender を一時停止してスナップショット取得中…';
  try{
    const d=await(await fetch('/api/snapshot')).json();
    if(d.error){st.textContent='Error: '+d.error;}
    else{
      if(d.cam0){$('prev0').src=d.cam0;}else{st.textContent+=' cam0: '+(d.cam0_err||'fail');}
      if(d.cam1){$('prev1').src=d.cam1;}else{st.textContent+=' cam1: '+(d.cam1_err||'fail');}
      st.textContent='撮影完了 (sender 再起動済み)';
    }
  }catch(e){st.textContent='Error: '+e;}
  btn.disabled=false; btn.textContent='📷 Snapshot';
}
</script>
</body>
</html>"""

@app.get("/")
def index():
    return Response(_HTML, mimetype="text/html")


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="MetaPuppet Pi Control Server")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=7000)
    a = ap.parse_args()
    print(f"MetaPuppet Pi Control  →  http://stereocam.local:{a.port}")
    app.run(host=a.host, port=a.port, threaded=True)
