# VOTOL Pod Remote — UI/UX Designer Handoff & Specifications

This document accompanies the screenshots for designing or redesigning the **VOTOL Pod Remote** mobile app UI/UX.

---

## 1. Live Interactive Web Preview & Gallery

The application is currently running live in the browser and accessible over LAN:
- **Interactive App (Live Simulation):** [http://192.168.1.55:8096/](http://192.168.1.55:8096/) or [http://localhost:8096/](http://localhost:8096/)
- **Visual Design Gallery (All Screens):** [http://192.168.1.55:8096/gallery.html](http://192.168.1.55:8096/gallery.html) or [http://localhost:8096/gallery.html](http://localhost:8096/gallery.html)

---

## 2. Screenshot Files

High-resolution screenshots are located in `/home/ther12k/Workspace/Learning/esp-votol/output/screenshots/`:

| File Name | Screen / State | Description |
|---|---|---|
| `01_control_armed_mobile.png` | **Remote Control (ARMED)** | Hero card in Crimson (`#2A1115`) with bold prohibited 🚫 symbol, 72.4V battery, fob status, and large single **[ 🔓 DISARM ]** Emerald Green toggle button. |
| `02_control_disarmed_mobile.png` | **Remote Control (DISARMED)** | Hero card in Dark Emerald (`#10241C`) with unlocked 🔓 symbol, and single **[ 🔒 ARM ]** Crimson outline toggle button. |
| `03_control_offline_mobile.png` | **Remote Control (OFFLINE)** | Hero card showing 📡 OFFLINE when BLE connection is severed, with dimmed toggle button. |
| `04_arm_confirm_modal_mobile.png` | **ARM Confirmation Dialog** | Safety dialog modal asking *"ARM the alarm? Pod screen will enter armed standby mode."* to prevent pocket/accidental arming. |
| `05_config_tab_mobile.png` | **Configuration Tab** | Settings surface: Security PIN update, wheel circumference speed calibration, physical iTag fob management, and VOTOL Bridge CAN/BT link toggle. |
| `06_pairing_tab_mobile.png` | **Device Pairing Tab** | Pair surface: Prominent **📷 Scan QR Code** camera button (for scanning pod LCD `SYS → SET`), plus fallback 32-hex secret key entry. |
| `07_mockup_armed.png` | **Presentation Mockup (ARMED)** | Framed smartphone mockup view. |
| `08_mockup_disarmed.png` | **Presentation Mockup (DISARMED)** | Framed smartphone mockup view. |
| `09_interactive_inspector_overview.png` | **Full Interactive Inspector** | Desktop view with interactive state-switching toolbar. |

---

## 3. Product & Functional Flow Overview

### What the App Controls:
- An **ESP32 WEMOS D1 R32 display pod** mounted on an electric motorcycle handlebar with a 2.4" TFT LCD and keyless immobilizer relay.
- The pod communicates with this app via **Bluetooth Low Energy (BLE)** GATT server.

### Key UX Principles:
1. **Single Context-Aware Toggle Button:**
   - Instead of separate Arm and Disarm buttons, the hero control action is a single, large (76dp high, 20dp rounded) toggle button.
   - When the bike is **ARMED**, the button is bright Emerald Green `[ 🔓 DISARM ]` for instant one-tap unlocking.
   - When the bike is **DISARMED**, the button changes to Crimson `[ 🔒 ARM ]`. Tapping it presents a quick confirmation dialog before locking to prevent accidental arming while riding.
2. **Glanceable Telemetry Hero Card:**
   - Big symbol + status word (`🚫 ARMED` / `🔓 DISARMED`).
   - High-contrast Cyan battery voltage readout (e.g. `72.4 V`).
   - Fob presence indicator (`near` / `away`).
3. **Quick Actions:**
   - `Connect / Disconnect` button (Tonal style).
   - `🚨 PANIC` button (Crimson style) for immediate alarm chirp / siren trigger.
4. **Bottom Navigation (Material 3):**
   - 3 tabs: **Control** (daily riding/security), **Config** (bike settings), **Pairing** (QR camera pairing).

---

## 4. Current Color Palette & Design Tokens

- **Background:** `#07090D` (Deep obsidian black)
- **Surfaces & Cards:** `#161B27` (Hero status card), `#11151F` (Settings cards), `#0B0E14` (Bottom nav bar)
- **Card Borders:** `1dp solid #2A3242`
- **Primary Accents:**
  - Amber `#F59E0B`: Brand titles, badges, scan QR button
  - Emerald Green `#10B981`: Disarmed state, DISARM action button, connected badge
  - Crimson Red `#EF4444`: Armed state, ARM button, Panic button, danger actions
  - Electric Cyan `#00C8FF`: High-visibility battery voltage
- **Typography:** Inter / Roboto for UI text, JetBrains Mono for hex keys and device logs.
