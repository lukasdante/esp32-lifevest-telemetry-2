#include <Arduino.h>
#include "esp_timer.h"

#define BAUDRATE        115200
#define ECG_PIN         34
#define ADC_RES         12
#define SIM_USE         false

// Rate limiting parameters (adjust for more/less responsive output)
#define MAX_BPM_DELTA_PER_SEC  1.0f  // max BPM change per second (lower = smoother, higher = more responsive)

#define SAMPLE_HZ       200
#define PERIOD_US       (1000000 / SAMPLE_HZ)
#define BUFFER_LEN      1000

//const int SIM_LEN = sizeof(sim_arr) / sizeof(sim_arr[0]);
const int SIM_LEN = 1;

// Forward declarations for filter helpers used before their definitions
float lowpass_ringbuf(float yn1, float yn2, volatile uint16_t ring_buf[BUFFER_LEN], int bfidx);

// Ring Buffer
volatile uint16_t ecg_buffer[BUFFER_LEN];
volatile int buf_index = 0;
volatile unsigned long sample_counter = 0; // absolute sample count
volatile bool buffer_full = false;

// Filtering steps (Pan-Tompkins at 200 Hz)
volatile float lp_buffer[BUFFER_LEN];     // low-pass output
volatile float hp_buffer[BUFFER_LEN];     // high-pass output
volatile float der_buffer[BUFFER_LEN];    // derivative output
volatile float sq_buffer[BUFFER_LEN];     // squared output
volatile float mwi_buffer[BUFFER_LEN];    // moving window integration output

// State for IIR sections
float lp_yn1 = 0.00f, lp_yn2 = 0.00f;    // low-pass y[n-1], y[n-2]
float hp_yn1 = 0.00f;                    // high-pass y[n-1]

// Moving window integration running sum
const int MWI_WIN = 30;                  // 150 ms at 200 Hz
float mwi_sum = 0.0f;

// Peak detection/adaptive thresholds
const int REFRACTORY_SAMPLES = (int)(0.3f * SAMPLE_HZ); // 300 ms min separation
volatile long last_r_sample_abs = -REFRACTORY_SAMPLES;
float SPKI = 0.0f, NPKI = 0.0f;          // signal/noise levels for integrated signal
float THRESH_I1 = 0.0f;                  // adaptive threshold
// Startup gating: 0-5s warm-up, 5-10s initialize thresholds, >10s detect/HR
const unsigned long WARMUP_SAMPLES = (unsigned long)(5UL * SAMPLE_HZ);
const unsigned long INIT_SAMPLES   = (unsigned long)(10UL * SAMPLE_HZ);
volatile bool thresholds_initialized = false;
volatile float init_mwi_max = 0.0f;

// Filter padding to prevent DC gain and drift
const int LP_PADDING = 12;  // Low-pass filter needs 12 samples padding
const int HP_PADDING = 32;  // High-pass filter needs 32 samples padding
volatile bool lp_filter_ready = false;
volatile bool hp_filter_ready = false;

// R-peak storage (timestamps in samples)
volatile unsigned long rpeaks_idx[BUFFER_LEN];
volatile float rpeaks_amp[BUFFER_LEN];
volatile int rpeaks_count = 0;

// Track recent absolute R-peak times for HR calculation
volatile unsigned long recent_r_times[64];
volatile int recent_r_count = 0; // number of valid entries in recent_r_times

// Peak count limiting (max 2 peaks per 200ms window)
volatile unsigned long recent_peak_times[8]; // track last 8 peak times
volatile int recent_peak_count = 0;

// Rate-limited output variables
volatile float last_smoothed_bpm = 0.0f;  // last smoothed BPM output

// Timer Interrupt Handler
esp_timer_handle_t periodic_timer = nullptr;

// Sim setup
volatile int sim_ind = 0;

