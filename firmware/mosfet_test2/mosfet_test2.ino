// Define the MOSFET Control Pin
const int mosfetPin = 27;

void setup() {
  // Initialize GPIO 27 as an output
  pinMode(mosfetPin, OUTPUT);
    
  // Start with the MOSFET OFF
  digitalWrite(mosfetPin, LOW);
  
  Serial.begin(115200);
  Serial.println("MOSFET Control Initialized on Pin 27");
}

void loop() {
  // Turn MOSFET ON (High sends 3.3V to the Gate)
  Serial.println("Load ON");
  digitalWrite(mosfetPin, HIGH); 
  delay(2000); // Wait for 2 seconds

  // Turn MOSFET OFF (Low sends 0V to the Gate)
  Serial.println("Load OFF");
  digitalWrite(mosfetPin, LOW);
  delay(2000); // Wait for 2 seconds
}