/*
 * VOTOL bike — keyless + alarm module (separate ESP32, independent of the bridge)
 *
 * Evolves "Keyless Motor/Keyless_motor_itag.ino" (BLE iTag presence -> relay+buzzer)
 * into a non-blocking state machine with NVS config, fail-safe immobilizer relay,
 * optional SW-420 vibration sensor, web status/control page and OTA.
 *
 * Behavior ("like widely-used motorcycle alarms"):
 *   - fob (BLE tag) near  -> DISARMED   (relay contact closed in immobilizer mode)
 *   - fob gone > armAfter -> ARMED      (contact opens -> controller brain unpowered)
 *   - ARMED + vibration   -> siren 30 s (retriggerable)
 *   - boot always starts DISARMED for bootGrace seconds (a dead/rebooting module
 *     never strands you; NC wiring means module off = bike startable)
 *
 * Pins (strapping-safe; old sketch's GPIO12/14 deliberately avoided):
 *   relay GPIO26, buzzer GPIO25, SW-420 DO GPIO34 (input-only), button GPIO27, LED GPIO2
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>

// WiFi credentials live in src/wifi_secrets.h (gitignored — copy from
// wifi_secrets.h.example). Missing file = harmless placeholder build.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#pragma message("wifi_secrets.h not found — using placeholder credentials")
#define WIFI_SSID "CONFIGURE-ME"
#define WIFI_PASS "CONFIGURE-ME"
#endif

#define LED_PIN 2
#define RELAY_PIN 26
#define BUZZER_PIN 25
#define VIBE_PIN 34    // input-only, no internal pull: SW-420 module drives it
#define BUTTON_PIN 27  // optional button to GND; hold 2 s = arm/disarm toggle
// Ignition/key sense: tap the CONTROLLER side of the E-LOCK+ cut through a
// divider (1M top / 27k bottom, see WIRING.md). Key ON = line hot = arming is
// REFUSED, so a lost fob signal can never cut power while the bike is in use.
// Internal pulldown keeps an unwired pin reading cold (arming still works).
#define IGN_PIN 33
// Master switch (the old build's "saklar"): latching switch to GND.
// OFF (open, pin HIGH) suspends keyless entirely — relay forced to its idle
// state (NC closed = bike rideable, NO open = kontak off, like the old
// sketch), arming refused, siren off. GPIO32 (not 34-39: input-only pins
// have no internal pull).
#define MASTER_PIN 32

#define PORTAL_AFTER_MS 180000UL
#define SIREN_S 30UL          // siren window per trigger / panic
#define VIBE_SETTLE_MS 8000UL // ignore shakes this long after arming
#define VIBE_DEBOUNCE_MS 500UL

enum RelayMode : uint8_t {
  RM_IMMOBILIZER_NC = 0,  // fail-safe: energize (contact OPENS) only when armed
  RM_IGNITION_NO = 1,     // old-sketch style: energize (contact closes) while fob present
  RM_ALARM_ONLY = 2       // nothing cuts the bike; relay follows the siren (strobe output)
};

// page header as a plain macro (F()/PSTR are not valid at global scope)
#define PAGE_TOP \
  "<!DOCTYPE html><html><head><meta charset=utf-8>" \
  "<meta http-equiv=refresh content=3><title>VOTOL Keyless</title>" \
  "<meta name=viewport content='width=device-width,initial-scale=1'>" \
  "<style>body{font-family:sans-serif;margin:16px;background:#f5f5f5}" \
  "table{border-collapse:collapse;background:#fff;box-shadow:0 1px 3px #aaa;margin-bottom:14px}" \
  "td,th{border:1px solid #ddd;padding:8px 14px;text-align:left}" \
  "th{background:#2c3e50;color:#fff}" \
  ".ok{color:#0a7d15;font-weight:bold}.bad{color:#c0392b;font-weight:bold}.dim{color:#888}" \
  "a{text-decoration:none;color:#2563eb;font-weight:600}" \
  "input,select{margin:4px 0}" \
  "button{padding:6px 12px;margin:2px;cursor:pointer}" \
  ".btn{display:inline-block;padding:6px 12px;margin:2px;border:1px solid #2563eb;" \
  "border-radius:4px;color:#2563eb;background:#fff;font-weight:600}" \
  ".row{background:#fff;padding:10px 14px;box-shadow:0 1px 3px #aaa;margin-bottom:14px}" \
  "</style></head><body><h2>VOTOL Keyless &amp; Alarm</h2><table>"

// ---- persisted config (Preferences "vtkl") ----
#define MAX_FOBS 4
// fob entries: "AA:BB:CC:DD:EE:FF" (match by MAC — iTags) or "N:name"
// (match by advertised NAME — phone beacon apps, whose BLE MAC rotates)
String fobs[MAX_FOBS];
int fobCount = 0;
int seenIdx = -1;  // entry matched in the last successful scan
int rssiThr = -80;             // a fob must beat this to count as present
uint32_t armAfterS = 30;       // fob absent this long -> auto arm
uint32_t bootGraceS = 60;      // boot starts DISARMED at least this long
uint8_t presentHits = 2;       // consecutive scan hits required
uint8_t relayMode = RM_IMMOBILIZER_NC;
bool relayActiveLow = false;   // relay module IN energizes on LOW (common opto boards)
bool vibeEnable = false;       // OFF until an SW-420 is actually wired (GPIO34 floats)
String webPin = "1234";        // access PIN for state-changing web endpoints
bool ignSenseWired = false;    // IGN divider connected; MUST be ON before wiring the E-LOCK cut
bool benchMode = false;        // relay contacts guaranteed DISCONNECTED — relaxes interlocks for bench testing
bool masterWired = false;      // saklar switch connected to GPIO32
bool masterOff = false;        // saklar OFF: keyless suspended (bike stays usable)

// Tri-state ignition: UNKNOWN (sense not wired/validated) is never treated as
// "key off" — a cut command requires COLD, or benchMode.
enum IgnState : uint8_t { IGN_UNKNOWN = 0, IGN_COLD = 1, IGN_HOT = 2 };
IgnState ignNow = IGN_UNKNOWN;
bool ignRawHot = false;            // instantaneous ADC reading (wired only)
uint32_t ignColdSinceMs = 0;       // HOT->COLD must persist this long:
#define IGN_COLD_CONFIRM_MS 10000UL  // a broken sense wire must not instantly
                                     // masquerade as "key off" mid-ride
uint32_t ignFaultMs = 0;           // sense HOT while the NC cut is OPEN = fault

// ---- runtime state ----
bool armed = false;
bool fobSeenThisScan = false;
bool fobPresent = false;
uint8_t consecHits = 0;
int fobRssi = 0;
uint32_t lastSeenMs = 0;       // 0 = never seen since boot
uint32_t absentSinceMs = 0;
uint32_t bootMs = 0;
uint32_t armedSinceMs = 0;
uint32_t alarmStartMs = 0;   // siren window START (elapsed math survives millis() wrap)
uint16_t vibeEvents = 0;
uint32_t lastVibeMs = 0;

Preferences prefs;
BLEScan *pScan = nullptr;
WiFiManager wm;
WebServer web(80);
bool portalStarted = false;

String sanitizeFob(const String &e);  // defined with the web handlers

// ---- one-shot BLE device list for /scan (fob learning) ----
bool collectScan = false;
struct ScanEntry { String mac, name; int rssi; };
ScanEntry scanList[15];
int scanCount = 0;

// ---------------- buzzer: non-blocking pattern player ----------------
// steps alternate [on_ms, off_ms, on_ms, ...]; trailing 0 terminates; repeat loops
struct Pattern { const uint16_t *steps; uint8_t n; bool repeat; };
static const uint16_t ST_ARM[]    = {600, 0};
static const uint16_t ST_DISARM[] = {150, 150, 150, 0};
static const uint16_t ST_BOOT[]   = {80, 80, 80, 0};
static const uint16_t ST_DENY[]   = {100, 100, 100, 0};  // action refused
static const uint16_t ST_SIREN[]  = {400, 250};
static const Pattern PAT_ARM    {ST_ARM,    2, false};
static const Pattern PAT_DISARM {ST_DISARM, 4, false};
static const Pattern PAT_BOOT   {ST_BOOT,   4, false};
static const Pattern PAT_DENY   {ST_DENY,   4, false};
static const Pattern PAT_SIREN  {ST_SIREN,  2, true};

Pattern curPat{nullptr, 0, false};
bool patOn = false;
uint8_t patIdx = 0;
uint32_t patMs = 0;

bool sirenActive() { return alarmStartMs && millis() - alarmStartMs < SIREN_S * 1000UL; }

void playPattern(const Pattern &p, bool force = false) {
  if (!force && sirenActive() && curPat.steps == ST_SIREN) return;  // siren wins
  curPat = p;
  patIdx = 0;
  patOn = true;
  patMs = millis();
  digitalWrite(BUZZER_PIN, HIGH);
}
void stopPattern() {
  curPat.steps = nullptr;
  digitalWrite(BUZZER_PIN, LOW);
}
void patternTick() {
  if (!curPat.steps) return;
  if (curPat.steps == ST_SIREN && !sirenActive()) {  // siren window over
    alarmStartMs = 0;
    stopPattern();
    return;
  }
  uint32_t now = millis();
  if (now - patMs < (uint32_t)curPat.steps[patIdx]) return;  // current slot not over
  patMs = now;
  patOn = !patOn;
  digitalWrite(BUZZER_PIN, patOn ? HIGH : LOW);
  uint8_t next = patIdx + 1;                       // advance EVERY slot: [on,off,on,off,...]
  if (next < curPat.n && curPat.steps[next] != 0) patIdx = next;
  else if (curPat.repeat) { patIdx = 0; patOn = true; digitalWrite(BUZZER_PIN, HIGH); }
  else stopPattern();
}

// ---------------- relay ----------------
int relayEnergizeLevel() { return relayActiveLow ? LOW : HIGH; }
int relayIdleLevel() { return relayActiveLow ? HIGH : LOW; }
bool relayShouldEnergize() {
  if (masterOff) return false;  // saklar OFF: relay always idle (old-sketch "kontak off")
  switch (relayMode) {
    case RM_IMMOBILIZER_NC: return armed;                    // armed -> contact OPEN
    // old-sketch fob-follow contact, but LATCHED while the key is ON: once
    // you're riding, a dropped/missed fob scan can NOT open the contact —
    // it releases when the key turns off (or the fob is absent with the key
    // off, or the master switch opens). Requires the ignition sense wire.
    case RM_IGNITION_NO:    return !armed && (fobPresent || ignNow == IGN_HOT);
    case RM_ALARM_ONLY:     return sirenActive();            // strobe/siren output
  }
  return false;
}
bool relayAppliedE = false;  // last GPIO level actually written (requested != applied is a bug)
void applyRelay() {
  relayAppliedE = relayShouldEnergize();
  digitalWrite(RELAY_PIN, relayAppliedE ? relayEnergizeLevel() : relayIdleLevel());
}

bool armingCutsPower() { return relayMode == RM_IMMOBILIZER_NC || relayMode == RM_IGNITION_NO; }

// ---- central safety gate: EVERY path that would newly energize the coil in a
// power-cutting mode passes through gateFor() — auto-arm, manual arm, relay
// test, settings changes (which gate the PROPOSED configuration, not the
// current one). 0 = allowed.
//   1 master OFF · 2 ignition HOT · 3 ignition sense not confirmed COLD
int gateFor(uint8_t mode, bool bench) {
  if (masterOff) return 1;
  if (bench) return 0;         // contacts physically disconnected
  if (mode != RM_IMMOBILIZER_NC && mode != RM_IGNITION_NO) return 0;
  if (ignNow == IGN_HOT) return 2;
  if (ignNow == IGN_UNKNOWN) return 3;
  return 0;
}
int armGate() { return gateFor(relayMode, benchMode); }
const char *armGateText(int g) {
  switch (g) {
    case 1: return "master switch (saklar) is OFF";
    case 2: return "ignition/key line is hot";
    case 3: return "ignition sense not validated (wire the divider, or tick bench mode)";
    default: return "";
  }
}

// ---------------- fob matching ----------------
int matchFob(const String &mac, const String &name) {
  for (int i = 0; i < fobCount; i++) {
    if (fobs[i].length() == 17 && fobs[i] == mac) return i;
    if (fobs[i].startsWith("N:") && fobs[i].length() > 2 && name.length() &&
        name.equalsIgnoreCase(fobs[i].substring(2))) return i;
  }
  return -1;
}
int findFob(const String &entry) {
  for (int i = 0; i < fobCount; i++) if (fobs[i] == entry) return i;
  return -1;
}

// ---------------- escaping (BLE names are attacker-controlled input) ----------------
String htmlEsc(const String &s) {
  String o;
  o.reserve(s.length());
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += F("&amp;");
    else if (c == '<') o += F("&lt;");
    else if (c == '>') o += F("&gt;");
    else if (c == '"') o += F("&quot;");
    else if (c == '\'') o += F("&#39;");
    else o += c;
  }
  return o;
}
String urlEnc(const String &s) {
  String o;
  o.reserve(s.length());
  const char *hex = "0123456789ABCDEF";
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum(c)) o += c;
    else { o += '%'; o += hex[(uint8_t)c >> 4]; o += hex[(uint8_t)c & 15]; }
  }
  return o;
}

// ---------------- config persistence ----------------
void loadCfg() {
  prefs.begin("vtkl", true);
  String all = prefs.getString("fob", "");   // comma-joined, backward compatible
  rssiThr        = prefs.getInt("rssi", -80);
  armAfterS      = prefs.getUInt("armAfter", 30);
  bootGraceS     = prefs.getUInt("grace", 60);
  presentHits    = prefs.getUChar("hits", 2);
  relayMode      = prefs.getUChar("mode", RM_IMMOBILIZER_NC);
  relayActiveLow = prefs.getBool("actlow", false);
  vibeEnable     = prefs.getBool("vibe", false);
  ignSenseWired  = prefs.getBool("ignwired", false);
  masterWired    = prefs.getBool("masterwired", false);
  benchMode      = prefs.getBool("bench", false);
  webPin         = prefs.getString("pin", "1234");
  if (!webPin.length()) webPin = "1234";
  prefs.end();
  fobCount = 0;
  int s = 0;
  while (s >= 0 && fobCount < MAX_FOBS) {
    int c = all.indexOf(',', s);
    String e = (c < 0) ? all.substring(s) : all.substring(s, c);
    e.trim();
    e = sanitizeFob(e);   // legacy NVS entries may predate the character filter
    if (e.length() && findFob(e) < 0) fobs[fobCount++] = e;
    s = (c < 0) ? -1 : c + 1;
  }
}
void saveCfg() {
  prefs.begin("vtkl", false);
  String all;
  for (int i = 0; i < fobCount; i++) {
    if (i) all += ",";
    all += fobs[i];
  }
  prefs.putString("fob", all);
  prefs.putInt("rssi", rssiThr);
  prefs.putUInt("armAfter", armAfterS);
  prefs.putUInt("grace", bootGraceS);
  prefs.putUChar("hits", presentHits);
  prefs.putUChar("mode", relayMode);
  prefs.putBool("actlow", relayActiveLow);
  prefs.putBool("vibe", vibeEnable);
  prefs.putBool("ignwired", ignSenseWired);
  prefs.putBool("masterwired", masterWired);
  prefs.putBool("bench", benchMode);
  prefs.putString("pin", webPin);
  prefs.end();
}

// ---------------- BLE ----------------
class ScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    String mac = d.getAddress().toString().c_str();
    mac.toUpperCase();
    if (collectScan && scanCount < 15) {
      bool dup = false;
      for (int i = 0; i < scanCount; i++) if (scanList[i].mac == mac) dup = true;
      if (!dup) {
        scanList[scanCount].mac = mac;
        scanList[scanCount].name = d.haveName() ? String(d.getName().c_str()) : String("");
        scanList[scanCount].rssi = d.getRSSI();
        scanCount++;
      }
    }
    String nm = d.haveName() ? String(d.getName().c_str()) : String("");
    int m = matchFob(mac, nm);
    if (m >= 0) {
      fobSeenThisScan = true;
      fobRssi = d.getRSSI();
      seenIdx = m;
      if (!collectScan) pScan->stop();  // found one — cut the blocking scan short
    }
  }
};

void scanCycle() {  // one blocking BLE pass (callback may end it early)
  fobSeenThisScan = false;
  pScan->setActiveScan(true);
  pScan->start(1, false);
  if (collectScan) collectScan = false;  // one-shot list built
  if (fobSeenThisScan && fobRssi > rssiThr) {
    lastSeenMs = millis();
    if (consecHits < 255) consecHits++;
  } else if (lastSeenMs == 0 || millis() - lastSeenMs > 8000) {
    consecHits = 0;  // never seen, or a few missed cycles = truly gone
  }
}

// ---------------- state machine ----------------
void setArmed(bool a, bool manual);  // defined below tryArm

// Ignition/key sense: divider 1M/27k from the controller side of the E-LOCK+
// cut → 35–100 V packs read 0.6–1.7 V. Threshold ≈0.25 V (ADC ~310/4095),
// averaged over 4 samples; internal pulldown makes an unwired pin read cold.
bool ignHot() {
  if (!ignSenseWired) return false;
  int s = 0;
  for (int i = 0; i < 4; i++) s += analogRead(IGN_PIN);
  return (s >> 2) > 310;
}

bool armingCutsPower();  // defined with the relay logic above

// Manual/auto arm goes through here, gated by the central safety policy
// (armGate). Refusals chirp + log; the dashboard sees the same state.
bool tryArm(bool a, bool manual) {
  if (a) {
    int g = armGate();
    if (g) {
      playPattern(PAT_DENY, true);
      Serial.printf("[KEY] arm REFUSED — %s\n", armGateText(g));
      return false;
    }
  }
  setArmed(a, manual);
  return true;
}

void setArmed(bool a, bool manual) {
  if (a == armed) {
    if (manual) absentSinceMs = millis();  // manual disarm: restart the re-arm clock
    return;
  }
  armed = a;
  armedSinceMs = millis();
  absentSinceMs = millis();
  playPattern(a ? PAT_ARM : PAT_DISARM, true);
  applyRelay();
  Serial.printf("[KEY] %s (%s)\n", a ? "ARMED" : "DISARMED", manual ? "manual" : "auto");
}

void keylessTick() {
  uint32_t now = millis();
  bool wasMasterOff = masterOff;
  masterOff = masterWired && digitalRead(MASTER_PIN) == HIGH;
  if (masterOff != wasMasterOff) {
    Serial.printf("[KEY] master switch %s — keyless %s\n",
                  masterOff ? "OFF" : "ON", masterOff ? "SUSPENDED" : "active");
    if (masterOff) playPattern(PAT_DISARM, true);
    applyRelay();
  }
  if (masterOff) {
    // saklar OFF suspends everything: disarm, never arm, no siren
    if (armed) setArmed(false, false);
    if (sirenActive()) { alarmStartMs = 0; stopPattern(); }
    absentSinceMs = now;
    ignNow = IGN_UNKNOWN;
    patternTick();   // keep the suspend chirp playing / finish patterns
    return;
  }

  ignRawHot = ignHot();
  if (!ignSenseWired) { ignNow = IGN_UNKNOWN; ignColdSinceMs = 0; }
  else if (ignRawHot) { ignNow = IGN_HOT; ignColdSinceMs = 0; }
  else if (ignNow == IGN_COLD) { /* stays confirmed while continuously cold */ }
  else {
    // HOT -> cold: only trust it after IGN_COLD_CONFIRM_MS of continuous
    // cold. An open/broken sense wire while riding shows exactly this
    // transition — during the window the state is UNKNOWN, which blocks
    // arming (residual risk after the window is documented in WIRING.md).
    if (!ignColdSinceMs) ignColdSinceMs = now;
    ignNow = (now - ignColdSinceMs >= IGN_COLD_CONFIRM_MS) ? IGN_COLD : IGN_UNKNOWN;
  }
  // sense sanity: with the NC cut OPEN (armed + coil applied), the downstream
  // tap MUST be cold. Hot here = divider wired upstream of the relay, welded
  // contacts, or a short — fail SAFE by disarming (contact closes).
  if (ignSenseWired && armed && relayMode == RM_IMMOBILIZER_NC && relayAppliedE) {
    if (ignRawHot) {
      if (!ignFaultMs) ignFaultMs = now;
      else if (now - ignFaultMs > 2000) {
        Serial.println("[SENSE] FAULT: line HOT while NC cut is open — disarming (fail-safe)");
        playPattern(PAT_DENY, true);
        setArmed(false, false);
        ignFaultMs = 0;
      }
    } else ignFaultMs = 0;
  } else ignFaultMs = 0;

  if (ignNow == IGN_HOT) absentSinceMs = now;   // key on: arm clock never runs while riding
  bool wasPresent = fobPresent;
  fobPresent = consecHits >= presentHits && lastSeenMs && (now - lastSeenMs < 8000);
  if (fobPresent) absentSinceMs = now;          // hold the clock while the fob is here:
                                                // leaving must cost the FULL armAfter,
                                                // not "grace since boot/disarm" (review #3)

  if (fobPresent && armed) setArmed(false, false);          // approach -> disarm
  else if (!fobPresent && !armed) {
    bool graceOver = now - bootMs > bootGraceS * 1000UL;
    bool goneEnough = now - absentSinceMs > armAfterS * 1000UL;
    if (graceOver && goneEnough && armGate() == 0) setArmed(true, false);  // walked away -> arm
  }
  if (fobPresent != wasPresent)
    Serial.printf("[KEY] fob %s (%d dBm)\n", fobPresent ? "present" : "gone", fobRssi);

  // vibration -> siren (only when armed, settled, and sensor enabled + wired)
  if (armed && vibeEnable && now - armedSinceMs > VIBE_SETTLE_MS && now - lastVibeMs > VIBE_DEBOUNCE_MS) {
    if (digitalRead(VIBE_PIN) == HIGH) {
      lastVibeMs = now;
      vibeEvents++;
      alarmStartMs = now;                     // (re)trigger
      playPattern(PAT_SIREN, true);
      Serial.printf("[ALARM] vibration #%u — siren %lus\n", vibeEvents, (unsigned long)SIREN_S);
    }
  }
  // keep the GPIO glued to the computed state: in ignition-NO mode the relay
  // depends on continuously-varying inputs (fob/ignition), not just arming
  // transitions — first fob after boot must close the contact (review #5)
  if (relayShouldEnergize() != relayAppliedE) applyRelay();
}

