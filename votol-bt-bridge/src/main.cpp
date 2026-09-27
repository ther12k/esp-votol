#include <Arduino.h>
#include <BluetoothSerial.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#ifdef WITH_KEYLESS
#include <BLEDevice.h>
#include <BLEScan.h>
#include <Preferences.h>
#endif

const char *BT_NAME = "VOTOL-BT";
const char *BT_PIN  = "1234";

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
#define PORTAL_AFTER_MS 180000UL

int rxPin = 18;
int txPin = 19;
uint32_t currentBaud = 115200;

BluetoothSerial SerialBT;
WiFiManager wm;
WebServer web(80);
WiFiServer tcpServer(6638);
WiFiClient tcpClient;
bool portalStarted = false;

volatile uint32_t txBytes = 0;
volatile uint32_t rxBytes = 0;
volatile uint32_t lastRxMs = 0;
uint8_t rxBuf[64];
uint8_t rxLen = 0;

#ifdef WITH_KEYLESS
// ================= keyless & alarm (same feature set as keyless-alarm/) =========
// Fob presence -> auto arm/disarm; fail-safe NC relay on the key/E-LOCK+ line;
// boot grace always disarmed; optional SW-420 vibration -> 30 s siren.
// BLE scan uses the NON-blocking BLEScan::start(duration, cb) so the bridge
// passthrough (TCP 6638 / SPP) never stalls.
#define K_RELAY_PIN 26
#define K_BUZZER_PIN 25
#define K_VIBE_PIN 34     // input-only; SW-420 module drives it
#define K_BUTTON_PIN 27   // optional button to GND; hold 2 s toggles
// Ignition/key sense: divider (1M top / 27k bottom) from the CONTROLLER side
// of the E-LOCK+ cut. Key ON = hot = arming REFUSED, so a lost fob signal can
// never cut power while the bike is in use. Pulldown keeps unwired = cold.
#define K_IGN_PIN 33
// Master switch (the old build's "saklar"): latching switch to GND on GPIO32.
// OFF (open, pin HIGH) suspends keyless — relay idle (NC closed = rideable,
// NO open = kontak off), arming refused, siren off. GPIO32 because 34-39 are
// input-only without internal pulls.
#define K_MASTER_PIN 32
#define K_SIREN_S 30UL
#define K_VIBE_SETTLE_MS 8000UL
#define K_VIBE_DEBOUNCE_MS 500UL

enum RelayMode : uint8_t {
  RM_IMMOBILIZER_NC = 0,  // fail-safe: energize (contact OPENS) only when armed
  RM_IGNITION_NO = 1,     // old-sketch style: energize (contact closes) while fob present
  RM_ALARM_ONLY = 2       // nothing cuts the bike; relay follows the siren
};

#define K_MAX_FOBS 4
// fob entries: "AA:BB:CC:DD:EE:FF" (MAC — iTags) or "N:name" (advertised NAME
// — phone beacon apps, whose BLE MAC rotates)
String kFobs[K_MAX_FOBS];
int kFobCount = 0;
int kSeenIdx = -1;
int rssiThr = -80;
uint32_t armAfterS = 30;
uint32_t bootGraceS = 60;
uint8_t presentHits = 2;
uint8_t relayMode = RM_IMMOBILIZER_NC;
bool relayActiveLow = false;
bool vibeEnable = false;   // OFF until an SW-420 is wired (GPIO34 floats)
String kWebPin = "1234";   // access PIN for state-changing /k/* endpoints
bool kIgnSenseWired = false; // divider connected; REQUIRED before E-LOCK cut
bool kBenchMode = false;     // relay contacts DISCONNECTED — relax interlocks (bench only)
bool kMasterWired = false;  // saklar switch connected to GPIO32
bool kMasterOff = false;    // saklar OFF: keyless suspended (bike stays usable)

// Tri-state ignition: UNKNOWN (sense not wired/validated) is never treated as
// "key off" — a cut command requires COLD, or benchMode.
enum KIgnState : uint8_t { KIGN_UNKNOWN = 0, KIGN_COLD = 1, KIGN_HOT = 2 };
KIgnState kIgnNow = KIGN_UNKNOWN;
uint32_t kAuthTok = 0;      // per-boot browser-session token

bool kArmed = false;
bool kFobSeen = false;
bool kFobPresent = false;
bool kScanning = false;
uint8_t kHits = 0;
int kRssi = 0;
uint32_t kLastSeenMs = 0;
uint32_t kAbsentSinceMs = 0;
uint32_t kBootMs = 0;
uint32_t kArmedSinceMs = 0;
uint32_t kAlarmStartMs = 0;   // siren window START (elapsed math survives millis() wrap)
uint16_t kVibeEvents = 0;
uint32_t kLastVibeMs = 0;
uint32_t kLastScanMs = 0;
bool kCollectScan = false;
struct KScanEntry { String mac, name; int rssi; };
KScanEntry kScanList[15];
int kScanCount = 0;

Preferences kPrefs;
BLEScan *kScan = nullptr;

// ---- buzzer patterns: [on_ms, off_ms, ...], 0 = end, repeat loops ----
struct KPattern { const uint16_t *steps; uint8_t n; bool repeat; };
static const uint16_t KST_ARM[]    = {600, 0};
static const uint16_t KST_DISARM[] = {150, 150, 150, 0};
static const uint16_t KST_BOOT[]   = {80, 80, 80, 0};
static const uint16_t KST_DENY[]   = {100, 100, 100, 0};
static const uint16_t KST_SIREN[]  = {400, 250};
static const KPattern KPAT_ARM    {KST_ARM,    2, false};
static const KPattern KPAT_DISARM {KST_DISARM, 4, false};
static const KPattern KPAT_BOOT   {KST_BOOT,   4, false};
static const KPattern KPAT_DENY   {KST_DENY,   4, false};
static const KPattern KPAT_SIREN  {KST_SIREN,  2, true};
KPattern kPat{nullptr, 0, false};
bool kPatOn = false;
uint8_t kPatIdx = 0;
uint32_t kPatMs = 0;

