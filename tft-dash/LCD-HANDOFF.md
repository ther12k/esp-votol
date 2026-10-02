# Handoff: programming the 2.4" TFT LCD + touch on the WEMOS D1 R32

Everything you need to build your own firmware/app on this exact
hardware, distilled from a working project (`tft-dash/`). Read §8
(gotchas) before writing code — every item there cost us real debugging
time.

## 1. The hardware

| Thing | What it is |
|---|---|
| Board | WEMOS D1 R32 (clone: "Wifi&Bluetooth R32 Base ESP32 V1.0.0") — ESP32-WROOM in Arduino UNO form factor, micro-USB (CH340). **Ours: ESP32-D0WD-V3, MAC `b4:bf:e9:22:85:dc`** (how to tell it from the other ESP32s on the desk: §10.1) |
| Shield | 2.4" UNO TFT shield, mcufriend-style **8-bit parallel**, controller **ILI9341** (reads ID `0x9341`), 240×320 |
| Touch | **resistive film** wired to 4 UNO pins shared with the LCD — there is **no XPT2046 touch chip** on this shield (proven empirically) |
| SD slot | present, unused (its pins are free — see §3) |
| Jumpers | **3 wires soldered/socketed on the ESP board** (§2) — required, the LCD does not work without them |

## 2. Why 3 jumper wires (the one non-obvious thing)

The shield expects a real Arduino UNO, where every header pin is
bidirectional. On the D1 R32 the UNO positions **A2/A3/A4 are wired to
ESP32 GPIO 35/34/36 — input-only pins** (no output driver in silicon).
The shield's LCD control lines RS/CS/RST land exactly there, so they are
dead ends straight through the socket.

Fix: jumper them to output-capable GPIOs **on the ESP board itself**
(same-number sockets are the same net inside the PCB, so the shield's
signals that arrive via the outer sockets are also on the inner rails):

| From (inner rail socket) | To (ESP pin) | Carries |
|---|---|---|
| **35** | **15** | LCD_RS |
| **34** | **33** | LCD_CS |
| **36** | **32** | LCD_RST |

Safe because each wire creates one net with exactly ONE talker
(GPIO15/33/32, output-capable) and one silent listener (35/34/36 —
input-only, can never contend). See `wiring-board-jumpers.png` for the
physical layout. Cross-confirmed by three sources: the arduino-esp32
`d1_uno32` variant file, MCUFRIEND_kbv's ESP32 pin block, and
Bodmer's TFT_eSPI discussion #2299.

## 3. Complete pin map (as we use it)

LCD (13 pins) — the MCUFRIEND_kbv official D1-R32 block:

| Function | GPIO | Note |
|---|---|---|
| data D0–D7 | **12, 13, 26, 25, 17, 16, 27, 14** | straight through the UNO digital rail |
| LCD_RD | **2** | also the on-board LED — never use as an LED |
| LCD_WR | **4** | |
| LCD_RS | **15** | via jumper from the 35/A2 net |
| LCD_CS | **33** | via jumper from the 34/A3 net |
| LCD_RST | **32** | via jumper from the 36/A4 net |

Touch film (4 pins, **shared with LCD nets** — variant "0" mapping):

| Film | GPIO | Note |
|---|---|---|
| X+ | **27** | shared with LCD data D6 |
| X− | **15** | the RS jumper net (GPIO35 listens on it too) |
| Y+ | **4** | shared with LCD_WR |
| Y− | **14** | shared with LCD data D7 |

Free pins: **5, 18, 19, 21, 22, 23** (+ **39**, input-only). On our pod
GPIO5 = buzzer. SD slot shares 18/19/23 + CS (we used CS=21 probe once).

**Never use:** GPIO **0** (boot strap), **1/3** (USB serial), and the
empty through-holes labeled **SD0/SD1/SD2/SD3/CMD/CLK = GPIO 6–11 —
that's the internal flash bus**, connecting anything there can brick the
board. GPIO 34/35/36/39 have no output drivers and no internal pull-ups.

## 4. Toolchain & project setup

PlatformIO (`pipx/uv tool install platformio`), no other tooling.
`platformio.ini` that is proven to work:

