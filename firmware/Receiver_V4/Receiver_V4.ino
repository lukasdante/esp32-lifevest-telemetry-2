#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// --- PIN DEFINITIONS ---
#define NSS  4  
#define RST  5  
#define DIO0 2

// --- NETWORK CONFIGURATION ---
const char* ssid = "ub-lifevest"; 
const char* password = "ub-lifevest"; 
const char* telemetryEndpoint = "https://smartlifevest.com/api/update-telemetry/";

const uint8_t PASSWORD_BYTE = 0x69;

// --- GLOBAL OBJECTS ---
WiFiClientSecure client; 
HTTPClient http;

// --- DATA STRUCTURES ---
struct __attribute__((packed)) DataPacket {
  uint8_t password_byte;
  uint8_t deviceID;
  uint8_t bpm;
  float lat;
  float lng;
};

struct __attribute__((packed)) CommandPacket {
  uint8_t password_byte;
  uint8_t targetDeviceID;
  uint8_t commandCode;
};

// --- STATE VARIABLES ---
bool loraInitialized = false;
bool wifiConnected = false;

// --- FUNCTION PROTOTYPES ---
void attemptLoraConnection();
void attemptWifiConnection();
void sendDataAndCheckPiggyback(DataPacket received);
void sendCommandToEdge(uint8_t targetID, uint8_t cmdCode);
void showReceivedData(DataPacket received);
void showSignalQuality();

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- Smart Lifevest Receiver (Piggyback Mode) ---");

  pinMode(RST, OUTPUT);
  
  SPI.begin(18, 19, 23, NSS);
  LoRa.setPins(NSS, RST, DIO0);

  attemptLoraConnection();
  
  WiFi.begin(ssid, password);
  client.setInsecure(); 
  client.setTimeout(2); 
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    attemptWifiConnection();
  } else if (!wifiConnected) {
    wifiConnected = true;
    Serial.println("SUCCESS: WiFi Connected.");
  }

  // Receiver ONLY acts when it hears the Vest
  int packetSize = LoRa.parsePacket();
  if (packetSize == sizeof(DataPacket)) {
    DataPacket received;
    LoRa.readBytes((uint8_t*)&received, sizeof(DataPacket));
    
    if (received.password_byte == PASSWORD_BYTE) {
      Serial.println("\n[LoRa] Telemetry In...");
      showReceivedData(received);
      
      if (WiFi.status() == WL_CONNECTED) {
        sendDataAndCheckPiggyback(received);
      }
    }
  }
}

// ==========================================
// THE PIGGYBACK LOGIC
// ==========================================

void sendDataAndCheckPiggyback(DataPacket received) {
  http.begin(client, telemetryEndpoint);
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(1500);

  String httpRequestData = "{\"vest_id\":" + String(received.deviceID) + 
                           ",\"heart_rate\":" + String(received.bpm) + 
                           ",\"latitude\":" + String(received.lat, 6) + 
                           ",\"longitude\":" + String(received.lng, 6) + "}";

  Serial.println(">>> Sending Telemetry & Checking for Commands...");
  int httpResponseCode = http.POST(httpRequestData);
  
  if (httpResponseCode == 200 || httpResponseCode == 201) {
    String payload = http.getString();
    
    // Look for the "command" field in the server response
    int cmdIndex = payload.indexOf("\"command\":");
    if (cmdIndex != -1) {
      // Extract the integer value after the colon
      int commandInt = payload.substring(cmdIndex + 10).toInt();
      
      if (commandInt > 0) {
        Serial.printf("⚠️ PIGGYBACK COMMAND RECEIVED: #%d\n", commandInt);
        sendCommandToEdge(received.deviceID, (uint8_t)commandInt);
      } else {
        Serial.println("No pending commands on server.");
      }
    }
  } else {
    Serial.printf("Web Error: %d\n", httpResponseCode);
  }
  
  http.end();
}

void sendCommandToEdge(uint8_t targetID, uint8_t cmdCode) {
  CommandPacket cmd = {PASSWORD_BYTE, targetID, cmdCode};

  Serial.println("Triple-Firing Command to Vest...");
  for(int i = 0; i < 3; i++) {
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&cmd, sizeof(CommandPacket));
    LoRa.endPacket(); 
    delay(80); 
  }
  
  LoRa.receive(); 
  Serial.println("Done.");
}

// ==========================================
// UTILITIES
// ==========================================

void attemptLoraConnection() {
  digitalWrite(RST, LOW);  delay(100);
  digitalWrite(RST, HIGH); delay(100);
  while (!loraInitialized) {
    if (LoRa.begin(433E6)) {
      LoRa.setSyncWord(0x9E);
      LoRa.enableCrc();
      loraInitialized = true;
      Serial.println("SUCCESS: LoRa Active.");
    } else {
      delay(2000);
    }
  }
}

void attemptWifiConnection() {
  static unsigned long lastWifiAttempt = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > 5000) {
    WiFi.begin(ssid, password);
    lastWifiAttempt = millis();
  }
}

void showReceivedData(DataPacket received) {
  Serial.printf("ID: %d | BPM: %d | GPS: %.6f, %.6f\n", 
                received.deviceID, received.bpm, received.lat, received.lng);
}

void showSignalQuality() {
  Serial.printf("RSSI: % d | SNR: %.2f\n", LoRa.packetRssi(), LoRa.packetSnr());
  Serial.println("---------------------------------");
}