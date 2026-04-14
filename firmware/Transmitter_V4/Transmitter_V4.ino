#include <TinyGPSPlus.h>
#include <SPI.h>
#include <LoRa.h>

// --- HARDWARE CONFIGURATION ---
#define NSS 4
#define RST 5
#define DIO0 2
#define BUZZER_PIN 27   // Connected to MOSFET Gate
#define LED_PIN 26
#define SENSOR_PIN 34

#define TRANSMISSION_PERIOD 3000
#define DEVICE_ID 1
const uint8_t PASSWORD_BYTE = 0x69;

// --- OBJECTS ---
TinyGPSPlus gps;
HardwareSerial gpsSerial(2); // UART2 (GPIO 16, 17)

// --- NON-BLOCKING SIGNAL STATE (ASYNC LOGIC) ---
bool isSignaling = false;
int signalStepCount = 0;
int signalStepTotal = 0;
unsigned long lastSignalToggle = 0;
int signalInterval = 0;
bool signalState = false;

// --- TELEMETRY STATE ---
bool loraInitialized = false;
bool gpsInitialized = false;
unsigned long lastGpsLoraTime = 0;

// --- HEART RATE VARIABLES ---
int bpm = 0;
float baseline = 512, filtered = 0, peak = 0, threshold = 20.0;
bool beatState = false;
unsigned long lastBeatTime = 0;
const int intervalBufferSize = 15;
unsigned long intervalBuffer[intervalBufferSize];
int intervalIndex = 0;
bool intervalFilled = false;
const float baselineAlpha = 0.005;
const float smoothAlpha = 0.05;

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

// --- FUNCTION PROTOTYPES ---
void attemptLoraConnection();
void processHeartRate();
void updateGPS();
void sendDataToReceiver();
void listenForCommands();
void executeCommand(uint8_t cmdCode);
void startAsyncSignal(int count, int time_delay);
void updateAsyncSignal();

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- Smart Lifevest Transmitter (Edge) ---");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);

  LoRa.setPins(NSS, RST, DIO0);
  attemptLoraConnection();

  // Startup Beep (Asynchronous)
  startAsyncSignal(2, 200);
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  // 1. BACKGROUND TASKS (Low Latency)
  updateAsyncSignal(); 
  listenForCommands();
  updateGPS();

  // 2. HEART RATE SAMPLING (Fixed 10ms interval)
  static unsigned long lastSampleTime = 0;
  if (millis() - lastSampleTime >= 10) {
    lastSampleTime = millis();
    processHeartRate();
  }

  // 3. TELEMETRY TRANSMISSION (Every 3 seconds)
  if (millis() - lastGpsLoraTime >= TRANSMISSION_PERIOD) {
    lastGpsLoraTime = millis();

    if (gps.location.isValid() && gps.location.age() < 5000) {
      if (!gpsInitialized) {
        Serial.println("GPS Fix Acquired!");
        startAsyncSignal(3, 300);
        gpsInitialized = true;
      }
      sendDataToReceiver();
    } else {
      Serial.println("WAITING: No valid GPS fix.");
    }
  }
}

// ==========================================
// ASYNC SIGNALING (The "Async" Switch)
// ==========================================
void startAsyncSignal(int count, int time_delay) {
  isSignaling = true;
  signalStepTotal = count * 2; 
  signalStepCount = 0;
  signalInterval = time_delay;
  lastSignalToggle = millis();
}

void updateAsyncSignal() {
  if (!isSignaling) return;

  if (millis() - lastSignalToggle >= signalInterval) {
    lastSignalToggle = millis();
    signalState = !signalState;
    
    digitalWrite(BUZZER_PIN, signalState);
    digitalWrite(LED_PIN, signalState);
    
    signalStepCount++;
    if (signalStepCount >= signalStepTotal) {
      isSignaling = false;
      digitalWrite(BUZZER_PIN, LOW);
      digitalWrite(LED_PIN, LOW);
    }
  }
}

// ==========================================
// CORE LORA & GPS FUNCTIONS
// ==========================================
void attemptLoraConnection() {
  digitalWrite(RST, LOW);  delay(100);
  digitalWrite(RST, HIGH); delay(100);

  while (!loraInitialized) {
    if (LoRa.begin(433E6)) {
      LoRa.setSyncWord(0x9E);
      LoRa.enableCrc();
      LoRa.setTxPower(17);
      LoRa.receive(); 
      loraInitialized = true;
      Serial.println("SUCCESS: LoRa Active.");
    } else {
      Serial.println("FAILED: LoRa Error.");
      delay(1000);
    }
  }
}

void listenForCommands() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == sizeof(CommandPacket)) {
    CommandPacket cmd;
    LoRa.readBytes((uint8_t*)&cmd, sizeof(CommandPacket));
    
    if (cmd.password_byte == PASSWORD_BYTE && cmd.targetDeviceID == DEVICE_ID) {
      if (cmd.commandCode == 1) {
        Serial.println("Web Command: SIGNAL ACTIVATE");
        startAsyncSignal(15, 80);
      }
    }
  }
}

void sendDataToReceiver() {
  DataPacket packet;
  packet.password_byte = PASSWORD_BYTE;
  packet.deviceID = DEVICE_ID;
  packet.bpm = (uint8_t)bpm;
  packet.lat = (float)gps.location.lat();
  packet.lng = (float)gps.location.lng();

  Serial.println(">>> Sending Telemetry...");
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&packet, sizeof(DataPacket));
  LoRa.endPacket();
  
  LoRa.receive(); // Crucial: Re-enter receive mode immediately
}

void updateGPS() {
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }
}

// ==========================================
// HEART RATE PROCESSING
// ==========================================
void processHeartRate() {
  long sum = 0;
  for (int i = 0; i < 10; i++) sum += analogRead(SENSOR_PIN);
  float raw = sum / 10.0;

  baseline = (baseline * (1 - baselineAlpha)) + (raw * baselineAlpha);
  float centered = raw - baseline;
  filtered = (filtered * (1 - smoothAlpha)) + (centered * smoothAlpha);

  if (filtered > peak) peak = filtered;
  peak *= 0.998;
  threshold = peak * 0.7;

  unsigned long currentTime = millis();
  if (filtered > threshold && !beatState && (currentTime - lastBeatTime > 500)) {
    if (filtered > 5.0) {
      beatState = true;
      unsigned long interval = currentTime - lastBeatTime;
      if (lastBeatTime > 0 && interval < 2000) {
        intervalBuffer[intervalIndex] = interval;
        intervalIndex = (intervalIndex + 1) % intervalBufferSize;
        if (intervalIndex == 0) intervalFilled = true;
        
        unsigned long sumIntervals = 0;
        int count = intervalFilled ? intervalBufferSize : intervalIndex;
        for (int i = 0; i < count; i++) sumIntervals += intervalBuffer[i];
        bpm = 60000 / (sumIntervals / count);
      }
      lastBeatTime = currentTime;
    }
  }
  if (filtered < (threshold * 0.3)) beatState = false;
}