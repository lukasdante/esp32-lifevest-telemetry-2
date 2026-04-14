#include <TinyGPSPlus.h>
#include <SPI.h>
#include <LoRa.h>

#define NSS 4
#define RST 5
#define DI0 2
#define BUZZER_PIN 27
#define LED_PIN 26
#define SENSOR_PIN 34

#define HIGH_BPM_LIMIT 120    
#define LOW_BPM_LIMIT 45      
#define TRANSMISSION_PERIOD 3000
#define DEVICE_ID 1


TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

bool loraInitialized = false;
bool gpsInitialized = false;
unsigned long lastGpsLoraTime = 0;

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
const uint8_t PASSWORD_BYTE = 0x69;

unsigned long lastAlarmToggle = 0;
bool alarmState = false;

struct __attribute__((packed)) DataPacket {
  uint8_t password_byte;
  uint8_t deviceID;
  uint8_t bpm;
  float lat;
  float lng;
};

void attemptLoraConnection(void);
void processHeartRate(void);
void updateGPS(void);
void handeAlarms(void);
void sendDataToReceiver(void);
void signalInit(int count, int time_delay);

void setup() {
  Serial.begin(115200);

  delay(2000);

  Serial.println("Setting up transmitter station...");
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);

  LoRa.setPins(NSS, RST, DI0);
  attemptLoraConnection();
  Serial.println("Setup done, listening for incoming data...");

  delay(500);

  signalInit(2, 200);
}

void loop() {
  static unsigned long lastSampleTime = 0;

  if (millis() - lastSampleTime >= 10) {
    lastSampleTime = millis();
    processHeartRate();
  }
   
  handleAlarms();
  updateGPS();

  if (millis() - lastGpsLoraTime >= TRANSMISSION_PERIOD) {
      lastGpsLoraTime = millis();

      if (gps.location.isValid() && gps.location.age() < 2000) {
        if (!gpsInitialized) {
          signalInit(3, 200);
        }

        gpsInitialized = true;
        sendDataToReceiver();
      } else {
        Serial.println("WAITING: No valid GPS fix. Holding transmission...");
      }
  }
}

void signalInit(int count, int time_delay) {
  for (int i=0; i<count; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(time_delay);
    digitalWrite(BUZZER_PIN, LOW);
    delay(time_delay);
  }
}

void handleAlarms() {
  // Only trigger if we have a valid BPM reading outside the safe zone
  if (bpm > 0 && (bpm > HIGH_BPM_LIMIT || bpm < LOW_BPM_LIMIT)) {
   
    // Quick burst effect: Toggle every 150 milliseconds
    if (millis() - lastAlarmToggle > 150) {
      lastAlarmToggle = millis();
      alarmState = !alarmState; // Flip the state
     
      digitalWrite(BUZZER_PIN, alarmState ? HIGH : LOW);
      digitalWrite(LED_PIN, alarmState ? HIGH : LOW);
    }
  }
  else {
    // Safe zone: Ensure everything is forced OFF
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
    alarmState = false;
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

void processHeartRate() {
  unsigned long currentTime = millis();
  long sum = 0;
  for (int i = 0; i < 20; i++) sum += analogRead(SENSOR_PIN);
  float raw = sum / 20.0;

  // Filters (Your existing logic)
  baseline = (baseline * (1 - baselineAlpha)) + (raw * baselineAlpha);
  float centered = raw - baseline;
  filtered = (filtered * (1 - smoothAlpha)) + (centered * smoothAlpha);

  // Adaptive Peak Detection
  if (filtered > peak) peak = filtered;
  peak *= 0.998;
  threshold = peak * 0.8;

  // Beat Detection
  if (filtered > threshold && !beatState && (currentTime - lastBeatTime > 600)) {
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
  if (filtered < (threshold * 0.4)) {
      beatState = false;
  }
}

void updateGPS() {
  while (gpsSerial.available() > 0) {
      gps.encode(gpsSerial.read());
  }
}

void sendDataToReceiver() {
  DataPacket packet;
  packet.password_byte = PASSWORD_BYTE;
  packet.deviceID = DEVICE_ID;
  packet.bpm = (uint8_t)bpm;
  packet.lat = (float)gps.location.lat();
  packet.lng = (float)gps.location.lng();

  Serial.println("Transmitting Binary Packet");
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&packet, sizeof(DataPacket));
 
  if (LoRa.endPacket()) {
      Serial.println("LoRa Status: SENT");
  } else {
      Serial.println("LoRa Status: ERROR");
  }
}
