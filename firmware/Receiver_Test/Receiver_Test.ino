#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <LoRa.h>

// 1. SET YOUR WI-FI
const char* ssid = "LAGRAMADA_2.4Ghz";
const char* password = "ANJOHAMI07";

// 2. SET YOUR LIVE SECURE ENDPOINT! 
const char* endpoint = "https://www.smartlifevest.com/api/telemetry"; 

// 3. EXACT LORA PINS USED BY YOUR HARDWARE
#define NSS 4
#define RST 5
#define DI0 2
#define BAND 433E6 

// 4. DATA STRUCTURE (Must exactly match the Transmitter)
const uint8_t PASSWORD_BYTE = 0x69;

struct __attribute__((packed)) DataPacket {
  uint8_t password_byte;
  uint8_t deviceID;
  uint8_t bpm;
  float lat;
  float lng;
  uint8_t batt; // <-- Added battery data to match
};

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n--- ESP32 LORA TO HTTPS GATEWAY ---");
  
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nSUCCESS: WiFi Connected!");
  Serial.print("ESP32 IP Address: ");
  Serial.println(WiFi.localIP());

  LoRa.setPins(NSS, RST, DI0);
  Serial.print("Initializing LoRa... ");
  if (!LoRa.begin(BAND)) {
    Serial.println("FAILED!");
    Serial.println("Check your wiring or pin definitions.");
    while (1); 
  }
  
  LoRa.setSyncWord(0x9E); 
  LoRa.enableCrc();
  
  Serial.println("SUCCESS: Listening for LoRa Packets...");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected! Reconnecting...");
    WiFi.reconnect();
    delay(5000); 
    return;      
  }

  int packetSize = LoRa.parsePacket();
  
  if (packetSize) {
    if (packetSize == sizeof(DataPacket)) {
      DataPacket rxPacket;
      
      LoRa.readBytes((uint8_t *)&rxPacket, sizeof(DataPacket));
      
      if (rxPacket.password_byte == PASSWORD_BYTE) {
        Serial.println("\n[LoRa] Valid Packet Received!");
        
        int loraRssi = LoRa.packetRssi();
        float loraSnr = LoRa.packetSnr();

        String formatted_vest_id = "HW-ESP32-00" + String(rxPacket.deviceID);
        
        // NEW: Grab the actual battery data sent over LoRa
        String realBatt = String(rxPacket.batt);

        sendDataToCloud(
          formatted_vest_id, 
          String(rxPacket.lat, 6), 
          String(rxPacket.lng, 6), 
          String(rxPacket.bpm), 
          realBatt, // Pass it to the cloud function
          loraRssi, 
          loraSnr
        );
      } else {
         Serial.println("[LoRa] Ignored packet: Incorrect Password Byte.");
      }
    } else {
      Serial.print("[LoRa] Ignored packet: Size mismatch. Expected ");
      Serial.print(sizeof(DataPacket));
      Serial.print(" bytes, but got ");
      Serial.println(packetSize);
    }
  }
}

void sendDataToCloud(String v_id, String lat, String lng, String hr, String batt, int rssi, float snr) {
  WiFiClientSecure client; 
  client.setInsecure(); 
  HTTPClient http;
  
  http.begin(client, endpoint);
  http.addHeader("Content-Type", "application/json");

  String payload = "{";
  payload += "\"vest_id\":\"" + v_id + "\","; 
  payload += "\"latitude\":" + lat + ",";
  payload += "\"longitude\":" + lng + ",";
  payload += "\"heart_rate\":" + hr + ",";
  payload += "\"batt\":" + batt + ",";
  payload += "\"rssi\":" + String(rssi) + ",";
  payload += "\"snr\":" + String(snr, 1);
  payload += "}";

  Serial.print("Transmitting to LIVE SERVER: ");
  Serial.println(payload);

  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.print("HTTP Code: ");
    Serial.println(httpResponseCode);

    if (response.indexOf("\"command\":\"SIGNAL\"") != -1) {
      Serial.println("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
      Serial.println("🔥 PIGGYBACK COMMAND RECEIVED: ACTIVATE SIGNAL!");
      Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    }
  } else {
    Serial.print("ERROR! HTTP Code: ");
    Serial.println(httpResponseCode);
  }

  http.end();
}