```ini
[env:myapp]
platform = espressif32@7.1.3        # pinned — reproducible builds
board = esp32dev                    # D1 R32 = ESP32-WROOM in UNO clothes
framework = arduino
monitor_speed = 115200
upload_speed = 460800               # 921600 corrupted boot/NVS on our clone — twice
lib_deps =
    https://github.com/prenticedavid/MCUFRIEND_kbv.git#v3.0.0   # by git tag; the registry name fails
    adafruit/Adafruit GFX Library@1.11.9
# board_build.partitions = huge_app.csv   # only needed if you add BLE (default 1.3MB app overflows)
```

Build/flash: `pio run -e myapp -t upload`. Serial: `pio device monitor`
works interactively; in scripts use pyserial (§9).

## 5. Bringing the LCD up (minimal code)

```cpp
#include <MCUFRIEND_kbv.h>
MCUFRIEND_kbv tft;

void setup() {
  Serial.begin(115200);
  uint16_t id = tft.readID();          // expect 0x9341
  if (id == 0x0000 || id == 0xD3D3) id = 0x9341;   // 0xD3D3 = floating bus (no jumpers)
  tft.begin(id);
  tft.setRotation(0);                  // 0 = portrait 240x320, USB-left
  Serial.printf("LCD id 0x%04X\n", id);
  tft.fillScreen(tft.color565(10, 16, 26));
  tft.setTextColor(tft.color565(222, 230, 240));
  tft.setTextSize(2);                  // size 2 = 12x16 px/char
  tft.setCursor(8, 30);
  tft.print("hello bike");
  tft.fillCircle(120, 160, 40, tft.color565(0, 200, 255));
}
void loop() {}
```

`readID() == 0xD3D3` means the control lines are floating — reseat the
shield / check the 3 jumpers, don't chase software. All Adafruit-GFX
primitives work (`fillRect`, `drawCircle`, `drawLine`, `print`, …).

## 6. Reading the touch film

The film is a resistive divider. To read X: drive X+/X− with 3.3V/GND,
measure the tap position on Y+; same swapped for Y. Pressure = read two
cross-plated channels. Everything is plain `analogRead()` + `pinMode`
juggling — **no touch library, no SPI**.

**THE rule that shapes the architecture:** the film's analog pins
(4, 15-net, 27, 14) are all **ADC2** channels, and the ESP32's WiFi
driver owns ADC2 whenever WiFi is active. So: **touch and WiFi cannot
run at the same time.** Bluetooth Classic + BLE are fine. Our app runs
WiFi only for the first 30 s after boot (OTA window), then
`WiFi.mode(WIFI_OFF)` and touch comes alive.

Because the film pins are shared with LCD lines, deafen the LCD while
sampling (CS high), then restore:

```cpp
struct FilmMap { uint8_t xp, yp, xm, ym; };
const FilmMap M = {27, 4, 15, 14};          // variant 0 (see calibration)
#define TR_MIN 500
#define TR_MAX 4095

void lcdQuiesce() { digitalWrite(33, HIGH); }         // CS high = LCD ignores the bus
void lcdRestore() {                                   // give the nets back to the LCD
  pinMode(M.xp, OUTPUT); pinMode(M.xm, OUTPUT);
  pinMode(M.yp, OUTPUT); pinMode(M.ym, OUTPUT);
  digitalWrite(4, HIGH);                              // WR idle high
  digitalWrite(33, LOW);                              // CS active again
}
int median3(int a, int b, int c) {
  int mx = max(a, max(b, c)), mn = min(a, min(b, c));
  return a + b + c - mx - mn;
}
// pressure-only probe (cheap): use this in the idle loop
int filmZ() {
  lcdQuiesce();
  pinMode(M.xp, OUTPUT); digitalWrite(M.xp, LOW);
  pinMode(M.ym, OUTPUT); digitalWrite(M.ym, HIGH);
  pinMode(M.xm, INPUT); pinMode(M.yp, INPUT);
  delayMicroseconds(250);
  int z1 = analogRead(M.xm), z2 = analogRead(M.yp);
  lcdRestore();
  return 4095 - (z2 - z1);
}
bool readTouch(int &px, int &py) {          // -> screen pixel, false if idle
  int z = filmZ();
  if (z < 300 || z > 3800) return false;    // idle / hard-press garbage filter
  lcdQuiesce();
  int s[3];
  pinMode(M.yp, INPUT); pinMode(M.ym, INPUT);
  pinMode(M.xp, OUTPUT); pinMode(M.xm, OUTPUT);
  digitalWrite(M.xp, HIGH); digitalWrite(M.xm, LOW);
  delayMicroseconds(500);
  s[0]=analogRead(M.yp); s[1]=analogRead(M.yp); s[2]=analogRead(M.yp);
  int rx = 4095 - median3(s[0], s[1], s[2]);        // X
  pinMode(M.xp, INPUT); pinMode(M.xm, INPUT);
  pinMode(M.yp, OUTPUT); pinMode(M.ym, OUTPUT);
  digitalWrite(M.yp, HIGH); digitalWrite(M.ym, LOW);
  delayMicroseconds(500);
  s[0]=analogRead(M.xm); s[1]=analogRead(M.xm); s[2]=analogRead(M.xm);
  int ry = 4095 - median3(s[0], s[1], s[2]);        // Y
  lcdRestore();
  px = map(rx, TR_MIN, TR_MAX, 0, 240);
  py = map(ry, TR_MIN, TR_MAX, 0, 320);
  // this unit needs Y flipped — see calibration below
  py = 319 - py;
  px = constrain(px, 0, 239); py = constrain(py, 0, 319);
  return true;
}
```

