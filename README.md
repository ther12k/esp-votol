# VOTOL ESP32 Adapter

Two firmwares for one job — connecting the official VOTOL software to the
controller's 4-pin program socket without the USB cable — plus a standalone
keyless & alarm module for the bike:

| Firmware | Type | Use when | Status |
|---|---|---|---|
| **`votol-bt-bridge/`** (PlatformIO/Arduino) | Bluetooth Classic SPP | you use the **VOTOL phone app**, like the HC-05 in the [video](https://www.youtube.com/watch?v=f4K75fIzMHE) | currently flashed |
| `votol-wifi-bridge.yaml` (ESPHome) | WiFi TCP server (port 6638) + virtual COM | you use the **VOTOL PC software** on a laptop over WiFi | ready to flash |
| **`keyless-alarm/`** (PlatformIO/Arduino) | BLE keyfob presence + immobilizer relay | you want **keyless arm/disarm + alarm**, like widely-used motorcycle units (evolution of `Keyless Motor/`) | ready to flash |

ESPHome cannot do Bluetooth Classic (SPP) — only BLE — which is why the
Bluetooth variant is an Arduino sketch instead. It still gets OTA updates
(via WiFi), so USB is only needed for the first flash.

**WiFi credentials are local-only:** the PlatformIO firmwares read them from
`src/wifi_secrets.h` in each project — copy the committed
`src/wifi_secrets.h.example`, fill in your SSID/password, and never commit
the real file (it's gitignored). Without it the firmware builds with a
harmless placeholder and opens the setup hotspot. The ESPHome variant uses
its captive portal instead (no credentials in the yaml).

## Wiring (same for both firmwares)

Pins chosen so the wires cluster together (DOIT DevKit V1 layout — three
of them on consecutive pins `D21 · GND · D19 · D18` on the right column):

| VOTOL socket | Wire color (video) | ESP32 pin | Note |
|---|---|---|---|
| GND  | Black | **GND** (right column, between D21 and D19) | common ground |
| TX   | Green | **GPIO18 (RX)**, directly below D19 | **5V logic → use a divider (1kΩ/2kΩ) or level shifter** |
| RX   | White | **GPIO19 (TX)**, directly below the GND | 3.3V is read as HIGH by the VOTOL, direct is OK |
| +5V  | Red   | **VIN** (bottom corner, next to a GND) | powers the ESP32 from the controller |

Serial: **115200 8N1** (VOTOL standard; try 9600 only on older units —
`Serial2.begin()` in the sketch / `baud_rate:` in the YAML).

> Board pin caveats: on **WROVER (PSRAM) modules GPIO16/17 are reserved for
> the PSRAM** — never move the UART there (the earlier note suggesting 16/17
> as WROVER spares was backwards; WROOM boards like the ones used here can
> use them). Don't have USB and the VOTOL +5V connected at the same time.
> The ESP32 GPIO absolute max is 3.6 V: the table's "3.3 V TX is OK for the
> VOTOL" is an assumption that held on this bench, not a datasheet guarantee
> for your controller — measure the port's levels first and add a series
> resistor / level check before trusting it long-term.

## Bluetooth variant (phone app — like the video)

On the phone: pair with **`VOTOL-BT`** (PIN **`1234`**, like an HC-05),
then open the VOTOL app and connect. Onboard LED: short blink = waiting,
solid = phone connected.

### Status page (check wiring from any browser)

`http://192.168.1.190/` (or `http://votol-bt-bridge.local/` when mDNS
resolves — with Bluetooth running, multicast is flaky, prefer the IP).
Shows: phone-app connection state, WiFi/IP, uptime, **TX/RX byte counters
of the controller link**, the last bytes from the controller in hex, and a
"send test bytes" button. That button sends a raw `55 AA 55 AA` electrical
probe — **it is NOT a valid controller request**: a missing reply to it says
nothing about the controller link. Use the dashboard's framed commands
(Read parameters / Monitor) as the real controller-response test. Reading
the counters:

- **RX > 0 while using the app** — wiring and baud are correct
- **TX grows but RX stays 0** — TX/RX swapped, wrong baud, or missing GND
- **Both stay 0** — app not connected, or controller unpowered

Note: mDNS/OTA by name can fail while Bluetooth is active — use the IP
(`pio run -e votol-bt-bridge-ota -t upload --upload-port 192.168.1.190`).

## Keyless & alarm module (`keyless-alarm/`)

Auto-arms when the BLE keyfob walks away, disarms when it returns; armed +
vibration = 30 s siren. Up to **4 fobs** — iTag keychain **and** phone beacon
together (bike disarms while *any* is near; phone matched by advertised name
so Android's rotating BLE MAC doesn't break it) — and an open dashboard page
beeps/notifies the phone when the alarm fires. The immobilizer is a relay
**NC contact in series with the key/E-LOCK+ line** — module dead or powered
off = contact closed = bike still rideable (fail-safe, never strands you).
Boot always starts disarmed for a grace period. **Arming is only allowed with
the key OFF**: a 1 MΩ/27 kΩ sense divider on GPIO33 reads the E-LOCK line and
the firmware refuses to arm (and refuses the relay self-test) while it is hot,
so a lost fob signal can never cut the controller mid-ride. Old-style
ignition-NO mode gets the same guarantee via a **run latch** (the contact
survives fob dropouts while the key is on, like the original sketch's saklar),
and an optional latching **master switch on GPIO32** (the saklar itself)
suspends keyless entirely when off. Control endpoints require an access PIN
(default `1234`). Read
**[keyless-alarm/WIRING.md](keyless-alarm/WIRING.md)** before wiring the cut —
in particular the relay **contact DC rating must be ≥ the pack's max charge
voltage** (the common SRD-05VDC 30 VDC contact is *not* adequate on a 72 V
line) and the one-chip build must move VIN off the program-port 5V to a
battery-side supply.

Two ways to run it (wiring is identical — see
**[keyless-alarm/WIRING.md](keyless-alarm/WIRING.md)** for the full guide,
power options and the self-powering trap):

- **Separate ESP32** (default, `keyless-alarm/` → env `votol-keyless`) — the
  alarm keeps guarding the bike even if the bridge firmware crashes, and the
  bridge's Bluetooth stays dedicated to the VOTOL app link.
- **One chip** (`votol-bt-bridge/` → env `votol-bt-bridge-keyless`) — the same
  board that does VOTOL-BT + TCP 6638 also runs keyless; its BLE scan is async
  so the bridge passthrough never stalls, endpoints live under `/k/*` and the
  keyless rows appear on the bridge status page.

Everything is configurable on the module's web page (`/` or `/k/scan`):
learn the fob from a live BLE scan, RSSI threshold, arm-after timeout, relay
mode (immobilizer / ignition / alarm-only), vibration sensor on/off.
Settings live in NVS.

There is also a **TenunJS phone app** ([tenunjs `examples/votol`](https://github.com/ther12k/tenunjs/pull/215)):
telemetry rings, parameter browser, and the same keyless panel as a
display-list app, fed by this backend (`bun run votol:preview` in the
tenunjs checkout).

The web dashboard shows a 🔒/🔓 panel (fob RSSI, vibration events, Arm /
Disarm / Panic) — set `KEYLESS_HOST` in `webapp/app.py` to the module's IP
(separate build) **or the bridge IP** (one-chip build); empty keeps the panel
hidden. `KEYLESS_KEY` must match the module's access PIN.

Flash over USB:

```bash
sg dialout -c "cd ~/Workspace/Learning/esp-votol/keyless-alarm && \
  ../.venv/bin/pio run -e votol-keyless -t upload --upload-port /dev/ttyUSB0"
```

OTA: `pio run -e votol-keyless-ota -t upload --upload-port votol-keyless.local`

## Web dashboard (local app — control from a browser)

`webapp/app.py` — stdlib-only Python app; the ESP32 stays a dumb bridge and
the laptop speaks the VOTOL protocol over TCP 6638:

```bash
python3 webapp/app.py          # then open http://localhost:8080
```

Features: live telemetry cards (voltage, current, RPM, controller/motor
temp, gear, status, fault code), full parameter read (all 7 packets decoded
into a table), controller soft-reset, and a raw hex console. Telemetry
polling always uses **LOCAL mode (0xAA)** — it observes the controller and
never drives the motor. Protocol source: `research/VotolAIO` "Packket
sniff" notes (GPL-3.0).

Bench-test the whole stack without the bike:

```bash
python3 tools/fake_votol.py      # fake EM100s on 127.0.0.1:6638 (self-contained)
python3 webapp/app.py            # connect to 127.0.0.1:6638 in the UI
```

Related projects checked while designing this: Revvia (browser tuning, but
Web Serial only — can't reach a TCP bridge), VotolAIO's Windows tools +
EM100s emulator (protocol source), bananu7/votol (RE notes).

WiFi is configured to auto-connect to the home network at boot (SSID/password
in the gitignored `src/wifi_secrets.h`). If that network is unreachable for 3 minutes, the
**`VOTOL-BT-Setup`** hotspot opens so credentials can be changed without
reflashing. Bluetooth keeps working regardless.

Flash over USB:

```bash
sg dialout -c "cd ~/Workspace/Learning/esp-votol/votol-bt-bridge && \
  ../.venv/bin/pio run -t upload --upload-port /dev/ttyUSB0"
```

OTA update over WiFi (no cable, works whenever the ESP32 is powered and in
WiFi range — e.g. installed on the bike):

```bash
cd ~/Workspace/Learning/esp-votol/votol-bt-bridge
../.venv/bin/pio run -e votol-bt-bridge-ota -t upload --upload-port votol-bt-bridge.local
```

Notes: WiFi modem sleep stays at the default on purpose — the ESP32 refuses
WiFi-without-sleep while Bluetooth Classic is running (coexistence rule).
Expect ping jitter of ~50–150 ms; irrelevant for OTA and configuration use.

## WiFi variant (VOTOL PC software over network)

The ESP32 runs a raw TCP server on **port 6638**; create a virtual COM with
[HW VSP3](https://www.hw-group.com/software/hw-vsp3-virtual-serial-port)
pointing at `<esp-ip>:6638`, select that COM in the VOTOL software.

Flash over USB: `./flash.sh` (auto-detects the board, re-execs through
`sg dialout` if the current shell lacks the group). First boot opens the
hotspot `votol-wifi-bridge` (`votol1234`) to enter your WiFi. Status page
and live logs at `http://votol-wifi-bridge.local/`.

OTA update over WiFi:

```bash
.venv/bin/esphome run votol-wifi-bridge.yaml --device votol-wifi-bridge.local
```

Note: `components/stream_server/` is a vendored copy of
`oxan/esphome-stream-server` fixed for ESPHome ≥ 2026.9 (upstream calls a
removed API). Don't point the YAML back at the GitHub source.

## One-time host setup (already done on this machine)

```bash
python3 -m venv .venv && .venv/bin/pip install esphome   # ESPHome + PlatformIO
sudo usermod -aG dialout $USER                           # serial port access
```

## Troubleshooting

- **VOTOL software won't connect / no reply** — try baud 9600 (older units),
  check TX/RX are crossed, check GND.
- **Garbage data** — wrong baud rate or missing ground.
- **`VOTOL-BT` not visible on the phone** — power-cycle the ESP32, make sure
  it finished booting (~10 s); the LED blinks when ready.
- **Serial permission denied** — `sudo usermod -aG dialout $USER`, re-login,
  or prefix commands with `sg dialout -c "..."` as shown above.

## Known limitations (tracked, not yet fixed)

- **No exclusive UART ownership:** the bridge forwards TCP, Bluetooth and USB
  serial to the controller concurrently — two clients can interleave partial
  commands. Until fixed, use ONE client at a time (review recommendation:
  single active writer + busy status).
- **Dashboard API is unauthenticated** on whatever `BIND_HOST` allows — keep
  it on a trusted LAN or set `BIND_HOST = "127.0.0.1"`.
- **BLE presence is identification, not authentication** (MAC/name match);
  the physical key remains the real security boundary.
- Keyless firmware fixes are compile- and model-verified; relay behavior on
  real hardware (boot, brownout, sensor faults) still needs bench validation
  with the E-LOCK cut disconnected.
- The ignition-sense divider can still be defeated by a sense wire that stays
  broken for >10 s while riding (mitigated, not eliminated — see WIRING.md
  "Wire-break handling"); a current-loop sense input is the future fix.
