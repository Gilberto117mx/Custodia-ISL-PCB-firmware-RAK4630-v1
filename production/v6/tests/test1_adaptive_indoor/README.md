# v6 Test 1 — adaptive GPS + fix re-sync, indoors near a window (overnight)

First v6 field run. **70 cycles over ~13.8 h**, node + receiver on the same
PC over USB-C, electronics kept still all night next to a window. Strong pass:
no crashes, no wedges, correct real-time clock, robust sleep/wake and TX/ACK.

## As-run configuration
```
GNSS_PERIOD_MIN     = 10     // sleep between cycles (=> ~12-min ACTUAL cadence, see below)
SV_MIN              = 4
NO_SKY_ABORT_SEC    = 25
FIX_MAX_SEC         = 120
NOFIX_BACKOFF_AFTER = 999    // backoff DISABLED for this run
BACKOFF_PERIOD_HOURS= 6
TX_PULSE_GAP_SEC    = 30
SIMULATE_FIX        = 0
BACKOFF_BENCH_MIN   = 0
```

## Files
| File | What it is |
|---|---|
| `Node_log.txt` | ISL node serial (boot, per-cycle GPS/TX/sleep, RTC re-syncs) |
| `Receiver_log.txt`| Receiver serial (70 packets, RSSI/SNR, ACKs) |
| `PowerProfile_v6.csv` | Current trace, **10 samples/s** (see the caveat below) |

## What validated ✅
- **Strategy C (RTC from GNSS) — the 11-day bug is fixed.** Cold boot
  (`2000-01-01`, expected — VBACKUP has no battery), then `bootTimeSync` seeded
  **time-before-fix**: `[RTC] SET FROM GPS: 2026-07-14 17:37:20`. Every real fix
  then re-synced (cycle 33 → `2026-07-15 00:03:35`, cycles 58-66 → `05:03…06:31`).
  These are the **real UTC date**; timestamps are monotonic and genuine. The
  RV-3028 free-ran accurately for ~6.4 h between the boot seed and the first fix.
- **Strategy A "extend" branch.** SV was always **8-13 (≥ SV_MIN)**, so it
  correctly extended to `FIX_MAX` (`(sky, no fix)`) rather than aborting — and
  **12 of 70 cycles resolved a real fix indoors** (TTFF 16-115 s, `used=4-5`,
  cluster ≈ 22.556 N, 113.922 E).
- **Deep sleep / wake.** 70/70 `[WAKE] RTC P0.21 after ~600102 ms` — P0.21 RTC
  wake is dead-on (0.1 s overhead on a 600 s timer).
- **TX/ACK + ACK-loss handling.** 69/70 delivered+ACK'd (RSSI −18…−23 dBm). Cycle
  **68** lost its return ACK (`RXP2P RECEIVE TIMEOUT → not-ACK`); because it was a
  **no-fix heartbeat it was correctly dropped, not retained** (`pending=0`,
  `delivered` held). The receiver log shows it *did* receive seq 68 — only the ACK
  was lost — so nothing of value was lost. Best-effort policy behaving as designed.

## What this run did NOT exercise ⚠️
- **No-sky abort (25 s):** never triggered — SV was always ≥ 4 near the window.
  Needs a genuine no-sky spot (SV < 4) to validate the early-abort/energy save.
- **Delivery guarantee (#5):** the only ACK-loss was a *no-fix* packet (correctly
  dropped). A **real fix** never missed its ACK, so the retain / resend-newest-
  first path was never entered. Covered by the next test (`SIMULATE_FIX`).

## Notes for deployment
- **Cadence = sleep + GPS window.** `PERIOD=10 min` gave **~12-min** cycles
  (600 s sleep + up to 120 s GPS + TX). `GNSS_PERIOD_MIN` is the *sleep*, not the
  total cycle. Negligible at a 2 h cadence, but worth remembering.
- **Marginal sky is the energy worst case, and backoff (B) was OFF here (K=999).**
  25+ consecutive "sats visible but no fix" cycles each burned the full 120 s GPS
  window. That pattern is exactly what strategy **B** is meant to curb — enable it
  for the sealed collar.

## ⚠ Power-profile caveat (important)
`PowerProfile_v6.csv` was logged at **10 samples/second (100 ms)**. That rate is
fine for the sleep floor and the ~120 s GPS plateau, but it **under-samples the
LoRa TX burst** (a few ms), so the CSV's TX peaks (~40-46 mA) are **not the true
TX current** — the real LoRa TX draw is **~90 mA**. Treat the TX figure in this
trace as an artifact of the sample rate, not a measurement. Also note the whole
run was **USB-attached**, so the ~1.78 mA "floor" is the VBUS artifact, not the
real ~155 µA battery floor — this run cannot be used for battery-life estimates.
Observed (10 sps): sleep ≈ 1.78 mA (USB), GPS-on ≈ 40 mA, TX (undersampled) ≈ 40-46 mA.

## Bottom line
Clock discipline, adaptive extend, sleep/wake, and TX/ACK all correct and robust
over a long run. Remaining v6 checks: the **no-sky abort** (needs SV < 4) and the
**#5 delivery guarantee** (next test).
