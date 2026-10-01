/*
 * tft-dash v2 — Bluetooth display pod for the esp-votol system.
 *
 * Architecture change (2026-10-01): WiFi OFF during normal operation.
 * Reason: this shield's touch is a 4-wire resistive film wired to UNO pins
 * shared with the LCD (A1/A2/D6/D7 -> GPIO 4/15-net/27/14, all ADC2), and
 * the ESP32's WiFi driver locks ADC2 — with WiFi off and only Bluetooth
 * Classic running, the film is readable with the classic Adafruit
 * TouchScreen method.
 *
 * Data path: SPP client -> votol-bt-bridge ("VOTOL-BT", the same BT link
 * the VOTOL phone app uses) -> controller UART. The pod speaks the VOTOL
 * protocol itself (SHOW telemetry poll), so no laptop/webapp is needed at
 * the bike. Frame format + decode ported from webapp/app.py.
 *
 * WiFi appears ONLY for the first WIFI_WINDOW_S seconds after boot
 * (OTA + status page), then WiFi.mode(WIFI_OFF) and touch comes alive.
 *
 * LCD-safe touch: sampling drives the film's shared nets, so the LCD is
 * deafened first (CS high via the GPIO33 jumper net), and all four pins
 * are restored as outputs afterwards.
 *
 * Bring-up over USB serial (115200): 'v' toggle film mapping variant,
 * 's' swap axes, 'f'/'g' flip x/y, 'w' save touch setup to NVS,
 * 'r' 6 s raw dump, 'p' send SHOW now, 'd' display on 2 min,
 * 'f <mac>' set fob, 'c <metres>' wheel circumference (RIDE km/h),
 * 'a'/'A' preview the ARM/DISARM animation, 'B' BT link on/off
 * (off while the bridge talks CAN to the VOTOL instead of BT SPP).
 *
 * TELE has three panes (tap the content to page through): RIDE = one big
 * speed number (km/h once the wheel circumference is set, else motor rpm),
 * ELEC = V/A/W/gear, MOTOR = rpm/gear/temps/status. Riding (rpm >= 50 for
 * 2 s) locks TELE+RIDE; idle rotates ELEC<->MOTOR every 8 s.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <MCUFRIEND_kbv.h>

/* ---- wifi window ---- */
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID "CONFIGURE-ME"
#define WIFI_PASS "CONFIGURE-ME"
#endif
#define WIFI_WINDOW_S 30

Preferences prefs;   // NVS (declared early: fob fns + setup use it)
/* ---- bluetooth ---- */
#include "BluetoothSerial.h"
BluetoothSerial SerialBT;
#define BT_SERVER_NAME "VOTOL-BT"
#define BT_SERVER_PIN  "1234"
volatile bool btConnecting = false;
uint8_t btFailCount = 0;
void btConnectTask(void *) {
  Serial.println("[bt] connecting " BT_SERVER_NAME " ...");
  bool ok = SerialBT.connect(BT_SERVER_NAME);
  Serial.printf("[bt] connect %s\n", ok ? "ok" : "failed");
  if (ok) btFailCount = 0; else btFailCount++;
  btConnecting = false;
  vTaskDelete(nullptr);
}

/* ---- iTag keyless (BLE) ----
 * The fob is a BLE advertiser; presence = seen within FOB_TTL_S.
 * BLE scan runs in its own task, coexisting with the SPP client
 * (same Bluedroid stack as the one-chip bridge build). Buzzer on IO5
 * = the reserved "alarm out 1" pin (active-buzzer friendly square wave). */
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#define FOB_TTL_S       12     // absent if silent this long
#define ARM_AFTER_S     8      // sustained absence before ARM chirp
#define BOOT_GRACE_S    45
#define PANIC_S         30
#define BUZZ_PIN        5
#define BUZZ_CH         4

uint8_t fobMac[6]; int fobMacLen = 0;
volatile int fobRssi = -128;
volatile uint32_t fobLastSeenMs = 0;
volatile bool bleScanDump = false;      // CLI 'i': print next scan's devices
volatile bool bleLearn = false;         // CLI 'm': adopt strongest ITAG-named device
uint8_t learnMac[6]; int learnRssi = -128;
uint32_t learnAtMs = 0;

class FobCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (bleScanDump) {
      Serial.printf("[ble] %s  rssi %d  name \"%s\"\n",
                    dev.getAddress().toString().c_str(), dev.getRSSI(),
                    dev.haveName() ? dev.getName().c_str() : "");
    }
    std::string s = dev.getAddress().toString();   // "aa:bb:cc:dd:ee:ff"
    uint8_t mac[6];
    for (int i = 0; i < 6; i++)
      mac[i] = (uint8_t)strtoul(s.substr(i * 3, 2).c_str(), nullptr, 16);
    bool match = (fobMacLen == 6 && memcmp(mac, fobMac, 6) == 0);
    if (!match && bleLearn && dev.haveName()) {
      std::string n = dev.getName();
      for (auto &ch : n) ch = tolower((unsigned char)ch);
      if (n.find("itag") != std::string::npos && dev.getRSSI() > learnRssi) {
        memcpy(learnMac, mac, 6);
        learnRssi = dev.getRSSI(); learnAtMs = millis();
      }
    }
    if (match) { fobRssi = dev.getRSSI(); fobLastSeenMs = millis(); }
  }
};
FobCb fobCb;
BLEScan *bleScan = nullptr;

void bleTask(void *) {
  BLEDevice::init("");
  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(&fobCb, false);
  bleScan->setActiveScan(true);         // active: fetch names (iTag identifies itself)
  for (;;) {
    BLEScanResults r = bleScan->start(1.5, false);
    bleScan->clearResults();
    vTaskDelay(pdMS_TO_TICKS(100));   // tight duty cycle — fast fob pickup
  }
}

void fobSave() {
  prefs.putBytes("fob", fobMac, 6);
  prefs.putUChar("foblen", fobMacLen);
}
void fobLoad() {
  fobMacLen = 0;
  if (prefs.getBytesLength("fob") == 6) {
    prefs.getBytes("fob", fobMac, 6);
    fobMacLen = prefs.getUChar("foblen", 0);
  }
}
void fobMacStr(char *out, size_t n) {
  if (fobMacLen != 6) { snprintf(out, n, "not set"); return; }
  snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
           fobMac[0], fobMac[1], fobMac[2], fobMac[3], fobMac[4], fobMac[5]);
}

/* ---- buzzer ---- */
bool sirenOn = false;
uint32_t panicUntilMs = 0;
void buzzInit() {
  ledcSetup(BUZZ_CH, 2000, 8);
  ledcAttachPin(BUZZ_PIN, BUZZ_CH);
  ledcWriteTone(BUZZ_CH, 0);
}
void chirp(int n) {
  for (int i = 0; i < n; i++) {
    ledcWriteTone(BUZZ_CH, 2300); delay(90);
    ledcWriteTone(BUZZ_CH, 0);    delay(50);
  }
}
void buzzTick() {
  if (millis() < panicUntilMs) {
    ledcWriteTone(BUZZ_CH, ((millis() / 250) & 1) ? 2500 : 0);   // wail
    sirenOn = true;
  } else if (sirenOn) {
    ledcWriteTone(BUZZ_CH, 0);
    sirenOn = false;
  }
}