// Timer Interrupt Function 
void periodic_cb(void* /*arg*/) {

  // Choose whether the data should come from the sim arr or from actual readings
  if (SIM_USE == true) {
    //ecg_buffer[buf_index] = sim_arr[sim_ind];
    sim_ind = (sim_ind + 1) % SIM_LEN;
  } else {
    ecg_buffer[buf_index] = analogRead(ECG_PIN);
  }

  // Increment the buffer index and absolute sample counter
  buf_index = (buf_index + 1) % BUFFER_LEN;
  sample_counter++;
  if(buf_index == 0) buffer_full = true;

  // Low-pass filter
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

  // High-pass filter
  float hp_y = 0.0f;
  if (lp_filter_ready && (buffer_full || buf_index >= HP_PADDING + 1)) {
    int i0 = (buf_index - 1 + BUFFER_LEN) % BUFFER_LEN;      
    int i16 = (buf_index - 17 + BUFFER_LEN) % BUFFER_LEN;    
    int i17 = (buf_index - 18 + BUFFER_LEN) % BUFFER_LEN;    
    int i32 = (buf_index - 33 + BUFFER_LEN) % BUFFER_LEN;    
    float x0 = lp_buffer[i0];
    float x16 = lp_buffer[i16];
    float x17 = lp_buffer[i17];
    float x32 = lp_buffer[i32];
    hp_y = hp_yn1 - (x0 / 32.0f) + x16 - x17 + (x32 / 32.0f);
    hp_yn1 = hp_y;
    hp_filter_ready = true;
  } else {
    hp_y = 0.0f;
  }
  hp_buffer[(buf_index - 1 + BUFFER_LEN) % BUFFER_LEN] = hp_y;

  // Derivative
  float der_y = 0.0f;
  if (buffer_full || buf_index >= 5) {
    int i0 = (buf_index - 1 + BUFFER_LEN) % BUFFER_LEN;   
    int i1 = (buf_index - 2 + BUFFER_LEN) % BUFFER_LEN;   
    int i3 = (buf_index - 4 + BUFFER_LEN) % BUFFER_LEN;   
    int i4 = (buf_index - 5 + BUFFER_LEN) % BUFFER_LEN;   
    float x0 = hp_buffer[i0];
    float x1 = hp_buffer[i1];
    float x3 = hp_buffer[i3];
    float x4 = hp_buffer[i4];
    der_y = (2.0f * x0 + x1 - x3 - 2.0f * x4) * 0.125f; 
  }
  der_buffer[(buf_index - 1 + BUFFER_LEN) % BUFFER_LEN] = der_y;

  // Squaring
  float sq_y = der_y * der_y;
  sq_buffer[(buf_index - 1 + BUFFER_LEN) % BUFFER_LEN] = sq_y;

  // Moving Window Integration
  float mwi_y = 0.0f;
  int cur = (buf_index - 1 + BUFFER_LEN) % BUFFER_LEN;
  if (buffer_full || buf_index > 0) {
    mwi_sum += sq_y;
    int old = (cur - MWI_WIN + BUFFER_LEN) % BUFFER_LEN;
    if (buffer_full || buf_index >= MWI_WIN) {
      mwi_sum -= sq_buffer[old];
      mwi_y = mwi_sum / (float)MWI_WIN;
    } else {
      int win = buf_index;
      if (win < 1) win = 1;
      mwi_y = mwi_sum / (float)win;
    }
  }
  mwi_buffer[cur] = mwi_y;

  // Peak detection on MWI
  if (buffer_full || buf_index >= 3) {
    int i_prev2 = (cur - 2 + BUFFER_LEN) % BUFFER_LEN;
    int i_prev1 = (cur - 1 + BUFFER_LEN) % BUFFER_LEN;
    int i_curr  = cur;
    float v0 = mwi_buffer[i_prev2];
    float v1 = mwi_buffer[i_prev1];
    float v2 = mwi_buffer[i_curr];
    
    // Startup gating
    if (sample_counter < WARMUP_SAMPLES) return;
    
    if (sample_counter < INIT_SAMPLES) {
      if (v1 > init_mwi_max) init_mwi_max = v1;
      return;
    }
    
    if (!thresholds_initialized) {
      SPKI = 0.75f * init_mwi_max;
      NPKI = 0.25f * init_mwi_max;
      THRESH_I1 = NPKI + 0.25f * (SPKI - NPKI);
      thresholds_initialized = true;
    }

    bool is_peak = (v1 > v0) && (v1 >= v2);
    if (is_peak) {
      long sample_abs = (long)sample_counter - 1;
      if ((sample_abs - last_r_sample_abs) > REFRACTORY_SAMPLES) {
        
        // QRS width validation
        bool width_ok = true;
        int max_span = (int)(0.30f * SAMPLE_HZ);
        float half = v1 * 0.5f;
        int left = 0, right = 0;
        
        for (int s = 1; s <= max_span; s++) {
          int ii = (i_prev1 - s + BUFFER_LEN) % BUFFER_LEN;
          if (mwi_buffer[ii] < half) { left = s; break; }
        }
        for (int s = 1; s <= max_span; s++) {
          int ii = (i_prev1 + s) % BUFFER_LEN;
          if (mwi_buffer[ii] < half) { right = s; break; }
        }
        
        int width_samples = left + right;
        if (left == 0 || right == 0) {
          width_ok = false; 
        } else {
          int min_w = (int)(0.08f * SAMPLE_HZ);
          int max_w = (int)(0.20f * SAMPLE_HZ);
          width_ok = (width_samples >= min_w && width_samples <= max_w);
        }

        if (width_ok && v1 > THRESH_I1) {
          bool accept_peak = true;
          
          // Check max 2 peaks per 200ms window
          const int WINDOW_MS = 200;
          const int WINDOW_SAMPLES = (int)(WINDOW_MS * SAMPLE_HZ / 1000.0f);
          int peaks_in_window = 0;
          
          for (int i = 0; i < recent_peak_count; i++) {
            if ((sample_abs - recent_peak_times[i]) <= WINDOW_SAMPLES) {
              peaks_in_window++;
            }
          }
          
          if (peaks_in_window >= 2) {
            accept_peak = false; // Reject: too many peaks in window
          }
          
          if (accept_peak) {
            if (recent_peak_count < 8) {
              recent_peak_times[recent_peak_count++] = sample_abs;
            } else {
              for (int s = 1; s < 8; s++) {
                recent_peak_times[s - 1] = recent_peak_times[s];
              }
              recent_peak_times[7] = sample_abs;
            }

            SPKI = 0.125f * v1 + 0.875f * SPKI;
            last_r_sample_abs = sample_abs;
            
            if (rpeaks_count < BUFFER_LEN) {
              rpeaks_idx[rpeaks_count] = (unsigned long)sample_abs;
              rpeaks_amp[rpeaks_count] = v1;
              rpeaks_count++;
            }
            
            if (recent_r_count < (int)(sizeof(recent_r_times) / sizeof(recent_r_times[0]))) {
              recent_r_times[recent_r_count++] = (unsigned long)sample_abs;
            } else {
              for (int s = 1; s < (int)(sizeof(recent_r_times) / sizeof(recent_r_times[0])); s++) {
                recent_r_times[s - 1] = recent_r_times[s];
              }
              recent_r_times[(int)(sizeof(recent_r_times) / sizeof(recent_r_times[0])) - 1] = (unsigned long)sample_abs;
            }
          } else {
            NPKI = 0.125f * v1 + 0.875f * NPKI;
          }
        } else {
          NPKI = 0.125f * v1 + 0.875f * NPKI;
        }
        THRESH_I1 = NPKI + 0.25f * (SPKI - NPKI);
      }
    }
  }
}

