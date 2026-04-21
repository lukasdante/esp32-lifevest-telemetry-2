#include <SPI.h>
#include <LoRa.h>
#include <TinyGPS++.h>
#include <HardwareSerial.h>

// --- LoRa & Hardware Pins ---
#define NSS 4
#define RST 5
#define DI0 2
#define MOSFET_PIN 26

#define TRANSMISSION_PERIOD 3000
#define DEVICE_ID 1

// --- GPS Pins & Config ---
// GPS Module TX connects to ESP32 RX (16)
// GPS Module RX connects to ESP32 TX (17)
const int RX_PIN = 16;
const int TX_PIN = 17;
const uint32_t GPS_BAUD = 9600;

TinyGPSPlus gps;
HardwareSerial gpsSerial(2); // Use UART2

bool loraInitialized = false;
bool gpsInitialized = false; 
unsigned long lastTransmissionTime = 0;
unsigned long lastGpsWarningTime = 0;

// --- SENSOR DATA ---
int currentBpm = 75;  // Simulated
float currentLat = 0.0; // Updated by GPS
float currentLng = 0.0; // Updated by GPS
int currentBatt = 95; // Simulated

const uint8_t PASSWORD_BYTE = 0x69;

// Data structure for LoRa transmission
struct __attribute__((packed)) DataPacket {
  uint8_t password_byte;
  uint8_t deviceID;
  uint8_t bpm;
  float lat;
  float lng;
  uint8_t batt; 
};

// Function prototypes
void attemptLoraConnection(void);
void sendDataToReceiver(void);
void signalInit(int count, int time_delay);

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("Setting up Transmitter_MOSFET_GPS_test...");
  pinMode(MOSFET_PIN, OUTPUT);

  // Initialize GPS Serial
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);

  // Initialize LoRa
  LoRa.setPins(NSS, RST, DI0);
  attemptLoraConnection();
 
  Serial.println("Setup done. Waiting for GPS fix and preparing to transmit...");
  delay(500);
  signalInit(2, 200);
}

void loop() {
  // 1. Continuously feed GPS data into the TinyGPS++ library
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // Non-blocking warning if GPS wiring is disconnected
  if (millis() > 5000 && gps.charsProcessed() < 10) {
    if (millis() - lastGpsWarningTime > 2000) {
      Serial.println("Error: No GPS detected. Check wiring.");
      lastGpsWarningTime = millis();
    }
  }

  // 2. Handle Timed LoRa Transmissions
  if (millis() - lastTransmissionTime >= TRANSMISSION_PERIOD) {
      lastTransmissionTime = millis();

      // Trigger MOSFET signal once when GPS finally gets a fix
      if (!gpsInitialized && gps.location.isValid()) {
        Serial.println(">>> SATELLITE FIX ACQUIRED! <<<");
        signalInit(3, 200); 
        gpsInitialized = true;
      }

      // Update Simulated Data
      currentBpm = random(60, 100);
      currentBatt = random(80, 100); 

      // Update GPS Data
      if (gps.location.isValid()) {
        currentLat = gps.location.lat();
        currentLng = gps.location.lng();
      } else {
        Serial.println("Waiting for Satellite Fix... (Sending 0.0 or last known coordinates)");
      }

      sendDataToReceiver();
  }
}

void signalInit(int count, int time_delay) {
  for (int i = 0; i < count; i++) {
    digitalWrite(MOSFET_PIN, HIGH);
    delay(time_delay);
    digitalWrite(MOSFET_PIN, LOW);
    delay(time_delay);
  }
}

void attemptLoraConnection() {
  Serial.println("Attempting LoRa connection...");
  digitalWrite(RST, LOW);  delay(100);
  digitalWrite(RST, HIGH); delay(100);

  while (!loraInitialized) {
    if (LoRa.begin(433E6)) {
      LoRa.setSyncWord(0x9E);
      LoRa.enableCrc();
      LoRa.setTxPower(2);
      loraInitialized = true;
      Serial.println("SUCCESS: Connected to LoRa.");
    }
    else {
      loraInitialized = false;
      Serial.println("FAILED: Re-attempting LoRa connection...");
      delay(500);
    }
  }
}

void sendDataToReceiver() {
  DataPacket packet;
  packet.password_byte = PASSWORD_BYTE;
  packet.deviceID = DEVICE_ID;
  packet.bpm = (uint8_t)currentBpm;
  packet.lat = currentLat;
  packet.lng = currentLng;
  packet.batt = (uint8_t)currentBatt;

  Serial.print("Transmitting Packet -> BPM: ");
  Serial.print(currentBpm);
  Serial.print(" | Batt: ");
  Serial.print(currentBatt);
  Serial.print("% | Lat: ");
  Serial.print(currentLat, 6); // Increased precision to 6 for actual GPS coords
  Serial.print(" | Lng: ");
  Serial.println(currentLng, 6);

  LoRa.beginPacket();
  LoRa.write((uint8_t*)&packet, sizeof(DataPacket));
 
  if (LoRa.endPacket()) {
      Serial.println("LoRa Status: SENT\n");
  } else {
      Serial.println("LoRa Status: ERROR\n");
  }
}