/* ---- display ---- */
MCUFRIEND_kbv tft;
#define W 240
#define H 320
uint16_t cBg, cBg2, cTxt, cDim, cAcc, cGood, cWarn, cBad;

/* ---- pages ---- */
enum Page : uint8_t { PG_TELE = 0, PG_KEYLESS = 1, PG_SETUP = 2 };
const char *PAGE_NAMES[3] = {"TELE", "LOCK", "SYS"};   // LOCK: "KEYLESS" clips in 80px
uint8_t setupTab = 0;               // SYS page sub-tab: 0=STATUS 1=CONFIG
Page page = PG_TELE;
uint8_t telePane = 1;               // TELE panes: 0=RIDE 1=ELEC 2=MOTOR
const char *PANE_NAMES[3] = {"RIDE", "ELEC", "MOTOR"};

/* ---- touch: film on shared LCD nets ----
 * variant 0 (MCUFRIEND classic, XP=D6 XM=A2 YP=A1 YM=D7):
 *   XP=GPIO27  XM=GPIO15 (jumper net A2, GPIO35 bystander)  YP=GPIO4  YM=GPIO14
 * variant 1 (tutorial mapping, XP=D7 XM=A1 YP=A2 YM=D6):
 *   XP=GPIO14  XM=GPIO4   YP=GPIO15                          YM=GPIO27
 */
struct FilmMap { uint8_t xp, yp, xm, ym; };
const FilmMap FILMS[2] = {{27, 4, 15, 14}, {14, 15, 4, 27}};
int touchVariant = 0;
bool tSwapXY = false, tFlipX = false, tFlipY = false;
/* Calibrated 2026-10-01 from two real presses:
 * center -> raw(2156,2344), bottom-left TELE tab -> raw(1040,745).
 * X natural, Y INVERTED (larger raw = higher on screen) -> tFlipY.
 * Serial 'g' + 'w' persist the flip after this build. */
#define TR_MIN 500
#define TR_MAX 4095

/* ---- VOTOL protocol (ported from webapp/app.py) ---- */
uint8_t xor8(const uint8_t *d, int n) { uint8_t c = 0; while (n--) c ^= *d++; return c; }
void sendShow() {
  if (!SerialBT.connected()) return;
  uint8_t f[24];
  f[0] = 0xC9; f[1] = 0x14; f[2] = 0x02;
  memset(f + 3, 0, 19);
  memcpy(f + 3, "SHOW", 4);
  f[12] = 0xAA;                 // LOCAL observe — never drives the motor
  f[22] = xor8(f, 22);
  f[23] = 0x0D;
  SerialBT.write(f, sizeof(f));
}
struct Tele {
  bool has = false;
  float v = 0, a = 0;
  int32_t rpm = 0, tc = 0, tm = 0;
  char gear = '?';
  uint8_t status = 0;
  uint32_t fault = 0;
  uint32_t atMs = 0;
} tele;
const char *CTL_STATUS[8] = {"IDLE","INIT","START","RUN","STOP","BRAKE","WAIT","FAULT"};
const char *GEARS = "LMHS";
float wheelCircM = 0;   // tyre circumference m; >0 => RIDE pane km/h = rpm*circ*0.06

uint8_t rxbuf[512];
int rxlen = 0;
uint32_t rxCount = 0, txCount = 0, lastRxMs = 0;

int16_t be16(const uint8_t *f, int i) { return (int16_t)((f[i] << 8) | f[i + 1]); }

void decodeTelemetry(const uint8_t *f) {
  uint8_t flags = f[20];
  tele.has = true; tele.atMs = millis();
  tele.v = be16(f, 5) / 10.0f;
  tele.a = be16(f, 7) / 10.0f;
  tele.fault = ((uint32_t)f[10] << 24) | ((uint32_t)f[11] << 16) | (f[12] << 8) | f[13];
  tele.rpm = be16(f, 14);
  tele.tc = (int8_t)f[16] - 50;
  tele.tm = (int8_t)f[17] - 50;
  uint8_t g = flags & 0x03; tele.gear = (g < 4) ? GEARS[g] : '?';
  tele.status = f[21] & 0x07;
  rxCount++; lastRxMs = millis();
}

void parseRx() {
  while (SerialBT.available() && rxlen < (int)sizeof(rxbuf))
    rxbuf[rxlen++] = (uint8_t)SerialBT.read();
  int i = 0;
  while (true) {
    while (i + 23 < rxlen && !(rxbuf[i] == 0xC0 && rxbuf[i + 1] == 0x14 && rxbuf[i + 23] == 0x0D))
      i++;
    if (i + 24 > rxlen) break;
    uint8_t *f = rxbuf + i;
    if (xor8(f, 22) != f[22]) { i++; continue; }
    if (f[2] == 0x0D && f[3] == 0x59) decodeTelemetry(f);
    memmove(rxbuf, rxbuf + i + 24, rxlen - i - 24);
    rxlen -= i + 24;
    i = 0;
  }
  if (rxlen > 400) { memmove(rxbuf, rxbuf + 200, rxlen - 200); rxlen -= 200; }
}

/* ---- module state ---- */
WebServer web(80);
uint32_t bootMs = 0, lastPollMs = 0, lastTouchMs = 0, lastCycleMs = 0, lastBtTryMs = 0;
uint32_t btPauseUntilMs = 0;
bool btLinkOn = true;               // false = don't dial VOTOL-BT (bridge now talks CAN to the VOTOL)
bool wifiPhase = true;
char toastTxt[80] = "";
uint16_t toastCol = 0;
uint32_t toastAtMs = 0;
uint8_t pendingAnim = 0;            // 1 = ARM animation, 2 = DISARM (played when dispOn)
void toast(const char *t, uint16_t c) {
  strncpy(toastTxt, t, sizeof(toastTxt) - 1); toastTxt[sizeof(toastTxt) - 1] = 0;
  toastCol = c; toastAtMs = millis();
}
bool btPaused() { return millis() < btPauseUntilMs; }
void btPauseToggle() {
  if (!btLinkOn) { toast("BT link is off (serial 'B')", cDim); return; }
  if (btPaused()) {
    btPauseUntilMs = 0;
    toast("BT resume", cGood);
  } else {
    btPauseUntilMs = millis() + 5UL * 60UL * 1000UL;
    if (SerialBT.connected()) SerialBT.disconnect();
    toast("BT released 5min - connect phone", cAcc);
  }
  Serial.printf("[bt] %s\n", btPaused() ? "paused 5 min" : "resumed");
}

/* ---- slot-cached painter (fixed box, repaint on change) ---- */
struct Slot { char last[48] = "\x01"; uint16_t lastCol = 0xFFFF, lastBg = 0xFFFF; };
void slotPrint(Slot &s, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
               uint8_t size, const char *txt, uint16_t col, uint16_t bg) {
  if (!strcmp(s.last, txt) && s.lastCol == col && s.lastBg == bg) return;
  strncpy(s.last, txt, sizeof(s.last) - 1); s.last[sizeof(s.last) - 1] = 0;
  s.lastCol = col; s.lastBg = bg;
  tft.fillRect(x, y, w, h, bg);
  tft.setTextSize(size); tft.setTextColor(col);
  tft.setCursor(x + 2, y + ((h > 8 * size) ? (h - 8 * size) / 2 : 1));
  tft.print(txt);
}
Slot sTeleBig, sTeleCur, sTelePow, sTeleRpm, sTeleGear, sTeleTc, sTeleStat, sTeleLink;
Slot sSys[8], sToast, sHeader;
Slot sKlState, sKlFob, sKlInfo;
int8_t btnCache = -1;

