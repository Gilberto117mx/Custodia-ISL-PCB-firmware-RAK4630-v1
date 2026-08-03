# ISL_DeepSleep_Baseline — test #5 ✅ PASS

**Goal:** establish the ISL board's deep-sleep current floor, and validate the
schematic-v2 **RTC wake pin P0.21** for the first time.

Sleep path (the ISL standard the production firmware later reuses):
`api.ble.stop()` + `NVIC_DisableIRQ(FPU_IRQn)` (init) → peripherals parked →
RV-3028 periodic timer armed (single-shot, 1 Hz tick) → `clearFPU()` →
`api.system.sleep.all(backstop)` → wake on **P0.21 falling edge** → repeat (30 s cadence).

## Result (`PowerProfile_baseline.csv`, 76 s @ 10 ms samples, supply = 3.6 V)

| Phase | Time | Current |
|---|---|---|
| Inrush | one 10 ms sample | ~37 mA |
| Boot + alive-first init | ~8 s | ~3.5 mA |
| **Deep-sleep floor** | 11.4 s → end | **156.7 µA mean / median** (min 148.5, max 166.1, σ = 2.4) |
| RTC wake blips | at 41 s and 71 s | ~1.1 mA, few ms — **P0.21 wake confirmed**, 30 s cadence exact |

**ISL deep-sleep baseline = 157 µA @ 3.6 V.**

## Where the 157 µA goes — ✅ SOLVED (fixed in production v7)
| Contributor | Approx @ 3.6 V |
|---|---|
| Battery divider **R9/R10 = 1 MΩ/1 MΩ + C17** (3.6 V / 2 MΩ) | **~1.8 µA** |
| nRF System-ON + RV-3028 + SX1262 sleep + AS3933 + RT9080 LDO Iq | ~30 µA (this is the real floor) |
| **AIN7 input-buffer crowbar** — `pinMode(P0.31,INPUT)` at the 1 MΩ midpoint (~VDD/2) | **~118 µA ← the overage** |

> **RESOLVED (2026-07-19):** this baseline was run with the battery-sense pin parked
> as `INPUT`. P0.31 (AIN7) floats at the 1 MΩ divider midpoint (~VDD/2), and a
> *connected* digital input buffer there conducts ~118 µA of shoot-through ("crowbar")
> current — the entire overage. A step-by-step teardown proved it
> (method summarized in `../../docs/ROADMAP.md` §6; minimal sketch =
> 34 µA, park-breakdown = batt-sense pin alone moved 34↔152 µA). **Production v7**
> leaves that buffer disconnected → **34 µA on battery, verified**. The divider and
> the RT9080 LDO are both fine. See `../../docs/ISL_DeepSleep_Notes.md`.

## Conventions established here
- **Always measure/quote the sleep floor at 3.6 V** (battery nominal; nRF buck
  makes the floor voltage-dependent, I ∝ 1/Vin).
- Native USB (`Serial`) powers down in deep sleep → the COM port disappearing at
  `[SLEEP]` *is* the "entered deep sleep" signal; measure on battery.
- AS3933 left in its default listening state and the divider live — this is the
  **board-as-built** floor.

## Files
- `ISL_DeepSleep_Baseline.ino` — the test (alive-first).
- `PowerProfile_baseline.csv` — the capture analyzed above.