bool kSirenActive() { return kAlarmStartMs && millis() - kAlarmStartMs < K_SIREN_S * 1000UL; }

void kPlayPattern(const KPattern &p, bool force = false) {
  if (!force && kSirenActive() && kPat.steps == KST_SIREN) return;
  kPat = p;
  kPatIdx = 0;
  kPatOn = true;
  kPatMs = millis();
  digitalWrite(K_BUZZER_PIN, HIGH);
}
void kStopPattern() { kPat.steps = nullptr; digitalWrite(K_BUZZER_PIN, LOW); }
void kPatternTick() {
  if (!kPat.steps) return;
  if (kPat.steps == KST_SIREN && !kSirenActive()) { kAlarmStartMs = 0; kStopPattern(); return; }
  uint32_t now = millis();
  if (now - kPatMs < (uint32_t)kPat.steps[kPatIdx]) return;  // current slot not over
  kPatMs = now;
  kPatOn = !kPatOn;
  digitalWrite(K_BUZZER_PIN, kPatOn ? HIGH : LOW);
  uint8_t next = kPatIdx + 1;                       // advance EVERY slot: [on,off,on,off,...]
  if (next < kPat.n && kPat.steps[next] != 0) kPatIdx = next;
  else if (kPat.repeat) { kPatIdx = 0; kPatOn = true; digitalWrite(K_BUZZER_PIN, HIGH); }
  else kStopPattern();
}

int kRelayEnergizeLevel() { return relayActiveLow ? LOW : HIGH; }
int kRelayIdleLevel() { return relayActiveLow ? HIGH : LOW; }
bool kRelayShouldEnergize() {
  if (kMasterOff) return false;  // saklar OFF: relay always idle (old-sketch "kontak off")
  switch (relayMode) {
    case RM_IMMOBILIZER_NC: return kArmed;
    // old-sketch fob-follow contact, LATCHED while the key is ON: a dropped
    // fob scan can never open it mid-ride — releases when the key turns off,
    // the fob is absent with the key off, or the master switch opens.
    case RM_IGNITION_NO:    return !kArmed && (kFobPresent || kIgnNow == KIGN_HOT);
    case RM_ALARM_ONLY:     return kSirenActive();
  }
  return false;
}
bool kRelayAppliedE = false;  // last GPIO level actually written
void kApplyRelay() {
  kRelayAppliedE = kRelayShouldEnergize();
  digitalWrite(K_RELAY_PIN, kRelayAppliedE ? kRelayEnergizeLevel() : kRelayIdleLevel());
}

bool kArmingCutsPower();   // defined near kTryArm
void kRedirectHome();      // defined with the /k/* handlers

// ---- central safety gate: EVERY path that would newly energize the coil in a
// power-cutting mode passes through kArmGate() — auto-arm, manual arm, relay
// test, settings changes. 0 = allowed.
//   1 master OFF · 2 ignition HOT · 3 ignition sense UNKNOWN
int kArmGate() {
  if (kMasterOff) return 1;
  if (kBenchMode) return 0;
  if (!kArmingCutsPower()) return 0;
  if (kIgnNow == KIGN_HOT) return 2;
  if (kIgnNow == KIGN_UNKNOWN) return 3;
  return 0;
}
const char *kArmGateText(int g) {
  switch (g) {
    case 1: return "master switch (saklar) is OFF";
    case 2: return "ignition/key line is hot";
    case 3: return "ignition sense not validated (wire the divider, or tick bench mode)";
    default: return "";
  }
}

int kFindFob(const String &entry);
int kMatchFob(const String &mac, const String &name);
String kSanitizeFob(const String &e);  // defined with the /k/* handlers
void kSetArmed(bool a, bool manual);

// ---- escaping (BLE names are attacker-controlled input) ----
String kHtmlEsc(const String &s) {
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
String kUrlEnc(const String &s) {
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

void kLoadCfg() {
  kPrefs.begin("vtkl", true);
  String all = kPrefs.getString("fob", "");  // comma-joined, backward compatible
  rssiThr        = kPrefs.getInt("rssi", -80);
  armAfterS      = kPrefs.getUInt("armAfter", 30);
  bootGraceS     = kPrefs.getUInt("grace", 60);
  presentHits    = kPrefs.getUChar("hits", 2);
  relayMode      = kPrefs.getUChar("mode", RM_IMMOBILIZER_NC);
  relayActiveLow = kPrefs.getBool("actlow", false);
  vibeEnable     = kPrefs.getBool("vibe", false);
  kIgnSenseWired = kPrefs.getBool("ignwired", false);
  kMasterWired   = kPrefs.getBool("masterwired", false);
  kBenchMode     = kPrefs.getBool("bench", false);
  kWebPin        = kPrefs.getString("pin", "1234");
  if (!kWebPin.length()) kWebPin = "1234";
  kPrefs.end();
  kFobCount = 0;
  int s = 0;
  while (s >= 0 && kFobCount < K_MAX_FOBS) {
    int c = all.indexOf(',', s);
    String e = (c < 0) ? all.substring(s) : all.substring(s, c);
    e.trim();
    e = kSanitizeFob(e);  // legacy NVS entries may predate the character filter
    if (e.length() && kFindFob(e) < 0) kFobs[kFobCount++] = e;
    s = (c < 0) ? -1 : c + 1;
  }
}
void kSaveCfg() {
  kPrefs.begin("vtkl", false);
  String all;
  for (int i = 0; i < kFobCount; i++) {
    if (i) all += ",";
    all += kFobs[i];
  }
  kPrefs.putString("fob", all);
  kPrefs.putInt("rssi", rssiThr);
  kPrefs.putUInt("armAfter", armAfterS);
  kPrefs.putUInt("grace", bootGraceS);
  kPrefs.putUChar("hits", presentHits);
  kPrefs.putUChar("mode", relayMode);
  kPrefs.putBool("actlow", relayActiveLow);
  kPrefs.putBool("vibe", vibeEnable);
  kPrefs.putBool("ignwired", kIgnSenseWired);
  kPrefs.putBool("masterwired", kMasterWired);
  kPrefs.putBool("bench", kBenchMode);
  kPrefs.putString("pin", kWebPin);
  kPrefs.end();
}

int kMatchFob(const String &mac, const String &name) {
  for (int i = 0; i < kFobCount; i++) {
    if (kFobs[i].length() == 17 && kFobs[i] == mac) return i;
    if (kFobs[i].startsWith("N:") && kFobs[i].length() > 2 && name.length() &&
        name.equalsIgnoreCase(kFobs[i].substring(2))) return i;
  }
  return -1;
}
int kFindFob(const String &entry) {
  for (int i = 0; i < kFobCount; i++) if (kFobs[i] == entry) return i;
  return -1;
}

void kScanDone(BLEScanResults) {
  kScanning = false;
  if (kCollectScan) kCollectScan = false;  // one-shot list built
  if (kFobSeen && kRssi > rssiThr) {
    kLastSeenMs = millis();
    if (kHits < 255) kHits++;
  } else if (kLastSeenMs == 0 || millis() - kLastSeenMs > 8000) {
    kHits = 0;
  }
}
class KScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    String mac = d.getAddress().toString().c_str();
    mac.toUpperCase();
    if (kCollectScan && kScanCount < 15) {
      bool dup = false;
      for (int i = 0; i < kScanCount; i++) if (kScanList[i].mac == mac) dup = true;
      if (!dup) {
        kScanList[kScanCount].mac = mac;
        kScanList[kScanCount].name = d.haveName() ? String(d.getName().c_str()) : String("");
        kScanList[kScanCount].rssi = d.getRSSI();
        kScanCount++;
      }
    }
    String nm = d.haveName() ? String(d.getName().c_str()) : String("");
    int m = kMatchFob(mac, nm);
    if (m >= 0) { kFobSeen = true; kRssi = d.getRSSI(); kSeenIdx = m; }
  }
};

