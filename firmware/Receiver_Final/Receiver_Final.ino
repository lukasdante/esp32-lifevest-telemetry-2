#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ==========================================
// --- HARDWARE & NETWORK CONFIGURATION ---
// ==========================================
#define NSS  4  
#define RST  5  
#define DIO0 2

#define USE_LOCAL false



#if USE_LOCAL
  #include <WiFiClient.h>
  const char* telemetryEndpoint = "http://192.168.1.15:8787/api/telemetry";
  const char* ackEndpoint = "http://192.168.1.15:8787/api/telemetry/ack";
  WiFiClient client;
  const char* ssid = "LAGRAMADA_2.4Ghz";
  const char* password = "ANJOHAMI07";
#else
  #include <WiFiClientSecure.h>
  const char* telemetryEndpoint = "https://smartlifevest.com/api/telemetry";
  const char* ackEndpoint = "https://smartlifevest.com/api/telemetry/ack";
  WiFiClientSecure client;
  const char* ssid = "ub-lifevest";
  const char* password = "ub-lifevest";
#endif

const uint8_t PASSWORD_BYTE = 0x69;

// --- GLOBAL OBJECTS ---

HTTPClient http;

// ==========================================
// --- DATA STRUCTURES (Must match Transmitter!) ---
// ==========================================
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
     
  uint8_t autoSignal;      
  uint8_t hrThreshold;     
  uint8_t signalDuration;  
  uint8_t actuateLed;  
  uint8_t actuateBuzzer;
  uint8_t proxyGps;       // <--- NEW: Must match Transmitter struct!
  uint8_t proxyHrMin;     // <--- NEW
  uint8_t proxyHrMax;     // <--- NEW
};

struct __attribute__((packed)) AckPacket {
  uint8_t password_byte; 
  uint8_t deviceID;
  uint8_t ackedCommandCode;
};

// --- STATE VARIABLES ---
bool loraInitialized = false;
bool wifiConnected = false;

bool isAwaitingAck = false;
CommandPacket activeCommand;
unsigned long lastCommandTxTime = 0;
int retryCount = 0;

// --- FUNCTION PROTOTYPES ---
void attemptLoraConnection();
void attemptWifiConnection();
void sendDataAndCheckPiggyback(DataPacket received, int rssi, float snr);
void sendCommandToEdge(CommandPacket cmd);
void fireCommandOverLoRa();
void showReceivedData(DataPacket received, int rssi, float snr);
int extractJsonInt(String json, String key);

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- Smart Lifevest Receiver (Piggyback Gateway) ---");

  pinMode(RST, OUTPUT);
  
  SPI.begin(18, 19, 23, NSS);
  LoRa.setPins(NSS, RST, DIO0);

  attemptLoraConnection(); 
  
  WiFi.begin(ssid, password);
  
  #if !USE_LOCAL
    client.setInsecure();
  #endif
   
  client.setTimeout(2); 
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnected) {
      Serial.println("WARNING: WiFi Disconnected.");
      wifiConnected = false;
    }
    attemptWifiConnection();
  } else if (!wifiConnected) {
    wifiConnected = true;
    Serial.println("SUCCESS: WiFi Connected.");
  }

  // RETRY LOGIC
  if (isAwaitingAck && (millis() - lastCommandTxTime > 3000)) {
    if (retryCount < 5) { 
      retryCount++;
      fireCommandOverLoRa();
    } else {
      Serial.println("FAILED: Transmitter unreachable. Dropping command.");
      isAwaitingAck = false; 
    }
  }

  // Listen for LoRa Packets
  int packetSize = LoRa.parsePacket();
  
  if (packetSize == sizeof(DataPacket)) {
    DataPacket received;
    LoRa.readBytes((uint8_t*)&received, sizeof(DataPacket));
    
    if (received.password_byte == PASSWORD_BYTE) {
      int packetRssi = LoRa.packetRssi();
      float packetSnr = LoRa.packetSnr();

      Serial.println("\n[LoRa] Valid Telemetry Received...");
      showReceivedData(received, packetRssi, packetSnr);
      
      if (wifiConnected) {
        sendDataAndCheckPiggyback(received, packetRssi, packetSnr);
      } else {
        Serial.println("SKIPPED POST: No WiFi connection.");
      }
    } else {
      Serial.println("\n[LoRa] REJECTED: Invalid Password Byte.");
    }
  }
  else if (packetSize == sizeof(AckPacket)) {
    AckPacket ack;
    LoRa.readBytes((uint8_t*)&ack, sizeof(AckPacket));
    
    if (ack.password_byte == PASSWORD_BYTE && isAwaitingAck && ack.ackedCommandCode == activeCommand.commandCode) {
      Serial.println("ACK RECEIVED! Transmitter fulfilled the request.");
      isAwaitingAck = false; 
      
      http.begin(client, ackEndpoint); 
      http.addHeader("Content-Type", "application/json");
      String ackJson = "{\"vest_id\":\"" + String(ack.deviceID) + "\"}";
      http.POST(ackJson);
      http.end();
    }
  }
}

