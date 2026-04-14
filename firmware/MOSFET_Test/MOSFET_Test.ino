/*
 * MOSFET Simple Switch Test
 * Hardware: Pin 27 connected to MOSFET Gate
 */

#define MOSFET_PIN 27

void setup() {
  Serial.begin(115200);
  pinMode(MOSFET_PIN, OUTPUT);
  Serial.println("--- MOSFET Switch Test Initialized ---");
}

void loop() {
  Serial.println("MOSFET: ON (Gate High)");
  digitalWrite(MOSFET_PIN, HIGH);
  delay(2000); // 2 Seconds ON

  Serial.println("MOSFET: OFF (Gate Low)");
  digitalWrite(MOSFET_PIN, LOW);
  delay(2000); // 2 Seconds OFF
}