// ignition/key sense: divider 1M/27k → 35–100 V packs read 0.6–1.7 V on
// GPIO33; threshold ≈0.25 V (ADC ~310/4095), 4-sample average
bool kIgnHot() {
  if (!kIgnSenseWired) return false;
  int s = 0;
  for (int i = 0; i < 4; i++) s += analogRead(K_IGN_PIN);
  return (s >> 2) > 310;
}
bool kArmingCutsPower() { return relayMode == RM_IMMOBILIZER_NC || relayMode == RM_IGNITION_NO; }

// ARMING IS REFUSED while the ignition is hot: opening the cut with the bike
// in use would kill propulsion. state.json exposes ign+ignwired for the UI.
bool kTryArm(bool a, bool manual) {
  if (a) {
    int g = kArmGate();
    if (g) {
      kPlayPattern(KPAT_DENY, true);
      Serial.printf("[KEY] arm REFUSED — %s\n", kArmGateText(g));
      return false;
    }
  }
  kSetArmed(a, manual);
  return true;
}

void kSetArmed(bool a, bool manual) {
  if (a == kArmed) { if (manual) kAbsentSinceMs = millis(); return; }
  kArmed = a;
  kArmedSinceMs = millis();
  kAbsentSinceMs = millis();
  kPlayPattern(a ? KPAT_ARM : KPAT_DISARM, true);
  kApplyRelay();
  Serial.printf("[KEY] %s (%s)\n", a ? "ARMED" : "DISARMED", manual ? "manual" : "auto");
}

void kTick() {
  uint32_t now = millis();
  bool wasMasterOff = kMasterOff;
  kMasterOff = kMasterWired && digitalRead(K_MASTER_PIN) == HIGH;
  if (kMasterOff != wasMasterOff) {
    Serial.printf("[KEY] master switch %s — keyless %s\n",
                  kMasterOff ? "OFF" : "ON", kMasterOff ? "SUSPENDED" : "active");
    if (kMasterOff) kPlayPattern(KPAT_DISARM, true);
    kApplyRelay();
  }
  if (kMasterOff) {
    // saklar OFF suspends everything: disarm, never arm, no siren
    if (kArmed) kSetArmed(false, false);
    if (kSirenActive()) { kAlarmStartMs = 0; kStopPattern(); }
    kAbsentSinceMs = now;
    kIgnNow = KIGN_UNKNOWN;
    kPatternTick();   // keep the suspend chirp playing (was skipped before: review #8)
    return;
  }

  kIgnNow = kIgnSenseWired ? (kIgnHot() ? KIGN_HOT : KIGN_COLD) : KIGN_UNKNOWN;
  if (kIgnNow == KIGN_HOT) kAbsentSinceMs = now;  // key on: arm clock never runs while riding
  if (kFobPresent) kAbsentSinceMs = now;          // hold the clock while the fob is here:
                                                  // leaving must cost the FULL armAfter

  // scan scheduling: async 1 s scan every 2 s — never blocks the bridge
  if (kScan && !kScanning && now - kLastScanMs > 2000) {
    kLastScanMs = now;
    kFobSeen = false;
    kScanning = true;
    kScan->start(1, kScanDone, false);
  }

  // phone app connected = owner at the bike: keep it disarmed-friendly
  if (SerialBT.hasClient() && !kArmed) kLastSeenMs = now;

  bool wasPresent = kFobPresent;
  kFobPresent = kHits >= presentHits && kLastSeenMs && (now - kLastSeenMs < 8000);
  if (kFobPresent && kArmed) kSetArmed(false, false);
  else if (!kFobPresent && !kArmed) {
    bool graceOver = now - kBootMs > bootGraceS * 1000UL;
    bool goneEnough = now - kAbsentSinceMs > armAfterS * 1000UL;
    if (graceOver && goneEnough && kArmGate() == 0) kSetArmed(true, false);
  }
  if (kFobPresent != wasPresent)
    Serial.printf("[KEY] fob %s (%d dBm)\n", kFobPresent ? "present" : "gone", kRssi);

  if (kArmed && vibeEnable && now - kArmedSinceMs > K_VIBE_SETTLE_MS && now - kLastVibeMs > K_VIBE_DEBOUNCE_MS) {
    if (digitalRead(K_VIBE_PIN) == HIGH) {
      kLastVibeMs = now;
      kVibeEvents++;
      kAlarmStartMs = now;
      kPlayPattern(KPAT_SIREN, true);
      Serial.printf("[ALARM] vibration #%u — siren %lus\n", kVibeEvents, (unsigned long)K_SIREN_S);
    }
  }
  // keep the GPIO glued to the computed state: in ignition-NO mode the relay
  // depends on continuously-varying inputs, not just arming transitions
  if (kRelayShouldEnergize() != kRelayAppliedE) kApplyRelay();

  // optional physical button: hold 2 s toggles
  static uint32_t kPressMs = 0;
  static bool kHeld = false;
  bool down = digitalRead(K_BUTTON_PIN) == LOW;
  if (down && !kPressMs) { kPressMs = now; kHeld = false; }
  else if (down && kPressMs && !kHeld && now - kPressMs > 2000) { kHeld = true; kTryArm(!kArmed, true); }
  else if (!down && kPressMs) kPressMs = 0;

  kPatternTick();
}