bool fobPresent() {
  // lastSeen==0 means "never seen since boot" — without the guard the pod
  // thinks the fob is present for the first FOB_TTL_S after every power-on
  return fobMacLen == 6 && fobLastSeenMs != 0 &&
         (millis() - fobLastSeenMs) < FOB_TTL_S * 1000UL;
}

/* ---- display power: follows the registered iTag ----
 * unregistered fob -> always on (you must be able to see what you do);
 * registered: on while the fob is near (or during the boot wifi window /
 * a serial 'd' override); off = panel DISPOFF (GRAM keeps the frame, so
 * waking redraws only what changed). */
bool dispOn = true;
uint32_t dispForceUntilMs = 0;
uint16_t dotCache = 0xFFFF;
int8_t btOffIconShown = -1;          // crossed-BT badge state (top right)
void drawChrome();                                   // fwd
void uiInvalidate() {
  Slot *all[] = {&sTeleBig, &sTeleCur, &sTelePow, &sTeleRpm, &sTeleGear, &sTeleTc,
                 &sTeleStat, &sTeleLink, &sKlState, &sKlFob, &sKlInfo,
                 &sToast, &sHeader, sSys, sSys + 1, sSys + 2, sSys + 3,
                 sSys + 4, sSys + 5, sSys + 6, sSys + 7};
  for (Slot *s : all) { s->last[0] = 1; s->last[1] = 0; s->lastCol = 0xFFFF; }
}
void setDisplay(bool on) {
  if (on == dispOn) return;
  dispOn = on;
  if (on) {
    uiInvalidate();
    dotCache = 0xFFFF;                   // force header dot repaint
    btOffIconShown = -1;                 // and the BT-off badge
    drawChrome();
  } else {
    // NOT panel DISPOFF: on these shields the always-on backlight shines
    // through an undriven panel as WHITE. Black fill = visually off.
    pendingAnim = 0;                         // stale animation would be confusing
    tft.fillRect(0, 0, W, H, 0x0000);
  }
}
void displayTick() {
  // screen follows the fob from the very first boot second — the wifi/OTA
  // window runs headless unless the fob (or an override) is present
  bool want = (fobMacLen != 6) ||
              (millis() < dispForceUntilMs) || fobPresent();
  if (want != dispOn) {
    setDisplay(want);
    Serial.printf("[disp] %s (%s)\n", dispOn ? "on" : "off",
                  fobMacLen != 6 ? "no fob registered" :
                  millis() < dispForceUntilMs ? "override" :
                  fobPresent() ? "fob near" : "fob away");
  }
}

/* ---- keyless state machine ---- */
bool klArmed = false;
uint32_t absentSinceMs = 0;
void klTick() {
  if (fobMacLen != 6) return;                 // no fob learned yet
  uint32_t now = millis();
  if (now - bootMs < BOOT_GRACE_S * 1000UL) { absentSinceMs = now; return; }
  if (fobPresent()) {
    absentSinceMs = now;
    if (klArmed) { klArmed = false; chirp(1); toast("DISARMED - fob back", cGood); pendingAnim = 2; }
  } else {
    if (absentSinceMs == 0) absentSinceMs = now;
    // arm counted from the LAST SIGHTING: fires the moment the 12s fob
    // TTL lapses — BEFORE the display would sleep — so the sequence is
    // animation FIRST, screen off ~6s later (same 12s of silence needed
    // as before; no robustness change)
    if (!klArmed && (now - fobLastSeenMs) > ARM_AFTER_S * 1000UL) {
      klArmed = true; chirp(2);
      if (fobLastSeenMs >= bootMs) {       // fob was around this boot: show it
        toast("ARMED - fob away", cWarn); pendingAnim = 1;
        dispForceUntilMs = now + 6000;     // keep the screen lit through it
      } else {                             // powered on with fob already off:
        Serial.println("[kl] armed silently — fob off since boot");
      }                                    // never light the screen at all
      absentSinceMs = now;
    }
  }
}
void panicToggle() {
  if (millis() < panicUntilMs) { panicUntilMs = 0; toast("siren stopped", cGood); }
  else if (klArmed) { panicUntilMs = millis() + PANIC_S * 1000UL; toast("SIREN 30s", cBad); }
  else toast("arm first (fob away)", cWarn);
}

/* ---- riding detection: sustained wheel rpm locks TELE to the RIDE pane ---- */
bool riding = false;
uint32_t rideStartMs = 0, haltStartMs = 0;
void ridingTick() {
  bool spinning = tele.has && (millis() - tele.atMs) < 3000 && tele.rpm >= 50;
  if (spinning) {
    haltStartMs = 0;
    if (!rideStartMs) rideStartMs = millis();
    if (!riding && millis() - rideStartMs > 2000) {
      riding = true;
      page = PG_TELE; telePane = 0;         // riding: speed-maximized pane
      if (dispOn) { uiInvalidate(); drawChrome(); }
      Serial.println("[tele] riding — RIDE pane");
    }
  } else {
    rideStartMs = 0;
    if (!haltStartMs) haltStartMs = millis();
    if (riding && millis() - haltStartMs > 5000) riding = false;   // stopped
  }
}

/* =========================================================== touch */
void lcdQuiesce() {                 // deafen the LCD while nets are borrowed
  digitalWrite(33, HIGH);           // CS net (jumper) high = LCD ignores bus
}
void lcdRestore() {
  const FilmMap &m = FILMS[touchVariant];
  pinMode(m.xp, OUTPUT); pinMode(m.xm, OUTPUT);
  pinMode(m.yp, OUTPUT); pinMode(m.ym, OUTPUT);
  digitalWrite(4, HIGH);            // WR idle high
  digitalWrite(33, LOW);            // CS active again
}

/* ---- UI settings (CFG page, NVS-persisted, live-applied) ---- */
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
const uint16_t PALETTE[6] = {
  RGB565(0, 200, 255),   // cyan (default)
  RGB565(0, 255, 120),   // green
  RGB565(255, 200, 0),   // yellow
  RGB565(255, 150, 40),  // orange
  RGB565(235, 240, 245), // white
  RGB565(240, 90, 220),  // pink
};
uint8_t uiScale = 1;                 // 0 small, 1 medium (default), 2 large
uint16_t uiVal = RGB565(0, 200, 255);   // telemetry value color
uint16_t uiAcc = RGB565(0, 200, 255);   // accent (tabs, highlights)
void uiSave() {
  prefs.putUChar("uiscale", uiScale);
  prefs.putUShort("uival", uiVal);
  prefs.putUShort("uiacc", uiAcc);
}
void uiLoad() {
  uiScale = prefs.getUChar("uiscale", 1);
  uiVal = prefs.getUShort("uival", RGB565(0, 200, 255));
  uiAcc = prefs.getUShort("uiacc", RGB565(0, 200, 255));
}
uint8_t szBig()  { return uiScale == 0 ? 3 : uiScale == 1 ? 4 : 5; }
uint8_t szMid()  { return uiScale == 2 ? 3 : 2; }
uint8_t szHero() { return uiScale == 0 ? 3 : 4; }   // 5 would overflow DISARMED

