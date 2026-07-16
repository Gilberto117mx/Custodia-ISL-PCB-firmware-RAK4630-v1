# ISL Board — Status & Roadmap (single source of truth)

_Last updated: 2026-07-16._ What is validated, what is open, and the exact next
tests. Deployment target: a **sealed animal collar**, GNSS fix ~every **2 h**, that
must survive ~24 h of blind transport and long low-sky stretches, then run for
months on a LiSOCl₂ primary cell. Once sealed it cannot be reprogrammed.

---

## 1. Subsystem bring-up (tests #0–#7) — DONE
All in `../tests/` (full matrix in `../tests/README.md`).

| Subsystem | Result |
|---|---|
| MCU + I2C + debug | ✅ USB-C native `Serial`; RV-3028 @ 0x52 |
| RV-3028 RTC | ✅ keeps time; **wake on P0.21** |
| Battery ADC (AIN7/P0.31) | ✅ calibrated `raw×1795/1000`, ≤25 mV over 3.2–3.7 V |
| L76K GNSS | ✅ **Serial0/UART1**, EN=**P1.02** active-low; open-sky 12-sat 3D fix |
| Deep-sleep floor | ✅ measured **157 µA @ 3.6 V** — but its **cause is unexplained** (the real 1 MΩ/1 MΩ+C17 divider is only ~1.8 µA, not the floor). Headroom to improve — see §6. |
| GPS duty-cycle teardown | ✅ cut EN → quiet → `end()` → drive P0.19/P0.20 LOW (adds ~0 µA) |
| RTC ← GNSS UTC time | ✅ seed in seconds with sats=0 (time-before-fix) |
| AS3933 wake-up radio | ⚠️ SPI + config + RC-cal PASS; **real LF wake pending** (2nd engineer) |

## 2. Production firmware — v1 → v6
Each version adds one capability; `../production/vN/README.md` has the detail.

| Ver | Adds | State |
|---|---|---|
| v1 | full node loop (GNSS + LoRa TX/ACK + persistence + deep sleep) | validated happy-path |
| v2 | calibrated battery reader | validated |
| v3 | GPS→RTC time seed + long wakes (1/60 Hz tick) | superseded by v6 |
| v4/v5 | field runs; SV-in-view diagnostic in packet | field-tested |
| **v6** | **GNSS field strategy (A/B/C) + delivery guarantee** | **current — see §3** |

## 3. v6 validation — where we are now

**v6 behaviours** (full spec in `../production/v6/README.md`):
- **A** — SV-gated adaptive GPS timeout (abort on no-sky, extend when sats visible)
- **B** — no-sky backoff after K consecutive no-fix cycles
- **C** — re-sync RTC from **every real fix** → every timestamp is GNSS-derived
- **#5** — delivery guarantee: un-ACK'd real fixes held & re-sent newest-first, ≥30 s apart

| # | What | Test | Result |
|---|---|---|---|
| ✅ | Strategy **C** (real-UTC re-sync; 11-day bug gone) | `../production/v6/tests/test1_adaptive_indoor` | **PASS** |
| ✅ | Strategy **A extend** branch (SV≥4 → wait to FIX_MAX; fixes indoors) | test1 | **PASS** |
| ✅ | P0.21 RTC wake @ 1 Hz tick (≤68 min), 70/70 | test1 | **PASS** |
| ✅ | TX/ACK + ACK-loss handling (no-fix dropped, best-effort) | test1 | **PASS** |
| ✅ | **#5 delivery guarantee** (hold, newest-first drain, 30 s gap, overflow→archive, reboot persistence, dead-link conserve) | `../production/v6/tests/test2_delivery_guarantee` | **PASS** |
| ✅ | **2 h long wake / 1/60 Hz tick** (RTC-INT wake at +0.02 %, not backstop) | `../production/v6/tests/test3_longwake_2h` | **PASS** (2 cycles) |

