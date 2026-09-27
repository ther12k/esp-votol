#!/usr/bin/env python3
"""VOTOL web dashboard — local app that talks to the ESP32 bridge.

Connects over TCP (default 192.168.1.190:6638 — the raw serial bridge in the
votol-bt-bridge firmware) and speaks the VOTOL EM-series protocol:
  LDGET -> controller replies 7 parameter packets
  SHOW  -> controller replies one live telemetry frame (LOCAL mode: 0xAA)
  RESET -> soft-reset the controller
Protocol source: VotolAIO "Packket sniff" notes (GPL-3.0, Ming2k8-Coder).

Run:  python3 webapp/app.py     then open http://localhost:8080
"""
import json
import re
import socket
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ESP_HOST = "192.168.1.191"
ESP_PORT = 6638
# separate keyless-alarm ESP32 (keyless-alarm/ firmware), e.g. "192.168.1.50".
# Empty = keyless panel hidden on the dashboard.
KEYLESS_HOST = ""
# must match the keyless module's access PIN (settings page, default "1234")
KEYLESS_KEY = "1234"
HTTP_PORT = 8080
MONITOR_PERIOD = 0.4
WIRING_SVG = Path(__file__).resolve().parent.parent / "wiring-diagram.svg"

MODELS = {0x05: "EM-30s", 0x0A: "EM-50s", 0x14: "EM-100s", 0x1E: "EM-150s", 0x28: "EM-200s"}
BATTERY = {0: "48V", 1: "60V", 2: "72V", 3: "84V", 4: "96V"}
CTL_STATUS = ["IDLE", "INIT", "START", "RUN", "STOP", "BRAKE", "WAIT", "FAULT"]


