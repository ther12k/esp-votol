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
 * 'r' 6 s raw dump, 'p' send SHOW now.
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

/* ---- bluetooth ---- */
#include "BluetoothSerial.h"
BluetoothSerial SerialBT;
#define BT_SERVER_NAME "VOTOL-BT"
#define BT_SERVER_PIN  "1234"
volatile bool btConnecting = false;
void btConnectTask(void *) {
  Serial.println("[bt] connecting " BT_SERVER_NAME " ...");
  bool ok = SerialBT.connect(BT_SERVER_NAME);
  Serial.printf("[bt] connect %s\n", ok ? "ok" : "failed");
  btConnecting = false;
  vTaskDelete(nullptr);
}

/* ---- display ---- */
MCUFRIEND_kbv tft;
#define W 240
#define H 320
uint16_t cBg, cBg2, cTxt, cDim, cAcc, cGood, cWarn, cBad;

/* ---- pages ---- */
enum Page : uint8_t { PG_TELE = 0, PG_KEYLESS = 1, PG_SYS = 2 };
const char *PAGE_NAMES[3] = {"TELE", "KEYLESS", "SYS"};
Page page = PG_TELE;

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
#define TR_MIN 250
#define TR_MAX 3850

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
Preferences prefs;
WebServer web(80);
uint32_t bootMs = 0, lastPollMs = 0, lastTouchMs = 0, lastCycleMs = 0, lastBtTryMs = 0;
uint32_t btPauseUntilMs = 0;
void toast(const char *t, uint16_t c);               // fwd
bool btPaused() { return millis() < btPauseUntilMs; }
void btPauseToggle() {
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
bool wifiPhase = true;
char toastTxt[80] = "";
uint16_t toastCol = 0;
uint32_t toastAtMs = 0;
void toast(const char *t, uint16_t c) {
  strncpy(toastTxt, t, sizeof(toastTxt) - 1); toastTxt[sizeof(toastTxt) - 1] = 0;
  toastCol = c; toastAtMs = millis();
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
int8_t btnCache = -1;

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

struct RawTouch { int x, y, z; bool valid; };
RawTouch readFilm() {
  RawTouch r{0, 0, 0, false};
  const FilmMap &m = FILMS[touchVariant];
  lcdQuiesce();
  int s[2];
  // X: drive X+ high / X- low, sense on Y+
  pinMode(m.yp, INPUT); pinMode(m.ym, INPUT);
  pinMode(m.xp, OUTPUT); pinMode(m.xm, OUTPUT);
  digitalWrite(m.xp, HIGH); digitalWrite(m.xm, LOW);
  delayMicroseconds(300);
  s[0] = analogRead(m.yp); s[1] = analogRead(m.yp);
  if (abs(s[0] - s[1]) < 8) { r.x = 4095 - ((s[0] + s[1]) / 2); r.valid = true; }
  // Y: drive Y+ high / Y- low, sense on X-
  pinMode(m.xp, INPUT); pinMode(m.xm, INPUT);
  pinMode(m.yp, OUTPUT); pinMode(m.ym, OUTPUT);
  digitalWrite(m.yp, HIGH); digitalWrite(m.ym, LOW);
  delayMicroseconds(300);
  s[0] = analogRead(m.xm); s[1] = analogRead(m.xm);
  if (abs(s[0] - s[1]) < 8 && r.valid) r.y = 4095 - ((s[0] + s[1]) / 2);
  else r.valid = false;
  // pressure: X+ low, Y- high, read X- and Y+
  pinMode(m.xp, OUTPUT); digitalWrite(m.xp, LOW);
  pinMode(m.yp, INPUT);
  pinMode(m.ym, OUTPUT); digitalWrite(m.ym, HIGH);
  delayMicroseconds(300);
  int z1 = analogRead(m.xm), z2 = analogRead(m.yp);
  r.z = 4095 - (z2 - z1);
  lcdRestore();
  if (!r.valid) r.z = 0;
  return r;
}

int lastRawX = -1, lastRawY = -1, lastRawZ = -1;
void drawChrome();                                   // fwd (used by handleTouch)
void handleTouch() {
  static uint32_t lastSample = 0;
  if (millis() - lastSample < 60) return;
  lastSample = millis();
  RawTouch r = readFilm();
  lastRawX = r.x; lastRawY = r.y; lastRawZ = r.z;
  if (r.z < 300 || r.z > 3800) return;      // not a real press
  int px = map(r.x, TR_MIN, TR_MAX, 0, W);
  int py = map(r.y, TR_MIN, TR_MAX, 0, H);
  if (tSwapXY) { int t = px; px = py * W / H; py = t * H / W; }
  if (tFlipX) px = W - 1 - px;
  if (tFlipY) py = H - 1 - py;
  if ((uint32_t)px >= W || (uint32_t)py >= H) return;
  static uint32_t lastTapMs = 0;
  if (millis() - lastTapMs < 350) return;
  lastTapMs = millis(); lastTouchMs = millis();
  if (py >= 278) {                          // tab bar
    uint8_t np = px / 80;
    if (np != page) { page = (Page)np; drawChrome(); }
  } else if (page == PG_SYS && py >= 168 && py <= 212 && px >= 8 && px <= 232) {
    btPauseToggle();                        // release BT for the phone app
  }
}

/* =========================================================== drawing */
void drawTabBar() {
  for (uint8_t i = 0; i < 3; i++) {
    uint16_t x = i * 80;
    bool act = (i == page);
    tft.fillRect(x + 1, 278, 78, 40, act ? cAcc : cBg2);
    tft.drawRect(x + 1, 278, 78, 40, cDim);
    tft.setTextSize(2);
    tft.setTextColor(act ? cBg : cTxt);
    uint16_t tw = strlen(PAGE_NAMES[i]) * 12;
    tft.setCursor(x + (80 - tw) / 2, 293);
    tft.print(PAGE_NAMES[i]);
  }
}

void drawChrome() {
  tft.fillRect(0, 27, W, 250, cBg);
  drawTabBar();
  if (page == PG_TELE) {
    tft.setTextSize(2); tft.setTextColor(cDim);
    tft.setCursor(8, 32);   tft.print("BATTERY");
    tft.setCursor(128, 32); tft.print("CURRENT");
    tft.setCursor(8, 96);   tft.print("POWER");
    tft.setCursor(128, 96); tft.print("GEAR");
    tft.setCursor(8, 158);  tft.print("RPM");
    tft.setCursor(128, 158);tft.print("CTRL/MOT C");
  }
  btnCache = -1;
}

void drawHeader() {
  char h[30];
  uint16_t col = cTxt;
  uint32_t age = tele.has ? (millis() - tele.atMs) / 1000 : 999;
  if (wifiPhase)              snprintf(h, sizeof(h), "VOTOL setup window");
  else if (btPaused())        { snprintf(h, sizeof(h), "VOTOL  bt paused");  col = cAcc; }
  else if (!SerialBT.connected()) { snprintf(h, sizeof(h), "VOTOL  bt search"); col = cWarn; }
  else if (!tele.has || age > 10)  { snprintf(h, sizeof(h), "VOTOL  no data");   col = cWarn; }
  else                        snprintf(h, sizeof(h), "VOTOL %s", PAGE_NAMES[page]);
  slotPrint(sHeader, 0, 0, 190, 26, 2, h, col, cBg2);

  uint16_t dot = cBad;
  if (!wifiPhase && tele.has) {
    if (age < 5) dot = cGood; else if (age < 30) dot = cWarn;
  } else if (!wifiPhase && SerialBT.connected()) dot = cWarn;
  static uint16_t lastDot = 0xFFFF;
  if (dot != lastDot) {
    tft.fillRect(216, 7, 18, 12, cBg2);
    tft.fillCircle(225, 13, 6, dot);
    lastDot = dot;
  }
}

void drawTelemetry() {
  char b[32];
  if (tele.has) {
    dtostrf(tele.v, 4, 1, b); strcat(b, "V");
    slotPrint(sTeleBig, 8, 48, 150, 42, 5, b, cAcc, cBg);
    dtostrf(tele.a, 4, 1, b); strcat(b, " A");
    slotPrint(sTeleCur, 128, 48, 108, 26, 3, b, cTxt, cBg);
    snprintf(b, sizeof(b), "%ldW", (long)(tele.v * tele.a));
    slotPrint(sTelePow, 8, 112, 108, 26, 3, b, cTxt, cBg);
    snprintf(b, sizeof(b), "%c", tele.gear);
    slotPrint(sTeleGear, 128, 112, 108, 26, 3, b, cTxt, cBg);
    snprintf(b, sizeof(b), "%ld", (long)tele.rpm);
    slotPrint(sTeleRpm, 8, 174, 108, 26, 3, b, cTxt, cBg);
    snprintf(b, sizeof(b), "%ld/%ld", (long)tele.tc, (long)tele.tm);
    slotPrint(sTeleTc, 128, 174, 108, 26, 3, b, cTxt, cBg);
    if (tele.fault) snprintf(b, sizeof(b), "F:%04lX %.12s", (unsigned long)tele.fault,
                             CTL_STATUS[tele.status]);
    else            snprintf(b, sizeof(b), "%.16s", CTL_STATUS[tele.status]);
    slotPrint(sTeleStat, 8, 214, 224, 18, 2, b, tele.fault ? cWarn : cDim, cBg);
  } else {
    const char *m = !SerialBT.connected() ? "bluetooth: searching bridge"
                                          : "linked — waiting for frames";
    slotPrint(sTeleBig, 8, 48, 150, 42, 5, "--.-V", cDim, cBg);
    slotPrint(sTeleCur, 128, 48, 108, 26, 3, "--", cDim, cBg);
    slotPrint(sTelePow, 8, 112, 108, 26, 3, "--", cDim, cBg);
    slotPrint(sTeleGear, 128, 112, 108, 26, 3, "-", cDim, cBg);
    slotPrint(sTeleRpm, 8, 174, 108, 26, 3, "--", cDim, cBg);
    slotPrint(sTeleTc, 128, 174, 108, 26, 3, "--", cDim, cBg);
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
  slotPrint(sToast, 8, 44, 224, 26, 2, "KEYLESS", cAcc, cBg);
  const char *l1 = "Phase 2: rides on a small bridge";
  const char *l2 = "firmware update (BT side-channel).";
  const char *l3 = "Until then use the phone dashboard";
  const char *l4 = "to arm/disarm.";
  tft.setTextSize(1); tft.setTextColor(cDim);
  tft.setCursor(8, 84);  tft.print(l1);
  tft.setCursor(8, 98);  tft.print(l2);
  tft.setCursor(8, 112); tft.print(l3);
  tft.setCursor(8, 126); tft.print(l4);
}

void drawSystem() {
  char b[48];
  snprintf(b, sizeof(b), "bt %s %s", SerialBT.connected() ? "LINKED" : "search",
           BT_SERVER_NAME);
  slotPrint(sSys[0], 8, 34, 224, 18, 2, b, SerialBT.connected() ? cGood : cDim, cBg);
  snprintf(b, sizeof(b), "rx %lu tx %lu", (unsigned long)rxCount, (unsigned long)txCount);
  slotPrint(sSys[1], 8, 56, 224, 18, 2, b, cTxt, cBg);
  snprintf(b, sizeof(b), "touch v%d %s%s%s", touchVariant, tSwapXY ? "SW" : "",
           tFlipX ? "FX" : "", tFlipY ? "FY" : "");
  slotPrint(sSys[2], 8, 78, 224, 18, 2, b, cTxt, cBg);
  snprintf(b, sizeof(b), "raw %4d %4d %4d", lastRawX, lastRawY, lastRawZ);
  slotPrint(sSys[3], 8, 100, 224, 18, 2, b, cDim, cBg);
  snprintf(b, sizeof(b), "up %lus  heap %ukB", (unsigned long)((millis() - bootMs) / 1000),
           (unsigned)(ESP.getFreeHeap() / 1024));
  slotPrint(sSys[4], 8, 122, 224, 18, 2, b, cTxt, cBg);
  snprintf(b, sizeof(b), "lcd 0x%04X  wifi %ds@boot", (unsigned)0, WIFI_WINDOW_S);
  slotPrint(sSys[5], 8, 144, 224, 18, 2, b, cDim, cBg);

  // BT release button (lets the phone's VOTOL app take the link)
  int8_t st = btPaused() ? 1 : 0;
  if (st != btnCache) {
    btnCache = st;
    tft.fillRect(8, 168, 224, 44, cBg2);
    tft.drawRect(8, 168, 224, 44, btPaused() ? cGood : cAcc);
    tft.setTextSize(2); tft.setTextColor(btPaused() ? cGood : cAcc);
    const char *t = btPaused() ? "BT paused - tap to resume"
                               : "release BT for phone";
    tft.setCursor(8 + (224 - strlen(t) * 12) / 2, 182);
    tft.print(t);
  }
}

/* =========================================================== wifi window */
void handleWindowState() {
  char s[300];
  snprintf(s, sizeof(s),
    "{\"uptimeS\":%lu,\"phase\":\"wifi-window\",\"bt\":%s,\"rx\":%lu,\"tx\":%lu,"
    "\"touchVariant\":%d,\"raw\":[%d,%d,%d]}",
    (unsigned long)((millis() - bootMs) / 1000),
    SerialBT.connected() ? "true" : "false",
    (unsigned long)rxCount, (unsigned long)txCount,
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
void serialCli() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
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
      case 'b': btPauseToggle(); break;
      case 'r': {
        Serial.println("[cli] raw dump 6 s — press the panel");
        uint32_t t0 = millis();
        while (millis() - t0 < 6000) {
          RawTouch r = readFilm();
          Serial.printf("v%d x=%4d y=%4d z=%4d\n", touchVariant, r.x, r.y, r.z);
          delay(100);
        }
        break;
      }
      default: break;
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

  tft.fillScreen(cBg);
  drawChrome();

  wifiWindowSetup();
  SerialBT.begin("votol-dash", true);   // true = master/SPP-client mode
  SerialBT.setPin(BT_SERVER_PIN);
  Serial.printf("[tft-dash] wifi window %ds, then BT->%s\n", WIFI_WINDOW_S, BT_SERVER_NAME);
}

void loop() {
  serialCli();

  if (wifiPhase) {
    wifiWindowRun();
    if (millis() - bootMs > WIFI_WINDOW_S * 1000UL) {
      WiFi.mode(WIFI_OFF);
      wifiPhase = false;
      Serial.println("[tft-dash] wifi off — touch + BT mode");
      drawChrome();
    }
    delay(20);
    return;
  }

  // BT link maintenance — connect runs in its own task; the by-name
  // inquiry blocks 10-30 s and must not freeze touch/UI.
  if (!btPaused() && !SerialBT.connected() && !btConnecting &&
      millis() - lastBtTryMs > 15000) {
    lastBtTryMs = millis(); btConnecting = true;
    xTaskCreate(btConnectTask, "btc", 4096, nullptr, 1, nullptr);
  }
  parseRx();
  if (SerialBT.connected() && millis() - lastPollMs > 1000) {
    lastPollMs = millis();
    sendShow(); txCount++;
  }

  handleTouch();

  // no-touch fallback: slow auto-cycle if the screen was never touched
  if (millis() - lastTouchMs > 60000 && millis() - lastCycleMs > 15000) {
    lastCycleMs = millis();
    page = (Page)((page + 1) % 3);
    drawChrome();
  }

  drawHeader();
  switch (page) {
    case PG_TELE:    drawTelemetry(); break;
    case PG_KEYLESS: drawKeyless();   break;
    case PG_SYS:     drawSystem();    break;
  }
  delay(10);
}
