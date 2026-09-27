# Keyless & Alarm Module — Wiring Guide

Standalone ESP32 that guards the bike independently of the VOTOL bridge.
Firmware: [`src/main.cpp`](src/main.cpp) · Behavior, config and troubleshooting live on
its web page (`http://<module-ip>/`), not in code.

## One chip or two?

The keyless firmware exists in **two builds** — the wiring (relay, buzzer,
sensor, buttons, power, E-LOCK+ cut) is **identical** for both:

| Build | Where | Flash env | Notes |
|---|---|---|---|
| **Separate module** (default) | its own ESP32 | `keyless-alarm/` → `votol-keyless` | alarm survives bridge crashes/reflashes; bridge BT link untouched |
| **One chip** (combined) | the bridge ESP32 | `votol-bt-bridge/` → `votol-bt-bridge-keyless` | same board also does VOTOL-BT SPP + TCP 6638; keyless endpoints under `/k/*`, status rows on the bridge page |

The combined build runs the BLE scan **async** (never blocks the VOTOL
passthrough) and pauses arm-while-disarmed logic while the phone app is
connected (app in use = you're at the bike). It compiles at 87% flash / 20%
RAM, but BLE + Bluetooth Classic + WiFi on one radio is the tightest combo —
**on first boot check the Free heap row on the status page (want > ~40 KB) and
that the phone app still connects**. If either misbehaves, flash the plain
bridge env and use the separate module. Dashboard: same 🔒 panel either way —
set `KEYLESS_HOST` to the module's IP, or to the **bridge IP** in one-chip mode.

## Shopping list

| Part | ~Price (IDR) | Notes |
|---|---|---|
| ESP32 devkit (CP2102/CH340) | 50–80k | any 30/38-pin WROOM-32 board |
| Relay module **with NC terminal** | 10–20k | SRD-05VDC-SL-C on a 3-screw terminal board. ⚠️ many cheap boards expose only COM+NO — **you need the NC screw**. ⚠️⚠️ **SRD-05VDC contacts are rated 30 VDC — NOT enough for a 72 V-class E-LOCK cut.** For the immobilizer you need a relay whose **contact DC rating ≥ your pack's max charge voltage** (e.g. 100 VDC-rated signal relays like the Songle OJ series) — check the datasheet, not the board's "10 A" marketing. A 30 VDC relay will arc/weld on a 72 V line even at small current. While bench testing (contacts unconnected) any relay is fine. |
| 2× resistors for ignition sense | 2k | 1 MΩ + 27 kΩ (¼ W), see "Ignition sense" below — **required before the E-LOCK cut** |
| Active buzzer module | 5–10k | must be the *active* (oscillator) type, like the old `Keyless Motor` build |
| SW-420 vibration sensor | 10–15k | optional — leave firmware checkbox OFF until wired |
| Momentary button | – | optional arm/disarm override to GND |
| Latching switch (saklar) | 5k | optional master switch GPIO32→GND — OFF suspends keyless |
| BLE tag (iTag / Nut) | 15–25k | or reuse the one from the old build |
| 1 A inline fuse + holder | 5k | goes in the immobilizer cut |

## Module wiring (ESP32 side)

| ESP32 pin | Connects to |
|---|---|
| **GPIO26** | relay module **IN** |
| **GPIO25** | active buzzer **I/S** (buzzer + to GPIO25, − to GND if bare) |
| **GPIO34** | SW-420 **DO** (input-only pin; module VCC→**3V3** — never 5V, DO would swing above 3.3V) |
| **GPIO33** | ignition-sense divider midpoint (see below) — **required before the E-LOCK cut** |
| **GPIO32** | master switch ("saklar", the old build's run switch) → GND — latching toggle, optional |
| **GPIO27** | override button → GND (hold 2 s toggles arm/disarm) |
| **GPIO2** | onboard LED (already wired): slow blink = disarmed/scanning · solid = fob near · fast = armed · very fast = siren |
| 5V / VIN | 5V supply (see power section) |
| GND | GND (common with relay module and sensor) |

Relay module also needs VCC→5V and GND. If its coil energizes on LOW (most blue
opto boards), tick **"active-LOW"** in the web settings — verify with the
**[Test click+chirp]** button: 3 clicks then *silence* is correct in disarmed state.

## Ignition sense (REQUIRED before installing the E-LOCK cut)

Without this input the firmware cannot tell "parked" from "being ridden": a
lost/weak fob signal would arm while you ride and **cut the controller brain
mid-ride**. The divider tells the module when the key is ON, and while it is:

- **arming is refused** (auto *and* manual — dashboard/button get a refusal
  chirp, state shows "ignition ON — arm blocked"),
- the arm-after countdown is held, and
- **[Test click+chirp]** is refused (its clicks would pulse the cut line).

```
controller side of the E-LOCK+ cut (AFTER the relay NC contact)
        │
     [1 MΩ]
        ├──► ESP32 GPIO33        (ADC1_CH5 — works with WiFi on)
     [27 kΩ]   (optional: 3.3V zener + 100nF across this resistor
        │       for load-dump/transient protection)
       GND
```

- 35–100 V packs read 0.6–1.7 V at the midpoint — well within ADC range; the
  firmware threshold is ≈0.25 V, so 35 V (min E-LOCK) still registers hot.
- Current draw is ~0.1 mA — negligible.
- Internal pulldown: **an unwired GPIO33 safely reads "key off"**, so the
  module still works on the bench; you just MUST tick *"Ignition sense wired"*
  in settings (and physically wire it) **before** connecting the relay
  contacts into E-LOCK+. Until then the status page and dashboard show a
  warning.
- Test it before the cut goes live: key OFF → state shows no "ignition ON";
  key ON (relay still bypassed) → "ignition ON — arm blocked" appears and Arm
  is refused with the deny chirp.

## Master switch — the old build's "saklar" (optional, GPIO32)

The old `Keyless Motor` sketch had a run switch (`saklar`): the relay latched
ON and **stayed on even when the fob disappeared** — only flipping that
switch released it. The new firmware gives you the same guarantees two ways:

1. **Automatically (recommended)** — the ignition sense above. In the default
   immobilizer-NC mode the contact is closed whenever the bike is disarmed,
   and arming is refused while the key is ON, so a lost fob never stops a
   running bike. In old-style *ignition-NO* mode the contact now **latches
   while the key is ON**: it engages when the fob is near, then survives fob
   dropouts until you turn the key off (this mode therefore *requires* the
   ignition-sense wire).
2. **Manually** — wire the old saklar as a latching switch from **GPIO32 to
   GND** and tick *"Master switch wired"* in settings. Switch **ON** (closed
   to GND) = keyless active. Switch **OFF** (open) = keyless completely
   suspended: no arming, no siren, relay forced to its idle state — NC mode:
   contact closed, bike always rideable (valet/workshop mode); ignition-NO
   mode: contact open, kontak off — exactly like the old build. Arming while
   OFF is refused with the deny chirp.

GPIO32 because the input-only pins (34–39) have no internal pull-up; an
unwired GPIO32 reads ON, so the module works normally without the switch.

## Immobilizer wiring (the actual anti-theft cut)

The VOTOL controller brain is powered from the battery through the **E-LOCK+**
line (gray/purple; needs ≥35V, per the programming manual). Cut that line
anywhere in series and route it through the relay's **NC (normally-closed)
contact**:

```
battery+/key-switch ──[1A fuse]──● cut ●── relay COM
                                 relay NC ──● cut ●── E-LOCK+ → controller
```

- **DISARMED** → coil idle → NC contact closed → bike behaves exactly as before.
- **ARMED** → coil energizes → contact opens → controller brain unpowered →
  motor cannot run (throttle does nothing, display dead).
- **Module dead / loses power / crashes** → coil releases → contact closes →
  **bike still rideable** — you can never be stranded by the alarm.
- Boot always starts DISARMED for a grace period (default 60 s) as a second
  anti-strand layer.
- **Arming is only ever allowed with the key OFF** (ignition sense above), so
  the cut can never open while the bike is in use.
- The E-LOCK+ feed carries pack voltage (72V class) but only logic-buck current
  (well under 0.5A); the 1A fuse guards the new wiring run. **The relay contact
  must be DC-rated ≥ the pack's max charge voltage** (see shopping list) — the
  common SRD-05VDC 30 VDC contact is NOT adequate. If you prefer not to cut
  anything yet, leave the relay contacts unconnected — the module still
  arms/disarms/sirens (and the relay clicks so you can hear it working).
- ⚠️ After the cut is installed, **[Test click+chirp] pulses the E-LOCK line** —
  the firmware refuses it while the key is ON, but only run it with the bike
  parked and the key off.

⚠️ **Self-powering trap — read before choosing a power source.** If you power
this module from the controller's program-port +5V (or any 5V derived from
E-LOCK+), arming cuts the module's own power → coil releases → power returns →
module reboots → re-arms → cuts again → **permanent click/boot loop**.

## Power options (pick ONE, never USB + external 5V together)

1. **Recommended — always-on battery-side 5V**: the bike's USB charger output,
   or a 72V→5V buck fused 1A straight off the battery. Auto-arm works with the
   key off; the module draws ~80–150mA (BLE+WiFi peaks). Verify the chosen tap
   is actually always-on with the key off and E-LOCK open (some USB sockets are
   switched) and budget for parked drain.
2. **Bench/flashing**: USB from laptop (same rule as the bridge ESP).

⚠️ **One-chip (combined) build: this REPLACES the bridge's old power wire.**
The plain bridge is powered from the controller program-port +5V — that rail is
E-LOCK-derived, which is exactly the self-powering trap below. Before flashing
`votol-bt-bridge-keyless` onto the bridge board, **move its VIN from the
program-port +5V to a battery-side always-on 5V** (and never USB + VIN
together, as always).

## Fobs: keychain iTag + phone together

Up to **4 fobs** can be learned; the bike stays disarmed while **any** of them
is nearby and arms only when **all** are gone. Entries match by **MAC**
(iTags — stable) or by **advertised NAME** (phones). Each entry is added from
the scan page (`[+ by MAC]` / `[+ by name]`) and removed with the ✕ next to it.

**Phone as fob (Android):** phones don't advertise BLE by default — install a
beacon app (e.g. *Beacon Simulator*, *Quick Beacon*) and let it advertise with
Bluetooth on. Then:

- If the app lets you set a **name/ID**, prefer **[+ by name]** — Android
  rotates the phone's BLE MAC address (on Bluetooth restart, or every ~15 min
  on some models), which silently breaks MAC entries. Name entries survive it.
