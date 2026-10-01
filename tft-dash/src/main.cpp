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
 * 'f <mac>' add iTag fob, 'f N:<name>' add phone fob (by advertised
 * name — Android rotates its MAC), 'L' list fobs, 'x <n>' remove fob,
 * 'M' clear all, 'c <metres>' wheel circumference (RIDE km/h),
 * 'a'/'A' preview the ARM/DISARM animation, 'B' BT link on/off
 * (off while the bridge talks CAN to the VOTOL instead of BT SPP),
 * 'k'/'K' show/regenerate the app pair key, 'P <pin>' app PIN.
 *
 * Phone-app link (BLE GATT server, service c9d01402-…): write
 * "ARM:SECRET" / "DISARM:SECRET" / "PANIC:SECRET" / "STAT:SECRET".
 * SECRET = 128-bit pair key (QR shown in SYS->SET) or the PIN.
 * Any BLE phone works (Android AND iPhone — this is a connection, not
 * the Android-only fob advertising). 3 bad keys = 15 s lockout.
 * Phone/PIN DISARM is sticky: display on + no auto re-arm until the
 * app sends ARM (a real fob sighting also restores normal mode).
 *
 * ARMED standby: the backlight can't be turned off on this shield, so
 * armed idle shows a STILL image (closed lock + battery voltage) — no
 * animation. Double-tap it -> PIN keypad -> OK disarms into the app.
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
/* BLE GATT UUIDs for the phone-app command link ("CMD:SECRET" writes) */
#define BLE_SVC_UUID  "c9d01402-a1b2-4c3d-8e9f-aabbccddeeff"
#define BLE_CMD_UUID  "c9d01403-a1b2-4c3d-8e9f-aabbccddeeff"
#define BLE_STAT_UUID "c9d01404-a1b2-4c3d-8e9f-aabbccddeeff"
void btConnectTask(void *) {
  Serial.println("[bt] connecting " BT_SERVER_NAME " ...");
  bool ok = SerialBT.connect(BT_SERVER_NAME);
  Serial.printf("[bt] connect %s\n", ok ? "ok" : "failed");
  if (ok) btFailCount = 0; else btFailCount++;
  btConnecting = false;
  vTaskDelete(nullptr);
}

/* ---- keyless fobs (BLE) — up to 4: iTag by MAC, phone by NAME ----
 * Phones rotate their BLE MAC (Android ~15 min / BT restart), so phone
 * fobs match by advertised NAME ("N:<name>" entries) — the phone must be
 * advertising (Android advertiser app; iOS can't in background). Presence
 * = ANY entry heard within FOB_TTL_S. Buzzer on IO5 = the reserved
 * "alarm out 1" pin (active-buzzer friendly square wave). */
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "vendor/qrcodegen.h"
#define FOB_TTL_S       12     // absent if silent this long
#define ARM_AFTER_S     8      // sustained absence before ARM chirp
#define BOOT_GRACE_S    45
#define PANIC_S         30
#define BUZZ_PIN        5
#define BUZZ_CH         4

struct Fob { uint8_t mac[6]; char name[14]; };   // name[0]==0 => MAC entry
Fob fobs[4];
uint8_t fobCount = 0;
char fobLastLabel[20] = "";              // entry heard last (LOCK page)
volatile int fobRssi = -128;
volatile uint32_t fobLastSeenMs = 0;
volatile bool bleScanDump = false;      // CLI 'i': print next scan's devices
volatile bool bleLearn = false;         // CLI 'm': adopt strongest ITAG-named device
uint8_t learnMac[6]; int learnRssi = -128;
uint32_t learnAtMs = 0;

/* ---- phone-app command link (BLE GATT) ----
 * The pod also runs a GATT server: an app connects, writes
 * "CMD:SECRET" (ARM/DISARM/PANIC/STAT) and reads status. SECRET =
 * the 128-bit pair key (QR in SYS->SET) or the optional short PIN.
 * v1 caveat: no BLE bonding — the secret rides the (unencrypted) link,
 * guarded by a 3-strike 15 s lockout. */
uint8_t pairKey[16];
char pairPin[13] = "";                 // optional weaker manual secret
BLEServer *bleServer = nullptr;
BLECharacteristic *statChr = nullptr;
volatile bool bleConnected = false;
volatile bool bleCmdPending = false;
char bleCmdBuf[80], bleReply[40] = "";
void pairKeyGen() {
  for (int i = 0; i < 16; i++) pairKey[i] = (uint8_t)esp_random();
  prefs.putBytes("pairkey", pairKey, 16);
}
void pairKeyLoad() {
  if (prefs.getBytesLength("pairkey") == 16) prefs.getBytes("pairkey", pairKey, 16);
  else pairKeyGen();
}
void pairKeyHex(char *out) {           // 32 hex chars
  for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02X", pairKey[i]);
}
uint8_t authFails = 0; uint32_t authLockUntilMs = 0;
bool authOk(const char *sec) {
  if (millis() < authLockUntilMs) return false;
  bool ok = false;
  if (strlen(sec) == 32) {             // pair key, case-insensitive
    char kh[33]; pairKeyHex(kh);
    ok = true;
    for (int i = 0; i < 32; i++)
      if (toupper((unsigned char)sec[i]) != kh[i]) { ok = false; break; }
  }
  if (!ok && pairPin[0] && !strcmp(sec, pairPin)) ok = true;
  if (ok) authFails = 0;
  else if (++authFails >= 3) {
    authLockUntilMs = millis() + 15000; authFails = 0;
    Serial.println("[ble] auth locked 15 s (3 bad keys)");
  }
  return ok;
}
bool fobPresent();                     // fwd (defined with display state)
extern bool klArmed;                   // fwd (keyless state machine)
class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override { bleConnected = true; Serial.println("[ble] app connected"); }
  void onDisconnect(BLEServer *s) override {
    bleConnected = false; Serial.println("[ble] app gone");
    s->getAdvertising()->start();
  }
};
class CmdCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    std::string v = c->getValue();
    if (v.size() > 70) return;
    memcpy(bleCmdBuf, v.data(), v.size());
    bleCmdBuf[v.size()] = 0;
    bleCmdPending = true;              // loop() executes it (chirp/draw safe there)
  }
};
class StatCb : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *c) override {
    char st[32];
    snprintf(st, sizeof(st), "%s %s", klArmed ? "ARMED" : "DISARMED",
             fobPresent() ? "FON" : "FOFF");
    c->setValue((uint8_t *)st, strlen(st));
  }
};