The three big behavioural unknowns (C, A-extend, #5, long-wake) are all **green**.

---

## 4. What's next to test (prioritized)

> These are mostly **endurance + real power numbers**, not logic. Bench hooks
> (`SIMULATE_FIX`, `BACKOFF_BENCH_MIN`) let several run indoors — see the v6 README.

| Pri | Test | Goal | How | Receiver |
|---|---|---|---|---|
| **1** | **Headless battery-floor run + floor teardown** | Real floor + true battery voltage under v6 (every run so far was USB-attached → 1.78 mA VBUS artifact, fake 3.9 V). **Also the entry point for §6**: confirm the ~155 µA and start isolating the **unexplained ~150 µA** (it is NOT the divider). Unblocks the battery-life estimate. | Run on **battery only**, no USB; standalone power meter. `SIMULATE_FIX=1`, `GNSS_PERIOD_MIN=120`; then isolate rails/peripherals per §6. | optional |
| 2 | **Multi-wake 2 h endurance** | Confirm the long-wake path over a **full night (≥4 wakes)** — mechanism already proven in test3. | `SIMULATE_FIX=1`, `GNSS_PERIOD_MIN=120`, overnight. | ON |
| 3 | **Strategy B backoff** | K consecutive no-fix → cadence stretches, snaps back on first fix. | `SIMULATE_FIX=0`, `NOFIX_BACKOFF_AFTER=3`, `BACKOFF_BENCH_MIN=3`; needs no-fix cycles. | ON |
| 4 | **Strategy A no-sky abort (25 s)** | Confirm early abort when `SV<4` (energy save in dens/canopy). | `SIMULATE_FIX=0` in a genuine no-sky interior (`SV<4`); watch `(no-sky abort)` at ~25 s not 120 s. | optional |
| 5 | **Real TX current** | Confirm true LoRa TX ≈ **90 mA** (10 sps under-samples the ms-scale burst; traces so far showed 40–102 mA artifacts). | Higher-rate power capture during a TX. | — |
| 6 | **AS3933 WUR real wake** | A real LF wake event → P1.04 IRQ + pattern match; then set `ENABLE_WUR_WAKE 1`. | `reference/AS3933_wakeup/WuTx*` transmitter. | — (2nd engineer) |

### Robustness checklist (still to close before deployment)
- Brownout / blackout mid-cycle (power yanked during GPS or TX) → flash integrity + resume.
- `>68 min` cadence **endurance** (item 2) — single 2 h wake already proven.
- Battery reading under real load on battery (not USB back-feed).

---

## 5. Open design questions (no code change yet — decide, then implement)
- **Retention depth vs. archive replay.** `#5` keeps only the newest
  `PENDING_SLOTS` (=5) fixes; older ones during a long receiver outage roll into
  the `undelivered` flash archive, which is currently **fire-and-forget (never
  re-transmitted)**. At 2 h cadence that's a ~10 h outage window before the oldest
  start dropping from the over-air guarantee. Options: raise `PENDING_SLOTS`, or add
  an "undelivered replay" drain after `pending`. (Seen live in test2: 9 fixes archived.)
- **AS3933 WUR enable.** Gated behind `ENABLE_WUR_WAKE 0` until the real-wake test
  passes.

## 6. Deep-sleep floor — unexplained ~150 µA (NEW, high-value)
**Correction (2026-07-16):** the board's battery divider is **1 MΩ/1 MΩ + C17**
(~1.8 µA), **not** 10k/10k — earlier docs used a wrong diagram. So the measured
**157 µA floor is NOT the divider**, and known parts (LDO Iq + nRF sleep + RTC +
AS3933) should total <~20 µA. **~130–150 µA is unaccounted for → real headroom.**
- **Next step:** a **headless battery-only teardown** — measure the floor, then
  isolate rails/peripherals one at a time (LDO quiescent path, GPS/WUR domain
  leakage, any un-parked pull-up/peripheral) to find the missing current.
- The old "raise the divider to 1–2 MΩ" idea is **obsolete** — already done.
- Battery calibration is unaffected (divider ratio is still 2.0). Details:
  `ISL_DeepSleep_Notes.md` (correction note) and `ISL_Pinout.md`.
- Future schematic revs: follow the one-authoritative-schematic rule in
  `../hardware/README.md` (new rev → `iteration3/`, move the pointer, update
  `ISL_Pinout.md` in the same commit).

---

### Quick pointers
- Pin map of record: **`ISL_Pinout.md`** · Deep-sleep rules: **`ISL_DeepSleep_Notes.md`**
- GNSS field findings + strategy origin: **`GNSS_FieldStrategy.md`**
- Current firmware + knobs: **`../production/v6/README.md`** · Test logs: `../production/v6/tests/`