int median3(int a, int b, int c) {
  int mx = max(a, max(b, c)), mn = min(a, min(b, c));
  return a + b + c - mx - mn;
}

// quick pressure-only probe (2 analog reads) — keeps idle loop light
int filmZ() {
  const FilmMap &m = FILMS[touchVariant];
  lcdQuiesce();
  pinMode(m.xp, OUTPUT); digitalWrite(m.xp, LOW);
  pinMode(m.ym, OUTPUT); digitalWrite(m.ym, HIGH);
  pinMode(m.xm, INPUT); pinMode(m.yp, INPUT);
  delayMicroseconds(250);
  int z1 = analogRead(m.xm), z2 = analogRead(m.yp);
  lcdRestore();
  return 4095 - (z2 - z1);
}

struct RawTouch { int x, y, z; bool valid; };
RawTouch readFilm() {
  RawTouch r{0, 0, 0, false};
  const FilmMap &m = FILMS[touchVariant];
  int z = filmZ();
  if (z < 200 || z > 3900) return r;
  lcdQuiesce();
  int s[3];
  // X: drive X+ high / X- low, sense on Y+
  pinMode(m.yp, INPUT); pinMode(m.ym, INPUT);
  pinMode(m.xp, OUTPUT); pinMode(m.xm, OUTPUT);
  digitalWrite(m.xp, HIGH); digitalWrite(m.xm, LOW);
  delayMicroseconds(500);
  s[0] = analogRead(m.yp); s[1] = analogRead(m.yp); s[2] = analogRead(m.yp);
  r.x = 4095 - median3(s[0], s[1], s[2]);
  // Y: drive Y+ high / Y- low, sense on X-
  pinMode(m.xp, INPUT); pinMode(m.xm, INPUT);
  pinMode(m.yp, OUTPUT); pinMode(m.ym, OUTPUT);
  digitalWrite(m.yp, HIGH); digitalWrite(m.ym, LOW);
  delayMicroseconds(500);
  s[0] = analogRead(m.xm); s[1] = analogRead(m.xm); s[2] = analogRead(m.xm);
  r.y = 4095 - median3(s[0], s[1], s[2]);
  lcdRestore();
  r.z = z;
  r.valid = true;
  return r;
}

int lastRawX = -1, lastRawY = -1, lastRawZ = -1;
void drawChrome();                                   // fwd (used by handleTouch)
void handleTouch() {
  static uint32_t lastSample = 0;
  if (millis() - lastSample < 30) return;
  lastSample = millis();
  int z = filmZ();                          // cheap probe first
  if (z < 300 || z > 3800) return;          // idle: 2 reads, no spam
  RawTouch r = readFilm();                  // pressed: full x/y now
  lastRawX = r.x; lastRawY = r.y; lastRawZ = r.z;
  if (!r.valid) return;
  int px = map(r.x, TR_MIN, TR_MAX, 0, W);
  int py = map(r.y, TR_MIN, TR_MAX, 0, H);
  if (tSwapXY) { int t = px; px = py * W / H; py = t * H / W; }
  if (tFlipX) px = W - 1 - px;
  if (tFlipY) py = H - 1 - py;
  px = constrain(px, 0, W - 1);
  py = constrain(py, 0, H - 1);
  Serial.printf("[tap] raw(%d,%d,%d) -> px=%d py=%d\n", r.x, r.y, r.z, px, py);
  static uint32_t lastTapMs = 0;
  if (millis() - lastTapMs < 180) return;   // fast debounce — taps feel instant
  lastTapMs = millis(); lastTouchMs = millis();
  if (py >= 270) {                          // tab bar (+margin for finger size)
    uint8_t np = px / 80;
    if (np != page) { page = (Page)np; uiInvalidate(); drawChrome(); toast(PAGE_NAMES[page], uiAcc); }
  } else if (page == PG_SETUP && py >= 24 && py <= 74) {   // sub-tabs (+margin)
    uint8_t nt = px < 120 ? 0 : 1;
    if (nt != setupTab) { setupTab = nt; uiInvalidate(); drawChrome(); }
  } else if (page == PG_SETUP && setupTab == 1) {          // CONFIG content
    if (py >= 84 && py <= 124 && px >= 8) {                // size S/M/L
      uiScale = px < 86 ? 0 : px < 154 ? 1 : 2;
      uiSave(); drawChrome(); toast("size saved", uiAcc);
    } else if (py >= 146 && py <= 174 && px >= 8 && px <= 230) {   // value color
      uiVal = PALETTE[(px - 8) / 38];
      uiSave(); drawChrome(); toast("value color saved", uiAcc);
    } else if (py >= 198 && py <= 226 && px >= 8 && px <= 230) {   // accent color
      uiAcc = PALETTE[(px - 8) / 38];
      uiSave(); drawChrome(); toast("accent color saved", uiAcc);
    }
  } else if (page == PG_SETUP && setupTab == 0 &&
             py >= 212 && py <= 256 && px >= 8 && px <= 232) {
    btPauseToggle();                        // release BT for the phone app
  } else if (page == PG_KEYLESS && py >= 190 && py <= 250 && px >= 8 && px <= 232) {
    panicToggle();                          // siren on/off
  } else if (page == PG_TELE && py >= 30) {
    telePane = (telePane + 1) % 3;          // tap content: RIDE -> ELEC -> MOTOR
    lastCycleMs = millis();
    uiInvalidate(); drawChrome();
  } else if (klArmed) {
    panicUntilMs = 0;                       // any tap elsewhere silences the wail
  }
}

/* =========================================================== drawing */

void drawTabBar() {
  for (uint8_t i = 0; i < 3; i++) {
    uint16_t x = i * 80;
    bool act = (i == page);
    tft.fillRect(x + 1, 278, 78, 40, act ? uiAcc : cBg2);
    tft.drawRect(x + 1, 278, 78, 40, cDim);
    uint8_t ts = strlen(PAGE_NAMES[i]) * 12 > 76 ? 1 : 2;   // never clip
    tft.setTextSize(ts);
    tft.setTextColor(act ? cBg : cTxt);
    uint16_t tw = strlen(PAGE_NAMES[i]) * 6 * ts;
    tft.setCursor(x + (80 - tw) / 2, ts == 2 ? 293 : 297);
    tft.print(PAGE_NAMES[i]);
  }
}

void drawSubTabs() {                // SYS page: STATUS / CONFIG
  const char *names[2] = {"STATUS", "CONFIG"};
  for (uint8_t i = 0; i < 2; i++) {
    uint16_t x = 8 + i * 116;
    bool act = (setupTab == i);
    tft.fillRect(x, 28, 108, 40, act ? uiAcc : cBg2);
    tft.drawRect(x, 28, 108, 40, cDim);
    tft.setTextSize(2);
    tft.setTextColor(act ? cBg : cTxt);
    uint16_t tw = strlen(names[i]) * 12;
    tft.setCursor(x + (108 - tw) / 2, 40);
    tft.print(names[i]);
  }
}