// ---- keyless web endpoints (all under /k/*; /state.json for the dashboard) ----
// state-changing endpoints require ?pin=<kWebPin> (default 1234). /state.json
// and the status page stay open for monitoring.
// machine API: ?pin= on any method (dashboard proxy). Browser session: cookie
// from POST /k/auth (SameSite=Strict, HttpOnly, 1 h) — cookies only count on
// POST. The PIN is never rendered into any page.
bool kAuthByPin() { return web.hasArg("pin") && web.arg("pin") == kWebPin; }
bool kAuthByCookie() {
  if (!kAuthTok || web.method() != HTTP_POST) return false;
  String c = web.header("Cookie");
  return c.length() && c.indexOf("kauth=" + String(kAuthTok)) >= 0;
}
bool kPinOk() { return kAuthByPin() || kAuthByCookie(); }
void kDenyPin() { web.send(403, "text/plain", "not authorized — POST /k/auth with pin=, or pass ?pin="); }
void handleKAuth() {
  if (!kAuthByPin()) { kDenyPin(); return; }
  web.sendHeader("Set-Cookie",
                 String("kauth=") + kAuthTok + "; Path=/; Max-Age=3600; SameSite=Strict; HttpOnly");
  kRedirectHome();
}

void kRedirectHome() { web.sendHeader("Location", "/"); web.send(303); }

void handleKState() {
  uint32_t now = millis();
  String s = String(F("{\"armed\":"));
  s += kArmed ? "true" : "false";
  s += F(",\"alarm\":");
  s += kSirenActive() ? "true" : "false";
  s += F(",\"fob\":{\"mac\":\"");
  s += (kFobPresent && kSeenIdx >= 0) ? kFobs[kSeenIdx] : String("");
  s += F("\",\"count\":");
  s += String(kFobCount);
  s += F(",\"present\":");
  s += kFobPresent ? "true" : "false";
  s += F(",\"rssi\":");
  s += String(kRssi);
  s += F(",\"ageS\":");
  s += kLastSeenMs ? String((now - kLastSeenMs) / 1000) : String("-1");
  s += F("},\"relay\":{\"mode\":");
  s += String(relayMode);
  s += F(",\"energized\":");
  s += kRelayShouldEnergize() ? "true" : "false";
  s += F("},\"vibe\":{\"enabled\":");
  s += vibeEnable ? "true" : "false";
  s += F(",\"events\":");
  s += String(kVibeEvents);
  s += F("},\"graceLeftS\":");
  s += (now - kBootMs < bootGraceS * 1000UL) ? String((bootGraceS * 1000UL - (now - kBootMs)) / 1000 + 1) : String("0");
  s += F(",\"ign\":");
  s += kIgnNow == KIGN_HOT ? "true" : "false";
  s += F(",\"ignstate\":\"");
  s += kIgnNow == KIGN_HOT ? "hot" : (kIgnNow == KIGN_COLD ? "cold" : "unknown");
  s += F("\",\"ignwired\":");
  s += kIgnSenseWired ? "true" : "false";
  s += F(",\"bench\":");
  s += kBenchMode ? "true" : "false";
  s += F(",\"relaypin\":");
  s += digitalRead(K_RELAY_PIN) == kRelayEnergizeLevel() ? "true" : "false";
  s += F(",\"master\":");
  s += kMasterOff ? "true" : "false";
  s += F(",\"uptimeS\":");
  s += String(now / 1000);
  s += F(",\"heap\":");
  s += String(ESP.getFreeHeap());
  s += F(",\"ip\":\"");
  s += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
  s += F("\"}");
  web.send(200, "application/json", s);
}

void handleKPanic() {
  if (!kPinOk()) { kDenyPin(); return; }
  kAlarmStartMs = millis();
  kPlayPattern(KPAT_SIREN, true);
  kApplyRelay();
  kRedirectHome();
}

void handleKTest() {
  if (!kPinOk()) { kDenyPin(); return; }
  web.sendHeader("Location", "/");
  web.send(303);
  int kg = kArmGate();  // clicks pulse whatever the relay drives — same policy as arming
  if (kg) {
    kPlayPattern(KPAT_DENY, true);
    Serial.printf("[TEST] refused — %s\n", kArmGateText(kg));
    return;
  }
  for (int i = 0; i < 3; i++) {
    digitalWrite(K_RELAY_PIN, kRelayEnergizeLevel()); delay(120);
    digitalWrite(K_RELAY_PIN, kRelayIdleLevel());     delay(120);
  }
  kApplyRelay();
  kPlayPattern(KPAT_BOOT, true);
}