// ---------------- optional physical button: hold 2 s toggles ----------------
void buttonTick() {
  static uint32_t pressMs = 0;
  static bool held = false;
  bool down = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (down && !pressMs) { pressMs = now; held = false; }
  else if (down && pressMs && !held && now - pressMs > 2000) {
    held = true;
    tryArm(!armed, true);
  }
  else if (!down && pressMs) { pressMs = 0; }
}

// ---------------- web ----------------
// Two auth paths, by design:
//  - machine API: ?pin=<webPin> on any method (dashboard proxy, curl)
//  - browser session: one-time POST /auth login -> cookie (SameSite=Strict,
//    HttpOnly, 1 h). Cookies only count on POST, so a cross-site page can
//    neither read the credential nor trigger state changes via GET links.
// The PIN itself is NEVER rendered into any page (no disclosure via HTML).
uint32_t authTok = 0;  // per-boot session token; 0 = no session support
void redirectHome();   // defined below

bool authByPin() { return web.hasArg("pin") && web.arg("pin") == webPin; }
bool authByCookie() {
  if (!authTok || web.method() != HTTP_POST) return false;
  String c = web.header("Cookie");
  return c.length() && c.indexOf("kauth=" + String(authTok)) >= 0;
}
bool authed() { return authByPin() || authByCookie(); }
void denyAuth() { web.send(403, "text/plain", "not authorized — POST /auth with pin=, or pass ?pin="); }