**Calibration (ours, from real presses — yours may differ):**
- center press ≈ raw(2156, 2344); bottom-left ≈ raw(1040, 745)
- X natural, **Y inverted** → flip Y (as in the code above)
- a mid-screen press proves wiring but NEVER orientation — calibrate
  with a **corner**
- if taps land mirrored/swapped: swap the X/Y mapping or flip an axis;
  keep the settings in NVS (Preferences) so they survive reboots

**Feel:** sample at ~30 ms, debounce ~180 ms, pressure-first probe (the
`filmZ()` gate) so the idle loop stays cheap. That combination gave us
reliable, instant-feeling taps with this film.

## 7. UI patterns that worked

- **Slot-cached painting:** wrap every text field in a struct that
  remembers the last string+colors; skip the redraw when unchanged.
  Repainting everything every loop flickers badly on this parallel bus.
- **Hit zones, not widgets:** one big invisible rectangle per button,
  `px/py` range checks. Finger-friendly minimum ≈ 40 px tall; our tab
  bar is 3 × 80 px.
- Default GFX font is 6×12 px per char × size. Width = `6 * size *
  strlen(text)` — always check against the box before renaming labels
  (we clipped "KEYLESS" in an 80 px tab once).
- No °/µ/emoji in the default font — use plain ASCII ("C", "--").
- RGB565 colors via `tft.color565(r,g,b)` or a macro
  `((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3))`.

## 8. Gotchas & hard rules (each one bit us)

1. **Backlight is hardwired to 3.3V** — no pin, no PWM, always on.
   "Screen off" = paint black (`fillScreen(0x0000)`).
2. **Never `pushCommand(0x28)` (DISPOFF)** — with the backlight hard-
   wired the panel shows **white**, not black.
3. **Touch vs WiFi** — ADC2 is owned by WiFi (§6). WiFi and touch are
   mutually exclusive on this shield.
4. **Upload at 460800, not 921600** — the fast rate twice left our
   clone boot-looping with wiped NVS. If the board ever boot-loops with
   `assert do_core_init` / `NVS Error 261`: erase + reflash (§9).
5. **GPIO 6–11 holes are the flash bus** — never connect anything.
6. GPIO 2 is LCD_RD **and** the on-board LED; GPIO 0/1/3 are boot/USB.
7. `readID()` 0xD3D3 = wiring (jumper/shield seat), not software.
8. Touch sampling borrows LCD nets — always quiesce/restore (§6), or
   the LCD glitches during touches.
9. If you add BLE, use `huge_app.csv` partitions — BLE + anything
   overflows the default 1.3 MB app slot. Partition change ⇒ full chip
   erase required.
10. After any full erase, NVS settings (touch calibration etc.) are
    gone — re-apply them.

## 9. Flashing & serial from a script

```bash
pio run -e myapp -t upload          # flash (retry once on a flake — esptool
                                    # occasionally drops packets; a retry always works)
```

`pio device monitor` refuses non-interactive shells. Scripted serial
instead (resets the board through the CH340):