def xor8(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
    return c


def frame(payload: bytes) -> bytes:
    """Build a 24-byte master frame: C9 14 02 + payload..., XOR, 0x0D."""
    assert len(payload) == 19, len(payload)
    body = bytes((0xC9, 0x14, 0x02)) + payload
    return body + bytes((xor8(body), 0x0D))


def cmd_ldget() -> bytes:
    return frame(b"LDGET" + bytes(14))


def cmd_show(remote=False, throttle_v=0.0, signals=0) -> bytes:
    # overall frame offsets (0-based): [12] remote flag (0x55 = remote-control
    # from UART, 0xAA = LOCAL observe), [13:15] throttle voltage * 5945,
    # [15] gear/brake/reverse switch bits; payload starts at overall [3]
    p = bytearray(19)
    p[0:4] = b"SHOW"
    p[9] = 0x55 if remote else 0xAA
    p[10:12] = int(throttle_v * 5945).to_bytes(2, "big")
    p[12] = signals & 0xFF
    return frame(bytes(p))


def cmd_reset() -> bytes:
    return frame(b"RESET" + bytes(14))


# ---------- ESP32 bridge config (proxied server-side: no CORS issues) ----------
def esp_http_get(host, path, timeout=3):
    try:
        with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
            return r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        # non-2xx still has a body (e.g. the keyless module's 403 PIN refusal)
        try:
            return e.read().decode("utf-8", "replace")
        except Exception:
            return None
    except Exception:
        return None


def esp_uart_state(host):
    """Current RX/TX pins + baud as reported by the bridge's status page (cached 3 s)."""
    now = time.time()
    if esp_uart_state.cache and now - esp_uart_state.cache[0] < 3:
        return esp_uart_state.cache[1]
    html = esp_http_get(host, "/")
    state = None
    if html:
        rx = re.search(r"ESP32 RX: <b>GPIO(\d+)</b>", html)
        tx = re.search(r"ESP32 TX: <b>GPIO(\d+)</b>", html)
        bd = re.search(r"Current: <b>(\d+)", html)
        if rx and tx and bd:
            state = {"rx": int(rx.group(1)), "tx": int(tx.group(1)), "baud": int(bd.group(1))}
    esp_uart_state.cache = (now, state)
    return state


esp_uart_state.cache = (0, None)


def keyless_state():
    """Keyless alarm module status via its /state.json (proxied; None = offline).

    Negative results cached longer (10 s) so an unreachable module can't stall
    the dashboard's 600 ms state.json polling with 1 s HTTP timeouts.
    """
    now = time.time()
    ttl = 3 if keyless_state.cache[1] else 10
    if keyless_state.cache and now - keyless_state.cache[0] < ttl:
        return keyless_state.cache[1]
    state = None
    if KEYLESS_HOST:
        # 2.5 s: the module's ~1 s blocking BLE scan can delay its web replies
        raw = esp_http_get(KEYLESS_HOST, "/state.json", timeout=2.5)
        if raw:
            try:
                state = json.loads(raw)
            except ValueError:
                pass
    keyless_state.cache = (now, state)
    return state


keyless_state.cache = (0, None)


class VotolLink(threading.Thread):
    """Background TCP link: reader thread + poller; safe to call from HTTP threads."""

    def __init__(self, host, port):
        super().__init__(daemon=True)
        self.host, self.port = host, port
        self.sock = None
        self.lock = threading.Lock()
        self.alive = threading.Event()
        self.monitor = threading.Event()
        self.telemetry = None       # latest decoded telemetry dict
        self.params = None          # list of decoded parameter rows
        self.hexlog = []            # (t, dir, hex) newest last
        self.tx_count = 0           # frames sent to the bridge
        self.rx_count = 0           # valid frames received from the controller
        self.connected = False
        self._rxbuf = bytearray()

    # ---------------- connection ----------------
    def connect(self):
        with self.lock:
            if self.sock:
                return True, "already connected"
            try:
                s = socket.create_connection((self.host, self.port), timeout=4)
            except OSError as e:
                return False, str(e)
            s.settimeout(0.2)
            self.sock = s
            self.connected = True
            self._rxbuf.clear()
        self.start()
        return True, "connected"

    def disconnect(self):
        self.monitor.clear()
        self.alive.clear()
        with self.lock:
            if self.sock:
                try:
                    self.sock.close()
                except OSError:
                    pass
            self.sock = None
            self.connected = False
            self.telemetry = None

    def _send(self, data: bytes):
        with self.lock:
            if not self.sock:
                return False
            try:
                self.sock.sendall(data)
            except OSError:
                self.connected = False
                return False
        self._log("TX", data)
        self.tx_count += 1
        return True

    def _log(self, d, data):
        with self.lock:
            self.hexlog.append((time.time(), d, data.hex(" ")))
            del self.hexlog[:-120]

    # ---------------- commands ----------------
    def read_params(self):
        self.params = None
        return self._send(cmd_ldget())

    def send_hex(self, hexstr):
        try:
            data = bytes.fromhex(hexstr.replace(" ", ""))
        except ValueError:
            return False, "bad hex"
        ok = self._send(data)
        return ok, "sent" if ok else "not connected"

    # ---------------- reader / poller ----------------
    def run(self):
        self.alive.set()
        last_poll = 0.0
        while self.alive.is_set():
            now = time.time()
            if self.monitor.is_set() and now - last_poll > MONITOR_PERIOD:
                last_poll = now
                self._send(cmd_show(remote=False))
            with self.lock:
                s = self.sock
            if not s:
                break
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                self.connected = False
                break
            if not chunk:
                self.connected = False
                break
            self._log("RX", chunk)
            self._rxbuf += chunk
            self._parse()

    def _parse(self):
        # frames: C0 14 ... XOR 0D, 24 bytes. NB: 0x0D also appears inside
        # telemetry frames (the type byte at offset 2!), so sync on header +
        # terminator-at-23 + checksum, never on bare 0x0D.
        buf = self._rxbuf
        i = 0
        while True:
            while i + 23 < len(buf) and not (
                buf[i] == 0xC0 and buf[i + 1] == 0x14 and buf[i + 23] == 0x0D
            ):
                i += 1
            if i + 24 > len(buf):
                if len(buf) > 1024:
                    del buf[:512]
                return
            f = bytes(buf[i:i + 24])
            if xor8(f[:22]) != f[22]:
                i += 1          # false sync, keep scanning
                continue
            del buf[: i + 24]
            i = 0
            self.rx_count += 1
            if f[2] == 0x0D and f[3] == 0x59:          # telemetry frame
                self.telemetry = decode_telemetry(f)
            elif f[2] == 0x05 and f[3] == 0x52:        # parameter packet 1..7
                rows = decode_param_packet(f)
                if rows is not None:
                    if self.params is None:
                        self.params = []
                    self.params = [r for r in self.params if r[0] != rows[0][0]] + rows


def decode_telemetry(f: bytes) -> dict:
    u16 = lambda i: int.from_bytes(f[i:i + 2], "big", signed=True)
    u32 = lambda i: int.from_bytes(f[i:i + 4], "big")
    flags, status = f[20], f[21] & 0x07
    gear = ["L", "M", "H", "S"][flags & 0x03] if (flags & 0x03) < 4 else "?"
    return {
        "ts": time.time(),
        "voltage_v": round(u16(5) / 10, 1),
        "current_a": round(u16(7) / 10, 1),
        "fault_code": u32(10),
        "rpm": u16(14),
        "controller_temp_c": f[16] - 50,
        "motor_temp_c": f[17] - 50,
        "gear": gear,
        "brake": bool(flags & 0x10),
        "reverse": bool(flags & 0x40),
        "regen": bool(flags & 0x80),
        "status": CTL_STATUS[status] if status < len(CTL_STATUS) else str(status),
        "raw": f.hex(" "),
    }


def _rows(idx, pairs):
    return [(f"P{idx} · {k}", v) for k, v in pairs]


def decode_param_packet(f: bytes):
    i = f[4]
    u16 = lambda o: int.from_bytes(f[5 + o:5 + o + 2], "big", signed=True)
    u8 = lambda o: f[5 + o]
    if i == 1:
        return _rows(1, [
            ("Model", MODELS.get(u8(0), hex(u8(0)))),
            ("Battery class", BATTERY.get(u8(1) & 0x0F, hex(u8(1)))),
            ("Overvoltage", f"{u16(2)/10:.1f} V"),
            ("Soft undervoltage", f"{u16(4)/10:.1f} V"),
            ("Undervolt variation", f"{u8(6)/10:.1f} V"),
            ("Regen current", f"{u16(7)/10:.1f} A"),
            ("Phase current", f"{u16(9)/10:.0f} A"),
            ("Undervoltage", f"{u16(11)/10:.1f} V"),
            ("Voltage calibration", u16(13)),
            ("Current calibration", u16(15)),
        ])
    if i == 2:
        return _rows(2, [
            ("Bus current limit", f"{u16(0)/10:.0f} A"),
            ("Gear 1 speed %", u8(2)), ("Gear 2 speed %", u8(3)), ("Gear 3 speed %", u8(4)),
            ("Flux weakening start", f"{u16(5)} rpm"),
            ("KP (high param1)", u16(7)),
            ("Start phase (mid param1)", u16(9)),
            ("Speed limit / flags byte", hex(u8(11))),
            ("Speed limit %", u8(12)),
            ("Soft start grade", u8(13)),
            ("Sport off after", f"{u8(14)} s"), ("Sport recovery", f"{u8(15)} s"),
            ("MTPA (torque per A)", u8(16)),
        ])
    if i == 3:
        hall = u8(1) if f[5] == 0 else u8(1) - 256
        return _rows(3, [
            ("Hall angle", hall),
            ("Controller high temp", f"{u8(2)} °C"),
            ("Controller over temp", f"{u8(3)} °C"),
            ("Temp limit current", f"{u8(4)} A"),
            ("TC1 (phase at temp limit)", u16(5)),
            ("TC2 (traction control)", u16(7)),
            ("TC3 (speed compensation)", u16(9)),
            ("Reverse speed %", u8(11)),
            ("Options byte", hex(u8(12))),
            ("Pole pairs", u8(13)),
            ("EABS %", u8(14)),
            ("Software version", u8(15)),
            ("Hardware version", u8(16)),
        ])
    if i == 4:
        return _rows(4, [
            ("Options byte", hex(u8(0))),
            ("MVB percent", round(u16(1) / 147, 1)),
            ("MVB torque", u16(3)),
            ("Max RPM", u16(5)),
            ("Gear 1 current %", u8(7)), ("Gear 2 current %", u8(8)), ("Gear 3 current %", u8(9)),
            ("KI (flux weakening 2)", u16(10)),
            ("Max phase limit", u16(12)),
            ("Max phase limit time", f"{u16(14)} s"),
            ("Flux weakening calibration", u8(16)),
        ])
    if i == 7:
        v = lambda o: round(u8(o) / 46, 2)
        return _rows(7, [
            ("Throttle start", f"{v(0)} V"),
            ("Throttle end", f"{v(1)} V"),
            ("Throttle low protect", f"{v(2)} V"),
            ("Throttle high protect", f"{v(3)} V"),
            ("Decel rate", u8(4)), ("Accel rate", u8(5)),
            ("Starting torque", u16(6)),
            ("Combined / delay torque", u16(8)),
        ])
    if i in (5, 6):
        pins5 = ["PD0", "JTCK", "SWD", "PA11", "PB3", "PD1", "PA12", "PC15"]
        pins6 = ["PA0", "PB9", "PB4", "PA15", "PB2", "PC14", "PB5", "PD15"]
        rows = []
        for n, name in enumerate(pins5 if i == 5 else pins6):
            o = n * 2
            cb, ty = u8(o), u8(o + 1)
            mode = "F" if not (cb & 0b110) else ("U" if cb & 0b010 else "D")
            extra = []
            if cb & 0b0001: extra.append("SW")
            if cb & 0b1000: extra.append("LA")
            if not (cb & 0b0110): extra.append("no checkbox")
            typ = "IO" if ty & 0x80 else f"range {ty & 0x7f}"
            rows.append((name, f"{mode} mode · {' · '.join(extra) if extra else '—'} · {typ}"))
        return _rows(i, rows)
    return _rows(i, [("raw", f[5:22].hex(" "))])


# ------------------------------- web UI ---------------------------------
PAGE = """<!DOCTYPE html><html lang=en data-theme=dark><head><meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>VOTOL Dashboard</title><style>
:root,[data-theme=dark]{--bg:#0b1020;--panel:#151b2e;--line:#26304a;--txt:#e5e7eb;
--mut:#8b96ad;--input:#0e1426;--hover:#1a2340;--grid:#1e2740}
[data-theme=light]{--bg:#eef2f8;--panel:#ffffff;--line:#d7dfeb;--txt:#0f172a;
--mut:#5b6a86;--input:#f8fafc;--hover:#eef2f9;--grid:#e5eaf2}
*{box-sizing:border-box}
body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--txt);margin:0;
padding:18px 18px 90px;max-width:1180px;margin-inline:auto;transition:background .25s,color .25s}
@media(min-width:900px){body{padding-left:226px}}
/* ---------- sidebar (desktop) ---------- */
.sidebar{position:fixed;left:0;top:0;bottom:0;width:200px;background:var(--panel);
border-right:1px solid var(--line);display:flex;flex-direction:column;gap:4px;padding:16px 10px;z-index:10}
.logo{font-size:1.2rem;font-weight:800;padding:2px 12px 14px;color:var(--txt)}
.logo i{font-style:normal;color:#f59e0b}
.sidebar .nav{display:flex;gap:11px;align-items:center;background:transparent;color:var(--mut);
border:0;padding:11px 14px;border-radius:10px;font-size:.93rem;cursor:pointer;font-weight:600;text-align:left}
.sidebar .nav:hover{background:var(--hover);color:var(--txt)}
.sidefoot{margin-top:auto;padding:10px 12px;color:var(--mut);font-size:.7rem;line-height:1.5}
/* ---------- bottom nav (mobile) ---------- */
.bottomnav{display:none;position:fixed;left:0;right:0;bottom:0;z-index:20;
background:var(--panel);border-top:1px solid var(--line);
padding:5px 4px calc(5px + env(safe-area-inset-bottom))}
.bottomnav .nav{flex:1;flex-direction:column;gap:3px;font-size:.6rem;padding:7px 2px;align-items:center}
.bottomnav .nav span:last-child{font-size:.62rem}
@media(max-width:899px){
 .sidebar{display:none}
 .bottomnav{display:flex}
}
/* ---------- nav buttons shared ---------- */
.nav{display:flex;align-items:center;gap:9px;background:transparent;color:var(--mut);
border:0;border-radius:10px;cursor:pointer;font-weight:600;transition:background .15s}
.nav .ico{font-size:1.15rem}
.nav.active{background:rgba(59,130,246,.15);color:#3b82f6}
.nav:not(.active):hover{color:var(--txt)}
/* ---------- topbar ---------- */
header{display:flex;align-items:center;gap:12px;flex-wrap:wrap;position:sticky;top:0;
background:var(--bg);padding:8px 0 10px;z-index:5;transition:background .25s}
h1{font-size:1.4rem;margin:0}
h2{font-size:1.02rem;margin:20px 0 10px;color:#818cf8}
[data-theme=light] h2{color:#4338ca}
h2 small{color:var(--mut);font-weight:400;font-size:.78rem}
.pill{padding:4px 14px;border-radius:99px;font-size:.82rem;font-weight:600}
.pill.ok{background:rgba(16,185,129,.15);color:#10b981}
.pill.bad{background:rgba(239,68,68,.15);color:#ef4444}
.muted{color:var(--mut);font-size:.85rem}
.bar{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:10px 0}
input{background:var(--input);color:var(--txt);border:1px solid var(--line);border-radius:8px;padding:8px 10px;font-size:.9rem}
input:focus{outline:1px solid #3b82f6}
select{background:var(--input);color:var(--txt);border:1px solid var(--line);border-radius:8px;padding:8px 10px;font-size:.9rem}
button{background:#2563eb;color:#fff;border:0;border-radius:8px;padding:9px 16px;
cursor:pointer;font-size:.9rem;font-weight:600;transition:filter .15s}
button:hover{filter:brightness(1.2)}
button.off{background:var(--input);color:var(--txt);border:1px solid var(--line)}
button.warn{background:#b91c1c}
button:disabled{opacity:.5;cursor:wait}
#linkpanel{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:12px 16px;margin:12px 0}
.st{font-weight:600;font-size:.95rem;min-height:1.2em}
.st.ok{color:#10b981}.st.bad{color:#ef4444}
#stats,#answer{margin:2px 0}
#answer.ok{color:#10b981;font-weight:600}
/* ---------- gauges ---------- */
.dash{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;margin:12px 0}
@media(max-width:640px){.dash{grid-template-columns:1fr 1fr}.gauge:last-child{grid-column:span 2}}
.gauge{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:14px 8px;position:relative}
.gauge svg{width:100%;max-width:190px;display:block;margin:auto}
.gbg{fill:none;stroke:var(--grid);stroke-width:9}
.gval{fill:none;stroke-width:9;stroke-linecap:round;transform:rotate(-90deg);transform-origin:60px 60px;
stroke-dasharray:326.7;transition:stroke-dashoffset .35s,stroke .25s}
.gnum{fill:var(--txt);font-size:19px;font-weight:700;text-anchor:middle;font-variant-numeric:tabular-nums}
.gsub{fill:var(--mut);font-size:9.5px;letter-spacing:.14em;text-anchor:middle}
.gmax{position:absolute;top:16px;right:14px;color:var(--mut);font-size:.7rem}
/* ---------- flags ---------- */
.flags{display:flex;gap:8px;flex-wrap:wrap;margin:4px 0 12px}
.flag{padding:5px 14px;border-radius:99px;font-size:.8rem;font-weight:600;
background:var(--panel);border:1px solid var(--line);color:var(--mut)}
.flag.on{background:rgba(59,130,246,.15);color:#3b82f6;border-color:transparent}
.flag.good{background:rgba(16,185,129,.15);color:#10b981;border-color:transparent}
.flag.alert{background:rgba(239,68,68,.15);color:#ef4444;border-color:transparent}
/* ---------- chart + meters ---------- */
.panel{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:14px 16px}
.panel h3{margin:0 0 8px;font-size:.92rem}
.legend{display:flex;gap:16px;flex-wrap:wrap;font-size:.8rem;color:var(--mut);margin-bottom:6px}
.legend b{font-variant-numeric:tabular-nums}
canvas{width:100%;height:190px;display:block}
.meters{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin:14px 0}
@media(max-width:640px){.meters{grid-template-columns:1fr}}
.meter{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:10px 14px}
.meter .row{display:flex;justify-content:space-between;font-size:.85rem;color:var(--mut);margin-bottom:7px}
.meter .row b{color:var(--txt);font-variant-numeric:tabular-nums}
.track{height:9px;border-radius:99px;background:var(--grid);overflow:hidden}
.fill{height:100%;border-radius:99px;background:#10b981;width:0;transition:width .35s,background .35s}
/* ---------- cards ---------- */
.two{display:grid;grid-template-columns:1fr 1fr;gap:18px;align-items:start}
@media(max-width:920px){.two{grid-template-columns:1fr}}
section.card2{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:4px 16px 16px}
section.card2 h2{margin-top:14px}
.scroll{max-height:430px;overflow-y:auto}
table{border-collapse:collapse;width:100%}
td{border-bottom:1px solid var(--grid);padding:6px 10px;font-size:.88rem}
tr:hover td{background:var(--hover)}
td:first-child{color:var(--mut)}
td:last-child{font-weight:600;font-variant-numeric:tabular-nums}
ol.steps{padding-left:20px;margin:10px 0;line-height:1.9}
.warnbox{background:rgba(245,158,11,.12);border-left:3px solid #f59e0b;color:#d97706;
padding:9px 12px;border-radius:0 8px 8px 0;font-size:.85rem}
[data-theme=dark] .warnbox{color:#fcd34d}
img{max-width:100%;border-radius:10px;border:1px solid var(--line);margin-top:10px;display:block;
background:#0f172a}
.imgwrap{position:relative;cursor:zoom-in}
.imgwrap::after{content:'🔍 tap to enlarge';position:absolute;right:14px;bottom:14px;
background:rgba(15,23,42,.8);color:#e5e7eb;font-size:.72rem;padding:4px 10px;border-radius:99px}
/* config chips + lock */
.chips{display:flex;gap:7px;flex-wrap:wrap;margin:4px 0 12px}
.chip{background:var(--input);color:var(--mut);border:1px solid var(--line);border-radius:99px;
padding:6px 14px;font-size:.8rem;font-weight:600;cursor:pointer;transition:all .15s}
.chip:hover{color:var(--txt)}
.chip.active{background:#2563eb;color:#fff;border-color:transparent}
.lockoverlay{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;
background:color-mix(in srgb,var(--bg) 55%,transparent);backdrop-filter:blur(3px);
border-radius:10px;z-index:2}
/* modal pan & zoom */
.modal{display:none;position:fixed;inset:0;z-index:50;background:rgba(5,8,18,.94);touch-action:none}
.modal.open{display:block}
.modalclose{position:fixed;top:14px;right:16px;background:#1f2937;color:#fff;border:1px solid #374151;
border-radius:99px;width:40px;height:40px;font-size:1.1rem;cursor:pointer;z-index:52}
.modaltools{position:fixed;top:14px;left:50%;transform:translateX(-50%);display:flex;gap:8px;
align-items:center;z-index:52;background:rgba(21,27,46,.9);border:1px solid #374151;
border-radius:12px;padding:6px 10px}
.modaltools button{padding:6px 13px;font-size:.95rem}
#zoomPct{font-variant-numeric:tabular-nums;color:#93c5fd}
#panzone{position:absolute;inset:0;overflow:hidden;cursor:grab;touch-action:none}
#panzone.panning{cursor:grabbing}
#diagImg{position:absolute;left:0;top:0;width:1200px;max-width:none;
transform-origin:0 0;user-select:none;-webkit-user-drag:none;will-change:transform}
pre{background:var(--input);border:1px solid var(--line);border-radius:10px;padding:10px;
font-size:.74rem;max-height:320px;overflow-y:auto;white-space:pre-wrap;min-height:38px}
pre .ok{color:#10b981}pre .mut{color:#64748b}
footer{margin:22px 0 8px;color:var(--mut);font-size:.78rem;opacity:.7}
/* ---------- mobile tuning ---------- */
@media(max-width:560px){
 body{padding:12px 10px 90px}
 h1{font-size:1.15rem}
 h2{font-size:.95rem}
 .bar button{flex:1 1 auto;padding:10px 8px;font-size:.85rem}
 .bar input{flex:1 1 110px;min-width:0}
 .pill{font-size:.74rem;padding:3px 10px}
 .flag{padding:5px 10px;font-size:.74rem}
 .card b{font-size:1.35rem}
 .legend{gap:10px;font-size:.72rem}
 .scroll{max-height:320px}
}
</style></head><body>

<aside class=sidebar>
 <div class=logo><i>⚡</i> VOTOL</div>
 <button class="nav active" data-v=dash><span class=ico>🏠</span><span>Dashboard</span></button>
 <button class=nav data-v=params><span class=ico>📋</span><span>Parameters</span></button>
 <button class=nav data-v=wiring><span class=ico>🔌</span><span>Wiring</span></button>
 <button class=nav data-v=console><span class=ico>⌨️</span><span>Console</span></button>
 <div class=sidefoot>Bridge: ESP32 votol-bt-bridge<br>TCP 6638 · LOCAL observe mode<br>data never leaves your network</div>
</aside>

<main>
<header>
 <h1>⚡ VOTOL Dashboard</h1>
 <span id=conn class="pill bad">connecting…</span>
 <span id=upd class=muted></span>
 <span style=flex:1></span>
 <button class=off id=themeBtn onclick=toggleTheme()>☀️</button>
</header>

<div id=linkpanel>
 <div class=bar style=margin:0>
  host <input id=host size=14 value=__HOST__>
  port <input id=port size=5 value=__PORT__>
  <button onclick=connect()>Connect</button>
  <button class=off onclick=api({action:'disconnect'})>Disconnect</button>
  <button class=off title="Swap ESP32 RX and TX pins in software - no rewiring needed" onclick=api({action:'swap_pins'})>⇄ Swap RX↔TX</button>
  <button class=off title="Move UART to pins D16/D17 (use if GPIO18 was damaged)" onclick=api({action:'set_pins',rx:16,tx:17})>Pins 16/17</button>
  <button class=off title="Move UART back to pins D18/D19" onclick=api({action:'set_pins',rx:18,tx:19})>Pins 18/19</button>
  <select id=baudsel title="Serial baud rate to the VOTOL controller" onchange="api({action:'set_baud',rate:+this.value})">
   <option value=115200>115200</option>
   <option value=9600>9600</option>
   <option value=19200>19200</option>
   <option value=38400>38400</option>
   <option value=57600>57600</option>
  </select>
 </div>
 <div id=status class="st muted">idle</div>
 <div id=stats class=muted></div>
 <div id=uartinfo class=muted></div>
 <div id=answer class=muted></div>
</div>

<section id=view-dash class=view>
 <h2>Live telemetry <small>· SHOW polls in LOCAL mode — never drives the motor</small></h2>
 <div class=bar>
  <button onclick=api({action:'monitor_on'})>▶ Monitor on</button>
  <button class=off onclick=api({action:'monitor_off'})>■ Monitor off</button>
 <button onclick=api({action:'read_params'})>Read parameters</button>
 <button class=warn title="Sends the RESET command: the controller soft-reboots its logic, like switching the ignition off and on. Your saved settings are NOT erased."
  onclick="if(confirm('Soft-reboot the controller now?\\n\\nThis is like switching the ignition off and on — settings are kept.'))api({action:'reset'})">⟳ Reboot controller</button>
 </div>
 <div class=dash>
  <div class=gauge id=gV>
   <span class=gmax id=gVmax>range 120 V</span>
   <svg viewBox="0 0 120 120">
    <circle class="gbg" cx="60" cy="60" r="52"/>
    <circle class="gval" id="arcV" cx="60" cy="60" r="52" style="stroke:#f59e0b"/>
    <text class="gnum" id="numV" x="60" y="58">–</text>
    <text class="gsub" x="60" y="76">BATTERY VOLTAGE</text>
   </svg>
  </div>
  <div class=gauge id=gA>
   <span class=gmax id=gAmax>range 100 A</span>
   <svg viewBox="0 0 120 120">
    <circle class="gbg" cx="60" cy="60" r="52"/>
    <circle class="gval" id="arcA" cx="60" cy="60" r="52" style="stroke:#3b82f6"/>
    <text class="gnum" id="numA" x="60" y="58">–</text>
    <text class="gsub" x="60" y="76">BATTERY CURRENT</text>
   </svg>
  </div>
  <div class=gauge id=gR>
   <span class=gmax id=gRmax>range 6000 rpm</span>
   <svg viewBox="0 0 120 120">
    <circle class="gbg" cx="60" cy="60" r="52"/>
    <circle class="gval" id="arcR" cx="60" cy="60" r="52" style="stroke:#8b5cf6"/>
    <text class="gnum" id="numR" x="60" y="58">–</text>
    <text class="gsub" x="60" y="76">MOTOR RPM</text>
   </svg>
  </div>
 </div>
 <div class=flags id=flags>
  <span class=flag id=fGear>gear –</span>
  <span class=flag id=fState>state –</span>
  <span class=flag id=fBrake>brake</span>
  <span class=flag id=fRev>reverse</span>
  <span class=flag id=fRegen>regen</span>
  <span class=flag id=fFault>fault 0</span>
 </div>
 <div class=panel id=keylessPanel hidden>
  <h3>Keyless &amp; alarm <span class=muted style=font-weight:400>· separate ESP32 module</span>
   <span id=kState class=pill style=float:right>…</span></h3>
  <div id=kInfo class=muted></div>
  <div class=bar style=margin:8px 0 0>
   <button title="Disarm now: fob rule takes over again on the next scan" onclick=kapi({action:'keyless_disarm'})>🔓 Disarm</button>
   <button class=warn title="Arm now: immobilizer contact opens until a fob returns" onclick=kapi({action:'keyless_arm'})>🔒 Arm</button>
   <button class=off title="Sound the bike's siren for 30 s (find the bike / scare off)" onclick=kapi({action:'keyless_panic'})>🚨 Panic siren</button>
  </div>
 </div>
 <div class=panel>
  <h3>Live graph <span class=muted style=font-weight:400>· last 2 minutes, auto-scaled</span></h3>
  <div class=legend>
   <span style=color:#f59e0b>● Volt <b id=lgV>–</b></span>
   <span style=color:#3b82f6>● Amp <b id=lgA>–</b></span>
   <span style=color:#8b5cf6>● RPM <b id=lgR>–</b></span>
  </div>
  <canvas id=chart></canvas>
 </div>
 <div class=meters>
  <div class=meter><div class=row><span>🌡 Controller temp</span><b id=mCt>–</b></div>
   <div class=track><div class=fill id=mC></div></div></div>
  <div class=meter><div class=row><span>🌡 Motor temp</span><b id=mMt>–</b></div>
   <div class=track><div class=fill id=mM></div></div></div>
 </div>
</section>

<section id=view-params class=view hidden>
 <section class=card2>
  <h2>Configuration pages <small>· mirrors the VOTOL app's setting pages · read-only for safety</small></h2>
  <div class=bar>
   <button onclick=api({action:'read_params'})>↻ Read from controller</button>
   <span class=muted id=pcount></span>
  </div>
  <div class=chips id=chips></div>
  <div style=position:relative>
   <div id=params class=scroll><span class=muted>connect to the controller, then press "Read from controller"</span></div>
   <div id=paramlock class=lockoverlay>
    <div style=text-align:center>
     <div style=font-size:2.2rem>🔒</div>
     <b>Controller not connected</b>
     <div class=muted style=max-width:340px;margin:6px auto 10px>Plug in the 4-wire link, switch the ignition on,
      press <b>Connect</b>, then <b>↻ Read from controller</b>. All config pages unlock once data arrives.</div>
    </div>
   </div>
  </div>
 </section>
</section>

<section id=view-wiring class=view hidden>
 <section class=card2>
  <h2>Wiring — VOTOL → ESP32</h2>
  <ol class=steps>
   <li><b style=color:#64748b>Black</b> → <b>GND</b> <span class=muted>(right column, 6th pin from top)</span></li>
   <li><b>White</b> → <b>D19</b> <span class=muted>(right below GND)</span></li>
   <li><b style=color:#16a34a>Green</b> → <b>D18</b> <span class=muted>(directly below D19) — a 1 kΩ series resistor is recommended</span></li>
   <li><b style=color:#dc2626>Red</b> → <b>VIN</b> <span class=muted>(bottom-left corner)</span></li>
  </ol>
  <p class=warnbox>⚠ The green wire carries the controller's 5 V logic. Many run it directly into
  the ESP32 without issue, but it is above the 3.3 V spec — a plain 1 kΩ resistor in series (no
  divider needed) is cheap insurance. Never leave USB plugged in while the bike's +5 V is connected.</p>
  <div class=imgwrap onclick=openModal()><img src=/wiring.svg alt="VOTOL to ESP32 wiring diagram"></div>
 </section>
</section>

<section id=view-console class=view hidden>
 <section class=card2>
  <h2>Raw serial console <small>· hex bytes, straight to the controller</small></h2>
  <div class=bar>
   <input id=hex size=44 placeholder='C9 14 02 4C 44 47 45 54 …'>
   <button onclick=api({action:'hex',hex:el('hex').value})>Send</button>
  </div>
  <pre id=log></pre>
 </section>
</section>

<footer>Bridge firmware: ESP32 votol-bt-bridge · VOTOL protocol decoded from VotolAIO notes ·
dashboard runs locally, data never leaves your network</footer>
</main>

<nav class=bottomnav>
 <button class="nav active" data-v=dash><span class=ico>🏠</span><span>Home</span></button>
 <button class=nav data-v=params><span class=ico>📋</span><span>Params</span></button>
 <button class=nav data-v=wiring><span class=ico>🔌</span><span>Wiring</span></button>
 <button class=nav data-v=console><span class=ico>⌨️</span><span>Console</span></button>
</nav>

<div id=imgModal class=modal>
 <div class=modaltools>
  <button class=off onclick=pzZoom(1.3)>＋</button>
  <button class=off onclick=pzZoom(0.77)>－</button>
  <button class=off onclick=pzReset()>⟲ 1:1</button>
  <span id=zoomPct class=pill style=background:var(--input)>100%</span>
 </div>
 <button class=modalclose onclick=closeModal()>✕</button>
 <div id=panzone><img id=diagImg src=/wiring.svg draggable=false
      alt="VOTOL to ESP32 wiring diagram (pan & zoom)"></div>
</div>

<script>
/* ---------- views / app menu ---------- */
function switchView(v){
 document.querySelectorAll('.view').forEach(s=>s.hidden = s.id!=='view-'+v);
 document.querySelectorAll('[data-v]').forEach(b=>b.classList.toggle('active', b.dataset.v===v));
 try{localStorage.vtView=v}catch(e){}
 if(v==='dash')requestAnimationFrame(drawChart);
 window.scrollTo({top:0});
}
document.querySelectorAll('[data-v]').forEach(b=>b.onclick=()=>switchView(b.dataset.v));
/* ---------- config pages ---------- */
const SKELETON_PARAMS = [["P1 \u00b7 Model", "EM-100s (spec)"], ["P1 \u00b7 Battery class", "72V (spec)"], ["P1 \u00b7 Overvoltage", "--.- V"], ["P1 \u00b7 Soft undervoltage", "--.- V"], ["P1 \u00b7 Undervolt variation", "--.- V"], ["P1 \u00b7 Regen current", "-- A"], ["P1 \u00b7 Phase current", "-- A"], ["P1 \u00b7 Undervoltage", "--.- V"], ["P1 \u00b7 Voltage calibration", "--"], ["P1 \u00b7 Current calibration", "--"], ["P2 \u00b7 Bus current limit", "-- A"], ["P2 \u00b7 Gear 1 speed %", "-- %"], ["P2 \u00b7 Gear 2 speed %", "-- %"], ["P2 \u00b7 Gear 3 speed %", "-- %"], ["P2 \u00b7 Flux weakening start", "-- rpm"], ["P2 \u00b7 KP (high param1)", "--"], ["P2 \u00b7 Start phase (mid param1)", "--"], ["P2 \u00b7 Speed limit / flags byte", "--"], ["P2 \u00b7 Speed limit %", "-- %"], ["P2 \u00b7 Soft start grade", "--"], ["P2 \u00b7 Sport off after", "-- s"], ["P2 \u00b7 Sport recovery", "-- s"], ["P2 \u00b7 MTPA (torque per A)", "--"], ["P3 \u00b7 Hall angle", "-- \u00b0"], ["P3 \u00b7 Controller high temp", "-- \u00b0C"], ["P3 \u00b7 Controller over temp", "-- \u00b0C"], ["P3 \u00b7 Temp limit current", "-- A"], ["P3 \u00b7 TC1 (phase at temp limit)", "--"], ["P3 \u00b7 TC2 (traction control)", "--"], ["P3 \u00b7 TC3 (speed compensation)", "--"], ["P3 \u00b7 Reverse speed %", "-- %"], ["P3 \u00b7 Options byte", "--"], ["P3 \u00b7 Pole pairs", "--"], ["P3 \u00b7 EABS %", "-- %"], ["P3 \u00b7 Software version", "--"], ["P3 \u00b7 Hardware version", "--"], ["P4 \u00b7 Options byte", "--"], ["P4 \u00b7 MVB percent", "-- %"], ["P4 \u00b7 MVB torque", "--"], ["P4 \u00b7 Max RPM", "-- rpm"], ["P4 \u00b7 Gear 1 current %", "-- %"], ["P4 \u00b7 Gear 2 current %", "-- %"], ["P4 \u00b7 Gear 3 current %", "-- %"], ["P4 \u00b7 KI (flux weakening 2)", "--"], ["P4 \u00b7 Max phase limit", "-- A"], ["P4 \u00b7 Max phase limit time", "-- s"], ["P4 \u00b7 Flux weakening calibration", "--"], ["P5 \u00b7 PD0", "Port disabled \u00b7 no signal"], ["P5 \u00b7 JTCK", "Port disabled \u00b7 no signal"], ["P5 \u00b7 SWD", "Port disabled \u00b7 no signal"], ["P5 \u00b7 PA11", "Port disabled \u00b7 no signal"], ["P5 \u00b7 PB3", "Port disabled \u00b7 no signal"], ["P5 \u00b7 PD1", "Port disabled \u00b7 no signal"], ["P5 \u00b7 PA12", "Port disabled \u00b7 no signal"], ["P5 \u00b7 PC15", "Port disabled \u00b7 no signal"], ["P6 \u00b7 PA0", "Port disabled \u00b7 no signal"], ["P6 \u00b7 PB9", "Port disabled \u00b7 no signal"], ["P6 \u00b7 PB4", "Port disabled \u00b7 no signal"], ["P6 \u00b7 PA15", "Port disabled \u00b7 no signal"], ["P6 \u00b7 PB2", "Port disabled \u00b7 no signal"], ["P6 \u00b7 PC14", "Port disabled \u00b7 no signal"], ["P6 \u00b7 PB5", "Port disabled \u00b7 no signal"], ["P6 \u00b7 PD15", "Port disabled \u00b7 no signal"], ["P7 \u00b7 Throttle start", "-.-- V"], ["P7 \u00b7 Throttle end", "-.-- V"], ["P7 \u00b7 Throttle low protect", "-.-- V"], ["P7 \u00b7 Throttle high protect", "-.-- V"], ["P7 \u00b7 Decel rate", "--"], ["P7 \u00b7 Accel rate", "--"], ["P7 \u00b7 Starting torque", "--"], ["P7 \u00b7 Combined / delay torque", "--"]];
const GROUPS=[['P1','P1 · Basic'],['P2','P2 · Speed'],['P3','P3 · Temp & TC'],['P4','P4 · Options'],
 ['P5','P5 · IO A'],['P6','P6 · IO B'],['P7','P7 · Throttle']];
let activeGroup='all',lastParams=null;
function buildChips(){
 el('chips').innerHTML='<button class="chip active" data-g="all">All</button>'+
  GROUPS.map(g=>`<button class="chip" data-g="${g[0]}">${g[1]}</button>`).join('');
 el('chips').querySelectorAll('.chip').forEach(c=>c.onclick=()=>{activeGroup=c.dataset.g;
  el('chips').querySelectorAll('.chip').forEach(x=>x.classList.toggle('active',x===c));renderParams();});}
function renderParams(){
 const isLive = !!(lastParams && lastParams.length);
 const ps = isLive ? lastParams : SKELETON_PARAMS;
 const want = activeGroup==='all' ? GROUPS.map(g=>g[0]) : [activeGroup];
 let h = '';
 GROUPS.forEach(g=>{
  if(!want.includes(g[0]))return;
  const rows = ps.filter(r=>r[0].toUpperCase().startsWith(g[0]+' · '));
  if(rows.length){
    h += `<div style="display:flex;align-items:center;justify-content:space-between;margin:16px 0 6px">
           <h3 style="margin:0;color:#818cf8;font-size:.9rem">${g[1]}</h3>
           <span style="font-size:.72rem;color:var(--mut)">${isLive ? '✓ live from VOTOL' : '⊘ offline preview (read-only)'}</span>
          </div>
          <table class="${isLive ? '' : 'disabled-table'}">` +
         rows.map(r=>`<tr><td>${r[0].replace(/^P\d\s*·\s*/,'')}</td><td style="${isLive ? '' : 'color:var(--mut);font-weight:normal'}">${r[1]}</td></tr>`).join('') +
         '</table>';
  }
 });
 el('params').innerHTML = h || '<span class=muted>no parameters in this group</span>';}
function setLock(locked){
 const lockEl = el('paramlock');
 if(lockEl) lockEl.style.display = 'none'; // We show the full config table even when disconnected!
 const cntEl = el('pcount');
 if(cntEl) {
   if(lastParams && lastParams.length) {
     cntEl.innerHTML = `<span class="pill ok" style="padding:2px 8px;font-size:.72rem">Live</span> ${lastParams.length} parameters loaded`;
   } else {
     cntEl.innerHTML = `<span class="pill" style="padding:2px 8px;font-size:.72rem;background:var(--line);color:var(--mut)">Preview</span> Connect controller & read to populate live values`;
   }
 }
}
/* ---------- wiring modal: pan & zoom ---------- */
const pz={x:0,y:0,s:1};let drag=null,pinch=null;const ptrs=new Map();
function applyPZ(){const im=el('diagImg');
 im.style.transform=`translate(${pz.x}px,${pz.y}px) scale(${pz.s})`;
 el('zoomPct').textContent=Math.round(pz.s*100)+'%';}
function clampS(v){return Math.min(6,Math.max(0.2,v));}
function pzZoom(f,cx,cy){const r=el('panzone').getBoundingClientRect();
 if(cx===undefined){cx=r.width/2+r.left;cy=r.height/2+r.top;}
 const ns=clampS(pz.s*f);
 const ix=(cx-r.left-pz.x)/pz.s,iy=(cy-r.top-pz.y)/pz.s;
 pz.x=cx-r.left-ix*ns;pz.y=cy-r.top-iy*ns;pz.s=ns;applyPZ();}
function pzReset(){
 const r=el('panzone').getBoundingClientRect();
 const imgW=1200, imgH=857;
 const scale=Math.min((r.width*0.94)/imgW, (r.height*0.88)/imgH, 1.2);
 pz.s=Math.max(0.25, scale);
 pz.x=(r.width - imgW*pz.s)/2;
 pz.y=(r.height - imgH*pz.s)/2 + 20;
 applyPZ();}
function openModal(){el('imgModal').classList.add('open');requestAnimationFrame(pzReset);}
function closeModal(){el('imgModal').classList.remove('open')}
document.addEventListener('keydown',e=>{if(e.key==='Escape')closeModal()});
(function(){
 const z=el('panzone');
 z.addEventListener('wheel',e=>{e.preventDefault();
  pzZoom(e.deltaY<0?1.15:1/1.15,e.clientX,e.clientY);},{passive:false});
 z.addEventListener('pointerdown',e=>{e.preventDefault();z.setPointerCapture(e.pointerId);
  ptrs.set(e.pointerId,{x:e.clientX,y:e.clientY});
  z.classList.add('panning');
  if(ptrs.size===2){drag=null;
   const [a,b]=[...ptrs.values()];pinch=Math.hypot(a.x-b.x,a.y-b.y);}
  else drag={x:e.clientX,y:e.clientY};});
 z.addEventListener('pointermove',e=>{if(!ptrs.has(e.pointerId))return;
  ptrs.set(e.pointerId,{x:e.clientX,y:e.clientY});
  if(ptrs.size===2&&pinch){const [a,b]=[...ptrs.values()];
   const d=Math.hypot(a.x-b.x,a.y-b.y)||1;
   pzZoom(d/pinch,(a.x+b.x)/2,(a.y+b.y)/2);pinch=d;}
  else if(drag){pz.x+=e.clientX-drag.x;pz.y+=e.clientY-drag.y;
   drag={x:e.clientX,y:e.clientY};applyPZ();}});
 const end=e=>{ptrs.delete(e.pointerId);if(ptrs.size<2)pinch=null;
  if(!ptrs.size){drag=null;z.classList.remove('panning');}};
 z.addEventListener('pointerup',end);z.addEventListener('pointercancel',end);
})();
/* ---------- theme ---------- */
function setTheme(t){document.documentElement.dataset.theme=t;
 el('themeBtn').textContent=t==='light'?'🌙':'☀️';localStorage.vtTheme=t;drawChart();}
function toggleTheme(){setTheme(document.documentElement.dataset.theme==='light'?'dark':'light')}
(function(){const s=localStorage.vtTheme||
 (matchMedia('(prefers-color-scheme: light)').matches?'light':'dark');
 document.documentElement.dataset.theme=s;el('themeBtn').textContent=s==='light'?'🌙':'☀️'})();
/* ---------- link ---------- */
let host='__HOST__',port=__PORT__,last=-1;
buildChips();
renderParams();
setLock(true);
setTimeout(()=>api({action:'connect'}),700);   // auto-connect on load
const NAMES={connect:'connect',disconnect:'disconnect',monitor_on:'monitor on',
 monitor_off:'monitor off',read_params:'read parameters',reset:'reset',hex:'send hex',
 swap_pins:'swap RX↔TX',set_pins:'set pins',set_baud:'set baud',
 keyless_arm:'arm keyless',keyless_disarm:'disarm keyless',keyless_panic:'panic siren'};
function el(i){return document.getElementById(i)}
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function status(t,cls){const s=el('status');s.textContent=t;s.className='st '+(cls||'muted')}
/* ---------- keyless phone alert ---------- */
let kAudioCtx=null;
function kBeep(){ // phone yell while the dashboard is open: 3x 880 Hz bursts
 kAudioCtx=kAudioCtx||new (window.AudioContext||window.webkitAudioContext)();
 for(let i=0;i<3;i++){const o=kAudioCtx.createOscillator(),g=kAudioCtx.createGain();
  o.connect(g);g.connect(kAudioCtx.destination);o.frequency.value=880;o.type='square';
  const t=kAudioCtx.currentTime+i*0.45;
  g.gain.setValueAtTime(0.25,t);g.gain.setValueAtTime(0,t+0.3);
  o.start(t);o.stop(t+0.3);}
}
function kapi(a){ // prime notification permission + audio unlock on first tap
 try{if(window.Notification&&Notification.permission==='default')Notification.requestPermission()}catch(e){}
 try{kAudioCtx=kAudioCtx||new (window.AudioContext||window.webkitAudioContext)();
  if(kAudioCtx.resume)kAudioCtx.resume()}catch(e){}
 api(a);
}
async function api(j){
 const name=NAMES[j.action]||j.action;
 j.host=el('host').value;j.port=+el('port').value;   // always send current inputs
 status('⏳ '+name+' …');
 document.querySelectorAll('.bar button,.bottomnav button,.sidebar button').forEach(b=>b.disabled=true);
 try{
  const r=await fetch('/api',{method:'POST',body:JSON.stringify(j)});
  const d=await r.json().catch(()=>({ok:false,msg:'server error '+r.status}));
  status((d.ok?'✓ ':'✗ ')+name+(d.msg?' — '+d.msg:''),d.ok?'ok':'bad');
 }catch(e){status('✗ '+name+' — '+e,'bad')}
 document.querySelectorAll('.bar button,.bottomnav button,.sidebar button').forEach(b=>b.disabled=false);
 draw(await (await fetch('/state.json')).json());
}
function connect(){host=el('host').value;port=+el('port').value;api({action:'connect'})}
setInterval(async()=>{try{const r=await fetch('/state.json');if(r.ok)draw(await r.json())}catch(e){}},600);
/* ---------- gauges ---------- */
const CIRC=2*Math.PI*52;
function setGauge(arc,num,val,max,txt){
 document.getElementById(arc).style.strokeDashoffset=CIRC*(1-Math.max(0,Math.min(1,val/max)));
 document.getElementById(num).textContent=txt;}
/* ---------- history chart ---------- */
const hist=[];let lastTs=0,maxA=50,maxR=3000,vMax=120;
function drawChart(){
 const c=el('chart');const dpr=window.devicePixelRatio||1;
 const w=c.clientWidth,h=c.clientHeight||190;
 if(!w)return;
 if(c.width!==Math.round(w*dpr)||c.height!==Math.round(h*dpr)){c.width=w*dpr;c.height=h*dpr;}
 const x=c.getContext('2d');x.setTransform(dpr,0,0,dpr,0,0);x.clearRect(0,0,w,h);
 const cs=getComputedStyle(document.documentElement);
 x.strokeStyle=cs.getPropertyValue('--grid');x.lineWidth=1;
 for(let i=1;i<4;i++){x.beginPath();x.moveTo(0,h*i/4);x.lineTo(w,h*i/4);x.stroke();}
 if(hist.length<2)return;
 const S=[{k:'v',c:'#f59e0b'},{k:'a',c:'#3b82f6'},{k:'r',c:'#8b5cf6'}];
 S.forEach(s=>{
  const vs=hist.map(p=>p[s.k]);
  let mn=Math.min(...vs),mx=Math.max(...vs);
  if(mx-mn<1e-6){mn-=1;mx+=1;}
  const pad=(mx-mn)*.18;mn-=pad;mx+=pad;
  x.strokeStyle=s.c;x.lineWidth=2;x.beginPath();
  vs.forEach((v,i)=>{const px=i/(hist.length-1)*(w-4)+2;
   const py=h-6-(v-mn)/(mx-mn)*(h-22);i?x.lineTo(px,py):x.moveTo(px,py);});
  x.stroke();
 });
}
function meter(fillId,txtId,temp){
 const f=el(fillId);const p=Math.max(0,Math.min(100,temp));f.style.width=p+'%';
 f.style.background=temp<50?'#10b981':temp<70?'#f59e0b':'#ef4444';
 el(txtId).textContent=temp+' °C';}
/* ---------- main draw ---------- */
function draw(s){
 if(window.DEBUG===false)0;
 el('pcount').textContent='dbg: conn='+s.connected+' params='+(s.params?s.params.length:'null')+' monitor='+s.monitor;
 el('conn').textContent=s.connected?('bridge '+host+':'+port):'offline';
 el('conn').className='pill '+(s.connected?'ok':'bad');
 el('upd').textContent='updated '+new Date().toLocaleTimeString();
 el('stats').textContent=s.connected?('link ok · '+s.host+':'+s.port+' · sent '+s.tx+' frames · received '+s.rx+' frames'):'no link — press Connect';
 el('answer').textContent=s.connected?(s.rx?'✓ controller is answering':'controller not answering yet — check wiring & ignition'):'';
 el('answer').className=s.rx?'ok':'muted';
 if(s.esp_uart){
  el('uartinfo').innerHTML='UART bridge → RX <b>GPIO'+s.esp_uart.rx+'</b> · TX <b>GPIO'+s.esp_uart.tx+
   '</b> · <b>'+s.esp_uart.baud+'</b> baud '+(s.esp_uart.rx!==18?'<span class=st bad>(swapped)</span>':'');
  const sel=el('baudsel');if(sel)sel.value=s.esp_uart.baud;
 } else el('uartinfo').textContent='';
 const kp=el('keylessPanel');
 if(kp){
  if(!s.keyless_host) kp.hidden=true;
  else{
   kp.hidden=false;
   if(!s.keyless){
    el('kState').textContent='module offline';
    el('kState').className='pill';
    el('kInfo').textContent='no response from '+s.keyless_host+' — check the keyless module (power / IP moved?)';
   }else{
    const k=s.keyless,f=k.fob||{};
    el('kState').textContent=k.armed?'🔒 ARMED':'🔓 disarmed';
    el('kState').className='pill '+(k.armed?'bad':'ok');
    let info='fobs '+(f.count||1)+
     (f.mac?' · '+esc(f.mac)+(f.count>1?' +'+(f.count-1)+' more':''):' (none learned)')+
     (f.present?' · <b style=color:#10b981>present</b> ('+f.rssi+' dBm)'
               :' · absent'+(f.ageS>=0&&f.ageS<3600?' · seen '+f.ageS+' s ago':''));
    info+=' · vibration '+((k.vibe&&k.vibe.events)||0)+' event(s)';
    if(k.graceLeftS>0)info+=' · boot grace '+k.graceLeftS+' s';
    if(k.master)info+=' · <b style=color:#f59e0b>master switch OFF — keyless suspended</b>';
    if(k.ign)info+=' · <b style=color:#f59e0b>ignition ON — arm blocked</b>';
    else if(!k.ignwired&&k.relay&&k.relay.mode!==2)
      info+=' · <b style=color:#f59e0b>ignition sense not wired — do NOT install the E-LOCK cut yet</b>';
    if(k.alarm)info+=' · <b style=color:#ef4444>SIREN ON</b>';
    el('kInfo').innerHTML=info;
    if(k.alarm&&!window.kPrevAlarm){
     try{if(window.Notification&&Notification.permission==='granted')
      new Notification('VOTOL alarm!',{body:'Vibration alarm triggered at the bike'})}catch(e){}
     try{kBeep()}catch(e){}
    }
    window.kPrevAlarm=!!k.alarm;
   }
  }
 }
 if(s.params){const ov=s.params.find(r=>r[0].includes('Overvoltage'));
  if(ov){const v=parseFloat(ov[1]);if(v>20)vMax=Math.ceil((v+10)/10)*10;el('gVmax').textContent='range '+vMax+' V';}}
 const t=s.telemetry;
 if(t){
  maxA=Math.max(maxA,t.current_a);maxR=Math.max(maxR,t.rpm);
  setGauge('arcV','numV',t.voltage_v,vMax,t.voltage_v.toFixed(1));
  setGauge('arcA','numA',t.current_a,maxA,t.current_a.toFixed(1));
  setGauge('arcR','numR',t.rpm,maxR,t.rpm);
  el('lgV').textContent=t.voltage_v.toFixed(1)+' V';
  el('lgA').textContent=t.current_a.toFixed(1)+' A';
  el('lgR').textContent=t.rpm+' rpm';
  meter('mC','mCt',t.controller_temp_c);meter('mM','mMt',t.motor_temp_c);
  el('fGear').textContent='gear '+t.gear;el('fGear').className='flag on';
  el('fState').textContent='state '+t.status;
  el('fState').className='flag '+(t.status==='FAULT'?'alert':(t.status==='RUN'?'good':'on'));
  el('fBrake').className='flag '+(t.brake?'alert':'');
  el('fRev').className='flag '+(t.reverse?'on':'');
  el('fRegen').className='flag '+(t.regen?'good':'');
  el('fFault').textContent='fault '+t.fault_code;
  el('fFault').className='flag '+(t.fault_code?'alert':'');
  if(t.ts!==lastTs){lastTs=t.ts;hist.push({v:t.voltage_v,a:t.current_a,r:t.rpm});
   if(hist.length>240)hist.shift();drawChart();}
 } else {
  ['fGear','fState','fBrake','fRev','fRegen','fFault'].forEach(i=>el(i).className='flag');
 }
 setLock(!(s.connected&&s.params&&s.params.length));
 if(s.params&&s.params.length){
  const changed=!lastParams||lastParams.length!==s.params.length;
  lastParams=s.params;
  if(changed)renderParams();
 }
 const cut=s.hexlog.findIndex(h=>h[0]===last);
 const news=(cut>=0?s.hexlog.slice(cut+1):s.hexlog).map(h=>`<span class=${h[1]=='TX'?'ok':'mut'}>${h[1]}</span> ${h[2]}`).join('\\n');
 if(news){el('log').innerHTML+=news+'\\n';el('log').scrollTop=1e9}
 if(s.hexlog.length)last=s.hexlog[s.hexlog.length-1][0];
}
window.addEventListener('resize',drawChart);
window.onerror=function(m,s,l){try{el('pcount').textContent='JS ERROR line '+l+': '+m}catch(e){}};
(function(){const v=(()=>{try{return localStorage.vtView}catch(e){return null}})()||'dash';switchView(v)})();
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    link = None  # class attribute: shared across request threads

    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path == "/":
            body = PAGE.replace("__HOST__", ESP_HOST).replace("__PORT__", str(ESP_PORT)).encode()
            self.send_response(200)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/wiring.svg":
            try:
                body = WIRING_SVG.read_bytes()
            except OSError:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Type", "image/svg+xml")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/state.json":
            ln = self.link
            self._json({
                "connected": ln.connected if ln else False,
                "monitor": bool(ln and ln.monitor.is_set()),
                "host": ln.host if ln else None,
                "port": ln.port if ln else None,
                "tx": ln.tx_count if ln else 0,
                "rx": ln.rx_count if ln else 0,
                "esp_uart": esp_uart_state(ln.host) if (ln and ln.connected) else None,
                "keyless": keyless_state(),
                "keyless_host": KEYLESS_HOST,
                "telemetry": ln.telemetry if ln else None,
                "params": ln.params if ln else None,
                "hexlog": ln.hexlog[-60:] if ln else [],
            })
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != "/api":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n) or b"{}")
        a = req.get("action")
        ln = self.link
        if a in ("connect", None):
            host = req.get("host", ESP_HOST)
            port = int(req.get("port", ESP_PORT))
            if ln and ln.connected and ln.host == host and ln.port == port:
                self._json({"ok": True, "msg": "already connected"})
            else:
                if ln:
                    ln.disconnect()
                    Handler.link = None
                ln = VotolLink(host, port)
                ok, msg = ln.connect()
                Handler.link = ln if ok else None
                self._json({"ok": ok, "msg": msg})
        elif a == "disconnect" and ln:
            ln.disconnect()
            Handler.link = None
            self._json({"ok": True})
        elif a in ("keyless_arm", "keyless_disarm", "keyless_panic"):
            # independent module — works even with no bridge link
            if not KEYLESS_HOST:
                self._json({"ok": False, "msg": "set KEYLESS_HOST in webapp/app.py first"}, 400)
            else:
                # /k/* works on both the standalone keyless module and the
                # combined bridge firmware (WITH_KEYLESS build)
                path = {"keyless_arm": "/k/arm", "keyless_disarm": "/k/disarm",
                        "keyless_panic": "/k/panic"}[a]
                resp = esp_http_get(KEYLESS_HOST, f"{path}?pin={KEYLESS_KEY}", timeout=2.5)
                keyless_state.cache = (0, None)
                if resp is None:
                    self._json({"ok": False, "msg": "keyless module unreachable", "keyless": None})
                elif "wrong or missing" in resp:
                    self._json({"ok": False, "msg": "wrong PIN — match KEYLESS_KEY to the module's access PIN", "keyless": keyless_state()})
                else:
                    st = keyless_state()
                    if a == "keyless_arm" and st and not st.get("armed"):
                        # module reachable but refused (e.g. ignition ON interlock)
                        self._json({"ok": False, "msg": "arm refused by the module (ignition ON?)", "keyless": st})
                    else:
                        self._json({"ok": True, "keyless": st})
        elif ln and a == "monitor_on":
            ln.monitor.set()
            self._json({"ok": True})
        elif ln and a == "monitor_off":
            ln.monitor.clear()
            self._json({"ok": True})
        elif ln and a == "read_params":
            self._json({"ok": ln.read_params()})
        elif ln and a == "reset":
            self._json({"ok": ln._send(cmd_reset())})
        elif ln and a == "hex":
            self._json({"ok": ln.send_hex(req.get("hex", ""))})
        elif a in ("swap_pins", "set_pins"):
            host = (ln.host if ln else req.get("host", ESP_HOST))
            if a == "swap_pins":
                esp_http_get(host, "/setpins?swap=1")
            else:
                esp_http_get(host, f"/setpins?rx={int(req.get('rx', 18))}&tx={int(req.get('tx', 19))}")
            esp_uart_state.cache = (0, None)      # invalidate cache
            self._json({"ok": True, "uart": esp_uart_state(host)})
        elif a == "set_baud":
            host = (ln.host if ln else req.get("host", ESP_HOST))
            rate = int(req.get("rate", 115200))
            esp_http_get(host, f"/baud?rate={rate}")
            esp_uart_state.cache = (0, None)
            self._json({"ok": True, "uart": esp_uart_state(host)})
        else:
            self._json({"ok": False, "msg": "not connected"}, 400)


def main():
    print(f"VOTOL dashboard: http://localhost:{HTTP_PORT}  (bridge {ESP_HOST}:{ESP_PORT})")
    ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
