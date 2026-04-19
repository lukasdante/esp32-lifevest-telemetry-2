const int ECG_PIN = 34; // AD8232 OUTPUT pin connected to G34

void setup() {
  // Initialize serial communication at a high speed for smoother graphing
  Serial.begin(115200);
  
  // G34 is input by default, but we can be explicit
  pinMode(ECG_PIN, INPUT);
}

void loop() {
  // Read the analog value (0 - 4095)
  int ecgValue = analogRead(ECG_PIN);
  
  // Print the value for the Serial Plotter
  Serial.println(ecgValue);
  
  // Small delay to prevent flooding the serial buffer 
  // and to match typical ECG sampling rates (~200Hz)
  delay(5); 
}
