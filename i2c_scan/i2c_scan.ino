// I2C scanner for the XIAO ESP32S3 Sense + I2C sensor carrier
// (BH1750 0x23, BME280 0x76, BNO055 0x29) on SDA=GPIO5, SCL=GPIO6.
#include <Wire.h>

void scanWith(int sda, int scl) {
  Wire.end();
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  Serial.printf("--- SDA=%d SCL=%d ---\n", sda, scl);
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Found device at 0x%02X\n", addr);
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
  Serial.println("I2C scan starting...");
}

void loop() {
  scanWith(5, 6);  // as documented for this board
  scanWith(6, 5);  // in case SDA/SCL are swapped on the carrier board
  Serial.println("=====");
  delay(3000);
}
