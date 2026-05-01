#include <TinyGPSPlus.h>
#include <SPI.h>
#include <LoRa.h>
#include <Preferences.h>
#include "esp_timer.h" // Added for accurate HR sampling

// --- HARDWARE CONFIGURATION ---
#define NSS 4
#define RST 5
#define DIO0 2
#define LED_PIN 25
#define BUZZER_PIN 26
#define SENSOR_PIN 34
#define LO_PLUS_PIN 35
#define LO_MINUS_PIN 32
#define PWM_FREQ 2000
#define PWM_RES 8
#define PWM_DUTY 128 
#define DEVICE_ID 1
#define PASSWORD_BYTE 0x69 

TinyGPSPlus gps;
HardwareSerial gpsSerial(2); 
Preferences preferences;

// --- GLOBAL SETTINGS ---
bool AUTOMATED_SIGNALING = false;
int HEART_RATE_THRESHOLD = 100;
int SIGNALING_DURATION = 2;
bool ACTUATE_LED = true; 
bool ACTUATE_BUZZER = true;
bool PROXY_GPS = false;          
uint8_t PROXY_HR_MIN = 0;        
uint8_t PROXY_HR_MAX = 0;        
int UPDATE_PERIOD = 3000;

// --- STATE VARIABLES ---
bool loraInitialized = false, gpsInitialized = false, isSignaling = false, signalState = false;
unsigned long lastGpsLoraTime = 0, lastGpsReceiveTime = 0, lastSignalToggle = 0, lastHrProcessTime = 0;
uint8_t currentBPM = 0;       
int signalStepTotal = 0, signalStepCount = 0, signalInterval = 0;

// ==========================================
// PAN-TOMPKINS ALGORITHM (SMOOTHED MODE 3)
// ==========================================
#define SAMPLE_HZ       200
#define PERIOD_US       (1000000 / SAMPLE_HZ)
#define BUFFER_LEN      1000
#define MAX_BPM_DELTA_PER_SEC  1.0f

volatile uint16_t ecg_buffer[BUFFER_LEN];
volatile int buf_index = 0;
volatile unsigned long sample_counter = 0; 
volatile bool buffer_full = false;

volatile float lp_buffer[BUFFER_LEN];     
volatile float hp_buffer[BUFFER_LEN];     
volatile float der_buffer[BUFFER_LEN];    
volatile float sq_buffer[BUFFER_LEN];     
volatile float mwi_buffer[BUFFER_LEN];    

float lp_yn1 = 0.00f, lp_yn2 = 0.00f;    
float hp_yn1 = 0.00f;                    

const int MWI_WIN = 30;                  
float mwi_sum = 0.0f;

const int REFRACTORY_SAMPLES = (int)(0.3f * SAMPLE_HZ); 
volatile long last_r_sample_abs = -REFRACTORY_SAMPLES;
float SPKI = 0.0f, NPKI = 0.0f;          
float THRESH_I1 = 0.0f;                  

const unsigned long WARMUP_SAMPLES = (unsigned long)(5UL * SAMPLE_HZ);
const unsigned long INIT_SAMPLES   = (unsigned long)(10UL * SAMPLE_HZ);
volatile bool thresholds_initialized = false;
volatile float init_mwi_max = 0.0f;

const int LP_PADDING = 12;  
const int HP_PADDING = 32;  
volatile bool lp_filter_ready = false;
volatile bool hp_filter_ready = false;

volatile unsigned long recent_r_times[64];
volatile int recent_r_count = 0; 
volatile unsigned long recent_peak_times[8]; 
volatile int recent_peak_count = 0;
volatile float last_smoothed_bpm = 0.0f;  
esp_timer_handle_t periodic_timer = nullptr;

float lowpass_ringbuf(float yn1, float yn2, volatile uint16_t ring_buf[BUFFER_LEN], int bfidx) {
  int idx1 = (bfidx - 1  + BUFFER_LEN) % BUFFER_LEN; 
  int idx2 = (bfidx - 7  + BUFFER_LEN) % BUFFER_LEN; 
  int idx3 = (bfidx - 13 + BUFFER_LEN) % BUFFER_LEN; 
  return 2 * yn1 - yn2 + ring_buf[idx1] - 2 * ring_buf[idx2] + ring_buf[idx3];
}