// ==========================================
// THE PIGGYBACK LOGIC 
// ==========================================
void sendDataAndCheckPiggyback(DataPacket received, int rssi, float snr) {
  http.begin(client, telemetryEndpoint);
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(1500);

  String httpRequestData = "{\"vest_id\":\"" + String(received.deviceID) + "\"" +
                           ",\"heart_rate\":" + String(received.bpm) + 
                           ",\"latitude\":" + String(received.lat, 6) + 
                           ",\"longitude\":" + String(received.lng, 6) + 
                           ",\"rssi\":" + String(rssi) + 
                           ",\"snr\":" + String(snr, 2) + "}";

  Serial.println(">>> Sending HTTP POST to Web...");
  int httpResponseCode = http.POST(httpRequestData);
  
  if (httpResponseCode == 200 || httpResponseCode == 201) {
    String payload = http.getString();
    
    int cmdCode = extractJsonInt(payload, "cmd"); 
    
    if (cmdCode > 0) {
      Serial.printf("⚠️ PIGGYBACK COMMAND RECEIVED: #%d\n", cmdCode);
      
      CommandPacket cmd;
      cmd.password_byte = PASSWORD_BYTE; 
      cmd.targetDeviceID = received.deviceID;
      cmd.commandCode = cmdCode;
      
      if (cmdCode == 2) {
        cmd.autoSignal     = extractJsonInt(payload, "auto");
        cmd.hrThreshold    = extractJsonInt(payload, "hr");
        cmd.signalDuration = extractJsonInt(payload, "dur");
        cmd.actuateLed     = extractJsonInt(payload, "led");
        cmd.actuateBuzzer  = extractJsonInt(payload, "buz");
        // <-- NEW: Extract Proxy Data from JSON 
        cmd.proxyGps       = extractJsonInt(payload, "pgps");
        cmd.proxyHrMin     = extractJsonInt(payload, "pmin");
        cmd.proxyHrMax     = extractJsonInt(payload, "pmax");
      } else {
        cmd.autoSignal = 0; 
        cmd.hrThreshold = 100;  
        cmd.signalDuration = 2;
        cmd.actuateLed = 1;
        cmd.actuateBuzzer = 1;
        cmd.proxyGps = 0;
        cmd.proxyHrMin = 0;
        cmd.proxyHrMax = 0;
      }

      sendCommandToEdge(cmd);
    } else {
      Serial.println("Server OK: No pending commands.");
    }
  } else {
    Serial.printf("Web Error: HTTP %d\n", httpResponseCode);
  }
  
  http.end();
}

// Triple Firing Logic
void sendCommandToEdge(CommandPacket cmd) {
  activeCommand = cmd;
  isAwaitingAck = true;
  retryCount = 0;
  
  fireCommandOverLoRa();
}

void fireCommandOverLoRa() {
  Serial.printf(">>> Firing Command #%d (Attempt %d)...\n", activeCommand.commandCode, retryCount + 1);
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&activeCommand, sizeof(CommandPacket));
  LoRa.endPacket(); 
  
  lastCommandTxTime = millis();
  LoRa.receive(); 
}

// ==========================================
// UTILITIES & HELPERS
// ==========================================
void attemptLoraConnection() {
  while (!loraInitialized) {
    digitalWrite(RST, LOW);  delay(10);
    digitalWrite(RST, HIGH); delay(10);

    if (LoRa.begin(433E6)) {
      LoRa.setSyncWord(0x9E);
      LoRa.enableCrc();
      LoRa.setTxPower(17); 
      loraInitialized = true;
      Serial.println("SUCCESS: LoRa Receiver Active.");
    } else {
      Serial.println("FAILED: LoRa Error. Retrying...");
      delay(1000);
    }
  }
}

void attemptWifiConnection() {
  static unsigned long lastWifiAttempt = 0;
  if (millis() - lastWifiAttempt > 5000) {
    WiFi.begin(ssid, password);
    lastWifiAttempt = millis();
    Serial.println("Attempting WiFi Connection...");
  }
}

int extractJsonInt(String json, String key) {
  String searchKey = "\"" + key + "\":";
  int startIdx = json.indexOf(searchKey);
  
  if (startIdx == -1) {
    searchKey = "\\\"" + key + "\\\":";
    startIdx = json.indexOf(searchKey);
    if (startIdx == -1) return -1;
  }
  
  startIdx += searchKey.length();
  int endIdx1 = json.indexOf(",", startIdx);
  int endIdx2 = json.indexOf("}", startIdx);
  int endIdx3 = json.indexOf("\\\"", startIdx); 
  
  int endIdx = json.length();
  if (endIdx1 != -1 && endIdx1 < endIdx) endIdx = endIdx1;
  if (endIdx2 != -1 && endIdx2 < endIdx) endIdx = endIdx2;
  if (endIdx3 != -1 && endIdx3 < endIdx) endIdx = endIdx3;
  
  return json.substring(startIdx, endIdx).toInt();
}

void showReceivedData(DataPacket received, int rssi, float snr) {
  Serial.printf("ID: %d | BPM: %d | GPS: %.6f, %.6f\n", 
                received.deviceID, received.bpm, received.lat, received.lng);
  Serial.printf("Signal Quality - RSSI: %d | SNR: %.2f\n", rssi, snr);
  Serial.println("---------------------------------");
}