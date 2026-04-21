const int ECG_PIN = 34;    
const int LO_PLUS = 35;    
const int LO_MINUS = 32;   

// --- Heart Rate State Variables ---
int rawBPM = 0;            // The instant, beat-to-beat calculation
boolean isPeak = false;
unsigned long lastBeatTime = 0;

// --- Adaptive Threshold Variables ---
int P = 2048;             
int T = 2048;             
int adaptiveThreshold = 2500; 
int amplitude = 100;      

// --- 3-Second Median Filter Variables ---
unsigned long lastBatchTime = 0;
int bpmBuffer[20];        // Array to store beats (max ~10 beats in 3s at 200bpm)
int beatCount = 0;
int reportedBPM = 0;      // The smoothed, agreed-upon number

void setup() {
  Serial.begin(115200);
  pinMode(ECG_PIN, INPUT);
  pinMode(LO_PLUS, INPUT);
  pinMode(LO_MINUS, INPUT);
}


void loop() {
  unsigned long currentTime = millis();
  unsigned long timeSinceLastBeat = currentTime - lastBeatTime;

  // 1. Leads-Off Detection
  if (digitalRead(LO_PLUS) == 1 || digitalRead(LO_MINUS) == 1) {
    rawBPM = 0;
    reportedBPM = 0;
    isPeak = false; 
    adaptiveThreshold = 2048; P = 2048; T = 2048;
    beatCount = 0; 
  } else {
    
    int ecgValue = analogRead(ECG_PIN);

    // --- ADAPTIVE THRESHOLD (Find Peaks & Troughs) ---
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
        // 1. Sort the array
        for (int i = 1; i < beatCount; ++i) {
          int key = bpmBuffer[i];
          int j = i - 1;
          while (j >= 0 && bpmBuffer[j] > key) {
            bpmBuffer[j + 1] = bpmBuffer[j];
            j = j - 1;
          }
          bpmBuffer[j + 1] = key;
        }
        
        // 2. Pick the Median 
        reportedBPM = bpmBuffer[beatCount / 2];
        
        // ---> NEW: Print the voted result to the Serial Monitor
        Serial.print("[INFO] 3-Second Window Closed | Voted BPM: ");
        Serial.println(reportedBPM);

        // 3. Clear the buffer
        beatCount = 0;
      } else {
        // No beats detected
        reportedBPM = 0;
        Serial.println("[WARNING] 3-Second Window Closed | No beats detected (0 BPM)");
      }
      
      lastBatchTime = currentTime;
    }

    // 3. Output to Serial Plotter
    // (Commented out so the standard Serial Monitor doesn't get spammed. 
    // Uncomment this and comment out the [INFO] prints above if you want to use the Plotter again!)
    
    // Serial.printf("%d,%d,%d\n", ecgValue, reportedBPM, adaptiveThreshold);
  }
  
  delay(5); 
}