// keep stored fob entries inert: MAC entries only [0-9A-F:], name entries
// only sane visible characters (they get rendered back into HTML/JSON)
String kSanitizeFob(const String &e) {
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

void handleKFob() {
  if (!kPinOk()) { kDenyPin(); return; }
  // add by MAC (?mac=), by advertised NAME (?name= — survives the phone's
  // rotating BLE MAC), or remove (?del=<entry>)
  if (web.hasArg("del")) {
    String e = kSanitizeFob(web.arg("del"));
    int i = kFindFob(e);
    if (i >= 0) {
      for (int j = i; j < kFobCount - 1; j++) kFobs[j] = kFobs[j + 1];
      kFobCount--;
      if (kSeenIdx == i) kSeenIdx = -1;
      Serial.printf("[KEY] fob removed: %s\n", e.c_str());
    }
  } else if (web.hasArg("name")) {
    String e = kSanitizeFob(String("N:") + web.arg("name"));
    if (e.length() > 2 && kFobCount < K_MAX_FOBS && kFindFob(e) < 0) {
      kFobs[kFobCount++] = e;
      Serial.printf("[KEY] fob added by name: %s\n", e.c_str());
    }
  } else {
    String e = kSanitizeFob(web.arg("mac"));
    if (e.length() == 17 && kFobCount < K_MAX_FOBS && kFindFob(e) < 0) {
      kFobs[kFobCount++] = e;
      Serial.printf("[KEY] fob added: %s\n", e.c_str());
    }
  }
  kHits = 0;
  kSaveCfg();
  kRedirectHome();
}

void handleKScan() {
  if (web.hasArg("go")) {
    if (!kPinOk()) { kDenyPin(); return; }
    kCollectScan = true;
    kScanCount = 0;
    web.sendHeader("Location", "/k/scan");
    web.send(303);
    return;
  }
  String h = String(F("<!DOCTYPE html><html><head><meta charset=utf-8>"
                      "<meta http-equiv=refresh content=4><title>BLE scan</title>"
                      "<meta name=viewport content='width=device-width,initial-scale=1'>"
                      "<style>body{font-family:sans-serif;margin:16px;background:#f5f5f5}"
                      "table{border-collapse:collapse;background:#fff}"
                      "td,th{border:1px solid #ddd;padding:6px 12px}"
                      "th{background:#2c3e50;color:#fff}a{color:#2563eb}</style></head><body>"
                      "<h2>BLE scan — pick the fob</h2><table><tr><th>MAC</th><th>Name</th>"
                      "<th>RSSI</th><th>add as fob</th></tr>"));
  h += F("<tr><td colspan=4>");
  h += kScanCount ? String(kScanCount) + F(" devices &mdash; ") : String(F("no devices captured yet &mdash; "));
  h += F("<form method=post action=/k/scan style='display:inline'>"
         "<input type=hidden name=go value=1><button>[scan 10 s]</button></form></td></tr>");
  for (int i = 0; i < kScanCount; i++) {
    h += F("<tr><td><code>"); h += kScanList[i].mac; h += F("</code></td><td>");
    if (kScanList[i].name.length()) h += kHtmlEsc(kScanList[i].name); else h += F("&mdash;");
    h += F("</td><td>"); h += String(kScanList[i].rssi);
    h += F("</td><td>");
    if (kScanList[i].name.length()) {
      h += F("<form method=post action=/k/fob style='display:inline'>"
             "<input type=hidden name=name value='");
      h += kHtmlEsc(kScanList[i].name);
      h += F("'><button>[+ by name]</button></form> ");
    }
    h += F("<form method=post action=/k/fob style='display:inline'>"
           "<input type=hidden name=mac value='");
    h += kScanList[i].mac;
    h += F("'><button>[+ by MAC]</button></form></td></tr>");
  }
  h += F("</table><p><b>Learned fobs</b> (");
  h += String(kFobCount);
  h += F("/");
  h += String(K_MAX_FOBS);
  h += F("): ");
  for (int i = 0; i < kFobCount; i++) {
    if (i) h += F(" &middot; ");
    h += F("<code>"); h += kHtmlEsc(kFobs[i]); h += F("</code>");
    if (kFobPresent && i == kSeenIdx) h += F(" <span style=color:#0a7d15>&#10003;</span>");
    h += F(" <form method=post action=/k/fob style='display:inline'>"
           "<input type=hidden name=del value='");
    h += kHtmlEsc(kFobs[i]);
    h += F("'><button>&#10005;</button></form>");
  }
  if (!kFobCount) h += F("<i>none yet</i>");
  h += F("</p><p><a href=/>&#8592; back to bridge status</a></p></body></html>");
  web.send(200, "text/html", h);
}

void handleKCfg() {
  if (!kPinOk()) { kDenyPin(); return; }
  if (web.hasArg("rssi")) rssiThr = web.arg("rssi").toInt();
  if (web.hasArg("after")) armAfterS = constrain(web.arg("after").toInt(), 5, 600);
  if (web.hasArg("grace")) bootGraceS = constrain(web.arg("grace").toInt(), 10, 600);
  if (web.hasArg("hits")) presentHits = constrain(web.arg("hits").toInt(), 1, 4);
  vibeEnable = web.hasArg("vibe");
  kIgnSenseWired = web.hasArg("ignwired");
  kMasterWired = web.hasArg("masterwired");
  kBenchMode = web.hasArg("bench");
  // relay mode / polarity are live safety controls — same central gate as arming
  uint8_t newMode = web.hasArg("mode") ? constrain(web.arg("mode").toInt(), 0, 2) : relayMode;
  bool newActlow = web.hasArg("actlow");
  if ((newMode != relayMode || newActlow != relayActiveLow) && kArmGate() != 0) {
    int g = kArmGate();
    kPlayPattern(KPAT_DENY, true);
    Serial.printf("[KCFG] relay mode/polarity change REFUSED — %s\n", kArmGateText(g));
  } else {
    relayMode = newMode;
    relayActiveLow = newActlow;
  }
  String np = web.arg("newpin");
  np.trim();
  if (np.length() >= 4 && np.length() <= 12) kWebPin = np;
  kSaveCfg();
  kApplyRelay();
  Serial.printf("[KCFG] rssi=%d after=%lu grace=%lu hits=%u mode=%u actlow=%d vibe=%d ignwired=%d masterwired=%d bench=%d\n",
                rssiThr, (unsigned long)armAfterS, (unsigned long)bootGraceS,
                presentHits, relayMode, relayActiveLow, vibeEnable, kIgnSenseWired, kMasterWired, kBenchMode);
  kRedirectHome();
}
#endif  // WITH_KEYLESS


void restartUART() {
  Serial2.end();
  delay(50);
  Serial2.begin(currentBaud, SERIAL_8N1, rxPin, txPin);
  Serial.printf("[UART] Restarted: Baud %u, RX=%d, TX=%d\n", currentBaud, rxPin, txPin);
}

void handleBaud() {
  if (web.hasArg("rate")) {
    uint32_t b = web.arg("rate").toInt();
    if (b == 9600 || b == 115200 || b == 19200 || b == 38400 || b == 57600) {
      currentBaud = b;
      restartUART();
    }
  }
  web.sendHeader("Location", "/");
  web.send(303);
}

void handlePins() {
#ifdef WITH_KEYLESS
  // never let UART config steal a keyless/safety pin (relay, buzzer, vibe,
  // button, ignition sense, master switch)
  auto pinBlocked = [](int p) {
    return p == K_RELAY_PIN || p == K_BUZZER_PIN || p == K_VIBE_PIN ||
           p == K_BUTTON_PIN || p == K_IGN_PIN || p == K_MASTER_PIN;
  };
  if (web.hasArg("rx") || web.hasArg("tx")) {
    int r = web.arg("rx").toInt(), t = web.arg("tx").toInt();
    if (pinBlocked(r) || pinBlocked(t)) {
      Serial.printf("[UART] setpins REFUSED: rx=%d tx=%d collide with keyless pins\n", r, t);
      web.sendHeader("Location", "/");
      web.send(303);
      return;
    }
  } else if (web.hasArg("swap")) {
    if (pinBlocked(rxPin) || pinBlocked(txPin)) {
      Serial.println("[UART] swap REFUSED: current pins collide with keyless pins");
      web.sendHeader("Location", "/");
      web.send(303);
      return;
    }
  }
#endif
  if (web.hasArg("swap")) {
    int tmp = rxPin;
    rxPin = txPin;
    txPin = tmp;
    restartUART();
  } else if (web.hasArg("rx") && web.hasArg("tx")) {
    rxPin = web.arg("rx").toInt();
    txPin = web.arg("tx").toInt();
    restartUART();
  }
  web.sendHeader("Location", "/");
  web.send(303);
}

void handleRoot() {
  char hex[3 * sizeof(rxBuf) + 1];
  hex[0] = 0;
  for (uint8_t i = 0; i < rxLen; i++) sprintf(hex + 3 * i, "%02X ", rxBuf[i]);

  uint32_t up = millis() / 1000;
  char upStr[20];
  sprintf(upStr, "%02u:%02u:%02u", (unsigned)(up / 3600), (unsigned)((up / 60) % 60), (unsigned)(up % 60));

  uint32_t sinceRx = lastRxMs ? (millis() - lastRxMs) / 1000 : 0;

  String h = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
               "<meta http-equiv=refresh content=2><title>VOTOL-BT Bridge</title>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<style>body{font-family:sans-serif;margin:16px;background:#f5f5f5}"
               "table{border-collapse:collapse;background:#fff;box-shadow:0 1px 3px #aaa}"
               "td,th{border:1px solid #ddd;padding:8px 14px;text-align:left}"
               "th{background:#2c3e50;color:#fff}code{background:#eee;padding:2px 6px}"
               ".ok{color:#0a7d15;font-weight:bold}.dim{color:#888}"
               "a{text-decoration:none;color:#2563eb;font-weight:600}"
               "button{padding:6px 12px;margin:2px;cursor:pointer}</style></head><body>"
               "<h2>VOTOL-BT Bridge</h2><table>"
               "<tr><th>Bluetooth</th><td>");
  h += String(BT_NAME) + F(" (PIN ") + BT_PIN + F(") &mdash; phone app: ");
  h += SerialBT.hasClient() ? F("<span class=ok>CONNECTED</span>") : F("<span class=dim>not connected</span>");
  h += F("</td></tr><tr><th>WiFi</th><td>");
  if (WiFi.status() == WL_CONNECTED) {
    h += String(WIFI_SSID) + F(", ");
    h += WiFi.localIP().toString();
    h += String(F(" (")) + WiFi.RSSI() + F(" dBm)");
  } else {
    h += F("<span class=dim>not connected</span>");
  }
  h += F("</td></tr><tr><th>Uptime</th><td>");
  h += upStr;
  h += F("</td></tr><tr><th>Controller link</th><td>TX ");
  h += String(txBytes) + F(" B / RX ");
  h += String(rxBytes) + F(" B &mdash; last data from controller: ");
  if (lastRxMs)
    h += String(sinceRx) + F(" s ago");
  else
    h += F("<span class=dim>never</span>");
  h += F("</td></tr><tr><th>Recent bytes<br>from controller</th><td><code>");
  h += rxLen ? String(hex) : String(F("&mdash;"));
  h += F("</code></td></tr>");
  h += F("<tr><th>TCP serial</th><td>port 6638 &mdash; ");
  h += tcpClient.connected() ? F("<span class=ok>client connected</span>")
                             : F("<span class=dim>no client</span>");
  h += F("</td></tr>");
  h += F("<tr><th>UART Pins</th><td>ESP32 RX: <b>GPIO");
  h += String(rxPin);
  h += F("</b> &middot; ESP32 TX: <b>GPIO");
  h += String(txPin);
  h += F("</b> &mdash; <a href='/setpins?swap=1'><b>[Swap RX ↔ TX]</b></a> &middot; Presets: "
         "<a href='/setpins?rx=18&tx=19'>[RX=18, TX=19]</a> "
         "<a href='/setpins?rx=19&tx=18'>[RX=19, TX=18]</a> "
         "<a href='/setpins?rx=16&tx=17'>[RX=16, TX=17]</a></td></tr>");
  h += F("<tr><th>Baud Rate</th><td>Current: <b>");
  h += String(currentBaud);
  h += F(" 8N1</b> &mdash; Switch: "
         "<a href='/baud?rate=115200'>[115200]</a> "
         "<a href='/baud?rate=9600'>[9600]</a> "
         "<a href='/baud?rate=19200'>[19200]</a> "
         "<a href='/baud?rate=38400'>[38400]</a> "
         "<a href='/baud?rate=57600'>[57600]</a></td></tr>");
#ifdef WITH_KEYLESS
  {  // keyless rows on the bridge status page
    uint32_t kn = millis();
    h += F("<tr><th>Keyless</th><td>");
    h += kArmed ? F("<span class=bad>&#128274; ARMED</span>") : F("<span class=ok>&#128275; disarmed</span>");
    if (kArmed && kSirenActive()) h += F(" <span class=bad>&#128680; SIREN</span>");
    if (kMasterOff) h += F(" <span class=bad>&#9888; master switch OFF &mdash; keyless suspended</span>");
    if (kIgnNow == KIGN_HOT) h += F(" <span class=bad>&#9888; ignition ON &mdash; arming blocked</span>");
    else if (kIgnNow == KIGN_UNKNOWN && kArmingCutsPower())
      h += kBenchMode ? F(" <span class=bad>&#9888; BENCH MODE &mdash; interlocks relaxed, contacts must be disconnected</span>")
                      : F(" <span class=bad>&#9888; ignition sense UNKNOWN &mdash; arming blocked (wire divider or tick bench mode)</span>");
    if (relayMode == RM_IGNITION_NO && kIgnNow != KIGN_COLD && !kBenchMode)
      h += F(" <span class=bad>&#9888; ignition-NO mode needs a validated sense wire for the ride latch</span>");
    if (!kArmed && kn - kBootMs < bootGraceS * 1000UL) {
      h += String(F(" <span class=dim>(grace ")) + String((bootGraceS * 1000UL - (kn - kBootMs)) / 1000 + 1) + F(" s)</span>");
    }
    if (kFobCount) {
      h += String(F("<br><span class=dim>fobs ")) + String(kFobCount) + F("/") + String(K_MAX_FOBS) + F(" — ");
      h += kFobPresent ? F("<span class=ok>present</span>") : F("<span class=dim>absent</span>");
      if (kFobPresent && kSeenIdx >= 0) { h += F(" ("); h += kFobs[kSeenIdx]; h += F(", "); h += String(kRssi); h += F(" dBm)"); }
      else if (kLastSeenMs) { h += F(" (last "); h += String((kn - kLastSeenMs) / 1000); h += F(" s ago)"); }
    } else {
      h += F("<br><span class=dim>no fobs learned</span>");
    }
    h += String(F("<br><span class=dim>relay idle</span></td></tr>"));  // kept minimal; /state.json has details
    h += String(F("<tr><th>Free heap</th><td>")) + String(ESP.getFreeHeap() / 1024) + F(" KB</td></tr>");
    h += F("<tr><th>Keyless actions</th><td>"
           "<form method=post action=/k/arm style=display:inline><button>&#128274; Arm</button></form> "
           "<form method=post action=/k/disarm style=display:inline><button>&#128275; Disarm</button></form> "
           "<form method=post action=/k/panic style=display:inline><button>&#128680; Panic</button></form> "
           "<form method=post action=/k/test style=display:inline><button>Relay test</button></form> "
           "<form method=post action=/k/scan style=display:inline><input type=hidden name=go value=1>"
           "<button>Learn fob</button></form> &middot; "
           "login: <form method=post action=/k/auth style=display:inline>"
           "<input name=pin type=password size=8><button>Go</button></form> "
           "<span class=dim>1 h cookie, or endpoints accept ?pin=</span></td></tr>");
  }
#endif
  h += F("</table><p>"
         "<form method=get action=/test style=display:inline><button>Send test bytes</button></form> "
         "<a href='/setpins?swap=1'><button type=button>Swap RX↔TX</button></a> "
         "<form method=get action=/reboot style=display:inline><button>Restart ESP32</button></form></p>"
         "<p class=dim>Tip: You can dynamically swap RX/TX pins or switch baud rate above to test without reflashing!</p>"
         "</body></html>");
  web.send(200, "text/html", h);
}

