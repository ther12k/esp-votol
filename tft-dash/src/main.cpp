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
#include <nvs_flash.h>
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
void blePushStatus();                  // fwd
class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    bleConnected = true;
    Serial.println("[ble] app connected");
    blePushStatus();
  }
  void onDisconnect(BLEServer *s) override {
    bleConnected = false; Serial.println("[ble] app gone");
    BLEDevice::startAdvertising();
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
  statChr->setCallbacks(new StatCb());
  statChr->addDescriptor(new BLE2902());
  statChr->setValue("DISARMED FOFF");
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
uint16_t cBg, cBg2, cTxt, cDim, cAcc, cGood, cWarn, cBad, cBrd;

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
/* ---- centered slot (hero numbers / state words) ---- */
void slotPrintC(Slot &s, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                uint8_t size, const char *txt, uint16_t col, uint16_t bg) {
  if (!strcmp(s.last, txt) && s.lastCol == col && s.lastBg == bg) return;
  strncpy(s.last, txt, sizeof(s.last) - 1); s.last[sizeof(s.last) - 1] = 0;
  s.lastCol = col; s.lastBg = bg;
  tft.fillRect(x, y, w, h, bg);
  tft.setTextSize(size); tft.setTextColor(col);
  uint16_t tw = strlen(txt) * 6 * size;
  tft.setCursor(x + (w > tw ? (w - tw) / 2 : 0),
                y + ((h > 8 * size) ? (h - 8 * size) / 2 : 1));
  tft.print(txt);
}

/* ---- Oct-2026 mockup kit widgets ---- */
int barBattCache = -1, barCurrCache = -1, pctBattCache = -1, pctCurrCache = -1;
int sigCache = 9999;                 // LOCK signal bars (cached on bar level)
int8_t lockShown = -1;               // LOCK padlock: 0 no-fob, 1 open, 2 closed
int8_t setBoxShown = -1;             // SET page key-box furniture
bool cfgDrawn = false;               // CFG page static furniture
float tripKm = 0;                      // RAM-only trip meter (resets at boot)
uint32_t lastTripMs = 0;
const char *GEAR_NAMES[4] = {"LOW", "MID", "HIGH", "SPORT"};

int battPct() {                        // crude SOC from pack voltage (20S)
  if (!tele.has || millis() - tele.atMs > 30000 || tele.v < 30) return -1;
  return (int)constrain((tele.v - 63.0f) / 21.0f * 100.0f, 0.0f, 100.0f);
}
void drawBar(uint16_t x, uint16_t y, uint16_t w, uint16_t h, int pct,
             uint16_t col, int &cache) {
  if (pct == cache) return;            // cached: repaint only on level change
  cache = pct;
  for (uint8_t i = 0; i < 10; i++)
    tft.fillRect(x + i * (w / 10), y, w / 10 - 1, h, pct > i * 10 ? col : cBrd);
}
void drawPct(uint16_t x, uint16_t y, int pct, int &cache) {
  if (pct == cache) return;
  cache = pct;
  char b[8] = "--";
  if (pct >= 0) snprintf(b, sizeof(b), "%d%%", pct);
  tft.fillRect(x, y, 26, 11, cBg);
  tft.setTextSize(1); tft.setTextColor(cDim);
  tft.setCursor(x, y); tft.print(b);
}
void drawSig(uint16_t x, uint16_t y, int rssi) {   // 4 ascending signal bars
  int lvl = 0;                                  // cache on bar LEVEL, not raw
  for (uint8_t i = 0; i < 4; i++)               // RSSI (jitters every scan)
    if (rssi > (-90 + i * 10)) lvl++;
  if (lvl == sigCache) return;
  sigCache = lvl;
  tft.fillRect(x - 2, y - 2, 46, 18, cBg);
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t th = 4 + i * 3;
    tft.fillRect(x + i * 6, y + 14 - th, 4, th, i < lvl ? cGood : cBrd);
  }
}
void drawLockIcon(uint16_t cx, uint16_t top, uint16_t col, bool closed) {
  // small padlock (~70x82): body 70x52 + shackle; open = right leg lifted
  uint16_t by = top + 30;
  tft.fillRect(cx - 23, top, 10, by + 12 - top, col);              // left (hinged)
  tft.fillRect(cx + 13, top, 10, closed ? by + 12 - top : by - 8 - top, col);
  tft.fillRect(cx - 23, top, 46, 10, col);                          // bridge
  tft.fillRoundRect(cx - 35, by, 70, 52, 8, col);
  tft.fillCircle(cx, by + 20, 7, cBg);
  tft.fillRect(cx - 3, by + 10, 6, 13, cBg);
}
void drawLockShape(uint16_t col, int shY, int rLeg) {   // big anim padlock
  tft.fillRect(80, shY, 12, 144 - shY, col);
  tft.fillRect(80, shY, 80, 12, col);
  tft.fillRect(148, shY, 12, rLeg, col);
  tft.fillRoundRect(70, 138, 100, 74, 10, col);
  tft.fillCircle(120, 176, 10, cBg);
  tft.fillRect(115, 162, 10, 18, cBg);
}
void drawBackspace(int cx, int cy) {
  tft.fillTriangle(cx - 16, cy, cx - 5, cy - 11, cx - 5, cy + 11, cDim);
  tft.fillRect(cx - 5, cy - 9, 22, 18, cDim);
  tft.drawLine(cx + 1, cy - 4, cx + 10, cy + 4, cBg);
  tft.drawLine(cx + 10, cy - 4, cx + 1, cy + 4, cBg);
}
void tripTick() {
  uint32_t now = millis();
  float dt = (now - lastTripMs) / 1000.0f;
  lastTripMs = now;
  if (wheelCircM <= 0 || !tele.has || dt <= 0 || dt > 5) return;
  if (tele.rpm > 0 && now - tele.atMs < 3000)
    tripKm += tele.rpm * wheelCircM * 0.06f * dt / 3600.0f;
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
uint8_t battCache = 0xFF;            // header battery-gauge cache key
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
void blePushStatus() {
  if (bleConnected && statChr) {
    char st[32];
    snprintf(st, sizeof(st), "%s %s", klArmed ? "ARMED" : "DISARMED",
             fobPresent() ? "FON" : "FOFF");
    statChr->setValue((uint8_t *)st, strlen(st));
    statChr->notify();
  }
}
void klTick() {
  if (fobCount == 0) return;                  // no fob learned yet
  uint32_t now = millis();
  if (now - bootMs < BOOT_GRACE_S * 1000UL) { absentSinceMs = now; return; }
  if (fobPresent()) {
    absentSinceMs = now;
    manualDisarmed = false;                   // owner (fob) is here — normal mode
    if (klArmed) {
      klArmed = false; chirp(1); toast("DISARMED - fob back", cGood); pendingAnim = 2;
      blePushStatus();
    }
  } else {
    if (absentSinceMs == 0) absentSinceMs = now;
    if (manualDisarmed) absentSinceMs = now;  // manual disarm: never auto-arm
    // arm counted from the LAST SIGHTING: fires the moment the 12s fob
    // TTL lapses — BEFORE the display would sleep — so the sequence is
    // animation FIRST, screen off ~6s later (same 12s of silence needed
    // as before; no robustness change)
    if (!klArmed && !manualDisarmed && (now - fobLastSeenMs) > ARM_AFTER_S * 1000UL) {
      klArmed = true; chirp(2);
      blePushStatus();
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
  tft.setCursor((W - (int)strlen(t) * 12) / 2, 78); tft.print(t);
  tft.setTextSize(1); tft.setTextColor(cDim);
  const char *s = "screen will rest at the lock screen";
  tft.setCursor((W - (int)strlen(s) * 6) / 2, 106); tft.print(s);
  tft.fillRect(8, 148, 224, 56, cBg2); tft.drawRect(8, 148, 224, 56, cBrd);
  tft.setTextSize(3); tft.setTextColor(cTxt);
  const char *n = "NO";
  tft.setCursor((W - (int)strlen(n) * 18) / 2, 165); tft.print(n);
  tft.fillRect(8, 218, 224, 58, cGood);
  tft.setTextSize(3); tft.setTextColor(cBg);
  const char *y = "YES, ARM";
  tft.setCursor((W - (int)strlen(y) * 18) / 2, 236); tft.print(y);
}
void doDeviceArm() {
  if (fobCount == 0) { toast("no fob registered", cDim); uiInvalidate(); drawChrome(); return; }
  if (fobPresent())  { toast("fob near - it would disarm", cDim); uiInvalidate(); drawChrome(); return; }
  klArmed = true; manualDisarmed = false; absentSinceMs = millis();
  chirp(2); pendingAnim = 1;
  blePushStatus();
  Serial.println("[kl] armed from device (YES confirmed)");
}
void armAskTouch() {
  int px, py;
  if (!mapTouch(px, py)) return;
  static uint32_t lastTapMs = 0;
  if (millis() - lastTapMs < 180) return;
  lastTapMs = millis();
  if (py >= 140 && py <= 214) {          // NO
    armAsk = false; uiInvalidate(); drawChrome();
    Serial.println("[kl] arm cancelled");
  } else if (py >= 216) {                // YES
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
  RGB565(0, 208, 255),   // cyan #00D0FF (default)
  RGB565(0, 255, 128),   // green
  RGB565(255, 200, 0),   // yellow
  RGB565(255, 150, 40),  // orange
  RGB565(235, 240, 245), // white
  RGB565(240, 90, 220),  // pink
};
uint8_t uiScale = 1;                 // 0 small, 1 medium (default), 2 large
uint16_t uiVal = RGB565(0, 208, 255);   // telemetry value color
uint16_t uiAcc = RGB565(0, 208, 255);   // accent (tabs, highlights)
void uiSave() {
  prefs.putUChar("uiscale", uiScale);
  prefs.putUShort("uival", uiVal);
  prefs.putUShort("uiacc", uiAcc);
}
void uiLoad() {
  uiScale = prefs.getUChar("uiscale", 1);
  uiVal = prefs.getUShort("uival", RGB565(0, 208, 255));
  uiAcc = prefs.getUShort("uiacc", RGB565(0, 208, 255));
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
    // zones match the DRAWN widgets (size y84-124, value y134-162,
    // accent y180-208, CHANGE PIN y232-266) with finger margins
    if (py >= 78 && py <= 130 && px >= 8) {                // size S/M/L
      uiScale = px < 86 ? 0 : px < 154 ? 1 : 2;
      uiSave(); drawChrome(); toast("size saved", uiAcc);
    } else if (py >= 128 && py <= 168 && px >= 8 && px <= 230) {   // value color
      uiVal = PALETTE[(px - 8) / 38];
      uiSave(); drawChrome(); toast("value color saved", uiAcc);
    } else if (py >= 174 && py <= 214 && px >= 8 && px <= 230) {   // accent color
      uiAcc = PALETTE[(px - 8) / 38];
      uiSave(); drawChrome(); toast("accent color saved", uiAcc);
    } else if (py >= 226 && py <= 274 && px >= 8 && px <= 232) {   // change PIN
      pinEntryMode = 1;
      drawPinScreen();
      Serial.println("[pin] PIN change started");
    }
  } else if (page == PG_SETUP && setupTab == 0 &&
             py >= 212 && py <= 256 && px >= 8 && px <= 232) {
    btPauseToggle();                        // release BT for the phone app
  } else if (page == PG_KEYLESS && py >= 224 && py <= 272) {
    if (klArmed) {                            // DISARM asks the PIN, PANIC siren
      if (px < 148) { pinEntryMode = 0; drawPinScreen(); }
      else panicToggle();
    } else if (fobCount) armAskOpen();        // YES/NO confirmation screen
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
  else if (i == 9) { if (pinLen) pinLen--; }      // backspace
  else if (i == 11) { pinOk(); return; }
  pinDrawEntry();
}

/* =========================================================== drawing */

void drawTabBar() {
  for (uint8_t i = 0; i < 3; i++) {
    uint16_t x = i * 80;
    bool act = (i == page);
    tft.fillRect(x + 1, 278, 78, 40, act ? uiAcc : cBg2);
    tft.drawRect(x + 1, 278, 78, 40, act ? uiAcc : cBrd);
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
    tft.fillRect(x, 32, 72, 36, act ? uiAcc : cBg2);
    tft.drawRect(x, 32, 72, 36, act ? uiAcc : cBrd);
    tft.setTextSize(2);
    tft.setTextColor(act ? cBg : cTxt);
    uint16_t tw = strlen(names[i]) * 12;
    tft.setCursor(x + (72 - tw) / 2, 42);
    tft.print(names[i]);
  }
}

void drawChrome() {
  tft.fillRect(0, 0, W, 278, cBg);
  tft.fillRect(0, 29, W, 1, cBrd);             // header divider
  barBattCache = barCurrCache = pctBattCache = pctCurrCache = -1;
  sigCache = 9999; lockShown = -1; setBoxShown = -1; cfgDrawn = false;
  dotCache = 0xFFFF; btOffIconShown = -1; battCache = 0xFF;   // header repaints
  drawTabBar();
  tft.setTextSize(2); tft.setTextColor(cDim);
  if (page == PG_SETUP) drawSubTabs();
  else if (page == PG_TELE) {
    char sb[24];
    snprintf(sb, sizeof(sb), "TELEMETRY (%s)", PANE_NAMES[telePane]);
    tft.setTextSize(1);
    tft.setCursor(8, 36); tft.print(sb);
    if (telePane == 0) {                    // RIDE
      tft.setTextSize(2);
      tft.setCursor(8, 46); tft.print("SPEED");
      tft.setCursor(236 - (wheelCircM > 0 ? 4 : 3) * 12, 46);
      tft.print(wheelCircM > 0 ? "KM/H" : "RPM");
      tft.setTextSize(1);
      tft.setCursor(8, 158);   tft.print("BATTERY");
      tft.setCursor(128, 158); tft.print("CURRENT");
      tft.setCursor(8, 224);   tft.print("RIDE MODE");
      tft.setCursor(128, 224); tft.print("TRIP");
      tft.fillRect(8, 152, 224, 1, cBrd);
      tft.fillRect(8, 218, 224, 1, cBrd);
    } else if (telePane == 1) {             // ELEC
      tft.setCursor(8, 48);   tft.print("BATTERY");
      tft.setCursor(128, 48); tft.print("CURRENT");
      tft.setTextSize(1);
      tft.setCursor(8, 126);   tft.print("POWER");
      tft.setCursor(128, 126); tft.print("GEAR");
      tft.fillRect(8, 120, 224, 1, cBrd);
    } else {                                 // MOTOR
      tft.setCursor(8, 48);   tft.print("RPM");
      tft.setCursor(128, 48); tft.print("GEAR");
      tft.setTextSize(1);
      tft.setCursor(8, 126);   tft.print("TEMPS");
      tft.setCursor(128, 126); tft.print("STATE");
      tft.fillRect(8, 120, 224, 1, cBrd);
    }
  } else {                                   // LOCK
    tft.setTextSize(1); tft.setTextColor(cDim);
    tft.setCursor(8, 36); tft.print("SECURITY");
  }
  btnCache = -1;
  qrStamp = 0;                       // SET page QR must repaint after chrome
}

void drawHeader() {
  char h[30];
  uint16_t col = cTxt;
  uint32_t age = tele.has ? (millis() - tele.atMs) / 1000 : 999;
  if (wifiPhase)             { snprintf(h, sizeof(h), "VOTOL setup");   col = uiAcc; }
  else if (btPaused())        { snprintf(h, sizeof(h), "VOTOL bt pause"); col = cAcc; }
  else if (!btLinkOn)         snprintf(h, sizeof(h), "VOTOL %s",   // plain — badge shows the state
                page == PG_TELE ? PANE_NAMES[telePane] : PAGE_NAMES[page]);
  else if (!SerialBT.connected()) { snprintf(h, sizeof(h), "VOTOL bt search"); col = cWarn; }
  else if (!tele.has || age > 10)  { snprintf(h, sizeof(h), "VOTOL no data");  col = cWarn; }
  else snprintf(h, sizeof(h), "VOTOL POD");
  slotPrint(sHeader, 0, 0, 190, 28, 2, h, col, cBg);

  uint16_t dot = cBad;
  if (!btLinkOn) dot = 0;                   // link intentionally off: no alarm dot
  else if (!wifiPhase && tele.has) {
    if (age < 5) dot = cGood; else if (age < 30) dot = cWarn;
  } else if (!wifiPhase && SerialBT.connected()) dot = cWarn;
  if (dot != dotCache) {
    tft.fillRect(196, 6, 16, 15, cBg);
    if (dot) tft.fillCircle(204, 13, 5, dot);
    dotCache = dot;
  }

  // BT-off badge: crossed "BT" left of the gauge
  bool off = !btLinkOn;
  if (off != (bool)btOffIconShown) {
    btOffIconShown = off;
    tft.fillRect(190, 4, 26, 20, cBg);
    if (off) {
      tft.setTextSize(1); tft.setTextColor(cDim);
      tft.setCursor(196, 10); tft.print("BT");
      tft.drawLine(192, 4, 214, 22, cBad);
      tft.drawLine(193, 4, 215, 22, cBad);
    }
  }

  // battery gauge (crude voltage SOC), far right
  int bp = battPct();
  uint8_t bk = bp < 0 ? 0xFF : (uint8_t)(bp / 10);
  if (bk != battCache) {
    battCache = bk;
    tft.fillRect(210, 6, 30, 16, cBg);
    tft.drawRect(212, 8, 26, 12, cDim);
    tft.fillRect(238, 11, 2, 6, cDim);
    uint16_t seg = bp < 15 ? cBad : bp < 35 ? cWarn : cGood;
    for (uint8_t i = 0; i < 4; i++)
      tft.fillRect(214 + i * 6, 10, 5, 8, bp > (int)i * 25 + 10 ? seg : cBrd);
  }
}

void drawTelemetry() {
  char b[32];
  int bp = battPct();
  uint16_t battCol = bp < 25 ? (bp < 10 ? cBad : cWarn) : cGood;
  if (telePane == 0) {                       // RIDE — one huge number
    if (!tele.has) {
      slotPrintC(sTeleBig, 0, 64, 240, 84, 8, "--", cDim, cBg);
      slotPrint(sTelePow, 8, 170, 110, 26, 3, "--.-V", cDim, cBg);
      slotPrint(sTeleGear, 128, 170, 110, 26, 3, "--.-A", cDim, cBg);
      slotPrint(sTeleCur, 8, 236, 110, 16, 2, "--", cDim, cBg);
      slotPrint(sTeleRpm, 128, 236, 110, 16, 2, "--", cDim, cBg);
      bp = -1;
    } else {
      if (wheelCircM > 0) dtostrf(tele.rpm * wheelCircM * 0.06f, 1, 0, b);
      else                snprintf(b, sizeof(b), "%ld", (long)tele.rpm);
      uint8_t sr = uiScale == 0 ? 6 : 8;
      if (strlen(b) > 3 && sr > 6) sr = 6;   // auto-fit
      if (strlen(b) > 4 && sr > 5) sr = 5;
      slotPrintC(sTeleBig, 0, 64, 240, 84, sr, b, uiVal, cBg);
      dtostrf(tele.v, 4, 1, b); strcat(b, "V");
      slotPrint(sTelePow, 8, 170, 110, 26, 3, b, uiVal, cBg);
      dtostrf(tele.a, 4, 1, b); strcat(b, " A");
      slotPrint(sTeleGear, 128, 170, 110, 26, 3, b, uiVal, cBg);
      const char *gn = "?";
      if (tele.gear) {
        const char *gp = strchr(GEARS, tele.gear);
        if (gp) gn = GEAR_NAMES[gp - GEARS];
      }
      slotPrint(sTeleCur, 8, 236, 110, 16, 2, gn, uiVal, cBg);
      if (wheelCircM > 0) snprintf(b, sizeof(b), "%.2f km", tripKm);
      else                snprintf(b, sizeof(b), "SET WHEEL");
      slotPrint(sTeleRpm, 128, 236, 110, 16, 2, b,
                wheelCircM > 0 ? cTxt : cDim, cBg);
    }
    drawBar(8, 200, 88, 8, bp < 0 ? 0 : bp, battCol, barBattCache);
    drawPct(100, 198, bp, pctBattCache);
    int ap = tele.has ? (int)(constrain((tele.a < 0 ? -tele.a : tele.a) / 40.0f,
                                        0.0f, 1.0f) * 100) : 0;
    drawBar(128, 200, 100, 8, ap, cAcc, barCurrCache);
  } else if (telePane == 1) {                // ELEC
    if (tele.has) {
      dtostrf(tele.v, 4, 1, b); strcat(b, "V");
      uint8_t sb = szBig();
      if (strlen(b) > 5 && sb > 4) sb = 4;   // auto-fit: 6+ chars never overflows
      slotPrint(sTeleBig, 2, 66, 124, 34, sb, b, uiVal, cBg);
      dtostrf(tele.a, 4, 1, b); strcat(b, " A");
      slotPrint(sTeleCur, 128, 66, 108, 34, 3, b, uiVal, cBg);
      snprintf(b, sizeof(b), "%ldW", (long)(tele.v * tele.a));
      slotPrint(sTelePow, 8, 138, 110, 28, 3, b, cTxt, cBg);
      snprintf(b, sizeof(b), "%c", tele.gear);
      slotPrint(sTeleGear, 128, 138, 110, 28, 3, b, uiVal, cBg);
    } else {
      slotPrint(sTeleBig, 2, 66, 124, 34, szBig(), "--.-V", cDim, cBg);
      slotPrint(sTeleCur, 128, 66, 108, 34, 3, "--", cDim, cBg);
      slotPrint(sTelePow, 8, 138, 110, 28, 3, "--", cDim, cBg);
      slotPrint(sTeleGear, 128, 138, 110, 28, 3, "-", cDim, cBg);
      bp = -1;
    }
    drawBar(8, 104, 88, 8, bp < 0 ? 0 : bp, battCol, barBattCache);
    drawPct(100, 102, bp, pctBattCache);
    int ap = tele.has ? (int)(constrain((tele.a < 0 ? -tele.a : tele.a) / 40.0f,
                                        0.0f, 1.0f) * 100) : 0;
    drawBar(128, 104, 100, 8, ap, cAcc, barCurrCache);
  } else {                                   // MOTOR
    if (tele.has) {
      snprintf(b, sizeof(b), "%ld", (long)tele.rpm);
      uint8_t sb = szBig();
      if (strlen(b) > 5 && sb > 4) sb = 4;
      slotPrint(sTeleRpm, 2, 66, 124, 34, sb, b, uiVal, cBg);
      snprintf(b, sizeof(b), "%c", tele.gear);
      slotPrint(sTeleGear, 128, 66, 108, 34, 3, b, uiVal, cBg);
      snprintf(b, sizeof(b), "%ld/%ldC", (long)tele.tc, (long)tele.tm);
      slotPrint(sTeleTc, 8, 138, 110, 28, 3, b, cTxt, cBg);
      slotPrint(sTelePow, 128, 138, 110, 28, 3, CTL_STATUS[tele.status],
                tele.status == 7 ? cBad : cTxt, cBg);
    } else {
      slotPrint(sTeleRpm, 2, 66, 124, 34, szBig(), "--", cDim, cBg);
      slotPrint(sTeleGear, 128, 66, 108, 34, 3, "-", cDim, cBg);
      slotPrint(sTeleTc, 8, 138, 110, 28, 3, "--", cDim, cBg);
      slotPrint(sTelePow, 128, 138, 110, 28, 3, "--", cDim, cBg);
    }
  }
  if (tele.has) {
    if (tele.fault) snprintf(b, sizeof(b), "F:%04lX %.12s", (unsigned long)tele.fault,
                             CTL_STATUS[tele.status]);
    else            snprintf(b, sizeof(b), "%.16s", CTL_STATUS[tele.status]);
    slotPrint(sTeleStat, 8, 256, 224, 10, 1, b, tele.fault ? cWarn : cDim, cBg);
  } else {
    const char *m = !btLinkOn ? "telemetry off"
                  : !SerialBT.connected() ? "bluetooth: searching bridge"
                                          : "linked — waiting for frames";
    slotPrint(sTeleStat, 8, 256, 224, 10, 1, m, cWarn, cBg);
  }
  slotPrint(sToast, 8, 266, 224, 9, 1,
            millis() - toastAtMs < 4000 ? toastTxt : "", toastCol, cBg);
}

void drawKeyless() {
  char b[48];
  // padlock + divider repaint on STATE CHANGE only — per-frame redraws
  // flicker badly on this parallel bus
  int8_t lk = fobCount == 0 ? 0 : (klArmed ? 2 : 1);
  if (lk != lockShown) {
    lockShown = lk;
    tft.fillRect(60, 46, 120, 90, cBg);        // clear the icon region
    drawLockIcon(120, 50, lk == 2 ? cBad : lk == 0 ? cDim : cGood, lk != 1);
    if (lk) tft.fillRect(8, 176, 224, 1, cBrd);
  }
  if (fobCount == 0) {
    slotPrintC(sKlState, 0, 138, 240, 32, 4, "NO FOB", cDim, cBg);
    slotPrint(sKlFob, 8, 182, 224, 14, 1, "register a fob over USB serial:", cWarn, cBg);
    slotPrint(sKlInfo, 8, 196, 224, 14, 1, "'m' iTag  'f <mac>'  'f N:<name>' phone", cWarn, cBg);
    tft.fillRect(8, 228, 224, 40, cBg);      // button area stays clear
    return;
  }
  bool grace = (millis() - bootMs) < BOOT_GRACE_S * 1000UL;
  bool armed = klArmed;
  slotPrintC(sKlState, 0, 138, 240, 32, 4, armed ? "ARMED" : "DISARMED",
             armed ? cBad : cGood, cBg);
  if (fobPresent()) snprintf(b, sizeof(b), "FOB: NEAR");
  else if (fobLastSeenMs) snprintf(b, sizeof(b), "FOB: AWAY %lds",
                                   (long)((millis() - fobLastSeenMs) / 1000));
  else snprintf(b, sizeof(b), "FOB: NO SIGNAL");
  slotPrint(sKlFob, 8, 182, 160, 16, 2, b, fobPresent() ? cGood : cWarn, cBg);
  drawSig(188, 184, fobPresent() ? fobRssi : -128);
  if (grace) snprintf(b, sizeof(b), "boot grace %lus",
                      (unsigned)((BOOT_GRACE_S * 1000UL - (millis() - bootMs)) / 1000));
  else if (fobPresent()) snprintf(b, sizeof(b), "rssi %d dBm", fobRssi);
  else snprintf(b, sizeof(b), "heard %.19s", fobLastLabel);
  slotPrint(sKlInfo, 8, 202, 224, 12, 1, b, cDim, cBg);
  char l1[44] = "FOBS";
  for (uint8_t i = 0; i < fobCount; i++) {
    char lb[20];
    fobLabel(lb, sizeof(lb), i);
    char e[24];
    snprintf(e, sizeof(e), " %d:%.16s", i + 1, lb);
    if (strlen(l1) + strlen(e) < 41) strcat(l1, e);
    else { strcat(l1, " +"); break; }
  }
  slotPrint(sKlEnt[0], 8, 214, 224, 12, 1, l1, cDim, cBg);

  bool panic = millis() < panicUntilMs;
  int8_t st = (armed ? 1 : 0) | (panic ? 2 : 0) | (fobPresent() ? 4 : 0);
  if (st != btnCache) {
    btnCache = st;
    if (armed) {                             // DISARM asks the PIN; PANIC siren
      tft.fillRect(8, 228, 140, 38, cBad);
      tft.setTextSize(2); tft.setTextColor(cBg);
      tft.setCursor(8 + (140 - 6 * 12 - 16) / 2, 240); tft.print("DISARM");
      tft.setCursor(126, 240); tft.print(">");
      tft.fillRect(152, 228, 80, 38, panic ? cGood : cBg2);
      tft.drawRect(152, 228, 80, 38, panic ? cGood : cBrd);
      tft.setTextSize(2);
      tft.setTextColor(panic ? cBg : cBad);
      tft.setCursor(152 + (80 - (panic ? 4 : 5) * 12) / 2, 240);
      tft.print(panic ? "STOP" : "PANIC");
    } else {
      tft.fillRect(8, 228, 224, 38, cGood);
      tft.setTextSize(3); tft.setTextColor(cBg);
      tft.setCursor(8 + (224 - 3 * 18 - 22) / 2, 236); tft.print("ARM");
      tft.setCursor(208, 236); tft.print(">");
    }
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
  slotPrint(sSys[3], 8, 140, 224, 18, 1, b, cDim, cBg);
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
    tft.fillRect(8, 212, 224, 44, btPaused() ? cGood : uiAcc);
    tft.setTextSize(2); tft.setTextColor(cBg);
    const char *t = btPaused() ? "TAP TO RESUME BT" : "RELEASE BT TO APP";
    tft.setCursor(8 + (224 - strlen(t) * 12) / 2, 226);
    tft.print(t);
  }
}

void drawCfg() {
  // content starts BELOW the sub-tab band (y28-68): nothing may paint there.
  // Static furniture drawn ONCE per chrome repaint — every control change
  // (size/color/PIN handlers) calls drawChrome, so this never goes stale.
  // Per-frame redraws flicker on this bus.
  if (!cfgDrawn) {
    cfgDrawn = true;
    tft.setTextSize(1); tft.setTextColor(cDim);
    tft.setCursor(8, 72); tft.print("TEXT SIZE");
    const char *sz[3] = {"S", "M", "L"};
    for (uint8_t i = 0; i < 3; i++) {
      uint16_t x = 8 + i * 78;
      bool act = (uiScale == i);
      tft.fillRect(x, 84, 68, 40, act ? uiAcc : cBg2);
      tft.drawRect(x, 84, 68, 40, act ? uiAcc : cBrd);
      tft.setTextSize(3); tft.setTextColor(act ? cBg : cTxt);
      tft.setCursor(x + (68 - 18) / 2, 95);
      tft.print(sz[i]);
    }
    for (uint8_t row = 0; row < 2; row++) {        // value then accent swatches
      uint16_t y = row == 0 ? 134 : 180;
      uint16_t cur = row == 0 ? uiVal : uiAcc;
      tft.setTextSize(1); tft.setTextColor(cDim);
      tft.setCursor(8, y - 12);
      tft.print(row == 0 ? "VALUE COLOR" : "ACCENT COLOR");
      for (uint8_t i = 0; i < 6; i++) {
        uint16_t x = 8 + i * 38;
        tft.fillRect(x, y, 32, 28, PALETTE[i]);
        if (PALETTE[i] == cur) tft.drawRect(x - 2, y - 2, 36, 32, cTxt);
      }
    }
    tft.setTextSize(1); tft.setTextColor(cDim);
    tft.setCursor(8, 222); tft.print("SECURITY");
    tft.fillRect(8, 232, 224, 34, cBg2);
    tft.drawRect(8, 232, 224, 34, cBrd);
    tft.setTextSize(2); tft.setTextColor(cTxt);
    tft.setCursor(8 + (224 - 10 * 12) / 2, 241);
    tft.print("CHANGE PIN");
  }
  slotPrint(sToast, 8, 270, 224, 14, 1,
            millis() - toastAtMs < 4000 ? toastTxt : "", toastCol, cBg);
}

/* ---- phone-app command execution (runs in loop context) ---- */
void bleExec(const char *cmd, const char *sec, const char *arg) {
  if (!authOk(sec)) {
    snprintf(bleReply, sizeof(bleReply), "ERR KEY");
    Serial.printf("[ble] cmd %s: BAD KEY\n", cmd);
    return;
  }
  if (!strcmp(cmd, "ARM")) {
    if (fobPresent()) { snprintf(bleReply, sizeof(bleReply), "ERR FOB NEAR"); return; }
    klArmed = true; manualDisarmed = false; chirp(2);
    toast("ARMED - app", cWarn);
    blePushStatus();
    if (dispOn) pendingAnim = 1;
    snprintf(bleReply, sizeof(bleReply), "OK ARMED");
  } else if (!strcmp(cmd, "DISARM")) {
    // sticky: stays disarmed + display on until ARM is sent (or fob seen)
    klArmed = false; manualDisarmed = true; absentSinceMs = millis(); chirp(1);
    toast("DISARMED - app", cGood);
    blePushStatus();
    if (dispOn) pendingAnim = 2;
    snprintf(bleReply, sizeof(bleReply), "OK DISARM");
  } else if (!strcmp(cmd, "PANIC")) {
    panicUntilMs = millis() + PANIC_S * 1000UL;
    snprintf(bleReply, sizeof(bleReply), "OK PANIC");
  } else if (!strcmp(cmd, "STAT")) {
    float v = tele.has ? tele.v : 0.0f;
    snprintf(bleReply, sizeof(bleReply), "%s %s %.1fV",
             klArmed ? "ARMED" : "DISARMED",
             fobPresent() ? "FON" : "FOFF", v);
  } else if (!strcmp(cmd, "GETCFG")) {
    snprintf(bleReply, sizeof(bleReply), "CFG:%s:%.2f:%d:%d",
             pairPin[0] ? pairPin : "-", wheelCircM, (int)fobCount, btLinkOn ? 1 : 0);
  } else if (!strcmp(cmd, "SETPIN")) {
    if (!arg || !*arg || !strcasecmp(arg, "off") || !strcmp(arg, "-")) {
      pairPin[0] = 0; prefs.remove("pin");
      toast("PIN cleared", cWarn);
      snprintf(bleReply, sizeof(bleReply), "OK PIN OFF");
    } else if (strlen(arg) >= 4 && strlen(arg) <= 12) {
      strncpy(pairPin, arg, 12); pairPin[12] = 0;
      prefs.putString("pin", pairPin);
      toast("PIN updated", cGood);
      snprintf(bleReply, sizeof(bleReply), "OK PIN %s", pairPin);
    } else {
      snprintf(bleReply, sizeof(bleReply), "ERR PIN LEN");
    }
  } else if (!strcmp(cmd, "SETWHEEL")) {
    float m = (float)atof(arg);
    if (m >= 0.5f && m <= 5.0f) {
      wheelCircM = m; prefs.putFloat("wcirc", m);
      toast("wheel saved", cGood);
      snprintf(bleReply, sizeof(bleReply), "OK WHEEL %.2f", m);
    } else {
      snprintf(bleReply, sizeof(bleReply), "ERR WHEEL VAL");
    }
  } else if (!strcmp(cmd, "ADDFOB")) {
    bool ok = false;
    if (arg && (arg[0] == 'N' || arg[0] == 'n') && arg[1] == ':') {
      ok = fobAddName(arg + 2);
    } else if (arg && strlen(arg) == 17) {
      uint8_t mac[6]; int n = 0; const char *p = arg;
      while (*p && n < 6) {
        if (*p == ':' || *p == '-') { p++; continue; }
        char hb[3] = {p[0], p[1], 0};
        mac[n++] = (uint8_t)strtoul(hb, nullptr, 16);
        p += 2;
      }
      if (n == 6) ok = fobAddMac(mac);
    }
    if (ok) {
      fobSave(); toast("fob added", cGood);
      snprintf(bleReply, sizeof(bleReply), "OK FOB %d", (int)fobCount);
    } else {
      snprintf(bleReply, sizeof(bleReply), "ERR FOB");
    }
  } else if (!strcmp(cmd, "CLRFOB")) {
    fobCount = 0; prefs.putString("fobs", "");
    toast("fobs cleared", cWarn);
    snprintf(bleReply, sizeof(bleReply), "OK FOBS CLR");
  } else if (!strcmp(cmd, "SETBT")) {
    btLinkOn = (atoi(arg) != 0);
    prefs.putBool("btlink", btLinkOn);
    snprintf(bleReply, sizeof(bleReply), "OK BT %d", btLinkOn ? 1 : 0);
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
  // format is "CMD:SECRET" or "CMD:SECRET:ARG"
  char *p1 = strpbrk(line, ":|");
  char *cmd = line;
  char *sec = "";
  char *arg = "";
  if (p1) {
    *p1 = 0;
    sec = p1 + 1;
    char *p2 = strpbrk(sec, ":|");
    if (p2) {
      *p2 = 0;
      arg = p2 + 1;
    }
  }
  for (char *p = cmd; *p; p++) *p = toupper((unsigned char)*p);
  bleExec(cmd, sec, arg);
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
  drawLockShape(col, shY, rLeg);
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
 * a bold PROHIBITED symbol (thick ring + thick X) — chosen over a
 * padlock because it stays legible from across a parking lot.
 * Double-tap wakes the PIN gate. */
void drawStandby() {
  pinScreen = false;
  tft.fillRect(0, 0, W, H, cBg);
  const int cx = 120, cy = 134, R = 98;
  for (int r = R; r > R - 14; r--)            // ~14px bold ring
    tft.drawCircle(cx, cy, r, cBad);
  // bold X: overlapping filled squares along each diagonal — offsetting
  // drawLine by (d,d) slides ALONG a 45° line (hairline bug), so union
  // squares instead: guaranteed solid ~34px strokes
  const int a = 66, sq = 24;
  for (int i = -a; i <= a; i += 2) {
    tft.fillRect(cx + i - sq / 2, cy + i - sq / 2, sq, sq, cBad);   // "\" arm
    tft.fillRect(cx + i - sq / 2, cy - i - sq / 2, sq, sq, cBad);   // "/" arm
  }
  tft.setTextColor(cBad); tft.setTextSize(4);
  tft.setCursor((W - 5 * 24) / 2, 244);       // "ARMED", centered
  tft.print("ARMED");
  tft.setTextSize(2); tft.setTextColor(cTxt);
  char b[24];
  if (tele.has && millis() - tele.atMs < 30000) snprintf(b, sizeof(b), "BAT %.1fV", tele.v);
  else snprintf(b, sizeof(b), "BAT --.-V");
  tft.setCursor((W - (int)strlen(b) * 12) / 2, 284);
  tft.print(b);
  tft.setTextSize(1); tft.setTextColor(cDim);
  const char *h = "double-tap: enter PIN";
  tft.setCursor((W - (int)strlen(h) * 6) / 2, 306);
  tft.print(h);
}

/* ---- PIN entry gate (over the standby screen) + on-device PIN change ----
 * pinEntryMode: 0 = disarm gate, 1 = new PIN first entry, 2 = repeat. */
uint8_t pinEntryMode = 0;
char pinNew[13];
Slot sPinMsg;
void pinTitle() {
  const char *t = pinEntryMode == 0 ? "ENTER PIN TO ARM"
                : pinEntryMode == 1 ? "SET NEW PIN"
                                    : "REPEAT NEW PIN";
  tft.fillRect(0, 2, W, 36, cBg);
  // mini padlock icon + title, centered as a group
  uint16_t tw = strlen(t) * 12;
  uint16_t tx = (W - (tw + 26)) / 2;
  tft.fillRect(tx, 10, 16, 12, uiAcc);           // body
  tft.drawRect(tx + 3, 4, 10, 8, uiAcc);         // shackle
  tft.fillRect(tx + 4, 8, 8, 4, uiAcc);
  tft.fillCircle(tx + 8, 15, 2, cBg);
  tft.setTextSize(2); tft.setTextColor(cTxt);
  tft.setCursor(tx + 26, 8); tft.print(t);
  const char *m = pinEntryMode == 0
                    ? (pairPin[0] ? "wrong PIN locks 15 s after 3 tries" : "no PIN set — set one in CONFIG")
                    : pinEntryMode == 1 ? "4-12 digits"
                                        : "repeat the same PIN";
  slotPrintC(sPinMsg, 0, 26, 240, 12, 1, m,
             pinEntryMode == 0 && !pairPin[0] ? cWarn : cDim, cBg);
}
void drawPinScreen() {
  pinScreen = true; pinAtMs = millis(); pinLen = 0;
  tft.fillRect(0, 0, W, H, cBg);
  // keypad edge-to-edge: 4 rows to the bottom, big keys (76x54)
  for (uint8_t r = 0; r < 4; r++)
    for (uint8_t c = 0; c < 3; c++) {
      uint8_t i = r * 3 + c;
      uint16_t x = 2 + c * 79, y = 78 + r * 60;
      bool ok = (i == 11), del = (i == 9);
      tft.fillRect(x, y, 76, 54, ok ? cGood : cBg2);
      tft.drawRect(x, y, 76, 54, ok ? cGood : cBrd);
      if (ok) {
        tft.setTextSize(3); tft.setTextColor(cBg);
        tft.setCursor(x + (76 - 2 * 18) / 2, y + 15);
        tft.print("OK");
      } else if (del) {
        drawBackspace(x + 38, y + 27);
      } else {
        tft.setTextSize(3); tft.setTextColor(cTxt);
        char lab[2] = {(char)(i == 10 ? '0' : '1' + i), 0};
        tft.setCursor(x + (76 - 18) / 2, y + 15);
        tft.print(lab);
      }
    }
  pinTitle();
  pinDrawEntry();
}
void pinDrawEntry() {
  // PIN boxes: dot per entered digit, cyan ring on the active box
  tft.fillRect(10, 40, 220, 34, cBg);
  const uint8_t NB = 6, bw = 28, bh = 30, gap = 7;
  uint16_t x0 = (W - (NB * bw + (NB - 1) * gap)) / 2;
  for (uint8_t i = 0; i < NB; i++) {
    uint16_t x = x0 + i * (bw + gap);
    bool act = (i == pinLen) && pinLen < NB;
    tft.drawRect(x, 42, bw, bh, act ? uiAcc : cBrd);
    if (act) tft.drawRect(x + 1, 43, bw - 2, bh - 2, uiAcc);
    if (i < pinLen) tft.fillCircle(x + bw / 2, 57, 4, cTxt);
  }
  if (pinLen > NB) {                     // 7th..12th digit: overflow marker
    char m[5];
    snprintf(m, sizeof(m), "+%d", pinLen - NB);
    tft.setTextSize(1); tft.setTextColor(cDim);
    tft.setCursor(x0 + NB * (bw + gap) + 1, 52);
    tft.print(m);
  }
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
      slotPrintC(sPinMsg, 0, 26, 240, 12, 1, "need 4-12 digits", cWarn, cBg);
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
    slotPrintC(sPinMsg, 0, 26, 240, 12, 1, "mismatch - start over", cWarn, cBg);
  } else if (!pairPin[0]) {
    slotPrintC(sPinMsg, 0, 26, 240, 12, 1, "no PIN set - CONFIG > CHANGE PIN", cWarn, cBg);
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
    slotPrintC(sPinMsg, 0, 26, 240, 12, 1,
               millis() < authLockUntilMs ? "LOCKED — wait" : "wrong PIN", cBad, cBg);
  }
  pinAtMs = millis();
}

/* ---- SET page: pairing QR for the phone app ---- */
void drawQr(const char *text) {
  // bare 32-hex at ECC LOW -> version 2 (25 modules): fewer, bigger
  // modules scan far more easily from a 2.4" glass than v4@6px did
  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(4)];
  static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(4)];
  if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW,
                            2, 4, qrcodegen_Mask_AUTO, true)) {
    Serial.println("[qr] encode failed");
    return;
  }
  int n = qrcodegen_getSize(qr);
  int sc = 100 / n; if (sc > 4) sc = 4;      // 25*4=100 — compact; manual entry
  int px = n * sc;                           // rides on the BIG key text below
  int ox = (W - (px + 16)) / 2, oy = 76;     // +16 = white quiet border
  tft.fillRect(ox, oy, px + 16, px + 16, 0xFFFF);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (qrcodegen_getModule(qr, x, y))
        tft.fillRect(ox + 8 + x * sc, oy + 8 + y * sc, sc, sc, 0x0000);
  Serial.printf("[qr] drawn %dx%d scale %d\n", n, n, sc);
}
void drawSet() {
  char kh[33]; pairKeyHex(kh);
  uint32_t h = 0x9E3779B9;                 // redraw only when key/screen changed
  for (int i = 0; i < 16; i++) h = (h << 5) ^ (h >> 27) ^ pairKey[i];
  if (h != qrStamp) { qrStamp = h; drawQr(kh); }
  if (setBoxShown != 1) {                  // label + box once (flicker fix)
    setBoxShown = 1;
    tft.setTextSize(1); tft.setTextColor(cDim);
    const char *lb = "SCAN QR - OR TYPE THE KEY";
    tft.setCursor((W - (int)strlen(lb) * 6) / 2, 202); tft.print(lb);
    tft.fillRect(18, 212, 204, 50, cBg2);
    tft.drawRect(18, 212, 204, 50, uiAcc);
  }
  char l1[17], l2[17];                     // big 2-line key = easy manual typing
  memcpy(l1, kh, 16);      l1[16] = 0;
  memcpy(l2, kh + 16, 16); l2[16] = 0;
  slotPrintC(sSys[6], 18, 216, 204, 18, 2, l1, uiAcc, cBg2);
  slotPrintC(sSys[7], 18, 238, 204, 18, 2, l2, uiAcc, cBg2);
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
  slotPrint(sHeader, 0, 0, 190, 28, 2, b, cAcc, cBg);
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
    if (dispOn) { uiInvalidate(); drawChrome(); }   // KM/H <-> RPM label swap
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

  /* UI kit (Oct 2026 TFT mockups): navy bg, slate text, cyan/green/red */
  cBg  = tft.color565(8, 17, 29);      // #08111D screen navy
  cBg2 = tft.color565(18, 28, 44);     // card fill
  cBrd = tft.color565(56, 72, 92);     // #38485C card borders / dividers
  cTxt = tft.color565(230, 238, 247);  // #E6EEF7
  cDim = tft.color565(134, 161, 191);  // #86A1BF muted labels
  cAcc = tft.color565(0, 208, 255);    // #00D0FF tabs / active
  cGood= tft.color565(0, 255, 128);    // #00FF80 disarmed / OK
  cWarn= tft.color565(255, 193, 7);    // #FFC107
  cBad = tft.color565(255, 59, 59);    // #FF3B3B armed / panic

  uint16_t id = tft.readID();
  if (id == 0x0000 || id == 0xD3D3) id = 0x9341;
  tft.begin(id);
  tft.setRotation(0);
  Serial.printf("[tft-dash] LCD id 0x%04X\n", id);

  esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
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
  tripTick();
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
