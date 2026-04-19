#include <TinyGPS++.h>
#include <HardwareSerial.h>

// GPS Module TX connects to ESP32 RX (16)
// GPS Module RX connects to ESP32 TX (17)
const int RX_PIN = 16;
const int TX_PIN = 17;
const uint32_t GPS_BAUD = 9600; // Standard baud rate for most GPS modules

TinyGPSPlus gps;
HardwareSerial gpsSerial(2); // Use UART2

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  
  Serial.println("Starting GPS Test...");
}

void loop() {
  // Feed the GPS data into the library
  while (gpsSerial.available() > 0) {
    if (gps.encode(gpsSerial.read())) {
      displayInfo();
    }
  }

  // If no GPS data is received after 5 seconds, alert the user
  if (millis() > 5000 && gps.charsProcessed() < 10) {
    Serial.println("Error: No GPS detected. Check wiring.");
    delay(2000);
  }
}

void displayInfo() {
  if (gps.location.isValid()) {
    Serial.print("Lat: ");
    Serial.print(gps.location.lat(), 6);
    Serial.print(" | Long: ");
    Serial.println(gps.location.lng(), 6);
  } else {
    Serial.println("Waiting for Satellite Fix...");
  }
}