void periodic_cb(void* arg) {
  // 1. Leads-Off Handling (Feed flatline if disconnected to prevent noise)
  if (digitalRead(LO_PLUS_PIN) == 1 || digitalRead(LO_MINUS_PIN) == 1) {
    ecg_buffer[buf_index] = 2048; 
  } else {
    ecg_buffer[buf_index] = analogRead(SENSOR_PIN);
  }

  buf_index = (buf_index + 1) % BUFFER_LEN;
  sample_counter++;
  if(buf_index == 0) buffer_full = true;

  // 2. Low-pass filter
  float lp_y = 0.0f;
  if (buffer_full || buf_index >= LP_PADDING + 1) {
    lp_y = lowpass_ringbuf(lp_yn1, lp_yn2, ecg_buffer, buf_index);
    lp_yn2 = lp_yn1;
    lp_yn1 = lp_y;
    lp_filter_ready = true;
  } else {
    lp_y = (float)ecg_buffer[0];
  }
  lp_buffer[(buf_index - 1 + BUFFER_LEN) % BUFFER_LEN] = lp_y;

  // 3. High-pass filter
  float hp_y = 0.0f;
  if (lp_filter_ready && (buffer_full || buf_index >= HP_PADDING + 1)) {
    int i0 = (buf_index - 1 + BUFFER_LEN) % BUFFER_LEN;      
    int i16 = (buf_index - 17 + BUFFER_LEN) % BUFFER_LEN;    
    int i17 = (buf_index - 18 + BUFFER_LEN) % BUFFER_LEN;    
    int i32 = (buf_index - 33 + BUFFER_LEN) % BUFFER_LEN;    
    hp_y = hp_yn1 - (lp_buffer[i0] / 32.0f) + lp_buffer[i16] - lp_buffer[i17] + (lp_buffer[i32] / 32.0f);
    hp_yn1 = hp_y;
    hp_filter_ready = true;
  } else { hp_y = 0.0f; }
  hp_buffer[(buf_index - 1 + BUFFER_LEN) % BUFFER_LEN] = hp_y;

  // 4. Derivative
  float der_y = 0.0f;
  if (buffer_full || buf_index >= 5) {
    int i0 = (buf_index - 1 + BUFFER_LEN) % BUFFER_LEN;   
    int i1 = (buf_index - 2 + BUFFER_LEN) % BUFFER_LEN;   
    int i3 = (buf_index - 4 + BUFFER_LEN) % BUFFER_LEN;   
    int i4 = (buf_index - 5 + BUFFER_LEN) % BUFFER_LEN;   
    der_y = (2.0f * hp_buffer[i0] + hp_buffer[i1] - hp_buffer[i3] - 2.0f * hp_buffer[i4]) * 0.125f; 
  }
  der_buffer[(buf_index - 1 + BUFFER_LEN) % BUFFER_LEN] = der_y;

  // 5. Squaring
  float sq_y = der_y * der_y;
  sq_buffer[(buf_index - 1 + BUFFER_LEN) % BUFFER_LEN] = sq_y;

  // 6. Moving Window Integration
  float mwi_y = 0.0f;
  int cur = (buf_index - 1 + BUFFER_LEN) % BUFFER_LEN;
  if (buffer_full || buf_index > 0) {
    mwi_sum += sq_y;
    int old = (cur - MWI_WIN + BUFFER_LEN) % BUFFER_LEN;
    if (buffer_full || buf_index >= MWI_WIN) {
      mwi_sum -= sq_buffer[old];
      mwi_y = mwi_sum / (float)MWI_WIN;
    } else {
      int win = buf_index < 1 ? 1 : buf_index;
      mwi_y = mwi_sum / (float)win;
    }
  }
  mwi_buffer[cur] = mwi_y;

  // 7. Peak Detection
  if (buffer_full || buf_index >= 3) {
    int i_prev2 = (cur - 2 + BUFFER_LEN) % BUFFER_LEN;
    int i_prev1 = (cur - 1 + BUFFER_LEN) % BUFFER_LEN;
    int i_curr  = cur;
    float v0 = mwi_buffer[i_prev2], v1 = mwi_buffer[i_prev1], v2 = mwi_buffer[i_curr];
    
    if (sample_counter < WARMUP_SAMPLES) return;
    if (sample_counter < INIT_SAMPLES) {
      if (v1 > init_mwi_max) init_mwi_max = v1;
      return;
    }
    if (!thresholds_initialized) {
      SPKI = 0.75f * init_mwi_max; NPKI = 0.25f * init_mwi_max;
      THRESH_I1 = NPKI + 0.25f * (SPKI - NPKI);
      thresholds_initialized = true;
    }

    if ((v1 > v0) && (v1 >= v2)) { // is_peak
      long sample_abs = (long)sample_counter - 1;
      if ((sample_abs - last_r_sample_abs) > REFRACTORY_SAMPLES) {
        
        // QRS Width Validation
        bool width_ok = true;
        int max_span = (int)(0.30f * SAMPLE_HZ);
        float half = v1 * 0.5f;
        int left = 0, right = 0;
        for (int s = 1; s <= max_span; s++) { if (mwi_buffer[(i_prev1 - s + BUFFER_LEN) % BUFFER_LEN] < half) { left = s; break; } }
        for (int s = 1; s <= max_span; s++) { if (mwi_buffer[(i_prev1 + s) % BUFFER_LEN] < half) { right = s; break; } }
        int width_samples = left + right;
        if (left == 0 || right == 0) width_ok = false; 
        else width_ok = (width_samples >= (int)(0.08f * SAMPLE_HZ) && width_samples <= (int)(0.20f * SAMPLE_HZ));

        if (width_ok && v1 > THRESH_I1) {
          bool accept_peak = true;
          const int WINDOW_SAMPLES = (int)(200 * SAMPLE_HZ / 1000.0f);
          int peaks_in_window = 0;
          for (int i = 0; i < recent_peak_count; i++) {
            if ((sample_abs - recent_peak_times[i]) <= WINDOW_SAMPLES) peaks_in_window++;
          }
          if (peaks_in_window >= 2) accept_peak = false;
          
          if (accept_peak) {
            if (recent_peak_count < 8) recent_peak_times[recent_peak_count++] = sample_abs;
            else {
              for (int s = 1; s < 8; s++) recent_peak_times[s - 1] = recent_peak_times[s];
              recent_peak_times[7] = sample_abs;
            }
            SPKI = 0.125f * v1 + 0.875f * SPKI;
            last_r_sample_abs = sample_abs;
            
            if (recent_r_count < (int)(sizeof(recent_r_times) / sizeof(recent_r_times[0]))) recent_r_times[recent_r_count++] = (unsigned long)sample_abs;
            else {
              for (int s = 1; s < (int)(sizeof(recent_r_times) / sizeof(recent_r_times[0])); s++) recent_r_times[s - 1] = recent_r_times[s];
              recent_r_times[(int)(sizeof(recent_r_times) / sizeof(recent_r_times[0])) - 1] = (unsigned long)sample_abs;
            }
          } else NPKI = 0.125f * v1 + 0.875f * NPKI;
        } else NPKI = 0.125f * v1 + 0.875f * NPKI;
        THRESH_I1 = NPKI + 0.25f * (SPKI - NPKI);
      }
    }
  }
}

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
  uint8_t autoSignal;      
  uint8_t hrThreshold;     
  uint8_t signalDuration;  
  uint8_t actuateLed;      
  uint8_t actuateBuzzer;
  uint8_t proxyGps;       
  uint8_t proxyHrMin;     
  uint8_t proxyHrMax;     
};