- If the app only shows a bare advertisement with no name, add by MAC and
  re-learn from the scan page if the bike starts arming with the phone right
  there (that's the tell-tale of MAC rotation).
- Keep the **iTag as the primary fob** — it works with your phone dead, which
  is exactly when you still need to ride.

**iOS:** apps can't keep BLE advertising reliably in the background, so an
iPhone can't be a presence fob — use the dashboard Arm/Disarm from the phone
instead (works over WiFi on the laptop/phone browser).

**Alarm alert on the phone:** while the dashboard page is open (e.g. on the
phone at home), a vibration alarm makes it **beep loudly** and pop a
notification (tap any keyless button once first so the browser allows sound).
It's passive listening — only works with the page open, not a push service.

## First bring-up (do this BEFORE touching the bike)

1. `pio run -e votol-keyless -t upload` (or flash `firmware.bin`), USB powered.
2. It joins the WiFi from `src/wifi_secrets.h` (copy
   `src/wifi_secrets.h.example` and fill in yours) — find the IP:
   `nmap -sn 192.168.1.0/24`, or try
   `http://votol-keyless.local` (mDNS works better here than on the bridge — no
   BT Classic). Fallback hotspot after 3 min: `VOTOL-KEYLESS-Setup`.
3. Web page → **[learn new fob]** → scan → put the iTag next to the bike →
   rescan → **[+ by MAC]**. Add the phone beacon the same way (see the fobs
   section below). If you replace a tag, scan and add the new one.
4. **[Test click+chirp]** — 3 relay clicks + beep. Check coil idles after.
5. Bench-test the loop: fob near → page shows *present*; walk away (set
   *arm after* to 5 s temporarily) → auto-ARM + long beep; return → auto-DISARM.
6. Wire the ignition-sense divider (1 MΩ/27 kΩ, controller side of the future
   cut — or temporarily straight to the key-switched line), tick *Ignition
   sense wired* in settings, and verify: key off → no banner, key on →
   "ignition ON — arm blocked" and Arm is refused (deny chirp).
7. Only then wire the NC cut into E-LOCK+ (ignition off, fuse in, DC-rated
   relay per the shopping list, multimeter: disarmed = COM↔NC beeps; armed =
   open).
8. At the bike: DISARMED + key ON → motor runs as usual; ARMED (key off) →
   controller dead. Remember: armed also kills the program port, so the bridge
   dashboard loses telemetry while armed — that's expected. With the key ON the
   module can never arm — that's the ride-safety interlock working.
9. Wire the SW-420 and only then tick *enable vibration* in settings (GPIO34
   floats when unwired → false alarms if enabled early).

## Dashboard integration

`webapp/app.py` reads `http://<module-ip>/state.json` (set `KEYLESS_HOST`) and
adds a 🔒/🔓 panel with Arm/Disarm/Panic to the main dashboard — proxied through
the backend, like all other ESP actions.

Arm/Disarm/Panic/fob/settings endpoints on the module require an **access PIN**
(default `1234`, change it in the module's settings page and mirror it in
`KEYLESS_KEY` in `webapp/app.py`). Status/state.json stays open for monitoring.
This keeps casual LAN users (or a borrowed phone) from disarming the bike —
it is not theft-proof: BLE presence fobs are spoofable by design and the web
has no per-user auth, so keep the physical key as the real boundary.