class FobCb : public BLEAdvertisedDeviceCallbacks {  void onResult(BLEAdvertisedDevice dev) override {
    if (bleScanDump) {
      Serial.printf("[ble] %s  rssi %d  name \"%s\"\n",
                    dev.getAddress().toString().c_str(), dev.getRSSI(),
                    dev.haveName() ? dev.getName().c_str() : "");
    }
    std::string s = dev.getAddress().toString();   // "aa:bb:cc:dd:ee:ff"
    uint8_t mac[6];
    for (int i = 0; i < 6; i++)
      mac[i] = (uint8_t)strtoul(s.substr(i * 3, 2).c_str(), nullptr, 16);
    for (uint8_t i = 0; i < fobCount; i++) {
      bool hit = false;
      if (fobs[i].name[0] == 0) {
        hit = (memcmp(mac, fobs[i].mac, 6) == 0);
      } else if (dev.haveName()) {       // name entries: case-insensitive
        std::string n = dev.getName();
        for (auto &ch : n) ch = tolower((unsigned char)ch);
        hit = (n.find(fobs[i].name) != std::string::npos);
      }
      if (hit) {
        fobRssi = dev.getRSSI(); fobLastSeenMs = millis();
        if (fobs[i].name[0])
          snprintf(fobLastLabel, sizeof(fobLastLabel), "N:%s", fobs[i].name);
        else
          snprintf(fobLastLabel, sizeof(fobLastLabel),
                   "%02X:%02X:%02X:%02X:%02X:%02X",
                   fobs[i].mac[0], fobs[i].mac[1], fobs[i].mac[2],
                   fobs[i].mac[3], fobs[i].mac[4], fobs[i].mac[5]);
      }
    }
    if (bleLearn && dev.haveName()) {
      std::string n = dev.getName();
      for (auto &ch : n) ch = tolower((unsigned char)ch);
      if (n.find("itag") != std::string::npos && dev.getRSSI() > learnRssi) {
        memcpy(learnMac, mac, 6);
        learnRssi = dev.getRSSI(); learnAtMs = millis();
      }
    }
  }
};
FobCb fobCb;
BLEScan *bleScan = nullptr;

void bleTask(void *) {
  BLEDevice::init("votol-dash");
  // GATT server: phone app writes "CMD:SECRET", reads status
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new SrvCb());
  BLEService *svc = bleServer->createService(BLE_SVC_UUID);
  BLECharacteristic *cmd = svc->createCharacteristic(
      BLE_CMD_UUID, BLECharacteristic::PROPERTY_WRITE);
  cmd->setCallbacks(new CmdCb());
  statChr = svc->createCharacteristic(
      BLE_STAT_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  statChr->addDescriptor(new BLE2902());
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setScanResponse(true);
  bleServer->startAdvertising();
  Serial.println("[ble] GATT server up (app link)");
  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(&fobCb, false);
  bleScan->setActiveScan(true);         // active: fetch names (iTag identifies itself)
  for (;;) {
    BLEScanResults r = bleScan->start(1.5, false);
    bleScan->clearResults();
    vTaskDelay(pdMS_TO_TICKS(100));   // tight duty cycle — fast fob pickup
  }
}

void macStr(char *out, const uint8_t *m) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
}
bool fobAddMac(const uint8_t *mac) {
  if (fobCount >= 4) return false;
  for (uint8_t i = 0; i < fobCount; i++)
    if (fobs[i].name[0] == 0 && memcmp(fobs[i].mac, mac, 6) == 0) return false;
  Fob &f = fobs[fobCount++];
  memcpy(f.mac, mac, 6); f.name[0] = 0;
  return true;
}
bool fobAddName(const char *nm) {
  if (fobCount >= 4) return false;
  char clean[14]; uint8_t n = 0;
  for (; *nm && n < 13; nm++)
    if (isalnum((unsigned char)*nm) || *nm == ' ' || *nm == '-' || *nm == '_')
      clean[n++] = tolower((unsigned char)*nm);
  clean[n] = 0;
  if (n == 0) return false;
  for (uint8_t i = 0; i < fobCount; i++)
    if (fobs[i].name[0] && strcmp(fobs[i].name, clean) == 0) return false;
  strcpy(fobs[fobCount++].name, clean);
  return true;
}
void fobLabel(char *out, size_t n, uint8_t i) {
  if (fobs[i].name[0]) snprintf(out, n, "N:%.12s", fobs[i].name);
  else                 macStr(out, fobs[i].mac);
}
void fobSave() {                    // NVS "fobs": comma-joined labels
  char all[90] = "";
  for (uint8_t i = 0; i < fobCount; i++) {
    if (i) strcat(all, ",");
    char lb[20]; fobLabel(lb, sizeof(lb), i);
    strcat(all, lb);
  }
  prefs.putString("fobs", all);
}
void fobLoad() {
  fobCount = 0;
  String s = prefs.getString("fobs", "");
  int from = 0;
  while (s.length() && fobCount < 4) {
    int comma = s.indexOf(',', from);
    String e = comma < 0 ? s.substring(from) : s.substring(from, comma);
    e.trim();
    if (e.length() >= 2 && (e[0] == 'N' || e[0] == 'n') && e[1] == ':') {
      fobAddName(e.substring(2).c_str());
    } else if (e.length() == 17) {
      uint8_t mac[6]; int n = 0; const char *p = e.c_str();
      while (*p && n < 6) {
        if (*p == ':' || *p == '-') { p++; continue; }
        char hb[3] = {p[0], p[1], 0};
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) break;
        mac[n++] = (uint8_t)strtoul(hb, nullptr, 16);
        p += 2;
      }
      if (n == 6) fobAddMac(mac);
    }
    if (comma < 0) break;
    from = comma + 1;
  }
  if (fobCount == 0 && prefs.getBytesLength("fob") == 6) {  // v3 single-fob NVS
    uint8_t mac[6];
    prefs.getBytes("fob", mac, 6);
    fobAddMac(mac);
    fobSave();
    Serial.println("[fob] migrated single-fob entry");
  }
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
bool klArmed = false;               // declared early: display logic reads them
uint32_t absentSinceMs = 0;
bool manualDisarmed = false;        // phone/PIN disarm: stays disarmed until phone ARM
bool armAsk = false;                // YES/NO arm confirmation screen open
uint32_t armAskUntilMs = 0;
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
Slot sKlState, sKlFob, sKlInfo, sKlEnt[4];
int8_t btnCache = -1;
uint32_t qrStamp = 0;               // SET page: QR redraw cache key

