// Basic blink sanity check for the XIAO ESP32S3.
// Onboard user LED is GPIO21 and is active-LOW (LOW = on).
const int LED_PIN = 21;

void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);
}

void loop() {
  digitalWrite(LED_PIN, LOW);   // on
  Serial.println("LED ON");
  delay(500);
  digitalWrite(LED_PIN, HIGH);  // off
  Serial.println("LED OFF");
  delay(500);
}