void handleAuth() {
  if (!authByPin()) { denyAuth(); return; }
  web.sendHeader("Set-Cookie",
                 String("kauth=") + authTok + "; Path=/; Max-Age=3600; SameSite=Strict; HttpOnly");
  redirectHome();
}

String modeName() {
  switch (relayMode) {
    case RM_IMMOBILIZER_NC: return String(F("immobilizer (NC, fail-safe)"));
    case RM_IGNITION_NO:    return String(F("ignition (NO, fob-follow)"));
    default:                return String(F("alarm-only (siren output)"));
  }
}

String fobEntriesHtml(bool withRemove) {
  String s;
  for (int i = 0; i < fobCount; i++) {
    if (i) s += F(" &middot; ");
    s += F("<code>"); s += htmlEsc(fobs[i]); s += F("</code>");
    if (fobPresent && i == seenIdx) s += F(" <span class=ok>&#10003;</span>");
    if (withRemove) {
      s += F(" <form method=post action=/fob style='display:inline'>"
             "<input type=hidden name=del value='");
      s += htmlEsc(fobs[i]);
      s += F("'><button>&#10005;</button></form>");
    }
  }
  return fobCount ? s : String(F("<span class=dim>none learned</span>"));
}

void handleRoot() {
  uint32_t now = millis();
  uint32_t up = now / 1000;
  char upStr[20];
  sprintf(upStr, "%02u:%02u:%02u", (unsigned)(up / 3600), (unsigned)((up / 60) % 60), (unsigned)(up % 60));
  bool alarm = sirenActive();
  bool en = relayShouldEnergize();

  String h = String(PAGE_TOP);
  h += F("<tr><th>Status</th><td>");
  h += armed ? F("<span class=bad>&#128274; ARMED</span>") : F("<span class=ok>&#128275; disarmed</span>");
  if (armed && alarm) h += F(" <span class=bad>&#128680; SIREN</span>");
  if (masterOff) h += F(" <span class=bad>&#9888; master switch OFF &mdash; keyless suspended</span>");
  if (ignNow == IGN_HOT) h += F(" <span class=bad>&#9888; ignition ON &mdash; arming blocked</span>");
  else if (ignNow == IGN_UNKNOWN && armingCutsPower())
    h += benchMode ? F(" <span class=bad>&#9888; BENCH MODE &mdash; interlocks relaxed, relay contacts must be disconnected</span>")
                   : F(" <span class=bad>&#9888; ignition sense UNKNOWN &mdash; arming blocked (wire the divider, or tick bench mode)</span>");
  if (relayMode == RM_IGNITION_NO && ignNow != IGN_COLD && !benchMode)
    h += F(" <span class=bad>&#9888; ignition-NO mode needs a validated sense wire for the ride latch</span>");
  if (!armed && now - bootMs < bootGraceS * 1000UL) {
    h += String(F(" <span class=dim>(boot grace ")) + String((bootGraceS * 1000UL - (now - bootMs)) / 1000 + 1) + F(" s)</span>");
  }
  h += F("</td></tr><tr><th>Fobs (");
  h += String(fobCount);
  h += F("/");
  h += String(MAX_FOBS);
  h += F(")</th><td>");
  h += fobEntriesHtml(false);
  h += F("<br>");
  h += fobPresent ? F("<span class=ok>present</span>") : F("<span class=dim>absent</span>");
  if (lastSeenMs) { h += F(" &middot; last seen "); h += String((now - lastSeenMs) / 1000); h += F(" s ago"); }
  if (fobPresent) { h += F(" &middot; "); h += String(fobRssi); h += F(" dBm"); }
  h += F(" &mdash; <form method=post action=/scan style='display:inline'>"
         "<input type=hidden name=go value=1><button>[learn new fob]</button></form>");
  h += F("</td></tr><tr><th>Relay</th><td>");
  h += modeName();
  h += F(" &mdash; coil ");
  h += en ? F("<b>energized</b>") : F("idle");
  h += F(" &middot; GPIO ");
  h += (digitalRead(RELAY_PIN) == relayEnergizeLevel()) ? F("<b>energized</b>") : F("idle");
  h += F("</td></tr><tr><th>Alarm</th><td>");
  if (alarm) { h += F("<span class=bad>SIREN ON</span> ("); h += String(SIREN_S - (now - alarmStartMs) / 1000); h += F(" s left)"); }
  else h += F("<span class=dim>silent</span>");
  h += F(" &middot; vibration events ");
  h += String(vibeEvents);
  if (lastVibeMs) { h += F(" (last "); h += String((now - lastVibeMs) / 1000); h += F(" s ago)"); }
  if (!vibeEnable) h += F(" <span class=dim>(sensor disabled)</span>");
  h += F("</td></tr><tr><th>WiFi</th><td>");
  if (WiFi.status() == WL_CONNECTED) {
    h += WIFI_SSID; h += F(", "); h += WiFi.localIP().toString();
    h += F(" ("); h += String(WiFi.RSSI()); h += F(" dBm)");
  } else h += F("<span class=dim>not connected</span>");
  h += F("</td></tr><tr><th>Uptime</th><td>");
  h += upStr;
  h += F("</td></tr></table><p>"
         "<form method=post action=/arm style=display:inline><button>&#128274; Arm</button></form> "
         "<form method=post action=/disarm style=display:inline><button>&#128275; Disarm</button></form> "
         "<form method=post action=/panic style=display:inline><button>&#128680; Panic siren</button></form> "
         "<form method=post action=/test style=display:inline><button>Test click+chirp</button></form> "
         "<form method=post action=/reboot style=display:inline><button>Restart</button></form></p>"
         "<div class=row><form method=post action=/auth>"
         "PIN <input name=pin type=password size=10 autocomplete=off> <button>Login</button></form>"
         " <span class=dim>buttons need a one-time login (1 h) &mdash; or call endpoints with ?pin= as before</span></div>");

  h += F("<div class=row><b>Settings</b> (saved to NVS, kept across reboots)"
         "<form method=post action=/cfg><table style='box-shadow:none'>"
         "<tr><th>Fob RSSI threshold</th><td><select name=rssi>");
  int opts[5] = {-70, -75, -80, -85, -90};
  for (int o : opts) {
    h += F("<option"); if (o == rssiThr) h += F(" selected"); h += F(">"); h += String(o); h += F("</option>");
  }
  h += F("</select> dBm (higher = must be nearer)</td></tr>"
         "<tr><th>Arm after fob absent</th><td><input name=after type=number min=5 max=600 value=");
  h += String(armAfterS);
  h += F("> s</td></tr>"
         "<tr><th>Boot grace (always starts disarmed)</th><td><input name=grace type=number min=10 max=600 value=");
  h += String(bootGraceS);
  h += F("> s</td></tr><tr><th>Hits required</th><td><select name=hits>");
  for (int o = 1; o <= 4; o++) {
    h += F("<option"); if (o == presentHits) h += F(" selected"); h += F(">"); h += String(o); h += F("</option>");
  }
  h += F("</select> consecutive scans</td></tr>"
         "<tr><th>Relay mode</th><td><select name=mode>"
         "<option value=0");
  if (relayMode == 0) h += F(" selected");
  h += F(">immobilizer NC (fail-safe)</option><option value=1");
  if (relayMode == 1) h += F(" selected");
  h += F(">ignition NO (fob-follow)</option><option value=2");
  if (relayMode == 2) h += F(" selected");
  h += F(">alarm-only (siren output)</option></select></td></tr>"
         "<tr><th>Relay module active-LOW</th><td><input type=checkbox name=actlow value=1");
  if (relayActiveLow) h += F(" checked");
  h += F("> opto boards that energize on LOW</td></tr>"
         "<tr><th>Vibration sensor</th><td><input type=checkbox name=vibe value=1");
  if (vibeEnable) h += F(" checked");
  h += F("> enable SW-420 on GPIO34 (wire it first!)</td></tr>"
         "<tr><th>Ignition sense wired</th><td><input type=checkbox name=ignwired value=1");
  if (ignSenseWired) h += F(" checked");
  h += F("> divider on GPIO33 &mdash; a power-cut mode will refuse to arm while the sense is UNKNOWN or the key is ON</td></tr>"
         "<tr><th>Bench mode</th><td><input type=checkbox name=bench value=1");
  if (benchMode) h += F(" checked");
  h += F("> relay contacts are DISCONNECTED (bench/testing only) &mdash; relaxes the arm interlocks so you can test without the sense wire</td></tr>"
         "<tr><th>Master switch (saklar) wired</th><td><input type=checkbox name=masterwired value=1");
  if (masterWired) h += F(" checked");
  h += F("> latching switch on GPIO32 to GND &mdash; OFF suspends keyless: no arming, no siren, relay idle (bike stays usable)</td></tr>"
         "<tr><th>Access PIN</th><td>new PIN: "
         "<input name=newpin type=text maxlength=12 size=12 autocomplete=off>"
         " (min 4 chars, empty = keep current &mdash; the current PIN is never displayed)</td></tr>"
         "</table><button>Save settings</button></form></div>"
         "<p class=dim>Fail-safe: boot always starts DISARMED, and NC wiring keeps the bike rideable if this module dies.</p>"
         "</body></html>");
  web.send(200, "text/html", h);
}

