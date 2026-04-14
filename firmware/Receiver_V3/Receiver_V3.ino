#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define NSS  4  
#define RST  5  
#define DIO0 2

const char* ssid = "LAGRAMADA_2.4Ghz"; // CHANGE: ub-lifevest
const char* password = "ANJOHAMI07"; // CHANGE: ub-lifevest
const char* endpoint = "http://192.168.1.10:8000/api/update-telemetry/"; // CHANGE: https://smartlifevest.com/api/update-telemetry/
const uint8_t PASSWORD_BYTE = 0x69;

struct __attribute__((packed)) DataPacket {
  uint8_t password_byte;
  uint8_t deviceID;
  uint8_t bpm;
  float lat;
  float lng;
};

bool loraInitialized = false;
bool wifiConnected = false;

void attemptLoraConnection(void);
void attemptWifiConnection(void);
void showReceivedData(DataPacket);
void sendDataToEndpoint(DataPacket);
void showSignalQuality(void);

void setup() {
  Serial.begin(115200);

  Serial.println("Setting up receiver station...");
  pinMode(RST, OUTPUT);
  SPI.begin(18, 19, 23, NSS);
  LoRa.setPins(NSS, RST, DIO0);

  attemptLoraConnection();
  attemptWifiConnection();
  Serial.println("Setup done, listening for incoming data...");
}

void loop() {
  int packetSize = LoRa.parsePacket();
 
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    attemptWifiConnection();
  }

  if (wifiConnected == false && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("SUCCESS: WiFi connection established.");
  }
 

  if (packetSize) {
    // Check if packet size matches our expected struct size
    if (packetSize == sizeof(DataPacket)) {
      Serial.println("New data received, parsing it...");
     
      // Put received data in the DataPacket struct
      DataPacket received;
      LoRa.readBytes((uint8_t*)&received, sizeof(DataPacket));
     
      if (received.password_byte == PASSWORD_BYTE) {
        Serial.println("Valid data received, parsing it...");
        showReceivedData(received);
        showSignalQuality();

        // Now handle the Wi-Fi transmission
        if (WiFi.status() == WL_CONNECTED) {
          sendDataToEndpoint(received);
        } else {
          Serial.println("ERROR: WiFi is disconnected.");
        }
      }
    }
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
      loraInitialized = true;
      Serial.println("SUCCESS: Connected to LoRa.");
    }
   
    else {
      loraInitialized = false;
      Serial.println("FAILED: Re-attempting connection...");
    }
  }
}

void attemptWifiConnection() {
  static unsigned long lastWifiAttempt = 0;
 
  // Only try to connect if not already connected AND it's been 10 seconds since last try
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > 5000) {
    Serial.println("WiFi disconnected. Re-attempting in background...");
    WiFi.begin(ssid, password);
    lastWifiAttempt = millis();
  }


}

void sendDataToEndpoint(DataPacket received) {
  // 1. Use standard WiFiClient for local HTTP
  WiFiClient client;
  HTTPClient http;
 
  http.begin(client, endpoint);
  http.addHeader("Content-Type", "application/json");
 
  // 2. Build JSON string (Removed quotes around deviceID to send as integer)
  String httpRequestData;
  httpRequestData.reserve(150);
  httpRequestData = "{";
  httpRequestData += "\"vest_id\":" + String(received.deviceID) + ",";
  httpRequestData += "\"heart_rate\":" + String(received.bpm) + ",";
  httpRequestData += "\"latitude\":" + String(received.lat, 6) + ",";
  httpRequestData += "\"longitude\":" + String(received.lng, 6);
  httpRequestData += "}";

  Serial.print("Sending payload: ");
  Serial.println(httpRequestData);

  // Send HTTP POST request
  int httpResponseCode = http.POST(httpRequestData);

  // Print return code (201 is Success/Created)
  if (httpResponseCode > 0) {
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    String payload = http.getString();
    Serial.println(payload);
  }
  else {
    Serial.print("ERROR: HTTP code ");
    Serial.println(httpResponseCode);
  }
 
  http.end();
}

void showReceivedData(DataPacket received) {
  Serial.print("Device ID: ");
  Serial.println(received.deviceID);

  Serial.print("HEART RATE: ");
  Serial.print(received.bpm);
  Serial.println(" BPM");
 
  Serial.print("LOCATION:   ");
  Serial.print(received.lat, 6);
  Serial.print(", ");
  Serial.println(received.lng, 6);
 
  Serial.print("MAP LINK:   https://www.google.com/maps/search/?api=1&query=");
  Serial.print(received.lat, 6);
  Serial.print(",");
  Serial.println(received.lng, 6);
}

void showSignalQuality() {
  Serial.print("STRENGTH:   ");
  Serial.print(LoRa.packetRssi());
  Serial.print(" dBm (SNR: ");
  Serial.print(LoRa.packetSnr());
  Serial.println(")");
  Serial.println("---------------------------------");
}
