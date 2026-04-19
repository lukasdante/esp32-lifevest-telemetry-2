#include <SPI.h>
#include <LoRa.h>

#define NSS 4
#define RST 5
#define DI0 2
#define MOSFET_PIN 26

#define TRANSMISSION_PERIOD 3000
#define DEVICE_ID 1

bool loraInitialized = false;
bool gpsInitialized = false; 
unsigned long lastTransmissionTime = 0;

// --- PROXY DATA ---
int proxyBpm = 75;
float proxyLat = 14.6760; 
float proxyLng = 121.0437;
int proxyBatt = 95; // NEW: Proxy battery level

const uint8_t PASSWORD_BYTE = 0x69;

// NEW: Updated Struct with battery
struct __attribute__((packed)) DataPacket {
  uint8_t password_byte;
  uint8_t deviceID;
  uint8_t bpm;
  float lat;
  float lng;
  uint8_t batt; // <-- Added battery data
};

void attemptLoraConnection(void);
void sendDataToReceiver(void);
void signalInit(int count, int time_delay);

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("Setting up transmitter TEST station...");
  pinMode(MOSFET_PIN, OUTPUT);

  LoRa.setPins(NSS, RST, DI0);
  attemptLoraConnection();
 
  Serial.println("Setup done, preparing to transmit proxy data...");
  delay(500);
  signalInit(2, 200);
}

void loop() {
  if (millis() - lastTransmissionTime >= TRANSMISSION_PERIOD) {
      lastTransmissionTime = millis();

      if (!gpsInitialized) {
        signalInit(3, 200); 
        gpsInitialized = true;
      }

      proxyBpm = random(60, 100);
      proxyBatt = random(80, 100); // Simulate battery draining/fluctuating

      sendDataToReceiver();
  }
}

void signalInit(int count, int time_delay) {
  for (int i=0; i<count; i++) {
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
      Serial.println("FAILED: Re-attempting connection...");
      delay(500);
    }
  }
}

void sendDataToReceiver() {
  DataPacket packet;
  packet.password_byte = PASSWORD_BYTE;
  packet.deviceID = DEVICE_ID;
  packet.bpm = (uint8_t)proxyBpm;
  packet.lat = proxyLat;
  packet.lng = proxyLng;
  packet.batt = (uint8_t)proxyBatt; // NEW: Pack battery into struct

  Serial.print("Transmitting Proxy Packet -> BPM: ");
  Serial.print(proxyBpm);
  Serial.print(" | Batt: ");
  Serial.print(proxyBatt);
  Serial.print("% | Lat: ");
  Serial.print(proxyLat, 4);
  Serial.print(" | Lng: ");
  Serial.println(proxyLng, 4);

  LoRa.beginPacket();
  LoRa.write((uint8_t*)&packet, sizeof(DataPacket));
 
  if (LoRa.endPacket()) {
      Serial.println("LoRa Status: SENT\n");
  } else {
      Serial.println("LoRa Status: ERROR\n");
  }
}