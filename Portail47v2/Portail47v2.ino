/*
  Gate controller — Portail47v2

  States
    CLOSED          gate closed, everything off
    LONG_CYCLING    one continuous sequence: open, hold open (long), close
    SHORT_CYCLING   same sequence with a short hold: re-opens a gate left open, then closes it
    OPEN            gate stopped somewhere (aborted cycle, or restored after a power loss)

  Transitions
    CLOSED     --button-->          LONG_CYCLING
    OPEN       --button-->          SHORT_CYCLING
    *_CYCLING  --button-->          OPEN    (gate stops at once; MOTOR + LIGHT stay on for a grace period)
    *_CYCLING  --cycle complete-->  CLOSED

  EEPROM only ever holds OPEN or CLOSED: OPEN is written as soon as any cycle starts, CLOSED only
  when a cycle completes. Power-on restores that state and does nothing else.

  Button input: the ISR only timestamps a falling edge. The main loop accepts it as a press if the
  input is still LOW after BUTTON_DEBOUNCE_MS, and at least BUTTON_GUARD_MS after the last press.

  Every timing is in the ON-SITE TUNING block below. Relays are active LOW.
*/

#include <EEPROM.h>
#include <avr/sleep.h>

// ============================ ON-SITE TUNING ============================
// All durations in milliseconds.

// Button / RF input
const uint32_t BUTTON_DEBOUNCE_MS = 50;    // input must still be LOW this long after the falling edge
const uint32_t BUTTON_GUARD_MS    = 1000;  // presses closer than this to the last accepted one are ignored

// Cycle (order of use: see cycleSteps[])
const uint32_t T_MOTOR_SPINUP_MS    = 2000;   // motor running before anything moves
const uint32_t T_LATCH_PULSE_MS     = 1000;   // door latch held to release the gate
const uint32_t T_OPEN_TRAVEL_MS     = 20000;  // OPEN valve on: gate travelling open
const uint32_t T_HOLD_OPEN_LONG_MS  = 60000;  // gate held open, long cycle
const uint32_t T_HOLD_OPEN_SHORT_MS = 2000;   // gate held open, short cycle
const uint32_t T_CLOSE_TRAVEL_MS    = 20000;  // CLOSE valve on: gate travelling closed
const uint32_t T_CLOSE_SETTLE_MS    = 1000;   // after CLOSE valve off, before MOTOR and LIGHT go off

// Abort
const uint32_t T_ABORT_GRACE_MS     = 20000;  // MOTOR + LIGHT kept on after an abort, waiting for another press

// Safety: a cycle running longer than its planned length plus this margin triggers an emergency stop
const uint32_t CYCLE_WATCHDOG_MARGIN_MS = 30000;
// ========================================================================

// ----- Pins -----
#define MOTOR_RELAY_PIN 6
#define OPEN_RELAY_PIN  7
#define CLOSE_RELAY_PIN 8
#define LIGHT_RELAY_PIN 9
#define DOOR_RELAY_PIN  10

#define BUTTON_PIN 2       // RF receiver / push button, pulls the input to GND

#define RELAY_ON  LOW
#define RELAY_OFF HIGH

const long SERIAL_BAUD = 9600;

// ----- EEPROM layout -----
const uint8_t EEPROM_SIGNATURE = 0x48;   // was 0x47 with the old 4-state layout, which is now ignored
const int EEPROM_ADDR_SIG = 0;
const int EEPROM_ADDR_STATE = 1;

// ----- State machine -----
enum GateState : uint8_t {
  CLOSED = 0,
  OPEN = 1,
  LONG_CYCLING = 2,
  SHORT_CYCLING = 3
};

GateState gateState = CLOSED;

// ----- Button (shared with the ISR) -----
volatile bool buttonEdgePending = false;
volatile uint32_t buttonEdgeMs = 0;
uint32_t lastPressMs = 0;

// ----- Cycle definition -----
struct Step {
  uint8_t pin;
  uint8_t value;            // RELAY_ON / RELAY_OFF
  const uint32_t *waitMs;   // wait after this step before the next one
};

