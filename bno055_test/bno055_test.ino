// BNO055 IMU test for XIAO ESP32S3, I2C address 0x29, SDA=GPIO5 SCL=GPIO6.
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BNO055.h>

#define SDA_PIN 5
#define SCL_PIN 6
#define BNO055_ADDR 0x29

Adafruit_BNO055 bno = Adafruit_BNO055(55, BNO055_ADDR, &Wire);

void setup() {
  Serial.begin(115200);
  delay(1500);
  Wire.begin(SDA_PIN, SCL_PIN);

  Serial.println("BNO055 test starting...");
  if (!bno.begin()) {
    Serial.println("ERROR: BNO055 not detected at 0x29 (check wiring/power).");
    while (1) {
      delay(1000);
    }
  }
  Serial.println("BNO055 detected OK.");
  delay(1000);
  bno.setExtCrystalUse(true);
}

void loop() {
  sensors_event_t orientationData, angVelData, linearAccelData;
  bno.getEvent(&orientationData, Adafruit_BNO055::VECTOR_EULER);
  bno.getEvent(&angVelData, Adafruit_BNO055::VECTOR_GYROSCOPE);
  bno.getEvent(&linearAccelData, Adafruit_BNO055::VECTOR_LINEARACCEL);

  Serial.printf("Orientation: X=%.2f Y=%.2f Z=%.2f\n",
                orientationData.orientation.x,
                orientationData.orientation.y,
                orientationData.orientation.z);

  uint8_t sys, gyro, accel, mag;
  bno.getCalibration(&sys, &gyro, &accel, &mag);
  Serial.printf("Calibration: sys=%d gyro=%d accel=%d mag=%d\n", sys, gyro, accel, mag);

  Serial.println("---");
  delay(500);
}