void handleTest() {
  const uint8_t t[] = {0x55, 0xAA, 0x55, 0xAA};
  Serial2.write(t, sizeof(t));
  txBytes += sizeof(t);
  web.sendHeader("Location", "/");
  web.send(303);
}

void handleLine() {
  // sample the UART lines: idle UART = HIGH; 0V/floating = no contact or dead controller
  int rx1 = digitalRead(rxPin); int tx1 = digitalRead(txPin);
  delayMicroseconds(500);
  int rx2 = digitalRead(rxPin); int tx2 = digitalRead(txPin);
  delay(1);
  int rx3 = digitalRead(rxPin); int tx3 = digitalRead(txPin);
  String r = "{\"rx\":" + String(rx1) + String(rx2) + String(rx3) +
             ",\"tx\":" + String(tx1) + String(tx2) + String(tx3) + "}";
  web.send(200, "application/json", r);
}

void handleReboot() {
  web.send(200, "text/html", "Rebooting...");
  delay(200);
  ESP.restart();
}

void setup() {
  pinMode(LED_PIN, OUTPUT);

  Serial.begin(115200);
  Serial2.begin(currentBaud, SERIAL_8N1, rxPin, txPin);

  SerialBT.setPin(BT_PIN);
  SerialBT.begin(BT_NAME);
  Serial.println(String("Bluetooth ready, discoverable as: ") + BT_NAME);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println(String("[WiFi] connecting to ") + WIFI_SSID + " ...");

  ArduinoOTA.setHostname("votol-bt-bridge");
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);   // define in wifi_secrets.h
#else
  Serial.println("[OTA] WARNING: no OTA_PASSWORD in wifi_secrets.h — OTA is UNPROTECTED");