void drawChrome() {
  tft.fillRect(0, 27, W, 250, cBg);
  drawTabBar();
  if (page == PG_SETUP) drawSubTabs();
  if (page == PG_TELE) {
    tft.setTextSize(2); tft.setTextColor(cDim);
    if (telePane == 0) {                    // RIDE
      tft.setCursor(8, 34);   tft.print("SPEED");
    } else if (telePane == 1) {             // ELEC
      tft.setCursor(8, 32);   tft.print("BATTERY");
      tft.setCursor(128, 32); tft.print("CURRENT");
      tft.setCursor(8, 96);   tft.print("POWER");
      tft.setCursor(128, 96); tft.print("GEAR");
    } else {                                 // MOTOR
      tft.setCursor(8, 32);   tft.print("RPM");
      tft.setCursor(128, 32); tft.print("GEAR");
      tft.setCursor(8, 100);  tft.print("CTRL/MOT C");
    }
  }
  btnCache = -1;
}

void drawHeader() {
  char h[30];
  uint16_t col = cTxt;
  uint32_t age = tele.has ? (millis() - tele.atMs) / 1000 : 999;
  if (wifiPhase)             { snprintf(h, sizeof(h), "VOTOL setup window"); col = uiAcc; }
  else if (btPaused())        { snprintf(h, sizeof(h), "VOTOL  bt paused");  col = cAcc; }
  else if (!btLinkOn)         snprintf(h, sizeof(h), "VOTOL %s",   // plain — badge shows the state
                page == PG_TELE ? PANE_NAMES[telePane] : PAGE_NAMES[page]);
  else if (!SerialBT.connected()) { snprintf(h, sizeof(h), "VOTOL  bt search"); col = cWarn; }
  else if (!tele.has || age > 10)  { snprintf(h, sizeof(h), "VOTOL  no data");   col = cWarn; }
  else snprintf(h, sizeof(h), "VOTOL %s",
                page == PG_TELE ? PANE_NAMES[telePane] : PAGE_NAMES[page]);
  slotPrint(sHeader, 0, 0, 190, 26, 2, h, col, cBg2);

  uint16_t dot = cBad;
  if (!btLinkOn) dot = 0;                   // link intentionally off: no alarm dot
  else if (!wifiPhase && tele.has) {
    if (age < 5) dot = cGood; else if (age < 30) dot = cWarn;
  } else if (!wifiPhase && SerialBT.connected()) dot = cWarn;
  if (dot != dotCache) {
    tft.fillRect(216, 7, 18, 12, cBg2);
    if (dot) tft.fillCircle(225, 13, 6, dot);
    dotCache = dot;
  }

  // BT-off badge: crossed "BT" left of the dot
  bool off = !btLinkOn;
  if (off != (bool)btOffIconShown) {
    btOffIconShown = off;
    tft.fillRect(190, 4, 24, 20, cBg2);
    if (off) {
      tft.setTextSize(1); tft.setTextColor(cDim);
      tft.setCursor(196, 10); tft.print("BT");
      tft.drawLine(193, 4, 213, 22, cBad);
      tft.drawLine(194, 4, 214, 22, cBad);
    }
  }
}

void drawTelemetry() {
  char b[32];
  if (telePane == 0) {                       // RIDE — one huge number
    if (!tele.has) {
      slotPrint(sTeleBig, 8, 58, 224, 76, uiScale == 0 ? 4 : 5, "--", cDim, cBg);
      slotPrint(sTelePow, 8, 170, 110, 24, szMid(), "--", cDim, cBg);
      slotPrint(sTeleGear, 122, 170, 110, 24, szMid(), "--", cDim, cBg);
    } else {
      if (wheelCircM > 0) dtostrf(tele.rpm * wheelCircM * 0.06f, 1, 0, b);
      else                snprintf(b, sizeof(b), "%ld", (long)tele.rpm);
      uint8_t sr = uiScale == 0 ? 4 : 5;
      if (strlen(b) > 4 && sr > 4) sr = 4;   // auto-fit
      slotPrint(sTeleBig, 8, 58, 224, 76, sr, b, uiVal, cBg);
      dtostrf(tele.v, 4, 1, b); strcat(b, "V");
      slotPrint(sTelePow, 8, 170, 110, 24, szMid(), b, cTxt, cBg);
      dtostrf(tele.a, 4, 1, b); strcat(b, " A");
      slotPrint(sTeleGear, 122, 170, 110, 24, szMid(), b, cTxt, cBg);
    }
    slotPrint(sTeleCur, 8, 140, 224, 20, 2,
              wheelCircM > 0 ? "km/h" : "motor rpm", cDim, cBg);
  } else if (telePane == 1) {                // ELEC
    if (tele.has) {
      dtostrf(tele.v, 4, 1, b); strcat(b, "V");
      uint8_t sb = szBig();
      if (strlen(b) > 5 && sb > 4) sb = 4;   // auto-fit: 6+ chars never overflows
      slotPrint(sTeleBig, 8, 48, 150, 42, sb, b, uiVal, cBg);
      dtostrf(tele.a, 4, 1, b); strcat(b, " A");
      slotPrint(sTeleCur, 128, 48, 108, 26, szMid(), b, cTxt, cBg);
      snprintf(b, sizeof(b), "%ldW", (long)(tele.v * tele.a));
      slotPrint(sTelePow, 8, 112, 108, 26, szMid(), b, cTxt, cBg);
      snprintf(b, sizeof(b), "%c", tele.gear);
      slotPrint(sTeleGear, 128, 112, 108, 26, szMid(), b, cTxt, cBg);
    } else {
      slotPrint(sTeleBig, 8, 48, 150, 42, szBig(), "--.-V", cDim, cBg);
      slotPrint(sTeleCur, 128, 48, 108, 26, szMid(), "--", cDim, cBg);
      slotPrint(sTelePow, 8, 112, 108, 26, szMid(), "--", cDim, cBg);
      slotPrint(sTeleGear, 128, 112, 108, 26, szMid(), "-", cDim, cBg);
    }
  } else {                                   // MOTOR
    if (tele.has) {
      snprintf(b, sizeof(b), "%ld", (long)tele.rpm);
      uint8_t sb = szBig();
      if (strlen(b) > 4 && sb > 4) sb = 4;
      slotPrint(sTeleRpm, 8, 48, 150, 42, sb, b, uiVal, cBg);
      snprintf(b, sizeof(b), "%c", tele.gear);
      slotPrint(sTeleGear, 128, 48, 108, 26, szMid(), b, cTxt, cBg);
      snprintf(b, sizeof(b), "%ld/%ldC", (long)tele.tc, (long)tele.tm);
      slotPrint(sTeleTc, 8, 116, 150, 26, szMid(), b, cTxt, cBg);
    } else {
      slotPrint(sTeleRpm, 8, 48, 150, 42, szBig(), "--", cDim, cBg);
      slotPrint(sTeleGear, 128, 48, 108, 26, szMid(), "-", cDim, cBg);
      slotPrint(sTeleTc, 8, 116, 150, 26, szMid(), "--", cDim, cBg);
    }
  }
  if (tele.has) {
    if (tele.fault) snprintf(b, sizeof(b), "F:%04lX %.12s", (unsigned long)tele.fault,
                             CTL_STATUS[tele.status]);
    else            snprintf(b, sizeof(b), "%.16s", CTL_STATUS[tele.status]);
    slotPrint(sTeleStat, 8, 214, 224, 18, 2, b, tele.fault ? cWarn : cDim, cBg);
  } else {
    const char *m = !btLinkOn ? "telemetry off"
                  : !SerialBT.connected() ? "bluetooth: searching bridge"
                                          : "linked — waiting for frames";
    slotPrint(sTeleStat, 8, 214, 224, 18, 2, m, cWarn, cBg);
  }
  uint32_t age = tele.has ? (millis() - tele.atMs) / 1000 : 0;
  snprintf(b, sizeof(b), "bt rx %lu  tx %lu  age %lus",
           (unsigned long)rxCount, (unsigned long)txCount, (unsigned long)age);
  slotPrint(sTeleLink, 8, 236, 224, 16, 1, b, cDim, cBg);
  slotPrint(sToast, 8, 256, 224, 16, 1,
            millis() - toastAtMs < 4000 ? toastTxt : "", toastCol, cBg);
}

