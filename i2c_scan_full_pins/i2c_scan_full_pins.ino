// Full-pin I2C brute-force scanner for the XIAO ESP32S3 (Sense).
// Tries every ordered pair of the board's castellated GPIOs (D0-D10) as
// SDA/SCL to find which pins an unknown sensor is actually wired to.
// Camera-reserved pins (10,11,12,13,14,15,16,17,18,38,39,40,47,48) and the
// boot-strap pin (GPIO0) are excluded so this won't disturb the onboard
// camera module or interfere with boot.
#include <Wire.h>

// D0..D10 on the XIAO ESP32S3 silkscreen, in GPIO numbers.
const uint8_t candidatePins[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 43, 44};
const uint8_t numPins = sizeof(candidatePins) / sizeof(candidatePins[0]);

void scanPair(uint8_t sda, uint8_t scl) {
  Wire.end();
  if (!Wire.begin(sda, scl)) {
    Serial.printf("--- SDA=GPIO%d SCL=GPIO%d --- (begin failed, skipped)\n", sda, scl);
    return;
  }
  Wire.setClock(100000);
  Serial.printf("--- SDA=GPIO%d SCL=GPIO%d ---\n", sda, scl);
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  >>> FOUND device at 0x%02X (SDA=GPIO%d, SCL=GPIO%d)\n", addr, sda, scl);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("  (nothing)");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("Full-pin I2C scan starting...");
  Serial.printf("Testing %d pins -> %d ordered combinations per sweep\n",
                 numPins, numPins * (numPins - 1));
}

void loop() {
  for (uint8_t i = 0; i < numPins; i++) {
    for (uint8_t j = 0; j < numPins; j++) {
      if (i == j) continue;
      scanPair(candidatePins[i], candidatePins[j]);
    }
  }
  Serial.println("===== full sweep complete =====");
  delay(5000);
}
