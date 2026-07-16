# v6 Test 3 — 2 h long-wake / 1/60 Hz RTC tick (deployment cadence)

Validates the **deployment 2 h wake path**, which had never run on hardware —
every prior run used ≤10 min = the 1 Hz tick. This exercises the RV-3028
**1/60 Hz countdown tick**, the minute-granularity preset, and the multi-hour
dynamic backstop. Isolated from GPS with `SIMULATE_FIX=1`. **Two full cycles**
captured (the full-night run is postponed) — enough to confirm the mechanism.

## As-run configuration
```
SIMULATE_FIX        = 1      // fabricated fix; isolates the RTC long-timer path from GPS
GNSS_PERIOD_MIN     = 120    // real 2 h cadence -> forces the 1/60 Hz tick (>4095 s)
NOFIX_BACKOFF_AFTER = 999    // backoff out of the way
TX_PULSE_GAP_SEC    = 30
```
Indoors, USB-attached, receiver ON.

## Files
| File | What it is |
|---|---|
| `Node_test3.log` | ISL node serial — the two 2 h sleep/wake cycles |
| `Receiver_test3.log`| Receiver serial — cadence cross-check + ACKs |
| `PowerProfile_test3.csv` | Current trace, 10 samples/s, ~4.2 h |

## What validated ✅
Both 2 h sleeps behaved exactly right:
```
[SLEEP] 7200 s
[RTC] timer: preset=120 min (1/60 Hz tick)     <- correct tick auto-selected (>4095 s)
[WAKE] RTC P0.21 after ~7201602 ms             <- woke on the RTC INT, NOT the backstop
```
- **Tick selection correct** — `preset=120 min (1/60 Hz tick)`, minutes not seconds.
- **Woke on `RTC P0.21`, not `other/backstop`.** Backstop was 7200 + 300 = 7500 s;
  it fired at **7201.6 s** via the real RTC interrupt. A broken 1/60 Hz path would
  have slipped to ~7500 s on the backstop — it did not.
- **Accuracy excellent:** 7201.6 s vs 7200 s target = **+1.6 s over 2 h (0.02 %)**,
  well inside the ±60 s the minute-granularity tick permits.
- **RTC holds time across the 2 h sleep** — timestamps advance ~7200 s each cycle
  (…156375 → …163606 → …170807), real UTC.
- **Power trace corroborates** — flat at the ~1.78 mA USB floor for the full 4.2 h
  with two brief TX blips; **no spurious wakes**. (p50/p90/p99 all ≈ 1.78 mA.)
- **Bonus — #5 at 2 h cadence:** the first captured pass shows the receiver getting
  seq **118 then 117** (newer before older) — the newest-first backlog drain firing
  again. TX/ACK clean, `delivered` 87→89, `pending=0`.

## Notes
- Only **2 cycles**; the full-night (multi-wake) run is postponed but the tick
  path, wake source, and accuracy are already conclusively demonstrated.
- `undelivered=23` is static — archived fixes from earlier receiver-off runs (the
  known "archive is never re-transmitted" bound); unchanged here (receiver on).
- USB-attached, so the ~1.78 mA floor is the VBUS artifact, not the ~155 µA battery
  floor; TX blips (max ~44 mA) are under-sampled at 10 sps (true TX ~90 mA).

## Bottom line
The 2 h deployment cadence works: correct 1/60 Hz tick, RTC-INT wake on P0.21 at
+0.02 % accuracy, clock held across the sleep, no early backstop wake. The single
biggest deployment-blocking unknown is now cleared (pending a longer multi-wake
confirmation run).