// Turn circular buffer into a linear buffer
void getLinearBuffer(uint16_t* linear_buffer, int len) {
  int start;
  if (buffer_full) {
    start = buf_index;
  } else {
    start = 0;
    len = buf_index;  
  }
  for (int i = 0; i < len; i++) {
    linear_buffer[i] = ecg_buffer[(start + i) % BUFFER_LEN];
  }
}

// Low pass filter function - Linear Buffer
float lowpass_linbuf(float yn1, float yn2, uint16_t lin_buf[BUFFER_LEN]) {
  return 2 * yn1 - yn2 + lin_buf[BUFFER_LEN - 1] - 2 * lin_buf[BUFFER_LEN - 7] + lin_buf[BUFFER_LEN - 13];
}

// Low pass filter function - Circular buffer
float lowpass_ringbuf(float yn1, float yn2, volatile uint16_t ring_buf[BUFFER_LEN], int bfidx) {
  int idx1 = (bfidx - 1  + BUFFER_LEN) % BUFFER_LEN; 
  int idx2 = (bfidx - 7  + BUFFER_LEN) % BUFFER_LEN; 
  int idx3 = (bfidx - 13 + BUFFER_LEN) % BUFFER_LEN; 
  return 2 * yn1 - yn2 + ring_buf[idx1] - 2 * ring_buf[idx2] + ring_buf[idx3];
}

// Circular buffer index
int idx(int i) { return (i + BUFFER_LEN) % BUFFER_LEN; }

