#include <TinyGPSPlus.h>
#include <SPI.h>
#include <LoRa.h>
#include <Preferences.h>

// ==========================================
// --- HARDWARE CONFIGURATION ---
// ==========================================
#define NSS 4
#define RST 5
#define DIO0 2

#define LED_PIN 25
#define BUZZER_PIN 26

// AD8232 Heart Rate Sensor Pins 
#define SENSOR_PIN 34
#define LO_PLUS_PIN 35
#define LO_MINUS_PIN 32

// ESP32 PWM Settings (For ESP32 Core v3.x)
#define PWM_FREQ 2000
#define PWM_RES 8
#define PWM_DUTY 128 // 50% Duty cycle to limit power draw

// ==========================================
// --- DEVICE-SPECIFIC CONSTANTS ---
// ==========================================
#define DEVICE_ID 1
#define PASSWORD_BYTE 0x69 

// --- OBJECTS ---
TinyGPSPlus gps;
HardwareSerial gpsSerial(2); // UART2 (GPIO 16, 17)
Preferences preferences;

// ==========================================
// --- ADJUSTABLE GLOBAL VARIABLES (Settings) ---
// ==========================================
bool AUTOMATED_SIGNALING = false;
int HEART_RATE_THRESHOLD = 100;
int SIGNALING_DURATION = 2;
bool ACTUATE_LED = true; // <--- NEW SETTING
bool ACTUATE_BUZZER = true;

int UPDATE_PERIOD = 3000;

// ==========================================
// --- TELEMETRY & STATE VARIABLES ---
// ==========================================
bool loraInitialized = false;
bool gpsInitialized = false;
unsigned long lastGpsLoraTime = 0;
unsigned long lastGpsReceiveTime = 0;

// --- Heart Rate State Variables ---
uint8_t currentBPM = 0;       
int rawBPM = 0;               
bool isPeak = false;
unsigned long lastBeatTime = 0;

// --- Adaptive Threshold Variables ---
int P = 2048;             
int T = 2048;             
int adaptiveThreshold = 2500; 
int amplitude = 100;      

// --- 3-Second Median Filter Variables ---
unsigned long lastBatchTime = 0;
int bpmBuffer[20];        
int beatCount = 0;

// Async Signaling State
bool isSignaling = false;
int signalStepTotal = 0;
int signalStepCount = 0;
int signalInterval = 0;
unsigned long lastSignalToggle = 0;
bool signalState = false;

// ==========================================
// --- DATA STRUCTURES ---
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
  uint8_t actuateLed;      // <--- NEW COMMAND VARIABLE
  uint8_t actuateBuzzer;
};

struct __attribute__((packed)) AckPacket {
  uint8_t password_byte;
  uint8_t deviceID;
  uint8_t ackedCommandCode;
};

// ==========================================
// --- FUNCTION PROTOTYPES ---
// ==========================================
void initHardware();
void attemptLoraConnection();
void updateGPS();
void processHeartRate();
void sendDataToReceiver();
void listenForCommands();
void executeCommand(CommandPacket cmd);
void startAsyncSignal(float duration_seconds, int time_delay);
void updateAsyncSignal();
void setPWM(bool state);
void showTransmittedData(DataPacket packet);
void sendAck(uint8_t cmdCode);

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- Smart Lifevest Transmitter (Edge) ---");

  // ---> NEW: Open memory and load settings
  preferences.begin("vest-settings", false); // "false" means Read/Write mode
  
  // Load variables (Key Name, Default Value if empty)
  AUTOMATED_SIGNALING = preferences.getBool("autoSignal", false);
  HEART_RATE_THRESHOLD = preferences.getUInt("hrThreshold", 100);
  SIGNALING_DURATION = preferences.getUInt("sigDuration", 2);
  ACTUATE_LED = preferences.getBool("actuateLed", true);
  ACTUATE_BUZZER = preferences.getBool("actuateBuzzer", true);

  Serial.println("Loaded Saved Settings!");
  Serial.print("Automated Signaling: ");  Serial.println(AUTOMATED_SIGNALING);
  Serial.print("Heart Rate Threshold: "); Serial.println(HEART_RATE_THRESHOLD);
  Serial.print("Signaling Duration: "); Serial.println(SIGNALING_DURATION);
  Serial.print("LED Actuation: "); Serial.println(ACTUATE_LED); 
  Serial.print("Buzzer Actuation: "); Serial.println(ACTUATE_BUZZER);  
  // <--- END NEW1

  initHardware();
  attemptLoraConnection();

  startAsyncSignal(1, 500); 
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  // 1. BACKGROUND TASKS (Non-Blocking)
  updateAsyncSignal();  
  listenForCommands();  
  updateGPS();          

  // 2. HEART RATE SAMPLING (Every 10ms)
  static unsigned long lastSampleTime = 0;
  if (millis() - lastSampleTime >= 10) {
    lastSampleTime = millis();
    processHeartRate(); 
  }

  // 3. TELEMETRY TRANSMISSION 
  if (millis() - lastGpsLoraTime >= UPDATE_PERIOD) {
    lastGpsLoraTime = millis();
    // sendDataToReceiver(); 
    sendProxyData();
  }
}

