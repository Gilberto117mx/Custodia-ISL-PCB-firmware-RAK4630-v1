# ISL Board — Production Firmware **v7** (deep-sleep floor fix)

v7 = v6 **+ one fix**: the deep-sleep floor drops from **~155 µA to ~35 µA**
(with the GNSS + WUR modules attached) — ~**4–5× longer idle battery life** on the
sealed collar. **Nothing else changes** from v6: strategy A/B/C, the delivery
guarantee, the RTC 2 h wake, the bench hooks — all identical.

## The fix (one line, huge effect)
A step-by-step deep-sleep teardown proved the entire ~120 µA of "unexplained"
sleep-floor overage was a single line:
**`pinMode(P0.31, INPUT)`** on the battery-sense pin.

- P0.31 (AIN7) floats at the **1 MΩ/1 MΩ divider midpoint ≈ VDD/2**. A *connected*
  digital input buffer held at mid-rail conducts continuous **shoot-through
  ("crowbar") current ≈ 118 µA**. Classic nRF52 gotcha.
- **Proof (modules attached):** parking that one pin as INPUT moved the floor
  **34 µA ↔ 152 µA**; every other init step (Wire, RV-3028 config, trickle-charge
  write, countdown timer, RTC-wake-pin setup) changed the floor by **~0 µA**.

v7 keeps that input buffer **DISCONNECTED** (`battPinDisconnect()` →
`NRF_P0->PIN_CNF[31] = 2`) everywhere it matters — after ADC init, after each
battery read, and immediately before every `deepSleep()`/`napSleep()`. The **SAADC
reads the analog voltage through its own mux**, so `analogRead()` / the calibrated
battery reading is **unaffected** (Nordic explicitly recommends leaving ADC pins
disconnected for exactly this reason).

This also finally closes the long-standing "155 µA = the divider" myth: the divider
draws only ~1.8 µA; it was the *nRF input buffer the divider midpoint fed* that
burned the current — our own `pinMode`, not the LDO, not the divider, not the modules.

## What did NOT change vs v6
GNSS SV-gated adaptive timeout (A), no-sky backoff (B), RTC re-sync on every fix
(C), the newest-first delivery guarantee (#5), the 1/60 Hz 2 h wake, battery
calibration (`raw×1795/1000`), flash persistence, and the `SIMULATE_FIX` /
`BACKOFF_BENCH_MIN` bench hooks are byte-for-byte the v6 logic.

## Defaults (deployment) & quick-verify knobs
Shipped with **deployment defaults**: `GNSS_PERIOD_MIN = 120` (2 h cadence),
`NOFIX_BACKOFF_AFTER = 3`, `SIMULATE_FIX = 0`.

For a **quick bench verification** (indoors, no sky needed) set:
| Knob | Verify value | Why |
|---|---|---|
| `GNSS_PERIOD_MIN` | `2` | ~2-min cycles so you get frequent sleep windows to measure |
| `SIMULATE_FIX` | `1` | fabricates a fix so it TXes without sky — exercises the whole loop |
| `NOFIX_BACKOFF_AFTER` | `999` | keeps backoff out of the way |

**Pass criteria (measure headless on battery, modules attached, receiver ON):**
- **Sleep floor ≈ 35 µA** between cycles (was ~155 µA) — the fix.
- `[BAT] ~3xxx mV` prints a **sane battery voltage** — proves the ADC still works.
- Packets TX + ACK, `[WAKE] RTC P0.21` each cycle — proves the loop is intact.

## Status — ✅ VERIFIED (2026-07-19)
Bench-verified headless on battery (modules attached, `logs/PowerProfile_v7_battery.csv`):
- **Sleep floor = 34 µA** (was ~155 µA) — **~4.5× lower idle current.** 🎯
- Battery reads a sane voltage (`[BAT] 3891 mV` on USB) — ADC unaffected by the fix.
- Collect → SIM fix → TX → sleep loop intact; receiver got the packets; the first
  un-ACK'd fix (receiver off) was correctly held in `pending` (delivery guarantee).
- The trace also caught a **~90 mA TX burst** (confirms true TX ≈ 90 mA).

This is the **deployment build**; supersedes v6. Remember to restore deployment
knobs (`GNSS_PERIOD_MIN=120`, `SIMULATE_FIX=0`, `NOFIX_BACKOFF_AFTER=3`) before sealing.