bool fobPresent() {
  // lastSeen==0 means "never seen since boot" — without the guard the pod
  // thinks the fob is present for the first FOB_TTL_S after every power-on
  return fobCount != 0 && fobLastSeenMs != 0 &&
         (millis() - fobLastSeenMs) < FOB_TTL_S * 1000UL;
}

/* ---- display power: follows the registered iTag ----
 * unregistered fob -> always on (you must be able to see what you do);
 * registered: on while the fob is near (or during the boot wifi window /
 * a serial 'd' override); off = panel DISPOFF (GRAM keeps the frame, so
 * waking redraws only what changed). */
bool dispOn = true;
bool pinScreen = false;              // PIN-entry gate over the standby screen
char pinEntry[13]; uint8_t pinLen = 0;
uint32_t pinAtMs = 0;
uint32_t dispForceUntilMs = 0;
uint16_t dotCache = 0xFFFF;
int8_t btOffIconShown = -1;          // crossed-BT badge state (top right)
void drawStandby();                  // fwd
void drawChrome();                                   // fwd
void uiInvalidate() {
  Slot *all[] = {&sTeleBig, &sTeleCur, &sTelePow, &sTeleRpm, &sTeleGear, &sTeleTc,
                 &sTeleStat, &sTeleLink, &sKlState, &sKlFob, &sKlInfo,
                 sKlEnt, sKlEnt + 1, sKlEnt + 2, sKlEnt + 3,
                 &sToast, &sHeader, sSys, sSys + 1, sSys + 2, sSys + 3,
                 sSys + 4, sSys + 5, sSys + 6, sSys + 7};
  for (Slot *s : all) { s->last[0] = 1; s->last[1] = 0; s->lastCol = 0xFFFF; }
}
void setDisplay(bool on) {
  if (on == dispOn) return;
  dispOn = on;
  if (on) {
    pinScreen = false;
    uiInvalidate();
    dotCache = 0xFFFF;                   // force header dot repaint
    btOffIconShown = -1;                 // and the BT-off badge
    drawChrome();
  } else {
    armAsk = false;                        // a dialog can't outlive a sleep
    if (pinScreen) {
      // PIN gate/entry stays as-is (it's its own mode)
    } else if (klArmed) {
      // backlight can't be killed on this shield — armed idle shows a
      // STILL image (lock + battery) instead of black; double-tap = PIN
      drawStandby();
      Serial.println("[disp] standby (armed)");
    } else {
      tft.fillRect(0, 0, W, H, 0x0000);    // not armed: plain dark
      Serial.println("[disp] off");
    }
  }
}
void displayTick() {
  // screen follows the fob from the very first boot second — the wifi/OTA
  // window runs headless unless the fob (or an override) is present
  bool want = (fobCount == 0) ||
              (millis() < dispForceUntilMs) || fobPresent() || manualDisarmed;
  if (want != dispOn) setDisplay(want);
}