void drawKeyless() {
  char b[48], macs[24];
  if (fobMacLen != 6) {
    slotPrint(sKlState, 8, 44, 224, 32, szHero(), "NO FOB", cDim, cBg);
    fobMacStr(macs, sizeof(macs));
    snprintf(b, sizeof(b), "learn: serial 'i' then 'f <mac>'");
    slotPrint(sKlFob, 8, 90, 224, 16, 1, b, cWarn, cBg);
    snprintf(b, sizeof(b), "now: %s", macs);
    slotPrint(sKlInfo, 8, 108, 224, 16, 1, b, cDim, cBg);
    tft.fillRect(8, 190, 224, 60, cBg);
    return;
  }
  bool grace = (millis() - bootMs) < BOOT_GRACE_S * 1000UL;
  snprintf(b, sizeof(b), "%s", klArmed ? "ARMED" : (grace ? "grace" : "DISARMED"));
  slotPrint(sKlState, 8, 44, 224, 32, szHero(), b, klArmed ? cBad : cGood, cBg);
  if (fobPresent()) snprintf(b, sizeof(b), "fob: near  %d dBm", fobRssi);
  else if (fobLastSeenMs) snprintf(b, sizeof(b), "fob: away  %lds",
                                   (long)((millis() - fobLastSeenMs) / 1000));
  else              snprintf(b, sizeof(b), "fob: no signal");
  slotPrint(sKlFob, 8, 90, 224, 18, 2, b, fobPresent() ? cGood : cDim, cBg);
  fobMacStr(macs, sizeof(macs));
  snprintf(b, sizeof(b), "%.23s", macs);
  slotPrint(sKlInfo, 8, 112, 224, 14, 1, b, cDim, cBg);

  bool panic = millis() < panicUntilMs;
  int8_t st = (klArmed ? 1 : 0) | (panic ? 2 : 0);
  if (st != btnCache) {
    btnCache = st;
    tft.fillRect(8, 190, 224, 60, cBg2);
    tft.drawRect(8, 190, 224, 60, panic ? cGood : cBad);
    tft.setTextSize(3);
    tft.setTextColor(panic ? cGood : cBad);
    const char *t = panic ? "STOP" : "PANIC";
    tft.setCursor(8 + (224 - strlen(t) * 18) / 2, 208);
    tft.print(t);
  }
}

void drawSystem() {
  char b[48];
  snprintf(b, sizeof(b), "bt %s %s",
           !btLinkOn ? "OFF" : SerialBT.connected() ? "LINKED" : "search",
           BT_SERVER_NAME);
  slotPrint(sSys[0], 8, 80, 224, 18, 2, b,
            (!btLinkOn || !SerialBT.connected()) ? cDim : cGood, cBg);
  snprintf(b, sizeof(b), "rx %lu tx %lu", (unsigned long)rxCount, (unsigned long)txCount);
  slotPrint(sSys[1], 8, 96, 224, 18, 2, b, cTxt, cBg);
  snprintf(b, sizeof(b), "touch v%d %s%s%s", touchVariant, tSwapXY ? "SW" : "",
           tFlipX ? "FX" : "", tFlipY ? "FY" : "");
  slotPrint(sSys[2], 8, 118, 224, 18, 2, b, cTxt, cBg);
  snprintf(b, sizeof(b), "raw %4d %4d %4d", lastRawX, lastRawY, lastRawZ);
  slotPrint(sSys[3], 8, 140, 224, 18, 2, b, cDim, cBg);
  snprintf(b, sizeof(b), "up %lus  heap %ukB", (unsigned long)((millis() - bootMs) / 1000),
           (unsigned)(ESP.getFreeHeap() / 1024));
  slotPrint(sSys[4], 8, 162, 224, 18, 2, b, cTxt, cBg);
  if (wheelCircM > 0) snprintf(b, sizeof(b), "wheel %.2fm  km/h", wheelCircM);
  else                snprintf(b, sizeof(b), "wheel unset (rpm)");
  slotPrint(sSys[5], 8, 184, 224, 18, 2, b, cDim, cBg);

  // BT release button (lets the phone's VOTOL app take the link)
  int8_t st = btPaused() ? 1 : 0;
  if (st != btnCache) {
    btnCache = st;
    tft.fillRect(8, 212, 224, 44, cBg2);
    tft.drawRect(8, 212, 224, 44, btPaused() ? cGood : uiAcc);
    tft.setTextSize(2); tft.setTextColor(btPaused() ? cGood : uiAcc);
    const char *t = btPaused() ? "BT paused - tap to resume"
                               : "release BT for phone";
    tft.setCursor(8 + (224 - strlen(t) * 12) / 2, 226);
    tft.print(t);
  }
}

void drawCfg() {
  tft.setTextSize(1); tft.setTextColor(cDim);
  tft.setCursor(8, 32); tft.print("TEXT SIZE  (live, auto-saved)");
  const char *sz[3] = {"S", "M", "L"};
  for (uint8_t i = 0; i < 3; i++) {
    uint16_t x = 8 + i * 78;
    bool act = (uiScale == i);
    tft.fillRect(x, 44, 68, 40, act ? uiAcc : cBg2);
    tft.drawRect(x, 44, 68, 40, act ? uiAcc : cDim);
    tft.setTextSize(3); tft.setTextColor(act ? cBg : cTxt);
    tft.setCursor(x + (68 - 18) / 2, 55);
    tft.print(sz[i]);
  }
  tft.setTextSize(1); tft.setTextColor(cDim);
  tft.setCursor(8, 100); tft.print("VALUE COLOR (telemetry numbers)");
  tft.setCursor(8, 156); tft.print("ACCENT COLOR (tabs, highlights)");
  for (uint8_t row = 0; row < 2; row++) {
    uint16_t y = row == 0 ? 112 : 168;
    uint16_t cur = row == 0 ? uiVal : uiAcc;
    for (uint8_t i = 0; i < 6; i++) {
      uint16_t x = 8 + i * 38;
      tft.fillRect(x, y, 32, 28, PALETTE[i]);
      if (PALETTE[i] == cur) tft.drawRect(x - 2, y - 2, 36, 32, cTxt);
    }
  }
  tft.setTextSize(1); tft.setTextColor(cDim);
  tft.setCursor(8, 210); tft.print("long values auto-shrink to fit the page");
  slotPrint(sToast, 8, 232, 224, 16, 1,
            millis() - toastAtMs < 4000 ? toastTxt : "", toastCol, cBg);
}