void handleState() {
  uint32_t now = millis();
  String s = String(F("{\"armed\":"));
  s += armed ? "true" : "false";
  s += F(",\"alarm\":");
  s += sirenActive() ? "true" : "false";
  s += F(",\"fob\":{\"mac\":\"");
  s += (fobPresent && seenIdx >= 0) ? fobs[seenIdx] : String("");
  s += F("\",\"count\":");
  s += String(fobCount);
  s += F(",\"present\":");
  s += fobPresent ? "true" : "false";
  s += F(",\"rssi\":");
  s += String(fobRssi);
  s += F(",\"ageS\":");
  s += lastSeenMs ? String((now - lastSeenMs) / 1000) : String("-1");
  s += F("},\"relay\":{\"mode\":");
  s += String(relayMode);
  s += F(",\"energized\":");
  s += relayShouldEnergize() ? "true" : "false";
  s += F("},\"vibe\":{\"enabled\":");
  s += vibeEnable ? "true" : "false";
  s += F(",\"events\":");
  s += String(vibeEvents);
  s += F("},\"graceLeftS\":");
  s += (now - bootMs < bootGraceS * 1000UL) ? String((bootGraceS * 1000UL - (now - bootMs)) / 1000 + 1) : String("0");
  s += F(",\"ign\":");
  s += ignNow == IGN_HOT ? "true" : "false";
  s += F(",\"ignstate\":\"");
  s += ignNow == IGN_HOT ? "hot" : (ignNow == IGN_COLD ? "cold" : "unknown");
  s += F("\",\"bench\":");
  s += benchMode ? "true" : "false";
  s += F(",\"relaypin\":");
  s += digitalRead(RELAY_PIN) == relayEnergizeLevel() ? "true" : "false";
  s += F(",\"master\":");
  s += masterOff ? "true" : "false";
  s += F(",\"uptimeS\":");
  s += String(now / 1000);
  s += F(",\"ip\":\"");
  s += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
  s += F("\"}");
  web.send(200, "application/json", s);
}