// ==========================================
// HARDWARE INITIALIZATION
// ==========================================
void initHardware() {
  ledcAttach(LED_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(BUZZER_PIN, PWM_FREQ, PWM_RES);

  setPWM(false); 

  pinMode(LO_PLUS_PIN, INPUT);
  pinMode(LO_MINUS_PIN, INPUT);

  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);
  LoRa.setPins(NSS, RST, DIO0);
}

// ==========================================
// HEART RATE SENSOR LOGIC
// ==========================================
void processHeartRate() {
  unsigned long currentTime = millis();
  unsigned long timeSinceLastBeat = currentTime - lastBeatTime;

  // 1. Leads disconnected check 
  if (digitalRead(LO_PLUS_PIN) == 1 || digitalRead(LO_MINUS_PIN) == 1) {
    rawBPM = 0;
    currentBPM = 0; 
    isPeak = false; 
    adaptiveThreshold = 2048; P = 2048; T = 2048;
    beatCount = 0;
    return;
  }

  int ecgValue = analogRead(SENSOR_PIN);

  // --- ADAPTIVE THRESHOLD ---
  if (ecgValue < adaptiveThreshold && timeSinceLastBeat > 300) {
    if (ecgValue < T) T = ecgValue; 
  }
  if (ecgValue > adaptiveThreshold && ecgValue > P) {
    P = ecgValue; 
  }

  // --- PEAK DETECTION ---
  if (timeSinceLastBeat > 300) { 
    if (ecgValue > adaptiveThreshold && !isPeak) {
      isPeak = true;
      
      rawBPM = 60000 / timeSinceLastBeat; 
      lastBeatTime = currentTime;             
      
      if (beatCount < 20) {
        bpmBuffer[beatCount] = rawBPM;
        beatCount++;
      }

      amplitude = P - T;
      adaptiveThreshold = T + (amplitude * 0.7); 
      P = adaptiveThreshold;
      T = adaptiveThreshold; 
    }
  }

  if (ecgValue < adaptiveThreshold && isPeak) {
    isPeak = false;
  }

  if (timeSinceLastBeat > 2500) {
    adaptiveThreshold = 2048; P = 2048; T = 2048;
    lastBeatTime = currentTime; 
    rawBPM = 0;             
  }

  // ==========================================
  // --- 3-SECOND MEDIAN FILTER LOGIC ---
  // ==========================================
  if (currentTime - lastBatchTime >= 3000) {
    
    if (beatCount > 0) {
      for (int i = 1; i < beatCount; ++i) {
        int key = bpmBuffer[i];
        int j = i - 1;
        while (j >= 0 && bpmBuffer[j] > key) {
          bpmBuffer[j + 1] = bpmBuffer[j];
          j = j - 1;
        }
        bpmBuffer[j + 1] = key;
      }
      
      currentBPM = bpmBuffer[beatCount / 2];
      beatCount = 0;
    } else {
      currentBPM = 0;
    }

    if (AUTOMATED_SIGNALING && currentBPM > HEART_RATE_THRESHOLD && !isSignaling) {
      startAsyncSignal(SIGNALING_DURATION, 100);
    }
    
    lastBatchTime = currentTime;
  }
}