```python
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)   # reset pulse
print(s.read(2000))                  # boot log
```

Full recovery from a corrupted flash/NVS (boot-loop, Error 261):

```bash
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32 --port /dev/ttyUSB0 --baud 115200 erase_flash
pio run -e myapp -t upload
```

`python3 -m esptool` does NOT work on this host — the system python has
no esptool module; always call the PlatformIO-bundled script by path
(and via `python3` — the `.py` is not executable).

## 10. Deploying the tft-dash firmware (runbook)

The exact procedure we use to push a new build onto the pod.

### 10.1 Identify the pod FIRST (several ESP32s live on this desk)

`ls /dev/ttyUSB*` — the pod is a **CH340** (`udevadm info -q property -n
/dev/ttyUSB0 | grep ID_MODEL_FROM_DATABASE`), ours is **ESP32-D0WD-V3,
MAC `b4:bf:e9:22:85:dc`**. A CH340 + ESP32 is *necessary but not
sufficient* — the bridge board looks the same. Confirm by sniffing the
boot banner (the tft firmware says `[tft-dash]`):

```python
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.5)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False   # reset pulse
t0 = time.time(); out = b""
while time.time() - t0 < 3: out += s.read(256)
print(out.decode('utf-8', 'replace'))   # expect "[tft-dash] LCD id 0x9341 ..."
```

Do **not** trust "capture right after `esptool chip_id` exits" — the
banner prints within ~1 s of the reset and you will lose the race; open
the port and pulse RTS yourself (as above). Flashing the wrong board
overwrites that board's firmware.

### 10.2 Flash (USB — the default path)

```bash
cd tft-dash
pio run -e votol-dash -t upload --upload-port /dev/ttyUSB0
```

~30 s; success = `Hash of data verified` + hard reset. A normal upload
**keeps NVS** — registered fobs, touch calibration, wheel size, PIN and
pair key all survive. Use `erase_flash` (§9) only for recovery, never
routinely.

### 10.3 OTA alternative (rarely worth it)

OTA answers only during the **first 30 s after boot** (then WiFi goes
off for touch, §6) — you must power-cycle the pod and immediately:

```bash
pio run -e votol-dash-ota -t upload --upload-port votol-dash.local
```

(UDP 3232, mDNS `votol-dash.local`.) USB needs no timing, so it is the
default. **At the bike: never USB and bike-5V power at the same time** —
unplug the bike feed before plugging USB.

### 10.4 Post-flash verification (serial CLI — no eyes needed)

Capture per §10.1 after the flash reset. Healthy boot shows:

```
[tft-dash] LCD id 0x9341
[tft-dash] fobs: N            (N = your fob count, NVS survived)
[tft-dash] wifi window 30s, then BT->VOTOL-BT
[tft-dash] wifi off — touch + BT mode
[kl] armed silently — fob off since boot      <-- fob away: NORMAL
```

The pod boots with the display DARK when the fob is away (armed) — the
banner is on serial even though the screen is black; don't re-flash,
just send single-letter commands (no newline needed):

| Key | Effect |
|---|---|
| `d` | display on, 2-min override — the screen lights now |
| `a` / `A` | play the ARM / DISARM animation (needs display on; one queued while dark fires the moment it wakes) |
| `L` | list fobs — proves NVS |
| `p` | send one SHOW poll frame |
| `B` | BT link on/off toggle |

Any panic / `Guru Meditation` / spontaneous reboot in the log = bad
build, not bad luck. At the desk `[bt] connect failed` retries are
expected (the VOTOL-BT bridge isn't here); it links at the bike.

## 11. Where to look in this repo

- `tft-dash/src/main.cpp` — the full app: LCD + touch + BLE keyless +
  BT + display power management. The touch section (§6 here) and the
  slot-cached painter are lifted straight from it.
- `tft-dash/src/diag.cpp` — LCD tester (ID + color bars + touch
  streaming). Flash this first on a new board.
- `tft-dash/src/touchprobe.cpp` — the one-shot probe that proved no
  XPT2046 exists behind the UNO sockets.
- `tft-dash/WIRING.md` — wiring + the product features built on top.
- `tft-dash/wiring-diagram.png`, `wiring-board-jumpers.png` — diagrams
  (generator scripts: `tools_gen_diagram*.py`).