void redirectHome() {
  web.sendHeader("Location", "/");
  web.send(303);
}

void handlePanic() {
  if (!authed()) { denyAuth(); return; }
  alarmStartMs = millis();
  playPattern(PAT_SIREN, true);
  applyRelay();
  redirectHome();
}

void handleTest() {
  if (!authed()) { denyAuth(); return; }
  web.sendHeader("Location", "/");
  web.send(303);
  int g = armGate();  // clicks pulse whatever the relay drives — same policy as arming
  if (g) {
    playPattern(PAT_DENY, true);
    Serial.printf("[TEST] refused — %s\n", armGateText(g));
    return;
  }
  Serial.println("[TEST] relay clicks");
  for (int i = 0; i < 3; i++) {  // audible/visible polarity check
    digitalWrite(RELAY_PIN, relayEnergizeLevel()); delay(120);
    digitalWrite(RELAY_PIN, relayIdleLevel());     delay(120);
  }
  applyRelay();
  playPattern(PAT_BOOT, true);
}

// keep stored fob entries inert: MAC entries only [0-9A-F:], name entries
// only sane visible characters (they get rendered back into HTML/JSON)
String sanitizeFob(const String &e) {
  String o;
  bool name = e.startsWith("N:");
  if (name) o += "N:";
  unsigned cap = name ? 22 : 17;
  for (unsigned i = name ? 2 : 0; i < e.length() && o.length() < cap; i++) {
    char c = e[i];
    if (name) { if (isalnum(c) || c == ' ' || c == '-' || c == '_') o += c; }
    else if (isxdigit(c) || c == ':') o += c;
  }
  o.toUpperCase();
  return o;
}