const uint32_t NO_WAIT = 0;
uint32_t cycleSpinupMs = 0;   // T_MOTOR_SPINUP_MS, or 0 when the motor is still running from an aborted cycle
uint32_t cycleHoldMs = 0;     // T_HOLD_OPEN_LONG_MS or T_HOLD_OPEN_SHORT_MS

// One cycle = open + hold + close. MOTOR and LIGHT stay on from the first steps to the last ones.
const Step cycleSteps[] = {
  { LIGHT_RELAY_PIN, RELAY_ON,  &NO_WAIT },
  { MOTOR_RELAY_PIN, RELAY_ON,  &cycleSpinupMs },
  { DOOR_RELAY_PIN,  RELAY_ON,  &T_LATCH_PULSE_MS },
  { DOOR_RELAY_PIN,  RELAY_OFF, &NO_WAIT },
  { OPEN_RELAY_PIN,  RELAY_ON,  &T_OPEN_TRAVEL_MS },
  { OPEN_RELAY_PIN,  RELAY_OFF, &cycleHoldMs },
  { CLOSE_RELAY_PIN, RELAY_ON,  &T_CLOSE_TRAVEL_MS },
  { CLOSE_RELAY_PIN, RELAY_OFF, &T_CLOSE_SETTLE_MS },
  { MOTOR_RELAY_PIN, RELAY_OFF, &NO_WAIT },
  { LIGHT_RELAY_PIN, RELAY_OFF, &NO_WAIT },
};
const int CYCLE_STEP_COUNT = sizeof(cycleSteps) / sizeof(Step);

// ----- Cycle runtime -----
int stepIndex = -1;           // -1 => no cycle running
uint32_t stepStartMs = 0;
uint32_t cycleStartMs = 0;
uint32_t cycleLimitMs = 0;    // watchdog limit for the running cycle

// ----- Abort grace period -----
bool graceActive = false;     // MOTOR + LIGHT still on after an abort
uint32_t graceStartMs = 0;

// ----- Logging helpers (strings kept in flash via F() to save RAM) -----
const __FlashStringHelper *stateName(GateState s) {
  switch (s) {
    case CLOSED:        return F("CLOSED");
    case OPEN:          return F("OPEN");
    case LONG_CYCLING:  return F("LONG_CYCLING");
    case SHORT_CYCLING: return F("SHORT_CYCLING");
  }
  return F("UNKNOWN");
}

const __FlashStringHelper *pinName(uint8_t pin) {
  switch (pin) {
    case MOTOR_RELAY_PIN: return F("MOTOR");
    case OPEN_RELAY_PIN:  return F("OPEN");
    case CLOSE_RELAY_PIN: return F("CLOSE");
    case LIGHT_RELAY_PIN: return F("LIGHT");
    case DOOR_RELAY_PIN:  return F("DOOR");
  }
  return F("?");
}

// Prints "[<millis> ms] " at the start of each log line
void logPrefix() {
  Serial.print('[');
  Serial.print(millis());
  Serial.print(F(" ms] "));
}

bool isCycling() {
  return gateState == LONG_CYCLING || gateState == SHORT_CYCLING;
}

// ----- Relay helpers -----
void setRelay(uint8_t pin, uint8_t value) {
  logPrefix();
  Serial.print(pinName(pin));
  Serial.println(value == RELAY_ON ? F(" ON") : F(" OFF"));
  digitalWrite(pin, value);
}

void allRelaysOff() {
  logPrefix();
  Serial.println(F("Relays: all OFF"));
  digitalWrite(MOTOR_RELAY_PIN, RELAY_OFF);
  digitalWrite(OPEN_RELAY_PIN, RELAY_OFF);
  digitalWrite(CLOSE_RELAY_PIN, RELAY_OFF);
  digitalWrite(DOOR_RELAY_PIN, RELAY_OFF);
  digitalWrite(LIGHT_RELAY_PIN, RELAY_OFF);
  digitalWrite(LED_BUILTIN, LOW);
}

// ----- EEPROM persistence (only CLOSED or OPEN) -----
void saveStoredState(GateState s) {
  bool changed = EEPROM.read(EEPROM_ADDR_SIG) != EEPROM_SIGNATURE
              || EEPROM.read(EEPROM_ADDR_STATE) != (uint8_t)s;
  logPrefix();
  Serial.print(F("EEPROM: "));
  Serial.print(stateName(s));
  Serial.println(changed ? F(" saved") : F(" (unchanged)"));
  // EEPROM.update only writes bytes that differ, sparing EEPROM wear
  EEPROM.update(EEPROM_ADDR_SIG, EEPROM_SIGNATURE);
  EEPROM.update(EEPROM_ADDR_STATE, (uint8_t)s);
}

