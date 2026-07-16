# ISL_Battery_ADC — test #2 ✅ CALIBRATED

Reads battery voltage from the AIN7 (P0.31) tap of the **1 MΩ/1 MΩ + C17** divider
(ratio 2.0). *(Earlier docs said 10k/10k — a wrong diagram; the ratio and this
calibration are unchanged either way. See `../../docs/ISL_DeepSleep_Notes.md`.)*
This took several iterations because the RUI3 ADC reference on this core is not
what the usual datasheet-style assumptions suggest.

## Final reader (production-ready)
```c
// one-time at boot: SAADC offset calibration (conditions the internal ref)
// then, per read:
analogReadResolution(12);
analogReference(AR_INTERNAL);
raw = median-of-31 analogRead(P0_31);
Vbat_mV = raw * 1795 / 1000;      // through-origin, two-sweep best fit
```
**Accuracy: ≤25 mV (<0.8%) across 3.2–3.7 V, no cliff.** This exact function is
what `production/v2` uses as `readVbat_mV()`.

## What was wrong, and how it was fixed
| Ver | Symptom | Cause | Fix |
|---|---|---|---|
| orig | 3.6 V read as ~2.53 V; readings went blind (a "cliff") at ~3.3 V | Assumed `AR_INTERNAL` = **2.4 V FS** and used `raw*4800/4095`. On THIS core `AR_INTERNAL` ≈ **3.67 V FS** (0.6 V ref, gain 1/6). Also the reference wasn't conditioned, so it read ~ratiometric to VDD and collapsed at the RT9080 LDO dropout. | correct model + condition the ref (below) |
| v2b | direct-register SAADC read = **flat/garbage** | the comparison `analogRead()` in the same loop clobbers the shared `NRF_SAADC->CH[0]`; the direct read inherited its config | (re-apply config each read) — but see v3 |
| v3/v4 | ✅ linear, accurate | direct SAADC returns **0** on this core (core's driver owns the SAADC → **abandoned**). `analogRead(AR_INTERNAL)` + a **one-time SAADC offset calibration** reads the internal ref cleanly. | single through-origin constant |

**Key facts about this board's ADC path:**
- `analogReference(AR_INTERNAL)` on this RUI3 core = **~3.67 V full scale** (0.6 V
  band-gap × gain 1/6), **not** 2.4 V. Do not assume a 2.4 V full scale here.
- **Direct nRF52840 SAADC register access returns 0** — the RUI3 core owns the
  SAADC; use `analogRead`, not raw registers.
- A **one-time `TASKS_CALIBRATEOFFSET`** at boot is kept as the "known-good
  enabler" that makes `analogRead` read the internal reference linearly.
- A **through-origin** fit (single constant, no offset) is used because a
  divider + ratiometric ADC is physically origin-crossing; it also extrapolates
  safely to the ends of the battery range.

## Calibration data
`calibration_data.csv` — two 6-point sweeps (3.2–3.7 V). Fit → **`Vbat_mV =
raw × 1795 / 1000`**, max error 25 mV, RMS 13 mV.

| Supply | raw (sweep 1 / 2) | Vbat @1795 | worst err |
|---|---|---|---|
| 3.7 V | 2062 / 2052 | 3.70 / 3.68 | −17 mV |
| 3.6 V | 2012 / 2003 | 3.61 / 3.60 | +12 mV |
| 3.5 V | 1953 / 1944 | 3.51 / 3.49 | −11 mV |
| 3.4 V | 1894 / 1882 | 3.40 / 3.38 | −22 mV |
| 3.3 V | 1837 / 1847 | 3.30 / 3.31 | +15 mV |
| 3.2 V | 1788 / 1797 | 3.21 / 3.22 | +25 mV |

**Repeatability floor:** raw drifts **~±10 counts (~±18 mV) per boot** — the nRF
internal-reference repeatability, inherent and not removable by a constant.
Negligible for battery state-of-charge (SoC curves need ~50 mV).

## Re-trimming (if a future board needs it)
The reader prints `raw` too. To retrim: set the supply to a known voltage, read
`raw`, and set `VBAT_CAL_NUM = round(1000 × V_known_mV / raw)`. One point is
enough (through-origin).

## Status
✅ **CALIBRATED** — folded into `production/v2` `readVbat_mV()`.
