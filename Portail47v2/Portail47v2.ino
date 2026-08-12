/*
  Gate controller
  - Data-driven step tables for opening & closing
  - Interruptible procedures (CLOSED -> OPENING -> OPENED -> CLOSING -> CLOSED)
  - ISR only sets a flag; 1 second debounce
  - Safety watchdog timeout for long operations
  - EEPROM persistence of last stable state (OPENED / CLOSED)

  Notes:
  - The relay logic follows the previous convention (HIGH = relay off for many boards).
  - Adjust the Step durations to match your mechanical timings.
*/

#include <EEPROM.h>

// ----- Pins -----
#define MOTOR_RELAY_PIN 6
#define OPEN_RELAY_PIN 7
#define CLOSE_RELAY_PIN 8
#define LIGHT_RELAY_PIN 9
#define DOOR_RELAY_PIN 10

#define INT_PIN 2          // RF / push button input
#define DEBOUNCE_MS 1000   // 1 second between valid interrupts

// ----- Safety -----
const uint32_t MAX_OPERATION_MS = 120000UL; // maximum allowed ms for any opening/closing (120s). Tune as needed.

// ----- EEPROM layout -----
const uint8_t EEPROM_SIGNATURE = 0x47;
const int EEPROM_ADDR_SIG = 0;
const int EEPROM_ADDR_STATE = 1;

// ----- State machine -----
enum GateState : uint8_t {
  CLOSED = 0,
  OPENING = 1,
  OPENED = 2,
  CLOSING = 3
};

volatile GateState gateState = CLOSED;     // volatile because ISR and main both touch flags
volatile bool buttonPressed = false;       // set by ISR when a valid press occurs
volatile unsigned long lastInterruptMs = 0;

// ----- Step structure & sequences -----
struct Step {
  uint8_t pin;
  uint8_t value;        // HIGH or LOW
  uint32_t duration;    // milliseconds to wait after setting this step before next
};

// Opening sequence (data-driven). Modify durations to tune behavior.
Step openingSteps[] = {
  // pin, value, duration
  { LIGHT_RELAY_PIN, LOW,   0 },    // lights ON (active LOW in original design)
  { MOTOR_RELAY_PIN, LOW,   2000 }, // start motor, wait T1
  { DOOR_RELAY_PIN,  LOW,   1000 }, // push door latch, wait T2
  { DOOR_RELAY_PIN,  HIGH,  5000 }, // retract latch, wait T3
  { OPEN_RELAY_PIN,  HIGH,  1000 }, // finalize, wait T4
  { MOTOR_RELAY_PIN, HIGH,  40000 } // stop motor, T5 (large travel time)
};
const int openingCount = sizeof(openingSteps) / sizeof(Step);

// Closing sequence
Step closingSteps[] = {
  { MOTOR_RELAY_PIN, LOW,   2000 },  // start motor T6
  { CLOSE_RELAY_PIN, LOW,   10000 }, // close valve T7
  { CLOSE_RELAY_PIN, HIGH,  1000 },  // release valve T8
  { MOTOR_RELAY_PIN, HIGH,  0 }      // stop motor, end
};
const int closingCount = sizeof(closingSteps) / sizeof(Step);

// ----- Sequence runtime variables -----
int stepIndex = -1;                      // -1 => no active sequence
uint32_t stepStartMs = 0;                // timestamp when current step was started
uint32_t sequenceStartMs = 0;            // timestamp when sequence (open/close) began (for watchdog)

// ----- Helpers for EEPROM persistence -----
void saveStateToEEPROM(GateState s) {
  // Only write signature + state when in stable state (CLOSED or OPENED).
  // Uses EEPROM.update to avoid unnecessary writes.
  EEPROM.update(EEPROM_ADDR_SIG, EEPROM_SIGNATURE);
  EEPROM.update(EEPROM_ADDR_STATE, (uint8_t)s);
}

GateState loadStateFromEEPROM() {
  if (EEPROM.read(EEPROM_ADDR_SIG) != EEPROM_SIGNATURE) {
    return CLOSED; // default safe
  }
  uint8_t stored = EEPROM.read(EEPROM_ADDR_STATE);
  if (stored <= 3) return (GateState)stored;
  return CLOSED;
}

