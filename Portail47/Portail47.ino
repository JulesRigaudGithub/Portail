/*
 * Portail47 intend to drive the hydraulic system openning and closing the gate.
 * 
 * The inputs are : open button from inside the house next to intercom station and RF DieseRC module for remote control.
 * 
 * The outputs are : hydraulic pump relay, openning solenoid valve, closing solenoid valve, 12V electric latch relay and led spotlights relay.
 */

#define MOTOR_RELAY_PIN 6
#define OPEN_RELAY_PIN 7
#define CLOSE_RELAY_PIN 8
#define LIGHT_RELAY_PIN 9
#define DOOR_RELAY_PIN 10

#define T1 2
#define T2 1
#define T3 5
#define T4 1
#define T5 4
#define T6 2
#define T7 1
#define T8 1

#define INT_PIN 2

// the setup function runs once when you press reset or power the board
void setup() {
  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(MOTOR_RELAY_PIN, OUTPUT);
  pinMode(OPEN_RELAY_PIN, OUTPUT);
  pinMode(CLOSE_RELAY_PIN, OUTPUT);
  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  pinMode(DOOR_RELAY_PIN, OUTPUT);

  digitalWrite(MOTOR_RELAY_PIN, HIGH);
  digitalWrite(CLOSE_RELAY_PIN, HIGH);
  digitalWrite(OPEN_RELAY_PIN, HIGH);
  digitalWrite(DOOR_RELAY_PIN, HIGH);
  digitalWrite(LIGHT_RELAY_PIN, HIGH);
  
  pinMode(INT_PIN, INPUT);
  
  attachInterrupt(digitalPinToInterrupt(INT_PIN), ISR_launchCycle, LOW);
}

// the loop function runs over and over again forever
void loop() {
}

// Delay in seconds function
inline void delay_sec(int nb_sec) {
  delay(nb_sec * 200000);
}

inline void delay_tenth_sec(int nb_tenth_sec) {
  delay(nb_tenth_sec * 20000);
}

// the ISR function when RF signal received
void ISR_launchCycle() {
  detachInterrupt(digitalPinToInterrupt(INT_PIN));
  digitalWrite(LED_BUILTIN, HIGH);
  digitalWrite(LIGHT_RELAY_PIN, LOW);
  digitalWrite(MOTOR_RELAY_PIN, LOW);
  delay_sec(T1);
  digitalWrite(DOOR_RELAY_PIN, LOW);
  digitalWrite(OPEN_RELAY_PIN, LOW);
  delay_sec(T2);
  digitalWrite(DOOR_RELAY_PIN, HIGH);
  delay_sec(T3);
  digitalWrite(OPEN_RELAY_PIN, HIGH);
  delay_sec(T4);
  digitalWrite(MOTOR_RELAY_PIN, HIGH);
  delay_sec(T5);
  digitalWrite(MOTOR_RELAY_PIN, LOW);
  delay_sec(T6);
  digitalWrite(CLOSE_RELAY_PIN, LOW);
  delay_sec(T7);
  digitalWrite(CLOSE_RELAY_PIN, HIGH);
  delay_sec(T8);
  digitalWrite(MOTOR_RELAY_PIN, HIGH);
  digitalWrite(LIGHT_RELAY_PIN, HIGH);
  delay_sec(2);
  digitalWrite(LED_BUILTIN, LOW);
  attachInterrupt(digitalPinToInterrupt(INT_PIN), ISR_launchCycle, LOW);
}