/* ---- keyless state machine ---- */
void drawStandby();                  // fwd (defined after playAnim)
void klTick() {
  if (fobCount == 0) return;                  // no fob learned yet
  uint32_t now = millis();
  if (now - bootMs < BOOT_GRACE_S * 1000UL) { absentSinceMs = now; return; }
  if (fobPresent()) {
    absentSinceMs = now;
    manualDisarmed = false;                   // owner (fob) is here — normal mode
    if (klArmed) { klArmed = false; chirp(1); toast("DISARMED - fob back", cGood); pendingAnim = 2; }
  } else {
    if (absentSinceMs == 0) absentSinceMs = now;
    if (manualDisarmed) absentSinceMs = now;  // manual disarm: never auto-arm
    // arm counted from the LAST SIGHTING: fires the moment the 12s fob
    // TTL lapses — BEFORE the display would sleep — so the sequence is
    // animation FIRST, screen off ~6s later (same 12s of silence needed
    // as before; no robustness change)
    if (!klArmed && !manualDisarmed && (now - fobLastSeenMs) > ARM_AFTER_S * 1000UL) {
      klArmed = true; chirp(2);
      if (fobLastSeenMs >= bootMs) {       // fob was around this boot: show it
        toast("ARMED - fob away", cWarn); pendingAnim = 1;
        dispForceUntilMs = now + 6000;     // keep the screen lit through it
      } else {                             // powered on with fob already off:
        Serial.println("[kl] armed silently — fob off since boot");
        if (!dispOn) drawStandby();        // silent = no anim, straight to still
      }                                    // image (lock + battery)
      absentSinceMs = now;
    }
  }
}
void panicToggle() {
  if (millis() < panicUntilMs) { panicUntilMs = 0; toast("siren stopped", cGood); }
  else if (klArmed) { panicUntilMs = millis() + PANIC_S * 1000UL; toast("SIREN 30s", cBad); }
  else toast("arm first (fob away)", cWarn);
}
/* device ARM — YES/NO confirmation screen (a stray tap can never arm) */
bool mapTouch(int &px, int &py);                     // fwd
void drawArmAsk() {
  tft.fillRect(0, 0, W, H, cBg);
  tft.setTextSize(2); tft.setTextColor(cTxt);
  const char *t = "ARM THE ALARM?";
  tft.setCursor((W - (int)strlen(t) * 12) / 2, 92); tft.print(t);
  tft.setTextSize(1); tft.setTextColor(cDim);
  const char *s = "screen will rest at the lock screen";
  tft.setCursor((W - (int)strlen(s) * 6) / 2, 120); tft.print(s);
  tft.fillRect(8, 168, 224, 58, cBg2); tft.drawRect(8, 168, 224, 58, cGood);
  tft.setTextSize(3); tft.setTextColor(cGood);
  const char *n = "NO";
  tft.setCursor((W - (int)strlen(n) * 18) / 2, 186); tft.print(n);
  tft.fillRect(8, 242, 224, 58, cBg2); tft.drawRect(8, 242, 224, 58, cBad);
  tft.setTextColor(cBad);
  const char *y = "YES, ARM";
  tft.setCursor((W - (int)strlen(y) * 18) / 2, 260); tft.print(y);
}
void doDeviceArm() {
  if (fobCount == 0) { toast("no fob registered", cDim); uiInvalidate(); drawChrome(); return; }
  if (fobPresent())  { toast("fob near - it would disarm", cDim); uiInvalidate(); drawChrome(); return; }
  klArmed = true; manualDisarmed = false; absentSinceMs = millis();
  chirp(2); pendingAnim = 1;
  Serial.println("[kl] armed from device (YES confirmed)");
}
void armAskTouch() {
  int px, py;
  if (!mapTouch(px, py)) return;
  static uint32_t lastTapMs = 0;
  if (millis() - lastTapMs < 180) return;
  lastTapMs = millis();
  if (py >= 160 && py <= 232) {          // NO
    armAsk = false; uiInvalidate(); drawChrome();
    Serial.println("[kl] arm cancelled");
  } else if (py >= 234) {                // YES
    armAsk = false;
    doDeviceArm();
  }
}
void armAskOpen() {
  if (fobCount == 0) { toast("no fob registered", cDim); return; }
  if (fobPresent())  { toast("fob near - it would disarm", cDim); return; }
  armAsk = true; armAskUntilMs = millis() + 10000;
  drawArmAsk();
  Serial.println("[kl] arm confirm asked");
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
bool mapTouch(int &px, int &py);                     // fwd (standby/PIN/arm dialogs)
void drawPinScreen();                                // fwd (CONFIG change-PIN)
extern uint8_t pinEntryMode;                         // defined with the PIN block
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
    uint8_t nt = px < 84 ? 0 : px < 160 ? 1 : 2;
    if (nt != setupTab) { setupTab = nt; uiInvalidate(); drawChrome(); }
  } else if (page == PG_SETUP && setupTab == 1) {          // CONFIG content
    // zones match the DRAWN widgets (size y44-84, value y112-140,
    // accent y168-196, CHANGE PIN y216-250) with finger margins
    if (py >= 40 && py <= 92 && px >= 8) {                 // size S/M/L
      uiScale = px < 86 ? 0 : px < 154 ? 1 : 2;
      uiSave(); drawChrome(); toast("size saved", uiAcc);
    } else if (py >= 106 && py <= 146 && px >= 8 && px <= 230) {   // value color
      uiVal = PALETTE[(px - 8) / 38];
      uiSave(); drawChrome(); toast("value color saved", uiAcc);
    } else if (py >= 162 && py <= 202 && px >= 8 && px <= 230) {   // accent color
      uiAcc = PALETTE[(px - 8) / 38];
      uiSave(); drawChrome(); toast("accent color saved", uiAcc);
    } else if (py >= 210 && py <= 256 && px >= 8 && px <= 232) {   // change PIN
      pinEntryMode = 1;
      drawPinScreen();
      Serial.println("[pin] PIN change started");
    }
  } else if (page == PG_SETUP && setupTab == 0 &&
             py >= 212 && py <= 256 && px >= 8 && px <= 232) {
    btPauseToggle();                        // release BT for the phone app
  } else if (page == PG_KEYLESS && py >= 190 && py <= 250 && px >= 8 && px <= 232) {
    if (klArmed) panicToggle();               // siren on/off
    else         armAskOpen();                // YES/NO confirmation screen
  } else if (page == PG_TELE && py >= 30) {
    telePane = (telePane + 1) % 3;          // tap content: RIDE -> ELEC -> MOTOR
    lastCycleMs = millis();
    uiInvalidate(); drawChrome();
  } else if (klArmed) {
    panicUntilMs = 0;                       // any tap elsewhere silences the wail
  }
}

/* ---- touch on the standby/PIN screens ----
 * (mapping duplicated from handleTouch — these run when the app UI doesn't) */