#endif
  ArduinoOTA.begin();
  MDNS.addService("http", "tcp", 80);

  web.on("/", handleRoot);
  web.on("/baud", handleBaud);
  web.on("/setpins", handlePins);
  web.on("/test", handleTest);
  web.on("/line", handleLine);
  web.on("/reboot", handleReboot);
  web.begin();

  tcpServer.begin();

#ifdef WITH_KEYLESS
  // config BEFORE driving the relay pin: saved polarity defines idle level
  kLoadCfg();
  // relay SAFE first: coil idle (contact closed in NC mode) as early as possible
  pinMode(K_RELAY_PIN, OUTPUT);
  digitalWrite(K_RELAY_PIN, kRelayIdleLevel());
  pinMode(K_BUZZER_PIN, OUTPUT);
  digitalWrite(K_BUZZER_PIN, LOW);
  pinMode(K_VIBE_PIN, INPUT);
  pinMode(K_BUTTON_PIN, INPUT_PULLUP);
  pinMode(K_IGN_PIN, INPUT_PULLDOWN);   // unwired sense reads cold, never blocks
  pinMode(K_MASTER_PIN, INPUT_PULLUP);  // unwired saklar reads ON (keyless active)
  kBootMs = millis();
  kAbsentSinceMs = kBootMs;
  // BLE attaches to the already-running dual-mode BT controller (SerialBT above)
  BLEDevice::init("");
  kScan = BLEDevice::getScan();
  kScan->setAdvertisedDeviceCallbacks(new KScanCb());
  kScan->setActiveScan(true);
  web.on("/state.json", handleKState);
  const char *kHdrs[] = {"Cookie"};
  web.collectHeaders(kHdrs, 1);
  kAuthTok = esp_random();  // per-boot: rebooting logs browsers out
  // machine API (any method, ?pin=) + browser session (POST + cookie)
  auto kArmH = []() { if (!kPinOk()) { kDenyPin(); return; } kTryArm(true, true); kRedirectHome(); };
  auto kDisarmH = []() { if (!kPinOk()) { kDenyPin(); return; } kSetArmed(false, true); kRedirectHome(); };
  web.on("/k/arm", HTTP_GET, kArmH);
  web.on("/k/arm", HTTP_POST, kArmH);
  web.on("/k/disarm", HTTP_GET, kDisarmH);
  web.on("/k/disarm", HTTP_POST, kDisarmH);
  web.on("/k/panic", HTTP_GET, handleKPanic);
  web.on("/k/panic", HTTP_POST, handleKPanic);
  web.on("/k/test", HTTP_GET, handleKTest);
  web.on("/k/test", HTTP_POST, handleKTest);
  web.on("/k/fob", HTTP_GET, handleKFob);
  web.on("/k/fob", HTTP_POST, handleKFob);
  web.on("/k/scan", HTTP_GET, handleKScan);
  web.on("/k/scan", HTTP_POST, handleKScan);
  web.on("/k/cfg", HTTP_POST, handleKCfg);
  web.on("/k/auth", HTTP_GET, handleKAuth);
  web.on("/k/auth", HTTP_POST, handleKAuth);
  Serial.println("[KEY] keyless active (disarmed, boot grace; relay NC fail-safe)");