// ==========================================
// ASYNC SIGNALING 
// ==========================================
void startAsyncSignal(float duration_seconds, int time_delay) {
  isSignaling = true;
  signalInterval = time_delay;
  
  int total_ms = (int)(duration_seconds * 1000.0);
  signalStepTotal = total_ms / signalInterval; 
  
  signalStepCount = 0;
  lastSignalToggle = millis();
  
  signalState = true;
  setPWM(true);
}

void updateAsyncSignal() {
  if (!isSignaling) return;

  if (millis() - lastSignalToggle >= signalInterval) {
    lastSignalToggle = millis();
    signalState = !signalState;
    
    setPWM(signalState);
    
    signalStepCount++;
    if (signalStepCount >= signalStepTotal) {
      isSignaling = false;
      setPWM(false); 
    }
  }
}

// ---> NEW: Independent LED Logic
void setPWM(bool state) {
  if (state) {
    if (ACTUATE_LED) {
      ledcWrite(LED_PIN, PWM_DUTY); // Only turn on if setting allows
    }
    if (ACTUATE_BUZZER) {
      ledcWrite(BUZZER_PIN, PWM_DUTY);  // Buzzer always turns on
    }
  } else {
    ledcWrite(LED_PIN, 0);            // Always turn off to ensure no ghosting
    ledcWrite(BUZZER_PIN, 0);
  }
}

// ==========================================
// CORE LORA & GPS FUNCTIONS
// ==========================================
void attemptLoraConnection() {
  while (!loraInitialized) {
    digitalWrite(RST, LOW);  delay(10);
    digitalWrite(RST, HIGH); delay(10);

    if (LoRa.begin(433E6)) {
      LoRa.setSyncWord(0x9E);
      LoRa.enableCrc();
      LoRa.setTxPower(17);
      LoRa.receive(); 
      loraInitialized = true;
      Serial.println("SUCCESS: LoRa Active.");
    } else {
      Serial.println("FAILED: LoRa Error. Retrying in 1 second...");
      delay(1000); 
    }
  }
}

void updateGPS() {
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
    lastGpsReceiveTime = millis();
  }

  if (millis() - lastGpsReceiveTime > 5000) {
    gpsInitialized = false;
  } else if (gps.location.isValid() && gps.location.age() < 5000) {
    if (!gpsInitialized) {
      Serial.println("GPS Fix Acquired!");
      startAsyncSignal(1.5, 250); 
      gpsInitialized = true;
    }
  }
}

void listenForCommands() {
  if (!loraInitialized) return;

  int packetSize = LoRa.parsePacket();
  
  // If we hear ANYTHING on the radio, print it out immediately
  if (packetSize > 0) {
    Serial.print("\n--- [LoRa] PACKET HEARD! Size: ");
    Serial.print(packetSize);
    Serial.print(" bytes (Expected: ");
    Serial.print(sizeof(CommandPacket));
    Serial.println(" bytes) ---");

    if (packetSize == sizeof(CommandPacket)) {
      CommandPacket cmd;
      LoRa.readBytes((uint8_t*)&cmd, sizeof(CommandPacket));
      
      Serial.print("Password Byte: 0x"); Serial.print(cmd.password_byte, HEX);
      Serial.print(" | Target ID: "); Serial.print(cmd.targetDeviceID);
      Serial.print(" | Command Code: "); Serial.println(cmd.commandCode);

      if (cmd.password_byte == PASSWORD_BYTE && cmd.targetDeviceID == DEVICE_ID) {
        Serial.println("-> AUTHENTICATED! Executing...");
        executeCommand(cmd);
      } else {
        Serial.println("-> REJECTED: Password or Device ID did not match.");
      }
    } else {
      Serial.println("-> REJECTED: Packet size mismatch. Ignoring data.");
      // Clear the buffer just in case
      while(LoRa.available()) LoRa.read(); 
    }
  }
}