// ----- ISR (very small) -----
void handleInterrupt() {
  unsigned long now = millis();
  if (now - lastInterruptMs > DEBOUNCE_MS) {
    buttonPressed = true;
    lastInterruptMs = now;
  }
}

// ----- Relay helpers -----
inline void allRelaysOffState() {
  // Set to "off" for relays (following original HIGH => off)
  digitalWrite(MOTOR_RELAY_PIN, HIGH);
  digitalWrite(OPEN_RELAY_PIN, HIGH);
  digitalWrite(CLOSE_RELAY_PIN, HIGH);
  digitalWrite(DOOR_RELAY_PIN, HIGH);
  digitalWrite(LIGHT_RELAY_PIN, HIGH);
  digitalWrite(LED_BUILTIN, LOW);
}

// Apply a step: set pin to value and record start time
void applyStep(const Step &s) {
  digitalWrite(s.pin, s.value);
  stepStartMs = millis();
}

// Abort the current running sequence
void abortSequence() {
  stepIndex = -1;
  // leave current relay states as-is (mechanical system is resilient and user wanted interruptible)
  // Optionally: set motor off for safety when aborting sequences
  // digitalWrite(MOTOR_RELAY_PIN, HIGH);
  // digitalWrite(OPEN_RELAY_PIN, HIGH);
  // digitalWrite(CLOSE_RELAY_PIN, HIGH);
}

// Emergency safe stop when watchdog expires
void emergencyStop() {
  allRelaysOffState();
  abortSequence();
  // Put into safest known state; CLOSED is chosen as safest.
  gateState = CLOSED;
  saveStateToEEPROM(gateState);
}

// Start opening sequence (called when transitioning to OPENING)
void startOpeningSequence() {
  stepIndex = 0;
  sequenceStartMs = millis();
  digitalWrite(LED_BUILTIN, HIGH);
  // Immediately apply first step:
  applyStep(openingSteps[0]);
}

// Run the opening state machine one tick
void runOpeningSequence() {
  if (stepIndex < 0) return; // aborted

  // Watchdog check
  if (millis() - sequenceStartMs > MAX_OPERATION_MS) {
    emergencyStop();
    return;
  }

  // If current step has duration == 0, move to next immediately (after having set pin)
  uint32_t dur = openingSteps[stepIndex].duration;
  if (dur == 0) {
    // advance to next step (but ensure we don't loop infinitely)
    stepIndex++;
    if (stepIndex >= openingCount) {
      // finished sequence
      gateState = OPENED;
      abortSequence();
      // Make sure stable outputs for OPENED: stop motor, lights ON (original behavior)
      digitalWrite(MOTOR_RELAY_PIN, HIGH);
      digitalWrite(LIGHT_RELAY_PIN, HIGH);
      digitalWrite(LED_BUILTIN, LOW);
      saveStateToEEPROM(gateState);
      return;
    } else {
      applyStep(openingSteps[stepIndex]);
      return;
    }
  }

  // otherwise check if duration has elapsed
  if (millis() - stepStartMs >= dur) {
    // special-case: some original logic toggled DOOR_RELAY between steps; we rely on table ordering
    stepIndex++;
    if (stepIndex >= openingCount) {
      // finished
      gateState = OPENED;
      abortSequence();
      digitalWrite(MOTOR_RELAY_PIN, HIGH);
      digitalWrite(LIGHT_RELAY_PIN, HIGH);
      digitalWrite(LED_BUILTIN, LOW);
      saveStateToEEPROM(gateState);
      return;
    } else {
      applyStep(openingSteps[stepIndex]);
    }
  }
}

// Start closing sequence (called when transitioning to CLOSING)
void startClosingSequence() {
  stepIndex = 0;
  sequenceStartMs = millis();
  digitalWrite(LED_BUILTIN, HIGH);
  applyStep(closingSteps[0]);
}