void handleFob() {
  if (!authed()) { denyAuth(); return; }
  // add by MAC (?mac=AA:BB:..), add by advertised NAME (?name=xyz — survives
  // the phone's rotating BLE MAC), or remove (?del=<entry>)
  if (web.hasArg("del")) {
    String e = sanitizeFob(web.arg("del"));
    int i = findFob(e);
    if (i >= 0) {
      for (int j = i; j < fobCount - 1; j++) fobs[j] = fobs[j + 1];
      fobCount--;
      if (seenIdx == i) seenIdx = -1;
      Serial.printf("[KEY] fob removed: %s\n", e.c_str());
    }
  } else if (web.hasArg("name")) {
    String e = sanitizeFob(String("N:") + web.arg("name"));
    if (e.length() > 2 && fobCount < MAX_FOBS && findFob(e) < 0) {
      fobs[fobCount++] = e;
      Serial.printf("[KEY] fob added by name: %s\n", e.c_str());
    }
  } else {
    String e = sanitizeFob(web.arg("mac"));
    if (e.length() == 17 && fobCount < MAX_FOBS && findFob(e) < 0) {
      fobs[fobCount++] = e;
      Serial.printf("[KEY] fob added: %s\n", e.c_str());
    }
  }
  consecHits = 0;
  saveCfg();
  redirectHome();
}