struct __attribute__((packed)) AckPacket {
  uint8_t password_byte;
  uint8_t deviceID;
  uint8_t ackedCommandCode;
};

// --- PROTOTYPES ---
void initHardware(); void attemptLoraConnection(); void updateGPS(); void processHeartRate();
void sendDataToReceiver(); void listenForCommands(); void executeCommand(CommandPacket cmd);
void startAsyncSignal(float duration_seconds, int time_delay); void updateAsyncSignal();
void setPWM(bool state); void showTransmittedData(DataPacket packet); void sendAck(uint8_t cmdCode);
uint8_t getActiveHeartRate(); void getActiveGps(float &lat, float &lng);

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("--- Smart Lifevest Transmitter (Edge) ---");

  preferences.begin("vest-settings", false); 
  AUTOMATED_SIGNALING = preferences.getBool("autoSignal", false);
  HEART_RATE_THRESHOLD = preferences.getUInt("hrThreshold", 100);
  SIGNALING_DURATION = preferences.getUInt("sigDuration", 2);
  ACTUATE_LED = preferences.getBool("actuateLed", true);
  ACTUATE_BUZZER = preferences.getBool("actuateBuzzer", true);
  PROXY_GPS = preferences.getBool("proxyGps", false);         
  PROXY_HR_MIN = preferences.getUInt("proxyHrMin", 0);        
  PROXY_HR_MAX = preferences.getUInt("proxyHrMax", 0);        

  Serial.printf("Loaded Settings -> Auto: %d | HR Thresh: %d | Dur: %d | LED: %d | BUZ: %d\n | pGPS: %d\n | pHRmin: %d\n | pHRmax: %d\n", 
                AUTOMATED_SIGNALING, HEART_RATE_THRESHOLD, SIGNALING_DURATION, ACTUATE_LED, ACTUATE_BUZZER, PROXY_GPS, PROXY_HR_MIN, PROXY_HR_MAX);

  initHardware();
  attemptLoraConnection();
  startAsyncSignal(1, 500); 
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  updateAsyncSignal();  
  listenForCommands();  
  updateGPS();          

  // Process and extract HR from the Pan-Tompkins buffers every 1 second
  if (millis() - lastHrProcessTime >= 1000) {
    lastHrProcessTime = millis();
    processHeartRate(); 
  }

  // Send LoRa Packet
  if (millis() - lastGpsLoraTime >= UPDATE_PERIOD) {
    lastGpsLoraTime = millis();
    sendDataToReceiver(); 
  }
}

