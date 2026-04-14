// Define the GPIO pin
const int mosfetPin = 26;

void setup() {
  // Initialize the serial monitor for debugging
  Serial.begin(115200);
  
  // Configure the MOSFET pin as an output
  pinMode(mosfetPin, OUTPUT);
  
  Serial.println("MOSFET Test Initialized on GPIO 26");
}

void loop() {
  Serial.println("MOSFET ON");
  digitalWrite(mosfetPin, HIGH); // Turn LED on
  delay(2000);                   // Wait 2 seconds

  Serial.println("MOSFET OFF");
  digitalWrite(mosfetPin, LOW);  // Turn LED off
  delay(2000);                   // Wait 2 seconds
}