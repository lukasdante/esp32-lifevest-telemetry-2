/*
  AD8232 ECG Heart Rate Monitor
  Output: Serial Plotter (Tools > Serial Plotter)
*/

const int ecgPin = 34;    // AD8232 Output
//const int loPlus = 32;   // Leads-off detect +
//const int loMinus = 35;  // Leads-off detect -

void setup() {
  // Initialize serial communication at 115200 baud
  Serial.begin(115200);
  
  // Set leads-off pins as inputs
//  pinMode(loPlus, INPUT);
//  pinMode(loMinus, INPUT);
}

void loop() {
  // Check if leads are disconnected
//  if ((digitalRead(loPlus) == 1) || (digitalRead(loMinus) == 1)) {
    // If disconnected, send a flat line to the plotter (optional)
//    Serial.println(0); 
//  } else {
    // Read the heart rate signal
    int ecgValue = analogRead(ecgPin);
    
    // Print the value to the Serial Plotter
    Serial.println(ecgValue);
//  }

  // Small delay to stabilize the ADC and match Plotter refresh rate
  delay(1); 
}