// ==========================================
// CORE FUNCTIONS
// ==========================================

void processHeartRate() {
  // If leads are disconnected, reset smoother and force 0 BPM
  if (digitalRead(LO_PLUS_PIN) == 1 || digitalRead(LO_MINUS_PIN) == 1) {
    currentBPM = 0;
    last_smoothed_bpm = 0.0f;
    return;
  }

  // Safely extract recent peaks from the timer interrupt
  unsigned long sc;
  int local_recent_count = 0;
  unsigned long local_recent[64];
  
  noInterrupts();
  sc = sample_counter;
  local_recent_count = recent_r_count;
  if (local_recent_count > 64) local_recent_count = 64;
  for (int i = 0; i < local_recent_count; i++) { local_recent[i] = recent_r_times[i]; }
  interrupts();

  if (sc >= INIT_SAMPLES && local_recent_count >= 2) {
    float bpm = 0.0f;
    int use_times = local_recent_count > 9 ? 9 : local_recent_count;
    int start = local_recent_count - use_times;
    unsigned long prev = local_recent[start];
    float sum_rr = 0.0f;
    int rr_used = 0;
    
    for (int i = start + 1; i < local_recent_count; i++) {
      unsigned long curr = local_recent[i];
      if (curr > prev) { sum_rr += (float)(curr - prev); rr_used++; }
      prev = curr;
      if (rr_used >= 8) break;
    }
    
    if (rr_used > 0) {
      float raw_bpm = 60.0f * ((float)SAMPLE_HZ) / (sum_rr / (float)rr_used);
      
      // Rate limiting
      if (last_smoothed_bpm == 0.0f) {
        bpm = raw_bpm;
      } else {
        float delta = raw_bpm - last_smoothed_bpm;
        if (delta > MAX_BPM_DELTA_PER_SEC) bpm = last_smoothed_bpm + MAX_BPM_DELTA_PER_SEC;
        else if (delta < -MAX_BPM_DELTA_PER_SEC) bpm = last_smoothed_bpm - MAX_BPM_DELTA_PER_SEC;
        else bpm = raw_bpm;
      }
      last_smoothed_bpm = bpm;
      
      // Assign final BPM to the global variable used by LoRa
      currentBPM = (uint8_t)bpm;
      
      // Check automated signaling logic
      if (AUTOMATED_SIGNALING && currentBPM > HEART_RATE_THRESHOLD && !isSignaling) {
        startAsyncSignal(SIGNALING_DURATION, 100);
      }
    }
  }
}