/* =========================================================== arm/disarm animation */
void animLock(uint16_t col, int shY, int rLeg) {
  // padlock stage. shY = shackle bridge top (66 raised = open, 92 = closed);
  // rLeg = right-leg length: short (ends in the AIR, gap to the body) = open,
  // reaches down into the body = closed — the gap is the unlock cue.
  tft.fillRect(0, 27, W, 250, cBg);
  tft.fillRect(80, shY, 12, 144 - shY, col);    // left leg (always anchored)
  tft.fillRect(80, shY, 80, 12, col);           // bridge
  tft.fillRect(148, shY, 12, rLeg, col);        // right leg (the "mouth")
  tft.fillRoundRect(70, 138, 100, 74, 10, col); // body
  tft.fillCircle(120, 176, 10, cBg);            // keyhole
  tft.fillRect(115, 162, 10, 18, cBg);
}
void playAnim(uint8_t kind) {            // 1 = ARM (close), 2 = DISARM (open)
  Serial.printf("[anim] %s\n", kind == 1 ? "arm" : "disarm");
  uint16_t col = kind == 1 ? cBad : cGood;
  const char *txt = kind == 1 ? "ARMED" : "DISARMED";
  const int shYs[5] = {66, 73, 79, 86, 92};
  const int rls[5]  = {20, 28, 36, 44, 52};
  if (kind == 1) {                       // right leg plugs in, shackle seats: LOCK
    for (uint8_t i = 0; i < 5; i++) { animLock(col, shYs[i], rls[i]); delay(60); }
  } else {                               // right leg lifts clear of the body: UNLOCK
    for (int8_t i = 4; i >= 0; i--) { animLock(col, shYs[i], rls[i]); delay(60); }
    for (int r = 52; r <= 116; r += 16) {       // "fob is back" sonar ping
      tft.drawCircle(120, 150, r, col); delay(40);
    }
  }
  for (uint8_t sz = 2; sz <= 4; sz++) {  // text zoom-in
    tft.fillRect(0, 228, W, 40, cBg);
    tft.setTextColor(col); tft.setTextSize(sz);
    tft.setCursor(120 - (int)strlen(txt) * 3 * sz, 232);
    tft.print(txt);
    delay(80);
  }
  delay(200);                            // brief beat on the zoomed text
  if (kind == 1 && !fobPresent()) {      // armed & fob still gone: the animation
    dispForceUntilMs = 0;                // was the goodbye — dark right after
    setDisplay(false);                   // (clear the hold or it would re-wake)
    Serial.println("[disp] off (armed)");
    return;
  }
  uiInvalidate(); drawChrome();          // restore the page underneath
}

/* =========================================================== wifi window */
void handleWindowState() {
  char s[300];
  snprintf(s, sizeof(s),
    "{\"uptimeS\":%lu,\"phase\":\"wifi-window\",\"bt\":%s,\"rx\":%lu,\"tx\":%lu,"
    "\"disp\":\"%s\",\"page\":%d,\"pane\":%d,\"touchVariant\":%d,\"raw\":[%d,%d,%d]}",
    (unsigned long)((millis() - bootMs) / 1000),
    SerialBT.connected() ? "true" : "false",
    (unsigned long)rxCount, (unsigned long)txCount,
    dispOn ? "on" : "off", (int)page, (int)telePane,
    touchVariant, lastRawX, lastRawY, lastRawZ);
  web.send(200, "application/json", s);
}
void handleReboot() { web.send(200, "application/json", "{\"ok\":true}"); delay(150); ESP.restart(); }

void wifiWindowSetup() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
  if (MDNS.begin("votol-dash")) MDNS.addService("http", "tcp", 80);
#ifdef OTA_PASSWORD
  ArduinoOTA.setHostname("votol-dash");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
#endif
  web.on("/state.json", handleWindowState);
  web.on("/reboot", handleReboot);
  web.begin();
}

void wifiWindowRun() {          // returns when the window is over
  ArduinoOTA.handle();
  web.handleClient();
  if (!dispOn) return;          // headless window: screen still follows the fob
  char b[30];
  snprintf(b, sizeof(b), "setup window %lus",
           (unsigned long)(WIFI_WINDOW_S + 1 - (millis() - bootMs) / 1000));
  slotPrint(sHeader, 0, 0, 190, 26, 2, b, cAcc, cBg2);
  tft.setTextSize(1); tft.setTextColor(cDim);
  tft.setCursor(8, 60);  tft.print("WiFi + OTA open for a short window");
  tft.setCursor(8, 74);  tft.print("after boot; then it turns off and");
  tft.setCursor(8, 88);  tft.print("Bluetooth + touch take over.");
  snprintf(b, sizeof(b), "ip %s", WiFi.status() == WL_CONNECTED
           ? WiFi.localIP().toString().c_str() : "no wifi");
  tft.setCursor(8, 112); tft.print(b);
}

/* =========================================================== serial CLI */
char cliLine[24]; uint8_t cliLen = 0;
void cliProcess(const char *line) {
  if (line[0] == 'f' && (line[1] == ' ' || line[1] == '=')) {
    // 'f AA:BB:CC:DD:EE:FF' or 'f AABBCCDDEEFF' — set fob MAC
    uint8_t mac[6]; int n = 0; const char *p = line + 2;
    while (*p && n < 6) {
      if (*p == ':' || *p == '-') { p++; continue; }
      char hb[3] = {p[0], p[1], 0};
      if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
        Serial.println("[cli] bad mac"); return;
      }
      mac[n++] = (uint8_t)strtoul(hb, nullptr, 16);
      p += 2;
    }
    if (n != 6) { Serial.println("[cli] need 6 bytes"); return; }
    memcpy(fobMac, mac, 6); fobMacLen = 6; fobLastSeenMs = 0; absentSinceMs = 0;
    fobSave();
    char ms[24]; fobMacStr(ms, sizeof(ms));
    Serial.printf("[cli] fob set %s\n", ms);
    toast("fob saved", cGood);
    return;
  }
  if (line[0] == 'c' && (line[1] == ' ' || line[1] == '=')) {
    // 'c 2.05' — tyre circumference in metres (RIDE pane: rpm -> km/h)
    float v = atof(line + 2);
    if (v < 0.5f || v > 5.0f) { Serial.println("[cli] need 0.5-5.0 m"); return; }
    wheelCircM = v; prefs.putFloat("wcirc", v);
    Serial.printf("[cli] wheel %.2f m (km/h = rpm x %.3f)\n", v, v * 0.06f);
    toast("wheel saved", cGood);
    return;
  }
  switch (line[0]) {
    case 'v': touchVariant ^= 1; Serial.printf("[cli] variant=%d\n", touchVariant); break;
    case 's': tSwapXY = !tSwapXY; Serial.printf("[cli] swap=%d\n", tSwapXY); break;
    case 'f': tFlipX = !tFlipX; Serial.printf("[cli] flipx=%d\n", tFlipX); break;
    case 'g': tFlipY = !tFlipY; Serial.printf("[cli] flipy=%d\n", tFlipY); break;
    case 'w':
      prefs.putUChar("tvar", touchVariant); prefs.putBool("tswap", tSwapXY);
      prefs.putBool("tfx", tFlipX); prefs.putBool("tfy", tFlipY);
      Serial.println("[cli] touch setup saved");
      break;
    case 'p': sendShow(); Serial.println("[cli] SHOW sent"); break;
    case 'a': pendingAnim = 1; break;      // test the ARM animation
    case 'A': pendingAnim = 2; break;      // test the DISARM animation
    case 'b': btPauseToggle(); break;
    case 'B':                               // VOTOL link moved to CAN — BT dial on/off
      btLinkOn = !btLinkOn;
      prefs.putBool("btlink", btLinkOn);
      if (!btLinkOn && SerialBT.connected()) SerialBT.disconnect();
      Serial.printf("[cli] bt link %s\n", btLinkOn ? "on" : "off");
      toast(btLinkOn ? "BT link on" : "BT link off", cAcc);
      break;
    case 'd':
      dispForceUntilMs = millis() + 120000UL;
      if (!dispOn) setDisplay(true);
      Serial.println("[cli] display on (2 min override)");
      break;
    case 'i':
      Serial.println("[cli] scanning 2 s — devices print below");
      bleScanDump = true;
      break;
    case 'm':
      Serial.println("[cli] learning strongest ITAG-named device for 4 s");
      bleLearn = true; learnRssi = -128;
      break;
    case 'M':
      fobMacLen = 0; prefs.putUChar("foblen", 0);
      Serial.println("[cli] fob cleared");
      break;
    case 'r': {
      Serial.println("[cli] raw dump 6 s — press the panel");
      uint32_t t0 = millis();
      while (millis() - t0 < 6000) {
        RawTouch rr = readFilm();
        Serial.printf("v%d x=%4d y=%4d z=%4d\n", touchVariant, rr.x, rr.y, rr.z);
        delay(100);
      }
      break;
    }
    default: break;
  }
}
void serialCli() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cliLen) { cliLine[cliLen] = 0; cliProcess(cliLine); cliLen = 0; }
    } else if (cliLen < sizeof(cliLine) - 1) {
      // single-letter commands fire immediately (no newline needed);
      // 'f <mac>' and 'c <metres>' wait for the newline
      if (c != ' ' && cliLen == 0 && c != 'f' && c != 'c') {
        char one[2] = {c, 0};
        cliProcess(one);
      } else cliLine[cliLen++] = c;
    }
  }
}

