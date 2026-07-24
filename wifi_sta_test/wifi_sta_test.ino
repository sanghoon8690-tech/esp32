// Minimal STA Wi-Fi connectivity test for XIAO ESP32S3.
// NOTE: ESP32-S3's radio is 2.4GHz-only (802.11 b/g/n) - it cannot join
// a 5GHz-band network even if the SSID name contains "5G".
#include <WiFi.h>

const char *STA_SSID = "your-wifi-ssid";
const char *STA_PASSWORD = "your-wifi-password";

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);

  Serial.println("Scanning for nearby 2.4GHz networks...");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    Serial.printf("  [%d] %s (RSSI %d)\n", i, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
  }
  Serial.println("--- scan done ---");

  Serial.printf("Connecting to '%s'...\n", STA_SSID);
  WiFi.begin(STA_SSID, STA_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("CONNECTED. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.printf("FAILED. WiFi.status()=%d\n", WiFi.status());
  }
}

void loop() {
  delay(2000);
  Serial.printf("status=%d rssi=%d\n", WiFi.status(), WiFi.RSSI());
}
