#include <Arduino.h>
#include <Adafruit_Fingerprint.h>

HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

#define FINGER_RX 13  // ESP32 menerima dari TX sensor
#define FINGER_TX 12  // ESP32 mengirim ke RX sensor

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Tes Fingerprint ESP32-CAM");

  fingerSerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);
  finger.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("Sensor fingerprint terdeteksi.");
  } else {
    Serial.println("Sensor fingerprint TIDAK terdeteksi.");
    while (true) delay(1000);
  }
}

void loop() {
  Serial.println("Tempelkan jari...");

  uint8_t p = finger.getImage();

  if (p == FINGERPRINT_OK) {
    Serial.println("Gambar jari terbaca.");
  } else if (p == FINGERPRINT_NOFINGER) {
    Serial.println("Belum ada jari.");
  } else {
    Serial.print("Error baca: ");
    Serial.println(p);
  }

  delay(1000);
}