/* =========================================================== setup/loop */
void setup() {
  Serial.begin(115200);
  bootMs = millis();
  delay(120);

  cBg  = tft.color565(10, 16, 26);
  cBg2 = tft.color565(18, 26, 40);
  cTxt = tft.color565(222, 230, 240);
  cDim = tft.color565(110, 124, 140);
  cAcc = tft.color565(0, 200, 255);
  cGood= tft.color565(0, 255, 120);
  cWarn= tft.color565(255, 200, 0);
  cBad = tft.color565(255, 60, 50);

  uint16_t id = tft.readID();
  if (id == 0x0000 || id == 0xD3D3) id = 0x9341;
  tft.begin(id);
  tft.setRotation(0);
  Serial.printf("[tft-dash] LCD id 0x%04X\n", id);

  prefs.begin("tftdash", false);
  touchVariant = prefs.getUChar("tvar", 0);
  tSwapXY = prefs.getBool("tswap", false);
  tFlipX = prefs.getBool("tfx", false);
  tFlipY = prefs.getBool("tfy", false);
  fobLoad();
  uiLoad();
  buzzInit();
  {
    char ms[24]; fobMacStr(ms, sizeof(ms));
    Serial.printf("[tft-dash] fob: %s\n", ms);
  }

  wheelCircM = prefs.getFloat("wcirc", 0);
  btLinkOn = prefs.getBool("btlink", true);

  // boot follows the fob: dark from the first second when registered+away
  dispOn = (fobMacLen != 6);
  if (dispOn) { tft.fillScreen(cBg); drawChrome(); }
  else         tft.fillScreen(0x0000);
  Serial.printf("[tft-dash] boot display %s\n",
                dispOn ? "on (no fob registered)" : "dark (waiting for fob)");

  wifiWindowSetup();
  SerialBT.begin("votol-dash", true);   // true = master/SPP-client mode
  SerialBT.setPin(BT_SERVER_PIN);
  xTaskCreate(bleTask, "ble", 6144, nullptr, 1, nullptr);
  Serial.printf("[tft-dash] wifi window %ds, then BT->%s + iTag BLE\n",
                WIFI_WINDOW_S, BT_SERVER_NAME);
}

void loop() {
  serialCli();

  if (wifiPhase) {
    wifiWindowRun();
    displayTick();                 // fob arriving mid-window still wakes it
    if (millis() - bootMs > WIFI_WINDOW_S * 1000UL) {
      WiFi.mode(WIFI_OFF);
      wifiPhase = false;
      lastCycleMs = millis();
      Serial.println("[tft-dash] wifi off — touch + BT mode");
      if (dispOn) drawChrome();
    }
    delay(20);
    return;
  }

  // BT link maintenance — connect runs in its own task; the by-name
  // inquiry blocks 10-30 s and must not freeze touch/UI. Failed attempts
  // (bridge unpowered) hog the radio and starve the BLE scan, so they
  // back off to 45 s after 3 straight misses.
  if (!btLinkOn || (!btPaused() && !SerialBT.connected() && !btConnecting &&
      millis() - lastBtTryMs > (btFailCount >= 3 ? 45000UL : 15000UL))) {
    if (btLinkOn) {
      lastBtTryMs = millis(); btConnecting = true;
      xTaskCreate(btConnectTask, "btc", 4096, nullptr, 1, nullptr);
    }
  }
  parseRx();
  if (SerialBT.connected() && millis() - lastPollMs > 1000) {
    lastPollMs = millis();
    sendShow(); txCount++;
  }

  // learn-mode completion: adopt the strongest ITAG seen
  if (bleLearn && learnRssi > -128 && millis() - learnAtMs > 2000) {
    bleLearn = false;
    memcpy(fobMac, learnMac, 6); fobMacLen = 6;
    fobLastSeenMs = 0; absentSinceMs = 0;
    fobSave();
    char ms[24]; fobMacStr(ms, sizeof(ms));
    Serial.printf("[ble] fob learned: %s (rssi %d)\n", ms, learnRssi);
    toast("fob learned", cGood);
  }

  klTick();
  buzzTick();
  ridingTick();
  displayTick();

  if (dispOn) {
    if (pendingAnim) {
      uint8_t a = pendingAnim; pendingAnim = 0;
      playAnim(a);
      if (!dispOn) return;   // ARM animation put us to dark — don't repaint
    }
    handleTouch();
    if (page == PG_TELE) {         // riding locks RIDE; idle rotates ELEC/MOTOR
      if (riding && telePane != 0) { telePane = 0; uiInvalidate(); drawChrome(); }
      else if (!riding && millis() - lastCycleMs > (telePane == 0 ? 10000 : 8000)) {
        lastCycleMs = millis();
        telePane = telePane == 2 ? 1 : (telePane + 1) % 3;
        uiInvalidate(); drawChrome();
        Serial.printf("[tele] pane %s\n", PANE_NAMES[telePane]);
      }
    }
    drawHeader();
    switch (page) {
      case PG_TELE:    drawTelemetry(); break;
      case PG_KEYLESS: drawKeyless();   break;
      case PG_SETUP:   setupTab ? drawCfg() : drawSystem(); break;
  }
  }
  delay(10);
}