void drawPinScreen();                // fwd (defined after playAnim)
void pinOk();
void pinDrawEntry();
bool mapTouch(int &px, int &py) {
  static uint32_t lastSample = 0;
  if (millis() - lastSample < 30) return false;
  lastSample = millis();
  int z = filmZ();
  if (z < 300 || z > 3800) return false;
  RawTouch r = readFilm();
  if (!r.valid) return false;
  px = map(r.x, TR_MIN, TR_MAX, 0, W);
  py = map(r.y, TR_MIN, TR_MAX, 0, H);
  if (tSwapXY) { int t = px; px = py * W / H; py = t * H / W; }
  if (tFlipX) px = W - 1 - px;
  if (tFlipY) py = H - 1 - py;
  px = constrain(px, 0, W - 1); py = constrain(py, 0, H - 1);
  return true;
}
void standbyTouch() {                 // double-tap anywhere -> PIN gate
  static uint32_t lastTapMs = 0, lastAnyMs = 0;
  static uint8_t taps = 0;
  int px, py;
  if (!mapTouch(px, py)) return;
  if (millis() - lastAnyMs < 180) return;
  lastAnyMs = millis();
  taps = (millis() - lastTapMs < 500) ? taps + 1 : 1;
  lastTapMs = millis();
  Serial.printf("[tap] standby (%d)\n", taps);
  if (taps >= 2) {
    taps = 0;
    Serial.println("[pin] gate open");
    drawPinScreen();
  }
}
void pinTouch() {
  int px, py;
  if (!mapTouch(px, py)) return;
  static uint32_t lastTapMs = 0;
  if (millis() - lastTapMs < 180) return;
  lastTapMs = millis(); pinAtMs = millis();
  if (py < 78) return;                   // full 60px bands = forgiving hits
  uint8_t col = px / 80;
  uint8_t row = (py - 78) / 60;
  if (col > 2 || row > 3) return;
  uint8_t i = row * 3 + col;             // 0..8 digits, 9=C, 10=0, 11=OK
  if (i <= 8 && pinLen < 12) pinEntry[pinLen++] = '1' + i;
  else if (i == 10 && pinLen < 12) pinEntry[pinLen++] = '0';
  else if (i == 9) pinLen = 0;
  else if (i == 11) { pinOk(); return; }
  pinDrawEntry();
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

void drawSubTabs() {                // SYS page: STATUS / CONFIG / SET
  const char *names[3] = {"STAT", "CFG", "SET"};
  for (uint8_t i = 0; i < 3; i++) {
    uint16_t x = 8 + i * 76;
    bool act = (setupTab == i);
    tft.fillRect(x, 28, 72, 40, act ? uiAcc : cBg2);
    tft.drawRect(x, 28, 72, 40, cDim);
    tft.setTextSize(2);
    tft.setTextColor(act ? cBg : cTxt);
    uint16_t tw = strlen(names[i]) * 12;
    tft.setCursor(x + (72 - tw) / 2, 40);
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
  qrStamp = 0;                       // SET page QR must repaint after chrome
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
  char b[48];
  if (fobCount == 0) {
    slotPrint(sKlState, 8, 44, 224, 32, szHero(), "NO FOB", cDim, cBg);
    slotPrint(sKlFob, 8, 90, 224, 16, 1, "add fobs over USB serial:", cWarn, cBg);
    slotPrint(sKlInfo, 8, 108, 224, 16, 1, "'m' iTag / 'f <mac>' / 'f N:<name>' phone", cWarn, cBg);
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
  snprintf(b, sizeof(b), "heard: %.19s", fobLastLabel);
  slotPrint(sKlInfo, 8, 112, 224, 14, 1, b, cDim, cBg);
  for (uint8_t i = 0; i < 4; i++) {           // registered fob list
    char lb[22] = "";
    if (i < fobCount) {
      lb[0] = '1' + i; lb[1] = ' ';
      fobLabel(lb + 2, sizeof(lb) - 2, i);
    }
    slotPrint(sKlEnt[i], 8, 130 + i * 14, 224, 14, 1, lb, cDim, cBg);
  }

  bool panic = millis() < panicUntilMs;
  int8_t st = (klArmed ? 1 : 0) | (panic ? 2 : 0);
  if (st != btnCache) {
    btnCache = st;
    tft.fillRect(8, 190, 224, 60, cBg2);
    tft.drawRect(8, 190, 224, 60, panic ? cGood : klArmed ? cBad : cDim);
    tft.setTextSize(3);
    tft.setTextColor(panic ? cGood : klArmed ? cBad : cTxt);
    const char *t = panic ? "STOP" : klArmed ? "PANIC" : "ARM";
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
  tft.setCursor(8, 206); tft.print("SECURITY");
  tft.fillRect(8, 216, 224, 34, cBg2);
  tft.drawRect(8, 216, 224, 34, cDim);
  tft.setTextSize(2); tft.setTextColor(cTxt);
  tft.setCursor(8 + (224 - 10 * 12) / 2, 225);
  tft.print("CHANGE PIN");
  slotPrint(sToast, 8, 256, 224, 16, 1,
            millis() - toastAtMs < 4000 ? toastTxt : "", toastCol, cBg);
}

/* ---- phone-app command execution (runs in loop context) ---- */
void bleExec(const char *cmd, const char *sec) {
  if (!authOk(sec)) {
    snprintf(bleReply, sizeof(bleReply), "ERR KEY");
    Serial.printf("[ble] cmd %s: BAD KEY\n", cmd);
    return;
  }
  if (!strcmp(cmd, "ARM")) {
    if (fobPresent()) { snprintf(bleReply, sizeof(bleReply), "ERR FOB NEAR"); return; }
    klArmed = true; manualDisarmed = false; chirp(2);
    toast("ARMED - app", cWarn);
    if (dispOn) pendingAnim = 1;
    snprintf(bleReply, sizeof(bleReply), "OK ARMED");
  } else if (!strcmp(cmd, "DISARM")) {
    // sticky: stays disarmed + display on until ARM is sent (or fob seen)
    klArmed = false; manualDisarmed = true; absentSinceMs = millis(); chirp(1);
    toast("DISARMED - app", cGood);
    if (dispOn) pendingAnim = 2;
    snprintf(bleReply, sizeof(bleReply), "OK DISARM");
  } else if (!strcmp(cmd, "PANIC")) {
    panicUntilMs = millis() + PANIC_S * 1000UL;
    snprintf(bleReply, sizeof(bleReply), "OK PANIC");
  } else if (!strcmp(cmd, "STAT")) {
    snprintf(bleReply, sizeof(bleReply), "%s %s", klArmed ? "ARMED" : "DISARMED",
             fobPresent() ? "FON" : "FOFF");
  } else {
    snprintf(bleReply, sizeof(bleReply), "ERR CMD");
    return;
  }
  Serial.printf("[ble] cmd %s -> %s\n", cmd, bleReply);
}
void bleProcessPending() {
  if (!bleCmdPending) return;
  char line[80];
  strncpy(line, bleCmdBuf, sizeof(line) - 1); line[sizeof(line) - 1] = 0;
  bleCmdPending = false;
  char *sep = strpbrk(line, ":|");
  if (sep) *sep = 0;
  for (char *p = line; *p; p++) *p = toupper((unsigned char)*p);
  bleExec(line, sep ? sep + 1 : "");
  if (bleConnected && statChr) {
    statChr->setValue((uint8_t *)bleReply, strlen(bleReply));
    statChr->notify();
  }
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
    dispForceUntilMs = 0;                // was the hello — now rest on the
    setDisplay(false);                   // still standby image (lock + battery)
    return;
  }
  uiInvalidate(); drawChrome();          // restore the page underneath
}

/* ---- armed standby screen: STILL image, FULL screen ----
 * The backlight can't be turned off on this shield, so armed idle shows
 * a static lock + battery instead of black — and it takes the whole
 * display (no header, no tab menu). Double-tap wakes the PIN gate. */
void drawStandby() {
  pinScreen = false;
  tft.fillRect(0, 0, W, H, cBg);
  const int oy = 4;                    // lock pose (closed), vertically centered
  tft.fillRect(80, 92 + oy, 12, 52, cBad);             // left leg
  tft.fillRect(148, 92 + oy, 12, 52, cBad);            // right leg
  tft.fillRect(80, 92 + oy, 80, 12, cBad);             // bridge
  tft.fillRoundRect(70, 138 + oy, 100, 74, 10, cBad); // body
  tft.fillCircle(120, 176 + oy, 10, cBg);             // keyhole
  tft.fillRect(115, 162 + oy, 10, 18, cBg);
  tft.setTextColor(cBad); tft.setTextSize(4);
  tft.setCursor(120 - 5 * 24, 236);                   // "ARMED"
  tft.print("ARMED");
  tft.setTextSize(2); tft.setTextColor(cTxt);
  char b[24];
  if (tele.has && millis() - tele.atMs < 30000) snprintf(b, sizeof(b), "BAT %.1fV", tele.v);
  else snprintf(b, sizeof(b), "BAT --.-V");
  tft.setCursor(120 - strlen(b) * 12, 280);
  tft.print(b);
  tft.setTextSize(1); tft.setTextColor(cDim);
  const char *h = "double-tap: enter PIN";
  tft.setCursor(120 - (int)strlen(h) * 3, 304);
  tft.print(h);
}

/* ---- PIN entry gate (over the standby screen) + on-device PIN change ----
 * pinEntryMode: 0 = disarm gate, 1 = new PIN first entry, 2 = repeat. */
uint8_t pinEntryMode = 0;
char pinNew[13];
Slot sPinDisp, sPinMsg;
void pinTitle() {
  const char *t = pinEntryMode == 0 ? "PIN TO DISARM"
                : pinEntryMode == 1 ? "NEW PIN - EMPTY OK = CANCEL"
                                    : "REPEAT NEW PIN";
  tft.fillRect(0, 18, W, 12, cBg);
  tft.setTextSize(1); tft.setTextColor(cDim);
  tft.setCursor((W - (int)strlen(t) * 6) / 2, 22);
  tft.print(t);
  const char *m = pinEntryMode == 0
                    ? (pairPin[0] ? "wrong PIN locks 15 s after 3 tries" : "no PIN set — set one in CONFIG")
                    : pinEntryMode == 1 ? "4-12 digits"
                                        : "repeat the same PIN";
  slotPrint(sPinMsg, 8, 64, 224, 12, 1, m,
            pinEntryMode == 0 && !pairPin[0] ? cWarn : cDim, cBg);
}
void drawPinScreen() {
  pinScreen = true; pinAtMs = millis(); pinLen = 0;
  tft.fillRect(0, 0, W, H, cBg);
  // entry display
  tft.fillRect(30, 34, 180, 26, cBg2); tft.drawRect(30, 34, 180, 26, cDim);
  // keypad edge-to-edge: 4 rows to the bottom, big keys (76x54)
  const char *lab[12] = {"1","2","3","4","5","6","7","8","9","C","0","OK"};
  for (uint8_t r = 0; r < 4; r++)
    for (uint8_t c = 0; c < 3; c++) {
      uint8_t i = r * 3 + c;
      uint16_t x = 2 + c * 79, y = 78 + r * 60;
      bool ok = (i == 11), clr = (i == 9);
      tft.fillRect(x, y, 76, 54, ok ? cGood : cBg2);
      tft.drawRect(x, y, 76, 54, ok ? cGood : clr ? cWarn : cDim);
      tft.setTextSize(3);
      tft.setTextColor(ok ? cBg : clr ? cWarn : cTxt);
      tft.setCursor(x + (76 - 18 * strlen(lab[i])) / 2, y + 15);
      tft.print(lab[i]);
    }
  pinTitle();
  sPinDisp.last[0] = 1;                  // force entry redraw
}
void pinDrawEntry() {
  // asterisks centered in the entry box (spaces do the centering)
  char line[17] = "";
  uint8_t pad = (15 - pinLen) / 2;
  for (uint8_t i = 0; i < pad; i++) line[i] = ' ';
  for (uint8_t i = 0; i < pinLen; i++) line[pad + i] = '*';
  line[pad + pinLen] = 0;
  slotPrint(sPinDisp, 34, 38, 172, 18, 2, line, cTxt, cBg2);
}
void pinExitToStandby() {
  pinScreen = false; pinEntryMode = 0;
  drawStandby();
}
void pinExitSetup(const char *msg, uint16_t col) {
  pinEntryMode = 0; pinScreen = false;
  toast(msg, col);
  if (dispOn) { uiInvalidate(); drawChrome(); }
  else if (klArmed) drawStandby();
  else tft.fillRect(0, 0, W, H, 0x0000);
}
void pinOk() {
  pinEntry[pinLen] = 0;
  if (pinEntryMode == 1) {               // new PIN, first entry
    if (pinLen == 0) { pinExitSetup("PIN change cancelled", cDim); return; }
    if (pinLen < 4 || pinLen > 12) {
      slotPrint(sPinMsg, 8, 64, 224, 12, 1, "need 4-12 digits", cWarn, cBg);
      pinLen = 0; pinDrawEntry(); pinAtMs = millis();
      return;
    }
    strcpy(pinNew, pinEntry); pinEntryMode = 2; pinLen = 0;
    pinTitle(); pinDrawEntry();
  } else if (pinEntryMode == 2) {        // repeat: must match
    if (!strcmp(pinNew, pinEntry)) {
      strncpy(pairPin, pinNew, 12); pairPin[12] = 0;
      prefs.putString("pin", pairPin);
      Serial.println("[pin] PIN changed on device");
      pinExitSetup("PIN saved", cGood);
      return;
    }
    pinEntryMode = 1; pinLen = 0;
    pinTitle(); pinDrawEntry();
    slotPrint(sPinMsg, 8, 64, 224, 12, 1, "mismatch - start over", cWarn, cBg);
  } else if (!pairPin[0]) {
    slotPrint(sPinMsg, 8, 64, 224, 12, 1, "no PIN set - CONFIG > CHANGE PIN", cWarn, cBg);
    pinLen = 0; pinDrawEntry();
  } else if (authOk(pinEntry)) {
    klArmed = false; manualDisarmed = true;
    absentSinceMs = millis();
    chirp(1);
    Serial.println("[pin] disarm OK (manual)");
    pendingAnim = 2;                     // unlock animation, then the app
    dispOn = false;                      // force a clean wake to full UI
    setDisplay(true);
    return;
  } else {
    Serial.println("[pin] wrong");
    pinLen = 0; pinDrawEntry();
    slotPrint(sPinMsg, 8, 64, 224, 12, 1,
              millis() < authLockUntilMs ? "LOCKED — wait" : "wrong PIN", cBad, cBg);
  }
  pinAtMs = millis();
}

/* ---- SET page: pairing QR for the phone app ---- */
void drawQr(const char *text) {
  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
  static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
  if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM,
                            qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true)) {
    Serial.println("[qr] encode failed");
    return;
  }
  int n = qrcodegen_getSize(qr);
  int sc = 200 / n; if (sc > 6) sc = 6;
  int px = n * sc;
  int ox = (W - px) / 2, oy = 48;
  tft.fillRect(ox - 6, oy - 6, px + 12, px + 12, 0xFFFF);   // white + quiet zone
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (qrcodegen_getModule(qr, x, y))
        tft.fillRect(ox + x * sc, oy + y * sc, sc, sc, 0x0000);
  Serial.printf("[qr] drawn %dx%d scale %d\n", n, n, sc);
}
void drawSet() {
  tft.setTextSize(1); tft.setTextColor(cDim);
  tft.setCursor(8, 34); tft.print("PAIRING - scan this QR in the app");
  char kh[33]; pairKeyHex(kh);
  uint32_t h = 0x9E3779B9;                 // redraw only when key/screen changed
  for (int i = 0; i < 16; i++) h = (h << 5) ^ (h >> 27) ^ pairKey[i];
  if (h != qrStamp) { qrStamp = h; drawQr((std::string("VOTOL:") + kh).c_str()); }
  slotPrint(sSys[6], 8, 244, 224, 14, 1, kh, cTxt, cBg);
  slotPrint(sSys[7], 8, 258, 224, 14, 1,
            "serial: K new key - P <pin> set pin", cDim, cBg);
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
    const char *arg = line + 2;
    if ((arg[0] == 'N' || arg[0] == 'n') && arg[1] == ':') {
      // 'f N:galaxy' — phone fob, matched by advertised NAME (Android
      // rotates its BLE MAC; the phone must run an advertiser app)
      if (fobAddName(arg + 2)) {
        fobSave();
        char lb[20]; snprintf(lb, sizeof(lb), "N:%s", fobs[fobCount - 1].name);
        Serial.printf("[cli] fob added %s\n", lb);
        toast("fob added", cGood);
      } else Serial.println("[cli] name empty/duplicate or list full");
      return;
    }
    // 'f AA:BB:CC:DD:EE:FF' — MAC fob (iTags have static MACs)
    uint8_t mac[6]; int n = 0;
    while (*arg && n < 6) {
      if (*arg == ':' || *arg == '-') { arg++; continue; }
      char hb[3] = {arg[0], arg[1], 0};
      if (!isxdigit((unsigned char)arg[0]) || !isxdigit((unsigned char)arg[1])) {
        Serial.println("[cli] bad mac"); return;
      }
      mac[n++] = (uint8_t)strtoul(hb, nullptr, 16);
      arg += 2;
    }
    if (n != 6) { Serial.println("[cli] need 6 bytes"); return; }
    if (fobAddMac(mac)) {
      fobSave();
      char lb[20]; macStr(lb, mac);
      Serial.printf("[cli] fob added %s\n", lb);
      toast("fob added", cGood);
    } else Serial.println("[cli] duplicate or list full");
    return;
  }
  if (line[0] == 'x' && line[1] == ' ') {          // 'x 2' — remove fob #2
    int n = atoi(line + 2);
    if (n >= 1 && n <= fobCount) {
      memmove(&fobs[n - 1], &fobs[n], (fobCount - n) * sizeof(Fob));
      fobCount--; fobSave();
      Serial.println("[cli] fob removed");
    } else Serial.println("[cli] bad index — 'L' lists fobs");
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
  if (line[0] == 'P' && (line[1] == ' ' || line[1] == '=')) {
    // 'P 1234' — optional short PIN for the app link; 'P off' disables
    const char *p = line + 2;
    if (!strncasecmp(p, "off", 3) || !strcmp(p, "-")) {
      pairPin[0] = 0; prefs.remove("pin");
      Serial.println("[cli] pin disabled");
    } else {
      int n = strlen(p);
      if (n < 4 || n > 12) { Serial.println("[cli] pin needs 4-12 chars"); return; }
      for (int i = 0; i < n; i++)
        if (!isalnum((unsigned char)p[i])) { Serial.println("[cli] alnum only"); return; }
      strncpy(pairPin, p, 12); pairPin[12] = 0;
      prefs.putString("pin", pairPin);
      Serial.println("[cli] pin saved");
    }
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
    case 'K': {                            // regenerate the app pair key
      pairKeyGen();
      char kh[33]; pairKeyHex(kh);
      Serial.printf("[cli] NEW pair key: %s\n", kh);
      toast("new pair key", cAcc);
      break;
    }
    case 'k': {
      char kh[33]; pairKeyHex(kh);
      Serial.printf("[cli] pair key: %s\n", kh);
      break;
    }
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
      fobCount = 0;
      prefs.putString("fobs", "");
      Serial.println("[cli] all fobs cleared");
      break;
    case 'L': {
      Serial.printf("[cli] fobs %d/4\n", fobCount);
      for (uint8_t i = 0; i < fobCount; i++) {
        char lb[20]; fobLabel(lb, sizeof(lb), i);
        Serial.printf("  %d  %s\n", i + 1, lb);
      }
      break;
    }
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
      // 'f <mac|N:name>', 'c <metres>', 'x <n>' and 'P <pin>' wait for newline
      if (c != ' ' && cliLen == 0 && c != 'f' && c != 'c' && c != 'x' && c != 'P') {
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
  Serial.printf("[tft-dash] fobs: %d\n", fobCount);
  for (uint8_t i = 0; i < fobCount; i++) {
    char lb[20]; fobLabel(lb, sizeof(lb), i);
    Serial.printf("[tft-dash]   %d %s\n", i + 1, lb);
  }

  wheelCircM = prefs.getFloat("wcirc", 0);
  btLinkOn = prefs.getBool("btlink", true);
  pairKeyLoad();
  { String pin = prefs.getString("pin", "");
    strncpy(pairPin, pin.c_str(), 12); pairPin[12] = 0; }

  // boot follows the fob: dark from the first second when registered+away
  dispOn = (fobCount == 0);
  if (dispOn) { tft.fillScreen(cBg); drawChrome(); }
  else         tft.fillScreen(0x0000);
  Serial.printf("[tft-dash] boot display %s\n",
                dispOn ? "on (no fob registered)" : "dark (waiting for fob)");

  wifiWindowSetup();
  SerialBT.begin("votol-dash", true);   // true = master/SPP-client mode
  SerialBT.setPin(BT_SERVER_PIN);
  xTaskCreate(bleTask, "ble", 8192, nullptr, 1, nullptr);
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
    if (fobAddMac(learnMac)) {
      fobSave();
      char lb[20]; macStr(lb, learnMac);
      Serial.printf("[ble] fob learned: %s (rssi %d)\n", lb, learnRssi);
      toast("fob learned", cGood);
    } else Serial.println("[ble] duplicate or list full");
  }

  bleProcessPending();
  klTick();
  buzzTick();
  ridingTick();
  displayTick();

  if (dispOn) {
    if (pendingAnim) {
      uint8_t a = pendingAnim; pendingAnim = 0;
      playAnim(a);
      if (!dispOn) return;   // ARM animation put us on standby — don't repaint
    }
    if (pinScreen) {         // keypad (gate or PIN-change) owns the screen
      pinTouch();
      if (pinScreen && millis() - pinAtMs > 20000) {
        Serial.println("[pin] timeout");
        if (pinEntryMode == 0) pinExitToStandby();
        else pinExitSetup("PIN change timed out", cDim);
      }
      delay(10);
      return;
    }
    if (armAsk) {            // YES/NO arm confirmation owns the screen
      armAskTouch();
      if (armAsk && millis() > armAskUntilMs) {
        armAsk = false; uiInvalidate(); drawChrome();
        Serial.println("[kl] arm confirm timeout");
      }
      delay(10);
      return;
    }
    handleTouch();
    if (pinScreen || armAsk) { delay(10); return; }   // a mode just opened
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
      case PG_SETUP:   setupTab == 0 ? drawSystem() :
                       setupTab == 1 ? drawCfg() : drawSet(); break;
  }
  } else if (pinScreen) {
    pinTouch();
    if (millis() - pinAtMs > 20000) {    // idle gate falls back to standby
      Serial.println("[pin] timeout -> standby");
      pinExitToStandby();
    }
  } else if (klArmed) {
    standbyTouch();                      // double-tap opens the PIN gate
  }
  delay(10);
}