void executeCommand(CommandPacket cmd) {
  Serial.print("Command Received: "); Serial.println(cmd.commandCode);
  
  switch(cmd.commandCode) {
    case 1: 
      startAsyncSignal(SIGNALING_DURATION, 100);
      Serial.println("Activating signaling!");
      break;
      
    case 2: 
      // 1. Update the variables in RAM
      AUTOMATED_SIGNALING = (cmd.autoSignal == 1);
      HEART_RATE_THRESHOLD = cmd.hrThreshold;
      SIGNALING_DURATION = cmd.signalDuration;
      ACTUATE_LED = (cmd.actuateLed == 1); 
      ACTUATE_BUZZER = (cmd.actuateBuzzer == 1);
      // 2. ---> NEW: Save the variables permanently to Flash Memory
      preferences.putBool("autoSignal", AUTOMATED_SIGNALING);
      preferences.putUInt("hrThreshold", HEART_RATE_THRESHOLD);
      preferences.putUInt("sigDuration", SIGNALING_DURATION);
      preferences.putBool("actuateLed", ACTUATE_LED);
      preferences.putBool("actuateBuzzer", ACTUATE_BUZZER);
      // <--- END NEW
      
      Serial.println("Settings Updated & Saved Permanently!");
      Serial.print("Auto Signal: "); Serial.println(AUTOMATED_SIGNALING);
      Serial.print("HR Threshold: "); Serial.println(HEART_RATE_THRESHOLD);
      Serial.print("Duration (s): "); Serial.println(SIGNALING_DURATION);
      Serial.print("Actuate LED: "); Serial.println(ACTUATE_LED);
      Serial.print("Actuate Buzzer: "); Serial.println(ACTUATE_BUZZER);
      break;
  }

  sendAck(cmd.commandCode);
}

void sendDataToReceiver() {
  if (!loraInitialized) return;

  DataPacket packet;
  packet.password_byte = PASSWORD_BYTE;
  packet.deviceID = DEVICE_ID;
  packet.bpm = currentBPM;

  if (gpsInitialized && gps.location.isValid() && gps.location.age() < 5000) {
    packet.lat = (float)gps.location.lat();
    packet.lng = (float)gps.location.lng();
  } else {
    packet.lat = 0.0;
    packet.lng = 0.0;
  }

  Serial.println(">>> Sending Telemetry...");
  showTransmittedData(packet); 

  LoRa.beginPacket();
  LoRa.write((uint8_t*)&packet, sizeof(DataPacket));
  LoRa.endPacket();
  
  LoRa.receive(); 
}

void showTransmittedData(DataPacket packet) {
  Serial.printf("ID: %d | BPM: %d | GPS: %.6f, %.6f\n", 
                packet.deviceID, packet.bpm, packet.lat, packet.lng);
  Serial.println("---------------------------------");
}

void sendProxyData() {
  if (!loraInitialized) return;

  // Static variables keep their value between function calls
  static float proxyLat = 14.599500; // Starting near Manila
  static float proxyLng = 120.984200;
  static uint8_t proxyBPM = 75;
  static bool increasingHR = true;

  // 1. Simulate subtle movement (walking/drifting)
  proxyLat += 0.000020; 
  proxyLng += 0.000015; 

  // 2. Simulate Heart Rate (Sweeps between 65 and 115 BPM)
  if (increasingHR) {
    proxyBPM += 3;
    if (proxyBPM >= 115) increasingHR = false;
  } else {
    proxyBPM -= 3;
    if (proxyBPM <= 65) increasingHR = true;
  }

  // 3. Pack the struct
  DataPacket packet;
  packet.password_byte = PASSWORD_BYTE;
  packet.deviceID = DEVICE_ID; // Keeps your static ID (1)
  packet.bpm = proxyBPM;
  packet.lat = proxyLat;
  packet.lng = proxyLng;

  Serial.println(">>> Sending PROXY Telemetry...");
  showTransmittedData(packet); 

  // 4. Fire via LoRa
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&packet, sizeof(DataPacket));
  LoRa.endPacket();
  
  // 5. Instantly resume listening for Piggyback commands
  LoRa.receive(); 
}

void sendAck(uint8_t cmdCode) {
  AckPacket ack;
  ack.password_byte = PASSWORD_BYTE;
  ack.deviceID = DEVICE_ID;
  ack.ackedCommandCode = cmdCode;

  Serial.printf(">>> Sending ACK for Command #%d\n", cmdCode);
  
  // Wait a tiny bit so the Receiver has time to switch back to listening mode
  delay(150); 

  LoRa.beginPacket();
  LoRa.write((uint8_t*)&ack, sizeof(AckPacket));
  LoRa.endPacket();
  
  LoRa.receive(); 
}