// Unknown or invalid content is treated as OPEN: the gate position can't be trusted,
// and the next press runs a short cycle, which always ends closed.
GateState loadStoredState() {
  if (EEPROM.read(EEPROM_ADDR_SIG) != EEPROM_SIGNATURE) {
    logPrefix();
    Serial.println(F("EEPROM: no valid signature, defaulting to OPEN"));
    return OPEN;
  }
  uint8_t stored = EEPROM.read(EEPROM_ADDR_STATE);
  if (stored == CLOSED || stored == OPEN) {
    logPrefix();
    Serial.print(F("EEPROM: loaded "));
    Serial.println(stateName((GateState)stored));
    return (GateState)stored;
  }
  logPrefix();
  Serial.print(F("EEPROM: invalid stored value "));
  Serial.print(stored);
  Serial.println(F(", defaulting to OPEN"));
  return OPEN;
}

// ----- Cycle -----
void applyStep(const Step &s) {
  logPrefix();
  Serial.print(stateName(gateState));
  Serial.print(F(" step "));
  Serial.print(stepIndex);
  Serial.print('/');
  Serial.print(CYCLE_STEP_COUNT - 1);
  Serial.print(F(": "));
  Serial.print(pinName(s.pin));
  Serial.print(s.value == RELAY_ON ? F(" ON") : F(" OFF"));
  Serial.print(F(", wait "));
  Serial.print(*s.waitMs);
  Serial.println(F(" ms"));
  digitalWrite(s.pin, s.value);
  stepStartMs = millis();
}

uint32_t plannedCycleMs() {
  uint32_t total = 0;
  for (int i = 0; i < CYCLE_STEP_COUNT; i++) total += *cycleSteps[i].waitMs;
  return total;
}

void startCycle(GateState kind) {
  bool motorRunning = graceActive;   // left on by an aborted cycle
  graceActive = false;
  cycleSpinupMs = motorRunning ? 0 : T_MOTOR_SPINUP_MS;
  cycleHoldMs = (kind == LONG_CYCLING) ? T_HOLD_OPEN_LONG_MS : T_HOLD_OPEN_SHORT_MS;

  // From now until a cycle completes, a power loss must leave the gate recorded as OPEN
  saveStoredState(OPEN);

  gateState = kind;
  cycleStartMs = millis();
  cycleLimitMs = plannedCycleMs() + CYCLE_WATCHDOG_MARGIN_MS;

  logPrefix();
  Serial.print(stateName(kind));
  Serial.print(F(" start, planned "));
  Serial.print(plannedCycleMs());
  Serial.println(F(" ms"));
  if (motorRunning) {
    logPrefix();
    Serial.println(F("Motor still running from the aborted cycle: spin-up skipped"));
  }

  digitalWrite(LED_BUILTIN, HIGH);
  stepIndex = 0;
  applyStep(cycleSteps[0]);
}

void finishCycle() {
  logPrefix();
  Serial.print(stateName(gateState));
  Serial.print(F(" complete in "));
  Serial.print(millis() - cycleStartMs);
  Serial.println(F(" ms -> CLOSED"));
  stepIndex = -1;
  allRelaysOff();   // the last steps already did this; makes the CLOSED outputs explicit
  gateState = CLOSED;
  saveStoredState(CLOSED);
}

// Button during a cycle: stop the gate now, keep MOTOR + LIGHT on for the grace period
void abortCycle() {
  logPrefix();
  Serial.print(stateName(gateState));
  Serial.print(F(" aborted at step "));
  Serial.print(stepIndex);
  Serial.println(F(" -> OPEN"));
  stepIndex = -1;
  setRelay(OPEN_RELAY_PIN, RELAY_OFF);
  setRelay(CLOSE_RELAY_PIN, RELAY_OFF);
  setRelay(DOOR_RELAY_PIN, RELAY_OFF);
  digitalWrite(LED_BUILTIN, LOW);
  gateState = OPEN;   // EEPROM already holds OPEN since the cycle started

  graceActive = true;
  graceStartMs = millis();
  logPrefix();
  Serial.print(F("Grace period: MOTOR + LIGHT stay on for "));
  Serial.print(T_ABORT_GRACE_MS);
  Serial.println(F(" ms"));
}