// ==========================================
// PROXY DATA GENERATORS
// ==========================================
uint8_t getActiveHeartRate() {
  if (PROXY_HR_MIN == 0 && PROXY_HR_MAX == 0) return currentBPM; 
  static uint8_t proxyBPM = PROXY_HR_MIN;
  static bool increasingHR = true;
  if (increasingHR) { proxyBPM += 3; if (proxyBPM >= PROXY_HR_MAX) increasingHR = false; } 
  else { proxyBPM -= 3; if (proxyBPM <= PROXY_HR_MIN) increasingHR = true; }
  return proxyBPM;
}

void getActiveGps(float &lat, float &lng) {
  if (!PROXY_GPS) {
    if (gps.location.isValid()) { lat = (float)gps.location.lat(); lng = (float)gps.location.lng(); } 
    else { lat = 0.0; lng = 0.0; }
    return;
  }
  static float proxyLat = 14.599500, proxyLng = 120.984200;
  proxyLat += 0.000020; proxyLng += 0.000015; 
  lat = proxyLat; lng = proxyLng;
}

void sendDataToReceiver() {
  if (!loraInitialized) return;
  DataPacket packet;
  packet.password_byte = PASSWORD_BYTE;
  packet.deviceID = DEVICE_ID;
  packet.bpm = getActiveHeartRate();
  float tempLat = 0.0, tempLng = 0.0;
  getActiveGps(tempLat, tempLng); 
  packet.lat = tempLat;           
  packet.lng = tempLng;

  Serial.println(">>> Sending Telemetry...");
  showTransmittedData(packet); 
  LoRa.beginPacket(); LoRa.write((uint8_t*)&packet, sizeof(DataPacket)); LoRa.endPacket();
  LoRa.receive(); 
}

void listenForCommands() {
  if (!loraInitialized) return;
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    if (packetSize == sizeof(CommandPacket)) {
      CommandPacket cmd;
      LoRa.readBytes((uint8_t*)&cmd, sizeof(CommandPacket));
      if (cmd.password_byte == PASSWORD_BYTE && cmd.targetDeviceID == DEVICE_ID) executeCommand(cmd);
    } else { while(LoRa.available()) LoRa.read(); }
  }
}

void executeCommand(CommandPacket cmd) {
  switch(cmd.commandCode) {
    case 1: 
      startAsyncSignal(SIGNALING_DURATION, 100);
      Serial.println("Activating signaling...");
      break;
    case 2: 
      AUTOMATED_SIGNALING = (cmd.autoSignal == 1); HEART_RATE_THRESHOLD = cmd.hrThreshold; SIGNALING_DURATION = cmd.signalDuration;
      ACTUATE_LED = (cmd.actuateLed == 1); ACTUATE_BUZZER = (cmd.actuateBuzzer == 1);
      PROXY_GPS = (cmd.proxyGps == 1); PROXY_HR_MIN = cmd.proxyHrMin; PROXY_HR_MAX = cmd.proxyHrMax;

      preferences.putBool("autoSignal", AUTOMATED_SIGNALING); preferences.putUInt("hrThreshold", HEART_RATE_THRESHOLD);
      preferences.putUInt("sigDuration", SIGNALING_DURATION); preferences.putBool("actuateLed", ACTUATE_LED);
      preferences.putBool("actuateBuzzer", ACTUATE_BUZZER); preferences.putBool("proxyGps", PROXY_GPS);
      preferences.putUInt("proxyHrMin", PROXY_HR_MIN); preferences.putUInt("proxyHrMax", PROXY_HR_MAX);
      Serial.println("Settings Updated!");
      Serial.print("AUTOMATED SIGNALING: "); Serial.println(AUTOMATED_SIGNALING);
      Serial.print("ACTUATE LED:         "); Serial.println(ACTUATE_LED);
      Serial.print("ACTUATE BUZZER:      "); Serial.println(ACTUATE_BUZZER);
      Serial.print("HEART RATE THRESH:   "); Serial.println(HEART_RATE_THRESHOLD);
      Serial.print("SIGNAL DURATION:     "); Serial.println(SIGNALING_DURATION);
      Serial.print("SIMULATE GPS:        "); Serial.println(PROXY_GPS);
      Serial.print("SIMULATE HR MIN:     "); Serial.println(PROXY_HR_MIN);
      Serial.print("SIMULATE HR MAX:     "); Serial.println(PROXY_HR_MAX);
      break;
  }
  sendAck(cmd.commandCode);
}

