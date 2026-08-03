# ISL Board — Deep-Sleep Rules (consolidated)

Every rule needed to reach and hold the ISL deep-sleep floor, established during
this board's bring-up (tests #5/#6) and the v7 floor fix. **The production firmware
must follow all of these.**

## The numbers (all @ 3.6 V — project convention, battery nominal)
| State | Floor |
|---|---|
| **ISL baseline, everything parked (test #5)** | **157 µA** — but this was an AIN7 GPIO crowbar (see RESOLVED note below), **fixed in v7 → 34 µA** |
| **ISL production v7 (crowbar fixed), modules attached** | **34 µA @ 3.6 V** (battery, verified `../production/v7/logs/`) |
| **ISL with GPS duty-cycled (test #6 v3)** | **157–159 µA** — GPS adds ~0 when torn down correctly |
| ISL with GPS torn down *wrong* (test #6 v2) | ~600 µA (phantom-powering the module — see rule 4) |

## Rules

1. **Sleep prerequisites (once, at init):** `api.ble.stop();`
   `NVIC_DisableIRQ(FPU_IRQn);` — and park the radio (`api.lora.precv(0)`) if LoRa
   was used.
2. **Immediately before every sleep:** `clearFPU();` (clear FPSCR exception bits +
   pending FPU IRQ). Keep float math out of the sleep path — integer math for
   packet formatting / battery mV.
3. **GPS teardown order (prevents the instant-wake):**
   cut `L76K_EN` (P1.02 HIGH) **first** → `delay(250)` while the module dies and
   its TX line goes quiet → **then** `Serial0.end()`. Tearing down the UART while
   NMEA edges are still arriving makes `api.system.sleep.all()` return in ~3 ms
   ("backstop" wake).
4. **GPS isolation during sleep (ISL-specific, test #6 v2→v3, −440 µA):** after
   `Serial0.end()`, drive **both UART1 pins LOW**:
   `pinMode(P0_19, OUTPUT); digitalWrite(P0_19, LOW);` (same for **P0.20**).
   The L76X is an *external* module with its own backup battery; with its main
   VCC dead, any UART pin left high (RX `INPUT_PULLUP`, or TX idle-high) sources
   ~440 µA through the module's I/O ESD clamps into the dead rail. On this board
   the leak path is the UART itself, so driving both lines LOW closes it.
5. **Park every other peripheral pin to a defined level** (no floating inputs):
   AS3933 SPI CLK/MOSI/CS driven LOW (CS is active-HIGH, so LOW = deselected),
   MISO/WAKE as INPUT, battery-sense tap as INPUT.
6. **Wake source:** RV-3028 periodic timer, single-shot → ~INT (falling edge) on
   **P0.21** (schematic v2 — *not* P1.03/P1.04). 12-bit preset; **auto-select the
   tick** (implemented in `production/v3`): ≤4095 s → 1 Hz tick (1 s granularity);
   longer → **1/60 Hz tick** (preset in minutes, max 4095 min ≈ 2.8 days, 1 min
   granularity; first period ±1 tick). **The sleep backstop must scale with the
   period** (period + margin) — a fixed 1 h backstop cuts a 2 h sleep short.
   Clear TF after every wake. Second wake source (WUR, rising edge on P1.04) to
   be armed once the AS3933 real-wake is validated.
7. **USB debug reality:** native USB CDC (`Serial`) powers down in deep sleep —
   the COM port vanishing at `[SLEEP]` is the *success* signal. Guard every
   print with `if (Serial)`, and do all power measurements on battery.
8. **Quote every floor with its supply voltage** (nRF buck ⇒ I ∝ 1/Vin). This
   project measures at **3.6 V**.
9. **Alive-first structure** (board quirk): tiny `setup()` (USB begin + banner),
   heartbeat then real init a few seconds into `loop()` — heavy init in `setup()`
   wedges the native-USB app before it prints.

## ⚠️ CORRECTION (2026-07-16): the 157 µA floor is NOT the divider
Earlier notes — and the v1–v6 pin-map comments and test READMEs — said the divider
is **10k/10k** (~180 µA) and that it *is* the 157 µA floor. **That was a wrong
schematic diagram; it was never the real board.** The actual board (schematic v2,
confirmed on hardware) has a **1 MΩ/1 MΩ divider + C17 100 nF filter**, drawing only
**~1.8 µA**. The battery-voltage calibration is unaffected (the ratio is still 2.0).

## ✅ RESOLVED (2026-07-19): the ~120 µA was an AIN7 GPIO crowbar — fixed in v7
A step-by-step deep-sleep teardown found it: the whole overage was
**`pinMode(P0.31, INPUT)`** on the battery-sense pin. P0.31 (AIN7) floats at the
**1 MΩ divider midpoint ≈ VDD/2**; a *connected* digital input buffer held at
mid-rail conducts ~**118 µA** of shoot-through ("crowbar") current.
- **Evidence:** minimal sketch = **32–37 µA** (modules attached).
  Additive sweep localized the jump to the GPIO-parking step (34 → 152 µA). The
  park-breakdown pinned it to ONE pin: batt-sense OFF = 34 µA, every other parked
  pin (GPS_EN/UART, WUR, RTC_INT) = 152 µA. Wire, RV-3028 config, the trickle-charge
  write, the countdown timer, and the wake-pin setup each cost **~0 µA**.
- **Fix (production v7):** keep P0.31's input buffer **DISCONNECTED** except during
  the SAADC sample — `battPinDisconnect()` writes `NRF_P0->PIN_CNF[31] = 2`, called
  after ADC init, after each read, and before every `deepSleep()`/`napSleep()`. The
  SAADC reads the analog voltage through its own mux, so the calibrated battery read
  is unaffected. **Floor: ~155 → 34 µA (with GNSS + WUR attached, verified on
  battery) ≈ 4.5× idle life.**
- The old "raise the divider" and "maybe it's the LDO / the modules" theories are
  both disproven. It was our own `pinMode`. The RT9080 LDO is fine (~32 µA bare).

### For any future firmware: never leave a connected input buffer on an analog /
### high-impedance pin sitting at mid-rail. Configure ADC pins as input-DISCONNECT.
