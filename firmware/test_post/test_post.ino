#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> // 1. NEW: Include the secure client library

// 1. SET YOUR WI-FI
const char* ssid = "LAGRAMADA_2.4Ghz";
const char* password = "ANJOHAMI07";

// 2. SET YOUR LIVE SECURE ENDPOINT! 
const char* endpoint = "https://www.smartlifevest.com/api/telemetry"; 

// 3. SET THE HARDWARE ID (Must exist in your remote Cloudflare database)
const char* vest_id = "HW-ESP32-001";

// Simulation State Variables
float currentLat = 14.599500;
float currentLng = 120.984200;
unsigned long lastSimulationTime = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n--- ESP32 HTTPS LIVE SIMULATOR ---");
  
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nSUCCESS: WiFi Connected!");
  Serial.print("ESP32 IP Address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // Fire a ping every 3 seconds
  if (millis() - lastSimulationTime > 3000) {
    if (WiFi.status() == WL_CONNECTED) {
      
      // Simulate movement and changing vitals
      currentLat += 0.000050;
      currentLng += 0.000050;
      int fakeHR = random(75, 115);
      int fakeBatt = random(85, 100);
      int fakeRssi = random(-110, -80);
      float fakeSnr = 9.5;

      sendSimulatedData(fakeHR, fakeBatt, fakeRssi, fakeSnr);
      
    } else {
      Serial.println("WiFi Disconnected! Reconnecting...");
      WiFi.reconnect();
    }
    lastSimulationTime = millis();
  }
}

void sendSimulatedData(int hr, int batt, int rssi, float snr) {
  // 2. NEW: Use the Secure Client
  WiFiClientSecure client; 
  
  // 3. NEW: Tell the ESP32 not to worry if Cloudflare rotates its SSL certificate
  client.setInsecure(); 
  
  HTTPClient http;
  
  // Connect using the secure client to your https:// endpoint
  http.begin(client, endpoint);
  http.addHeader("Content-Type", "application/json");

  // Build the JSON Payload
  String payload;
  payload.reserve(200);
  payload = "{";
  payload += "\"vest_id\":\"" + String(vest_id) + "\","; 
  payload += "\"latitude\":" + String(currentLat, 6) + ",";
  payload += "\"longitude\":" + String(currentLng, 6) + ",";
  payload += "\"heart_rate\":" + String(hr) + ",";
  payload += "\"batt\":" + String(batt) + ",";
  payload += "\"rssi\":" + String(rssi) + ",";
  payload += "\"snr\":" + String(snr, 1);
  payload += "}";

  Serial.print("\nTransmitting to LIVE SERVER: ");
  Serial.println(payload);

  // Send the secure POST request
  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.print("HTTP Code: ");
    Serial.println(httpResponseCode);

    // --- PIGGYBACK INTERCEPTION CHECK ---
    if (response.indexOf("\"command\":\"SIGNAL\"") != -1) {
      Serial.println("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
      Serial.println("🔥 PIGGYBACK COMMAND RECEIVED: ACTIVATE SIGNAL!");
      Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    }
  } else {
    Serial.print("ERROR! HTTP Code: ");
    Serial.println(httpResponseCode);
    Serial.println("Check your Wi-Fi connection and ensure the domain is correct.");
  }

  http.end();
}