void setup() {
  Serial.begin(BAUDRATE);
  delay(200);
  Serial.println("Pan-Tompkins ESP32C3 starting...");
  Serial.println("Output: Rate-Limited (Smoothed)");
  
  analogReadResolution(ADC_RES);
  pinMode(ECG_PIN, INPUT);

  const esp_timer_create_args_t args = {
    .callback = &periodic_cb,
    .arg = nullptr,
    .name = "adc_periodic"
  };

  esp_err_t r = esp_timer_create(&args, &periodic_timer);
  if (r != ESP_OK) {
    Serial.printf("esp_timer_create failed: %d\n", r);
    while (1) delay(1000);
  }

  r = esp_timer_start_periodic(periodic_timer, PERIOD_US);
  if (r != ESP_OK) {
    Serial.printf("esp_timer_start_periodic failed: %d\n", r);
    while (1) delay(1000);
  }

  Serial.println("System ready. Waiting 10 seconds for baseline initialization...");
}

void loop() {
  static unsigned long lastStatusMs = 0;

  if (millis() - lastStatusMs > 1000) {
    lastStatusMs = millis();
    
    unsigned long sc;
    int rc;
    noInterrupts();
    sc = sample_counter;
    rc = recent_r_count;
    interrupts();
    
    Serial.print("tick: samples="); Serial.print(sc);
    Serial.print(" recentR="); Serial.println(rc);

    int local_count = 0;
    unsigned long local_r_idx[BUFFER_LEN < 64 ? BUFFER_LEN : 64];
    float local_r_amp[BUFFER_LEN < 64 ? BUFFER_LEN : 64];
    unsigned long local_recent[64];
    int local_recent_count = 0;
    
    noInterrupts();
    local_count = rpeaks_count;
    for (int i = 0; i < local_count; i++) { 
        local_r_idx[i] = rpeaks_idx[i]; 
        local_r_amp[i] = rpeaks_amp[i]; 
    }
    rpeaks_count = 0;
    local_recent_count = recent_r_count;
    if (local_recent_count > 64) local_recent_count = 64;
    for (int i = 0; i < local_recent_count; i++) { 
        local_recent[i] = recent_r_times[i]; 
    }
    interrupts();

    if (local_count > 0) {
      Serial.print("Detected R-peaks (count=");
      Serial.print(local_count);
      Serial.println(") in last interval:");
      for (int i = 0; i < local_count; i++) {
        Serial.print(" idx="); Serial.print(local_r_idx[i]);
        Serial.print(" amp="); Serial.println(local_r_amp[i], 6);
      }
    }

    // Compute smoothed HR
    if (sample_counter >= INIT_SAMPLES && local_recent_count >= 2) {
      float bpm = 0.0f;
      
      int use_times = local_recent_count;
      if (use_times > 9) use_times = 9;
      
      int start = local_recent_count - use_times;
      unsigned long prev = local_recent[start];
      float sum_rr = 0.0f;
      int rr_used = 0;
      
      for (int i = start + 1; i < local_recent_count; i++) {
        unsigned long curr = local_recent[i];
        if (curr > prev) {
          unsigned long rr_samples = curr - prev;
          sum_rr += (float)rr_samples;
          rr_used++;
        }
        prev = curr;
        if (rr_used >= 8) break;
      }
      
      if (rr_used > 0) {
        float mean_rr_samples = sum_rr / (float)rr_used;
        float raw_bpm = 60.0f * ((float)SAMPLE_HZ) / mean_rr_samples;
        
        // Rate limiting logic
        if (last_smoothed_bpm == 0.0f) {
          bpm = raw_bpm;
          last_smoothed_bpm = bpm;
        } else {
          float delta = raw_bpm - last_smoothed_bpm;
          if (delta > MAX_BPM_DELTA_PER_SEC) {
            bpm = last_smoothed_bpm + MAX_BPM_DELTA_PER_SEC;
          } else if (delta < -MAX_BPM_DELTA_PER_SEC) {
            bpm = last_smoothed_bpm - MAX_BPM_DELTA_PER_SEC;
          } else {
            bpm = raw_bpm;
          }
          last_smoothed_bpm = bpm;
        }
        
        Serial.print("HR (raw="); Serial.print(raw_bpm, 1);
        Serial.print(", smoothed="); Serial.print(bpm, 1);
        Serial.print(", last "); Serial.print(rr_used); Serial.print(" RR): ");
        Serial.print(bpm, 1); Serial.println(" bpm");
        Serial.printf("Final Output HR: %d bpm\n", (int)bpm);
      }
    }
  }
}