void handleScan() {
  if (web.hasArg("go")) {
    if (!authed()) { denyAuth(); return; }
    collectScan = true;
    scanCount = 0;
    web.sendHeader("Location", "/scan");
    web.send(303);
    return;
  }
  String h = String(PAGE_TOP);
  h += F("<tr><th>BLE scan</th><td>");
  if (!scanCount) h += F("no devices captured yet &mdash; ");
  else h += String(scanCount) + F(" devices &mdash; ");
  h += F("<form method=post action=/scan style='display:inline'>"
         "<input type=hidden name=go value=1><button>[rescan]</button></form>");
  h += F("</td></tr></table>");
  if (scanCount) {
    h += F("<table><tr><th>MAC</th><th>Name</th><th>RSSI</th><th>add as fob</th></tr>");
    for (int i = 0; i < scanCount; i++) {
      h += F("<tr><td><code>"); h += scanList[i].mac; h += F("</code></td><td>");
      if (scanList[i].name.length()) h += htmlEsc(scanList[i].name); else h += F("<span class=dim>&mdash;</span>");
      h += F("</td><td>"); h += String(scanList[i].rssi);
      h += F("</td><td>");
      if (scanList[i].name.length()) {
        h += F("<form method=post action=/fob style='display:inline'>"
               "<input type=hidden name=name value='");
        h += htmlEsc(scanList[i].name);
        h += F("'><button>[+ by name]</button></form> ");
      }
      h += F("<form method=post action=/fob style='display:inline'>"
             "<input type=hidden name=mac value='");
      h += scanList[i].mac;
      h += F("'><button>[+ by MAC]</button></form></td></tr>");
    }
    h += F("</table><table><tr><th>Learned fobs</th><td>");
    h += fobEntriesHtml(true);
    h += F("</td></tr>");
  }
  h += F("</table><p><a class=btn href=/>&#8592; back</a></p></body></html>");
  web.send(200, "text/html", h);
}

void handleCfg() {
  if (!authed()) { denyAuth(); return; }
  if (web.hasArg("rssi")) rssiThr = web.arg("rssi").toInt();
  if (web.hasArg("after")) armAfterS = constrain(web.arg("after").toInt(), 5, 600);
  if (web.hasArg("grace")) bootGraceS = constrain(web.arg("grace").toInt(), 10, 600);
  if (web.hasArg("hits")) presentHits = constrain(web.arg("hits").toInt(), 1, 4);
  vibeEnable = web.hasArg("vibe");
  ignSenseWired = web.hasArg("ignwired");
  masterWired = web.hasArg("masterwired");
  // Relay mode / polarity are live safety controls. The gate must evaluate
  // the PROPOSED configuration (not the current one — switching away from
  // alarm-only must not skip the ignition check), and bench mode from the
  // PREVIOUS save may not authorize this save's relay change.
  uint8_t newMode = web.hasArg("mode") ? constrain(web.arg("mode").toInt(), 0, 2) : relayMode;
  bool newActlow = web.hasArg("actlow");
  bool relayChange = newMode != relayMode || newActlow != relayActiveLow;
  if (relayChange) {
    int g = gateFor(newMode, benchMode);
    if (g) {
      playPattern(PAT_DENY, true);
      Serial.printf("[CFG] relay mode/polarity change REFUSED — %s\n", armGateText(g));
    } else {
      relayMode = newMode;
      relayActiveLow = newActlow;
    }
  }
  // Enabling bench mode while the key is HOT is refused: bench work implies
  // key off, and a hot key here likely means the bike is in use.
  bool benchReq = web.hasArg("bench");
  if (benchReq && !benchMode && ignNow == IGN_HOT) {
    benchReq = false;
    playPattern(PAT_DENY, true);
    Serial.println("[CFG] enabling bench mode REFUSED — ignition/key line is hot");
  }
  benchMode = benchReq;
  String np = web.arg("newpin");
  np.trim();
  if (np.length() >= 4 && np.length() <= 12) webPin = np;
  saveCfg();
  applyRelay();
  Serial.printf("[CFG] rssi=%d after=%lus grace=%lus hits=%u mode=%u actlow=%d vibe=%d ignwired=%d masterwired=%d bench=%d\n",
                rssiThr, (unsigned long)armAfterS, (unsigned long)bootGraceS,
                presentHits, relayMode, relayActiveLow, vibeEnable, ignSenseWired, masterWired, benchMode);
  redirectHome();
}

