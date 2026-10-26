# Portail47v2 — state machine

What `Portail47v2.ino` does as currently written. Relays are active LOW (`RELAY_ON` = `LOW`). Every `T_…` / `BUTTON_…` name is a constant in the **ON-SITE TUNING** block at the top of the sketch.

## Gate states

```plantuml
@startuml
hide empty description
skinparam state {
  BackgroundColor<<stable>> #DDEEDD
  BackgroundColor<<motion>> #EEE6CC
}

[*] --> Boot

state Boot {
  state "All relays OFF\nattach INT0 (pin 2, FALLING)" as B0
  state "Read EEPROM" as B1
  [*] --> B0
  B0 --> B1
  B1 --> [*] : no action taken
}

state choose <<choice>>
Boot --> choose
choose --> CLOSED : stored CLOSED
choose --> OPEN : stored OPEN\nor no signature / bad value

state CLOSED <<stable>> : all relays OFF\nEEPROM = CLOSED

state OPEN <<stable>> {
  state "Grace\nMOTOR + LIGHT ON" as Grace
  state "Idle\nall relays OFF" as Idle
  [*] --> Idle : (from boot)
  Grace --> Idle : after T_ABORT_GRACE_MS\nMOTOR OFF, LIGHT OFF
}
OPEN : EEPROM = OPEN

state "LONG_CYCLING / SHORT_CYCLING" as CYCLING <<motion>> {
  state "0: LIGHT ON" as S0
  state "1: MOTOR ON" as S1
  state "2: DOOR ON" as S2
  state "3: DOOR OFF" as S3
  state "4: OPEN ON" as S4
  state "5: OPEN OFF" as S5
  state "6: CLOSE ON" as S6
  state "7: CLOSE OFF" as S7
  state "8: MOTOR OFF" as S8
  state "9: LIGHT OFF" as S9
  [*] --> S0 : EEPROM ← OPEN
  S0 --> S1 : immediately
  S1 --> S2 : after T_MOTOR_SPINUP_MS\n(0 if motor still on from Grace)
  S2 --> S3 : after T_LATCH_PULSE_MS
  S3 --> S4 : immediately
  S4 --> S5 : after T_OPEN_TRAVEL_MS
  S5 --> S6 : after T_HOLD_OPEN_LONG_MS (long)\nor T_HOLD_OPEN_SHORT_MS (short)
  S6 --> S7 : after T_CLOSE_TRAVEL_MS
  S7 --> S8 : after T_CLOSE_SETTLE_MS
  S8 --> S9 : immediately
  S9 --> [*]
}

CLOSED --> CYCLING : button\n(LONG_CYCLING)
OPEN --> CYCLING : button\n(SHORT_CYCLING)
CYCLING --> CLOSED : cycle complete\nEEPROM ← CLOSED
CYCLING --> Grace : button (abort)\nOPEN, CLOSE, DOOR OFF at once
CYCLING --> Idle : watchdog: planned length\n+ CYCLE_WATCHDOG_MARGIN_MS\nall relays OFF

note right of CLOSED
  "button" = falling edge on pin 2, still LOW
  after BUTTON_DEBOUNCE_MS, and at least
  BUTTON_GUARD_MS after the last accepted press
end note
@enduml
```

## Button handling, main loop and interrupt

```plantuml
@startuml
hide empty description

state "ISR onButtonEdge()" as ISR {
  state "edge already pending ?" as I0
  state "buttonEdgeMs = now\nbuttonEdgePending = true" as I1
  [*] --> I0
  I0 --> I1 : no
  I0 --> [*] : yes (ignored)
  I1 --> [*]
}

state "loop()" as LOOP {
  state "pollButton()" as P {
    state "edge pending and\nnow - edgeMs ≥ BUTTON_DEBOUNCE_MS ?" as P0
    state "clear pending\npin still LOW ?" as P1
    state "now - lastPressMs ≥ BUTTON_GUARD_MS ?" as P2
    state "accepted press\nlastPressMs = now" as P3
    [*] --> P0
    P0 --> [*] : no
    P0 --> P1 : yes
    P1 --> [*] : no (noise / bounce)
    P1 --> P2 : yes
    P2 --> [*] : no (guard)
    P2 --> P3 : yes
    P3 --> [*]
  }
  state "handleButtonPress()" as H
  state "cycling → runCycle()" as R
  state "updateGrace()" as G
  state "Sleep (IDLE)\nuntil next interrupt" as Z
  [*] --> P
  P --> H : press accepted
  P --> R : otherwise
  H --> R
  R --> G
  G --> Z
  Z --> P : woken by button ISR\nor Timer0 tick (~1 ms)
}

ISR -[dashed]-> LOOP : sets buttonEdgePending,\nwakes CPU
@enduml
```

## Wiring

| Pin | Name | Role |
| --- | --- | --- |
| 2 | `BUTTON_PIN` | RF receiver / push button, pulls to GND (INPUT_PULLUP, INT0 on FALLING edge) |
| 6 | `MOTOR_RELAY_PIN` | Motor / pump |
| 7 | `OPEN_RELAY_PIN` | OPEN valve |
| 8 | `CLOSE_RELAY_PIN` | CLOSE valve |
| 9 | `LIGHT_RELAY_PIN` | Light |
| 10 | `DOOR_RELAY_PIN` | Door latch |
| `LED_BUILTIN` | | ON while a cycle runs |

## On-site tuning (defaults)

The defaults are placeholders; measure the real values on site.

| Constant | Default | Used for |
| --- | --- | --- |
| `BUTTON_DEBOUNCE_MS` | 50 ms | Input must still be LOW this long after the edge. Lower it if short RF pulses show up as "edge rejected" |
| `BUTTON_GUARD_MS` | 1000 ms | Minimum gap between two accepted presses |
| `T_MOTOR_SPINUP_MS` | 2000 ms | Motor running before anything moves |
| `T_LATCH_PULSE_MS` | 1000 ms | Door latch pulse |
| `T_OPEN_TRAVEL_MS` | 20000 ms | OPEN valve on |
| `T_HOLD_OPEN_LONG_MS` | 60000 ms | Gate held open, long cycle |
| `T_HOLD_OPEN_SHORT_MS` | 2000 ms | Gate held open, short cycle |
| `T_CLOSE_TRAVEL_MS` | 20000 ms | CLOSE valve on |
| `T_CLOSE_SETTLE_MS` | 1000 ms | After CLOSE valve off, before MOTOR and LIGHT go off |
| `T_ABORT_GRACE_MS` | 20000 ms | MOTOR + LIGHT kept on after an abort |
| `CYCLE_WATCHDOG_MARGIN_MS` | 30000 ms | Added to the planned cycle length before an emergency stop |

With these defaults:

| Cycle | Planned length | Watchdog fires after |
| --- | --- | --- |
| Long | 104 s | 134 s |
| Short | 46 s | 76 s |
| Short, right after an abort (spin-up skipped) | 44 s | 74 s |

## Power loss

| Moment of the power cut | EEPROM holds | After power returns |
| --- | --- | --- |
| Gate CLOSED (a cycle completed) | `CLOSED` | CLOSED, nothing moves. Next press: long cycle |
| During any cycle, long or short | `OPEN` (written when the cycle started) | OPEN, nothing moves. Next press: short cycle |
| In OPEN, during or after the grace period | `OPEN` | OPEN, nothing moves. Next press: short cycle |
| First boot / EEPROM from the old sketch | no valid signature | OPEN, nothing moves. Next press: short cycle |