void emergencyStop() {
  logPrefix();
  Serial.print(F("!!! WATCHDOG: "));
  Serial.print(stateName(gateState));
  Serial.print(F(" exceeded "));
  Serial.print(cycleLimitMs);
  Serial.println(F(" ms, emergency stop -> OPEN"));
  stepIndex = -1;
  graceActive = false;
  allRelaysOff();
  gateState = OPEN;
  saveStoredState(OPEN);
}

// Advance through every step whose wait has elapsed (zero waits chain in the same call)
void runCycle() {
  if (millis() - cycleStartMs > cycleLimitMs) {
    emergencyStop();
    return;
  }
  while (millis() - stepStartMs >= *cycleSteps[stepIndex].waitMs) {
    if (++stepIndex >= CYCLE_STEP_COUNT) {
      finishCycle();
      return;
    }
    applyStep(cycleSteps[stepIndex]);
  }
}

void updateGrace() {
  if (!graceActive || millis() - graceStartMs < T_ABORT_GRACE_MS) return;
  graceActive = false;
  logPrefix();
  Serial.println(F("Grace period over"));
  setRelay(MOTOR_RELAY_PIN, RELAY_OFF);
  setRelay(LIGHT_RELAY_PIN, RELAY_OFF);
}

// ----- Button -----
// ISR: only timestamp the first falling edge; pollButton() decides whether it was a real press
void onButtonEdge() {
  if (!buttonEdgePending) {
    buttonEdgeMs = millis();
    buttonEdgePending = true;
  }
}

// Returns true once per accepted press
bool pollButton() {
  if (!buttonEdgePending) return false;

  noInterrupts();   // 32-bit value written by the ISR
  uint32_t edgeMs = buttonEdgeMs;
  interrupts();

  uint32_t now = millis();
  if (now - edgeMs < BUTTON_DEBOUNCE_MS) return false;

  bool stillLow = digitalRead(BUTTON_PIN) == LOW;
  buttonEdgePending = false;   // re-arm the ISR

  if (!stillLow) {
    logPrefix();
    Serial.println(F("Button: edge rejected (input not held LOW, noise/bounce)"));
    return false;
  }
  if (now - lastPressMs < BUTTON_GUARD_MS) {
    logPrefix();
    Serial.println(F("Button: press ignored (guard time)"));
    return false;
  }
  lastPressMs = now;
  return true;
}

void handleButtonPress() {
  logPrefix();
  Serial.print(F("Button press in state "));
  Serial.println(stateName(gateState));

  switch (gateState) {
    case CLOSED:
      startCycle(LONG_CYCLING);
      break;
    case OPEN:
      startCycle(SHORT_CYCLING);
      break;
    case LONG_CYCLING:
    case SHORT_CYCLING:
      abortCycle();
      break;
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  logPrefix();
  Serial.println(F("=== Gate controller boot ==="));

  // Drive the outputs OFF before making them outputs, so no relay clicks at boot
  allRelaysOff();
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(MOTOR_RELAY_PIN, OUTPUT);
  pinMode(OPEN_RELAY_PIN, OUTPUT);
  pinMode(CLOSE_RELAY_PIN, OUTPUT);
  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  pinMode(DOOR_RELAY_PIN, OUTPUT);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButtonEdge, FALLING);

  // Restore the state only: nothing moves after a power cut
  gateState = loadStoredState();
  logPrefix();
  Serial.print(F("Boot: restored "));
  Serial.print(stateName(gateState));
  Serial.println(F(", waiting for button"));
}

void loop() {
  if (pollButton()) handleButtonPress();
  if (isCycling()) runCycle();
  updateGrace();

  // Sleep until the next interrupt: the button ISR, or the Timer0 tick (~1 ms) that drives millis().
  // An interrupt landing just before the sleep only delays the next pass by one tick.
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_mode();
}