void handleReboot() {
  if (!authed()) { denyAuth(); return; }
  web.send(200, "text/html", "Rebooting...");
  delay(200);
  ESP.restart();
}

// ---------------- setup / loop ----------------
void setup() {
  bootMs = millis();
  absentSinceMs = bootMs;

  Serial.begin(115200);
  loadCfg();  // BEFORE driving the relay pin: the saved polarity defines idle

  // relay SAFE first: coil idle (contact closed in NC mode) as early as possible
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, relayIdleLevel());
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(VIBE_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(IGN_PIN, INPUT_PULLDOWN);   // unwired sense reads cold, never blocks
  pinMode(MASTER_PIN, INPUT_PULLUP);  // unwired saklar reads ON (keyless active)
  pinMode(LED_PIN, OUTPUT);

  BLEDevice::init("");
  pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCb());
  pScan->setActiveScan(true);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] connecting to %s ...\n", WIFI_SSID);

  ArduinoOTA.setHostname("votol-keyless");
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);   // define in wifi_secrets.h
  ArduinoOTA.begin();
#else
  // secure by default: no password configured = no OTA (flash via USB)
  Serial.println("[OTA] DISABLED — set OTA_PASSWORD in wifi_secrets.h to enable");
#endif
  MDNS.begin("votol-keyless");
  MDNS.addService("http", "tcp", 80);

  web.on("/", handleRoot);
  web.on("/state.json", handleState);
  const char *hdrs[] = {"Cookie"};
  web.collectHeaders(hdrs, 1);
  authTok = esp_random();  // per-boot: rebooting the module logs browsers out
  // machine API (any method, needs ?pin=) + browser session (POST + cookie)
  auto armH = []() { if (!authed()) { denyAuth(); return; } tryArm(true, true); redirectHome(); };
  auto disarmH = []() { if (!authed()) { denyAuth(); return; } setArmed(false, true); redirectHome(); };
  web.on("/arm", HTTP_GET, armH);
  web.on("/arm", HTTP_POST, armH);
  web.on("/disarm", HTTP_GET, disarmH);
  web.on("/disarm", HTTP_POST, disarmH);
  web.on("/panic", HTTP_GET, handlePanic);
  web.on("/panic", HTTP_POST, handlePanic);
  web.on("/test", HTTP_GET, handleTest);
  web.on("/test", HTTP_POST, handleTest);
  web.on("/fob", HTTP_GET, handleFob);
  web.on("/fob", HTTP_POST, handleFob);
  web.on("/scan", HTTP_GET, handleScan);
  web.on("/scan", HTTP_POST, handleScan);
  web.on("/cfg", HTTP_GET, handleCfg);
  web.on("/cfg", HTTP_POST, handleCfg);
  web.on("/auth", HTTP_GET, handleAuth);
  web.on("/auth", HTTP_POST, handleAuth);
  web.on("/reboot", HTTP_GET, handleReboot);
  web.on("/reboot", HTTP_POST, handleReboot);
  // aliases so the dashboard uses one path scheme for standalone and combined
  web.on("/k/arm", HTTP_GET, armH);
  web.on("/k/arm", HTTP_POST, armH);
  web.on("/k/disarm", HTTP_GET, disarmH);
  web.on("/k/disarm", HTTP_POST, disarmH);
  web.on("/k/panic", HTTP_GET, handlePanic);
  web.on("/k/panic", HTTP_POST, handlePanic);
  web.begin();
  Serial.println("[WEB] VOTOL Keyless ready (disarmed, boot grace active)");
  playPattern(PAT_BOOT, true);  // ready chirp
}

void loop() {
  static uint32_t lastScanMs = 0;
  uint32_t now = millis();

  // BLE cycle: ~1 s blocking scan + ~1 s housekeeping gap (ends early when fob found)
  if (now - lastScanMs > 2000) {
    lastScanMs = now;
    scanCycle();
  }

  keylessTick();
  buttonTick();
  patternTick();

  static bool wasConnected = false;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected) {
    wasConnected = connected;
    Serial.println(connected
        ? String("[WiFi] connected, IP: ") + WiFi.localIP().toString()
        : "[WiFi] disconnected");
  }
  if (!connected && !portalStarted && now > PORTAL_AFTER_MS) {
    portalStarted = true;
    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal("VOTOL-KEYLESS-Setup");
    Serial.println("[WiFi] setup hotspot opened: VOTOL-KEYLESS-Setup");
  }
  wm.process();
  ArduinoOTA.handle();
  web.handleClient();

  // LED: very fast = siren, fast = armed, solid = fob near, slow blink = disarmed/scanning
  if (sirenActive())      digitalWrite(LED_PIN, now % 120 < 60);
  else if (armed)         digitalWrite(LED_PIN, now % 300 < 150);
  else if (fobPresent)    digitalWrite(LED_PIN, HIGH);
  else                    digitalWrite(LED_PIN, now % 2000 < 100);
}