void sendAck(uint8_t cmdCode) {
  AckPacket ack = {PASSWORD_BYTE, DEVICE_ID, cmdCode};
  delay(150); 
  LoRa.beginPacket(); LoRa.write((uint8_t*)&ack, sizeof(AckPacket)); LoRa.endPacket();
  LoRa.receive(); 
}

// ==========================================
// HARDWARE & HELPERS
// ==========================================
void initHardware() {
  analogReadResolution(12); // Added to support accurate ECG values
  ledcAttach(LED_PIN, PWM_FREQ, PWM_RES); 
  ledcAttach(BUZZER_PIN, PWM_FREQ, PWM_RES);
  setPWM(false); 
  pinMode(LO_PLUS_PIN, INPUT); 
  pinMode(LO_MINUS_PIN, INPUT);
  
  gpsSerial.setRxBufferSize(1024); 
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);
  LoRa.setPins(NSS, RST, DIO0);

  // Initialize the Pan-Tompkins 200Hz Sampler
  const esp_timer_create_args_t args = {
    .callback = &periodic_cb,
    .arg = nullptr,
    .name = "adc_periodic"
  };
  esp_timer_create(&args, &periodic_timer);
  esp_timer_start_periodic(periodic_timer, PERIOD_US);
}

void attemptLoraConnection() {
  while (!loraInitialized) {
    digitalWrite(RST, LOW); delay(10); digitalWrite(RST, HIGH); delay(10);
    if (LoRa.begin(433E6)) {
      LoRa.setSyncWord(0x9E); LoRa.enableCrc(); LoRa.setTxPower(17); LoRa.receive(); 
      loraInitialized = true; Serial.println("SUCCESS: LoRa Active.");
    } else { delay(1000); }
  }
}

void updateGPS() {
  while (gpsSerial.available() > 0) { gps.encode(gpsSerial.read()); lastGpsReceiveTime = millis(); }
  if (millis() - lastGpsReceiveTime > 5000) gpsInitialized = false;
  else if (gps.location.isValid() && gps.location.age() < 5000 && !gpsInitialized) {
    startAsyncSignal(1.5, 250); gpsInitialized = true;
  }
}

void startAsyncSignal(float duration_seconds, int time_delay) {
  isSignaling = true; signalInterval = time_delay;
  signalStepTotal = (int)(duration_seconds * 1000.0) / signalInterval; 
  signalStepCount = 0; lastSignalToggle = millis(); signalState = true; setPWM(true);
}

void updateAsyncSignal() {
  if (!isSignaling) return;
  if (millis() - lastSignalToggle >= signalInterval) {
    lastSignalToggle = millis(); signalState = !signalState; setPWM(signalState);
    signalStepCount++;
    if (signalStepCount >= signalStepTotal) { isSignaling = false; setPWM(false); }
  }
}

void setPWM(bool state) {
  if (state) {
    if (ACTUATE_LED) ledcWrite(LED_PIN, PWM_DUTY); 
    if (ACTUATE_BUZZER) ledcWrite(BUZZER_PIN, PWM_DUTY); 
  } else {
    ledcWrite(LED_PIN, 0); ledcWrite(BUZZER_PIN, 0);
  }
}

void showTransmittedData(DataPacket packet) {
  Serial.printf("ID: %d | BPM: %d | GPS: %.6f, %.6f\n", packet.deviceID, packet.bpm, packet.lat, packet.lng);
  Serial.println("---------------------------------");
}