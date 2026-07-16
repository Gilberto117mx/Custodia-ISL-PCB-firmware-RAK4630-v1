# ISL_GNSS_DutyCycle — test #6 ✅ PASS

**Goal:** the core operate/sleep pattern for the ISL board — **GPS on, search up to
30 s (or until fix) → GPS off → deep-sleep 60 s (RTC wake on P0.21) → repeat** —
with the sleep floor returning to the board's baseline between searches.

Fuses the two previously-validated pieces:
- **GNSS** on `Serial0`/UART1 (P0.19/P0.20), `L76K_EN` = P1.02 active-LOW,
  9600 `RAK_CUSTOM_MODE` (test #3, `ISL_GNSS_Serial0`).
- **Deep sleep** from `ISL_DeepSleep_Baseline` (test #5): `api.ble.stop()` +
  `NVIC_DisableIRQ(FPU_IRQn)`, `clearFPU()` before sleep, RV-3028 periodic timer
  → INT falling edge on **P0.21**, `api.system.sleep.all()`. Baseline floor
  **157 µA @ 3.6 V**.

## Final result (v3, `PowerProfile_v3_isolated.csv`, 393 s, 4 full cycles)

| Cycle | GPS search | Sleep plateau | Sleep median |
|---|---|---|---|
| 1 | ~2–42 s @ ~35–43 mA | 42→102 s (**60.0 s**) | **159.5 µA** |
| 2 | ~101–133 s | 133→193 s (**60.0 s**) | **158.0 µA** |
| 3 | ~192–224 s | 224→284 s (**60.0 s**) | **157.4 µA** |
| 4 | ~283–315 s | 315→375 s (**60.0 s**) | **157.3 µA** |

- Sleep floor = **157–159 µA @ 3.6 V** → **matches the no-GPS baseline** (157 µA),
  i.e. the GPS circuit adds ~0 µA to the sleep floor. Acceptance criterion: GPS
  adds ~0 to the bare-board floor.
- Every sleep window is exactly 60.0 s and ends on the **RTC P0.21 wake** — no
  early wakes, no backstops.
- Run was indoor (no fix; `sats=0`); the duty-cycle/power behaviour is what this
  test validates. Fix validation needs sky view. The external L76X keeps its own
  backup battery, so field starts are **hot starts** (faster than the 30 s window).

## Debug history — how we got there

| Ver | Symptom | Cause | Fix |
|---|---|---|---|
| v1 | `[WAKE] backstop after ~3 ms` — sleep returned instantly; current stuck ~43 mA | GPS TX line still settling + un-parked UART1 RX (P0.19) fired a GPIO wake the moment `sleep.all()` ran | cut `L76K_EN` first → wait 250 ms for the TX line to go quiet → `Serial0.end()` → park RX |
| v2 | Sleep held, but floor ≈ **600 µA** instead of 157 µA (`PowerProfile_v2_phantom-power.csv`) | **Phantom-powering the external GPS module**: with its main VCC dead (EN off) but P0.19 left `INPUT_PULLUP` (and P0.20 high from `end()`), the MCU sourced ~440 µA through the module's I/O ESD clamps into its dead rail / backup cell | drive **both** UART lines **LOW** during sleep (`pinMode OUTPUT` + `digitalWrite LOW` on P0.19 & P0.20) — full module isolation |
| **v3** | ✅ 4× clean cycles, floor **157–159 µA** | — | as committed |

The complete teardown order (now the ISL standard, used by the production port):
```c
digitalWrite(GPS_EN_PIN, HIGH);      // 1. cut module power first (active-low EN)
delay(250);                          // 2. let the TX line go quiet (VCC-cap tail)
Serial0.end();                       // 3. release UART1
pinMode(P0_19, OUTPUT); digitalWrite(P0_19, LOW);   // 4. isolate: no back-feed
pinMode(P0_20, OUTPUT); digitalWrite(P0_20, LOW);   //    through either UART line
```

## Files
- `ISL_GNSS_DutyCycle.ino` — final v3 sketch (alive-first, USB prints guarded).
- `PowerProfile_v3_isolated.csv` — passing capture (10 ms samples, 393 s).
- `PowerProfile_v2_phantom-power.csv` — kept for history: sleep holds but floor
  ~600 µA (first ~1.8 mA portion of that capture is with USB still attached).

## Notes
- Native USB (`Serial`) drops at the first `[SLEEP]` — expected; measure on
  battery. All prints are `if (Serial)`-guarded so a dead port never blocks.
- Supply for all deep-sleep numbers: **3.6 V** (project convention — battery nominal).
- Average current at this cadence (30 s search / 60 s sleep, no fix, indoor):
  ≈ (30 s × ~38 mA + 60 s × 0.157 mA) / 90 s ≈ **12.8 mA** — dominated by GPS-on
  time; a hot-start fix outdoors shortens the on-phase substantially, and longer
  sleep periods (production knob) drop it further.

See `../../docs/ISL_DeepSleep_Notes.md` for the consolidated deep-sleep rules.
