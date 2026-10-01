/*
 * tft-dash TOUCH PROBE — answers ONE question with evidence:
 * is the shield's touch chip (XPT2046) already wired through the UNO
 * sockets (T_CLK=D13 T_DIN=D11 T_DO=D12 T_CS=D10), or does it need the
 * 4 extra wires?
 *
 * Method: bit-bang-free SPI on the VSPI pins those sockets lead to on a
 * D1 R32 (D13->GPIO18 SCK, D12->GPIO19 MISO, D11->GPIO23 MOSI), try chip
 * selects GPIO5 (D10, classic T_CS position) and GPIO21, and print raw
 * XPT2046 readings. Press the panel and watch z (pressure) jump.
 *
 * Verdict rules (printed every line):
 *   z in ~[100..3900] and changes with press  -> touch IS socket-wired
 *   z pegged 0/4095 or constant               -> touch NOT on these pins
 */

#include <Arduino.h>
#include <SPI.h>

SPIClass spi(HSPI);   // use HSPI object but with our own pins below

const int CANDS[] = {5, 21};       // candidate T_CS pins
const int SCK_PIN = 18, MISO_PIN = 19, MOSI_PIN = 23;

uint16_t rd(int cs, uint8_t cmd) {
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);
  spi.transfer(cmd);
  uint8_t hi = spi.transfer(0);
  uint8_t lo = spi.transfer(0);
  digitalWrite(cs, HIGH);
  spi.endTransaction();
  return ((uint16_t)((hi << 8) | lo) >> 3) & 0xFFF;
}

void setup() {
  Serial.begin(115200);
  delay(150);
  for (int cs : CANDS) { pinMode(cs, OUTPUT); digitalWrite(cs, HIGH); }
  spi.begin(SCK_PIN, MISO_PIN, MOSI_PIN);
  Serial.println(F("\n=== touch probe: sockets D13/D12/D11 = GPIO18/19/23 ==="));
  Serial.println(F("PRESS the panel now. z = pressure (jumps when touched)."));
  Serial.println(F("cs | z1    z2    pressure | X     Y"));
}

void loop() {
  static uint32_t t0 = 0;
  if (millis() - t0 < 400) return;
  t0 = millis();
  for (int cs : CANDS) {
    uint16_t z1 = rd(cs, 0xB1), z2 = rd(cs, 0xC1);
    int z = z1 + 4095 - z2;
    uint16_t x = rd(cs, 0x90), y = rd(cs, 0xD0);
    Serial.printf("cs%d | z1=%4u z2=%4u P=%5d | X=%4u Y=%4u %s\n",
                  cs, z1, z2, z, x, y,
                  (z > 100 && z < 4000) ? "<== PRESSURE" : "");
  }
  Serial.println();
}
