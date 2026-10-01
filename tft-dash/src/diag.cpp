/*
 * tft-dash DIAGNOSE — interactive LCD + touch test (flash this to verify
 * wiring before anything else):
 *   - reads and prints the LCD controller ID over USB serial (115200)
 *   - paints labeled color bars (verifies the 8 data lines + WR/RD)
 *   - TOUCH TEST: press the panel — a dot is painted where the finger is
 *     (verifies + calibrates the touch wiring); raw and mapped coordinates
 *     stream over serial and show on screen
 *   - a 2 s status heartbeat on serial so a capture shows everything even
 *     when nobody is touching
 * No WiFi, no webapp. Reset (replug or RTS pulse) clears the paint area.
 *
 * Expected wiring (WIRING.md): shield seated + jumpers A2->IO15, A3->IO33,
 * A4->IO32; touch (optional): T_CLK->IO18, T_DIN->IO23, T_DO->IO19, T_CS->IO21.
 */

#include <Arduino.h>
#include <MCUFRIEND_kbv.h>

#define TOUCH_CS_PIN 21
#if TOUCH_CS_PIN > 0
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
XPT2046_Touchscreen ts(TOUCH_CS_PIN);
// Raw bounds for map() — refine from the serial printout, swap a pair to
// mirror an axis. Same values as src/main.cpp.
#define TS_LEFT   200
#define TS_RT     3700
#define TS_TOP    240
#define TS_BOT    3800
#endif

MCUFRIEND_kbv tft;
uint16_t lcdId = 0;
uint32_t touchSamples = 0;
int lastRawX = -1, lastRawY = -1, lastZ = -1;

const char *idName(uint16_t id) {
  switch (id) {
    case 0x9341: return "ILI9341 (240x320) - the usual 2.4\" chip";
    case 0x7783: return "ST7783";
    case 0x7789: return "ST7789V";
    case 0x8347: case 0x8347D: return "HX8347";
    case 0x0154: return "S6D0154";
    case 0x9325: case 0x9320: return "ILI9325";
    case 0x0000: return "NONE - no answer (check the 3 jumpers + seating)";
    case 0xD3D3: return "NONE (write-only garbage - check jumpers)";
    default:     return "unknown";
  }
}

void drawStatic() {
  tft.fillScreen(tft.color565(0, 0, 0));

  tft.setTextSize(2); tft.setTextColor(tft.color565(0, 200, 255));
  tft.setCursor(8, 6); tft.print("LCD+TOUCH TEST");
  tft.setTextColor(tft.color565(220, 230, 240));
  tft.setCursor(8, 26); tft.print("id: 0x");
  tft.print(lcdId, HEX);

  // four labeled color bars: wrong bar colors = swapped data lines / other chip
  const char *names[4] = {"RED", "GRN", "BLU", "WHT"};
  uint16_t cols[4];
  cols[0] = tft.color565(255, 0, 0);   cols[1] = tft.color565(0, 255, 0);
  cols[2] = tft.color565(0, 60, 255);  cols[3] = tft.color565(255, 255, 255);
  for (int i = 0; i < 4; i++) {
    tft.fillRect(i * 60, 48, 60, 56, cols[i]);
    tft.setTextSize(1); tft.setTextColor(tft.color565(0, 0, 0), cols[i]);
    tft.setCursor(i * 60 + 18, 72); tft.print(names[i]);
  }

  tft.setTextSize(1); tft.setTextColor(tft.color565(120, 130, 140));
  tft.setCursor(8, 116); tft.print("bars OK = data bus + WR work");
  tft.setCursor(8, 128); tft.print("serial 115200 shows live touch raw");

  // touch status + paint area
  tft.drawRect(0, 140, 240, 60, tft.color565(120, 130, 140));
  tft.setCursor(8, 146); tft.print("TOUCH raw / mapped:");
  tft.setCursor(8, 162); tft.setTextColor(tft.color565(0, 255, 120));
  tft.print("waiting for press...");
  tft.fillRect(0, 204, 240, 116, tft.color565(6, 10, 16));
  tft.setTextSize(1); tft.setTextColor(tft.color565(120, 130, 140));
  tft.setCursor(8, 208); tft.print("PAINT AREA - drag your finger");
}

void showTouchStatus(int rx, int ry, int z, int mx, int my) {
  char b[44];
  if (rx < 0) snprintf(b, sizeof(b), "waiting for press...");
  else snprintf(b, sizeof(b), "x=%4d y=%4d z=%4d -> %d,%d", rx, ry, z, mx, my);
  tft.fillRect(8, 160, 224, 14, tft.color565(0, 0, 0));
  tft.setTextSize(1); tft.setTextColor(tft.color565(0, 255, 120));
  tft.setCursor(8, 162); tft.print(b);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== tft-dash LCD+touch test ==="));
  Serial.println(F("wiring: shield seated, jumpers A2->IO15 A3->IO33 A4->IO32"));
  Serial.println(F("touch : T_CLK->IO18 T_DIN->IO23 T_DO->IO19 T_CS->IO21"));

  lcdId = tft.readID();
  Serial.printf("LCD ID: 0x%04X  ->  %s\n", lcdId, idName(lcdId));
  bool guessed = false;
  if (lcdId == 0x0000 || lcdId == 0xD3D3) {
    Serial.println(F("retrying as ILI9341 anyway - if the bars show, the chip"));
    Serial.println(F("is write-only or needs different wiring; note the look."));
    lcdId = 0x9341; guessed = true;
  }
  tft.begin(lcdId);
  tft.setRotation(0);
  drawStatic();
  if (guessed) {
    tft.setTextSize(1); tft.setTextColor(tft.color565(255, 200, 0));
    tft.setCursor(96, 26); tft.print("(guess)");
  }

#if TOUCH_CS_PIN > 0
  ts.begin();
  ts.setRotation(0);
  Serial.println(F("TOUCH: press and drag on the panel."));
#endif
}

void loop() {
#if TOUCH_CS_PIN > 0
  TS_Point p = ts.getPoint();
  int z = p.z;
  // real press: z in [400, 3900]. Floating (unwired) reads peg at 4095/0.
  if (z >= 400 && z <= 3900) {
    int mx = map(p.x, TS_LEFT, TS_RT, 0, 240);
    int my = map(p.y, TS_TOP, TS_BOT, 0, 320);
    lastRawX = p.x; lastRawY = p.y; lastZ = z;
    touchSamples++;
    Serial.printf("touch raw: x=%4d y=%4d z=%4d -> mapped %3d,%3d\n",
                  p.x, p.y, z, mx, my);
    showTouchStatus(p.x, p.y, z, mx, my);
    if (my >= 204)   // paint only inside the PAINT AREA
      tft.fillCircle(constrain(mx, 2, 237), constrain(my, 206, 317), 2,
                     tft.color565(0, 255, 120));
  }
#endif

  // heartbeat so a plain serial capture always shows the state
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 2000) {
    lastBeat = millis();
    Serial.printf("[beat] LCD 0x%04X  touch samples %lu  last ",
                  (unsigned)lcdId, (unsigned long)touchSamples);
    if (lastRawX >= 0) Serial.printf("x=%d y=%d z=%d\n", lastRawX, lastRawY, lastZ);
    else Serial.println("none");
  }
  delay(30);
}