// Run closing sequence one tick
void runClosingSequence() {
  if (stepIndex < 0) return;

  // watchdog
  if (millis() - sequenceStartMs > MAX_OPERATION_MS) {
    emergencyStop();
    return;
  }

  uint32_t dur = closingSteps[stepIndex].duration;
  if (dur == 0) {
    stepIndex++;
    if (stepIndex >= closingCount) {
      gateState = CLOSED;
      abortSequence();
      allRelaysOffState();
      saveStateToEEPROM(gateState);
      return;
    } else {
      applyStep(closingSteps[stepIndex]);
      return;
    }
  }

  if (millis() - stepStartMs >= dur) {
    stepIndex++;
    if (stepIndex >= closingCount) {
      gateState = CLOSED;
      abortSequence();
      allRelaysOffState();
      saveStateToEEPROM(gateState);
      return;
    } else {
      applyStep(closingSteps[stepIndex]);
    }
  }
}

// Called when a debounced button press flag is seen in loop
void handleButtonEvent() {
  Serial.print("Button event !\n");
  // Abort active sequence (we will change state accordingly)
  // But do not forcibly change relay outputs beyond what we do below.
  abortSequence();

  switch (gateState) {
    case CLOSED:
      // CLOSED -> OPENING
      gateState = OPENING;
      startOpeningSequence();
      break;

    case OPENING:
      // OPENING -> OPENED (abort opening)
      gateState = OPENED;
      // Ensure stable outputs for OPENED
      digitalWrite(MOTOR_RELAY_PIN, HIGH);
      digitalWrite(OPEN_RELAY_PIN, HIGH);
      digitalWrite(CLOSE_RELAY_PIN, HIGH);
      digitalWrite(DOOR_RELAY_PIN, HIGH);
      digitalWrite(LIGHT_RELAY_PIN, HIGH);
      digitalWrite(LED_BUILTIN, LOW);
      saveStateToEEPROM(gateState);
      break;

    case OPENED:
      // OPENED -> CLOSING
      gateState = CLOSING;
      startClosingSequence();
      break;

    case CLOSING:
      // CLOSING -> CLOSED (abort closing)
      gateState = CLOSED;
      allRelaysOffState();
      saveStateToEEPROM(gateState);
      break;
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(9600);

  pinMode(MOTOR_RELAY_PIN, OUTPUT);
  pinMode(OPEN_RELAY_PIN, OUTPUT);
  pinMode(CLOSE_RELAY_PIN, OUTPUT);
  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  pinMode(DOOR_RELAY_PIN, OUTPUT);

  // Start in safe state (all relays off) until we restore from EEPROM
  allRelaysOffState();

  // Input with pullup to stabilize signal (assumes button pulls to ground)
  pinMode(INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(INT_PIN), handleInterrupt, FALLING);

  // Load previous state
  GateState stored = loadStateFromEEPROM();

  // Normalize: if previous run was mid-motion, treat as stable safe state
  if (stored == OPENING) stored = OPENED;
  if (stored == CLOSING) stored = CLOSED;

  gateState = stored;

  // Ensure outputs match loaded stable state
  if (gateState == OPENED) {
    // motor stop, lights set as at end of opening sequence
    digitalWrite(MOTOR_RELAY_PIN, HIGH);
    digitalWrite(OPEN_RELAY_PIN, HIGH);
    digitalWrite(CLOSE_RELAY_PIN, HIGH);
    digitalWrite(DOOR_RELAY_PIN, HIGH);
    digitalWrite(LIGHT_RELAY_PIN, HIGH);
    digitalWrite(LED_BUILTIN, LOW);
  } else { // CLOSED
    allRelaysOffState();
  }
}

// Main loop: check flags, run sequences
void loop() {
  // If ISR flagged a button press, handle it here
  if (buttonPressed) {
    noInterrupts();        // briefly disable interrupts while we consume the flag
    buttonPressed = false;
    interrupts();
    handleButtonEvent();
  }

  // Run background state machines
  if (gateState == OPENING) runOpeningSequence();
  if (gateState == CLOSING) runClosingSequence();

  // Keep loop responsive
  delay(5);
}