#endif
}

void loop() {
#ifdef WITH_KEYLESS
  kTick();
#endif
  static bool wasConnected = false;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected) {
    wasConnected = connected;
    Serial.println(connected
        ? String("[WiFi] connected, IP: ") + WiFi.localIP().toString()
        : "[WiFi] disconnected");
  }
  if (!connected && !portalStarted && millis() > PORTAL_AFTER_MS) {
    portalStarted = true;
    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal("VOTOL-BT-Setup");
    Serial.println("[WiFi] setup hotspot opened: VOTOL-BT-Setup");
  }
  wm.process();
  ArduinoOTA.handle();
  web.handleClient();

  if (tcpServer.hasClient()) {
    if (tcpClient.connected()) tcpClient.stop();
    tcpClient = tcpServer.accept();
  }
  while (tcpClient.connected() && tcpClient.available()) {
    Serial2.write(tcpClient.read());
    txBytes++;
  }

  while (SerialBT.available()) {
    Serial2.write(SerialBT.read());
    txBytes++;
  }

  while (Serial2.available()) {
    uint8_t c = Serial2.read();
    SerialBT.write(c);
    if (tcpClient.connected()) tcpClient.write(c);
    Serial.write(c);
    rxBytes++;
    lastRxMs = millis();
    if (rxLen < sizeof(rxBuf)) rxBuf[rxLen++] = c;
    else { memmove(rxBuf, rxBuf + 1, sizeof(rxBuf) - 1); rxBuf[sizeof(rxBuf) - 1] = c; }
  }

  while (Serial.available()) {
    Serial2.write(Serial.read());
    txBytes++;
  }

#ifdef WITH_KEYLESS
  // LED: very fast = siren, fast = armed, solid = fob near or phone app, slow = idle
  {
    uint32_t ln = millis();
    if (kSirenActive())      digitalWrite(LED_PIN, ln % 120 < 60);
    else if (kArmed)         digitalWrite(LED_PIN, ln % 300 < 150);
    else if (kFobPresent || SerialBT.hasClient()) digitalWrite(LED_PIN, HIGH);
    else                     digitalWrite(LED_PIN, ln % 2000 < 100);
  }
#else
  digitalWrite(LED_PIN, SerialBT.hasClient() ? HIGH : (millis() % 2000 < 100));
#endif
}
