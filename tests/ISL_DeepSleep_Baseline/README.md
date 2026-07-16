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

## Where the 157 µA goes — ⚠️ UNRESOLVED (see correction)
| Contributor | Approx @ 3.6 V |
|---|---|
| Battery divider **R9/R10 = 1 MΩ/1 MΩ + C17** (3.6 V / 2 MΩ) | **~1.8 µA** |
| RT9080-33 LDO quiescent + nRF System-ON + RV-3028 + SX1262 sleep + AS3933 listening | expected single-digit µA |
| **Unaccounted-for** | **~130–150 µA — cause unknown** |

> **CORRECTION (2026-07-16):** this test was run believing the divider was 10k/10k
> (~157 µA, "dominates"). **That diagram was wrong** — the real board is 1 MΩ/1 MΩ +
> C17 (~1.8 µA). The **157 µA measurement is valid** (real board), but the divider
> is *not* the cause, so **most of the floor is unexplained** and is genuine
> deep-sleep headroom. Investigate with a headless battery-only rail teardown —
> see `../../docs/ISL_DeepSleep_Notes.md` (correction note) and `ROADMAP.md` §6.

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
