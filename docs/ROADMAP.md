# ISL Board — Status & Roadmap (single source of truth)

_Last updated: 2026-08-11._ What is validated, what is open, and the exact next
tests. Deployment target: a **sealed animal collar**, GNSS fix ~every **2 h**, that
must survive ~24 h of blind transport and long low-sky stretches, then run for
months on a LiSOCl₂ primary cell. Once sealed it cannot be reprogrammed.

> **Pilot handover snapshot in the repo root: [`../HANDOVER.md`](../HANDOVER.md)** —
> end-to-end delivery (collar + SolarNodeSystem repeater/gateway + cloud) for the first
> medium-scale pilot.

> ## 🏁 Final status — **v17 FIELD-VALIDATED (pilot build)**
> **`production/v17` is the definitive, field-validated firmware version** (v16's feature
> set ported to **PCB iteration3**). **Field-validated end-to-end (real 2-day out-of-range
> trip, 2026-08-11):** the collar buffered fixes for ~2 days, then on first gateway contact
> delivered an **83-packet burst newest-first (seq 194→94), `emit=83 bad=0`**, ~15 s/packet
> — the durable backlog proven on real hardware (`../production/v17/logs/REALWORLD_BACKLOG.md`).
> Earlier bench (2026-08-03): hot GPS fixes (TTFF 6–7 s, 21–22 sats, `CELL=OK`), accel over
> SPI (`bad=0`), backlog with real fixes (`delivered=10`, zero loss); RTC/sleep/battery/flash OK.
>
> **Measured power (PPK 2026-08-03):** deep-sleep floor **~190 µA** on iteration3 →
> modelled **~3.3 yr** on a 9600 mAh LiSOCl₂ cell at the deployment cadence. Two
> **optimization-only** items (neither a blocker — see §6): the floor is a fixable P1
> input-buffer crowbar (→ ~35–45 µA ⇒ ~7 yr), and `TTFF`/`CELL` on backlogged packets
> read transmit-time state (positions are always correct).
>
> ⚠️ **Two items are NOT integrated (ISL lab engineers — see §7):** the **AS3933 WUR has
> never been integrated in any version** (the drone pass is faked with a timer,
> `SIMULATE_WUR_HOURS`; `ENABLE_WUR_WAKE=0`), and the **BLE offload protocol** is pending an
> update **already done by Omar Khalfa**, awaiting integration by the ISL team.

---

## 1. Subsystem bring-up (tests #0–#7) — DONE
All in `../tests/` (full matrix in `../tests/README.md`).

| Subsystem | Result |
|---|---|
| MCU + I2C + debug | ✅ USB-C native `Serial`; RV-3028 @ 0x52 |
| RV-3028 RTC | ✅ keeps time; **wake on P0.21** |
| Battery ADC (AIN7/P0.31) | ✅ calibrated `raw×1795/1000`, ≤25 mV over 3.2–3.7 V |
| L76K GNSS | ✅ **Serial0/UART1**, EN=**P1.02** active-low; open-sky 12-sat 3D fix |
| Deep-sleep floor | ✅ **34 µA on battery (v7)** — the old 157 µA was an AIN7 input-buffer crowbar (our `pinMode`), not the divider/LDO. Fixed & verified — see §6. |
| GPS duty-cycle teardown | ✅ cut EN → quiet → `end()` → drive P0.19/P0.20 LOW (adds ~0 µA) |
| RTC ← GNSS UTC time | ✅ seed in seconds with sats=0 (time-before-fix) |
| AS3933 wake-up radio | ⚠️ SPI + config + RC-cal PASS; **real LF wake pending** (2nd engineer) |

## 2. Production firmware — v1 → v17
Each version adds one capability; `../production/vN/README.md` has the detail.

| Ver | Adds | State |
|---|---|---|
| v1 | full node loop (GNSS + LoRa TX/ACK + persistence + deep sleep) | validated happy-path |
| v2 | calibrated battery reader | validated |
| v3 | GPS→RTC time seed + long wakes (1/60 Hz tick) | superseded |
| v4/v5 | field runs; SV-in-view diagnostic in packet | field-tested |
| v6 | GNSS field strategy (A/B/C) + delivery guarantee | validated (§3) |
| v7 | v6 + deep-sleep floor fix (AIN7 crowbar, ~155→34 µA) | ✅ VERIFIED (34 µA on battery) |
| v8 | v7 + accelerometer (5 s/fix) over BLE after the LoRa pass | ✅ bench-verified (accel→LoRa→BLE, 5 clean cycles; reboot-to-sleep fix) |
| v9 | v8 + drone-pass bulk offload: 10 s accel → flash ring, custom-open BLE blast | superseded by v10 |
| v10 | v9 + collar BLE offload reverts to v8's `api.ble.uart` fire-and-forget (name-advertised NUS), one-directional, ID-tagged | ✅ no-freeze proven (6+ passes) but data didn't validate — see `../production/v10/logs/ANALYSIS.md`. Kept as reference. |
| **v11** | **v10 + BLE offload data-integrity fix: every line ≤ 20 B (one NUS notification, 16-bit checksum), whole ring blasted x2 with seq-dedup; receiver hardened + accepts Just-Works. Root-causes v10's `n=4294952064` garbage.** | **✅ VERIFIED — LAST KNOWN-GOOD. Bench: 3 drone passes, 20 unique records, 0 bad/0 corrupt; collar never froze, LoRa 100%. Ring kept (drone dedups). See `../production/v11/logs/`.** |
| **v12** | **v11 but each pass transmits ONLY NEW accel data (clear-after-send): a drone pass carries just the records collected since the previous pass, never a re-send. On-collar buffering/replay intentionally dropped — the other engineer owns that with a different BLE approach.** | ✅ VERIFIED (bench): ~13 passes, new=35, 0 bad/0 corrupt, no cross-pass dup; passes as short as ~9 s; collar never froze, LoRa 100%. See `../production/v12/logs/`. |
| **v17** | **v16 PORTED to ISL PCB iteration3 (the "onboard accelerometer" board). Feature set byte-for-byte v16; ONLY the hardware interface changed in two places: (1) the accelerometer moved from an external Grove LIS3DHTR on I²C (P0.24/25) to the ONBOARD LIS3DHTR (U5) on a 4-wire SPI bus shared with the WuR (CLK P0.03, MOSI P0.30, MISO P0.29, accel CS P0.28) — driver rewritten I²C→bit-bang SPI (mode 3), same regs/records/ring, and the Grove module's ~296 µA parasitic is gone; (2) GPS power moved from an active-LOW P-FET to an active-HIGH TPS22918 load switch (`L76K_EN`=P1.02, HIGH=ON), guarded by `GPS_EN_ACTIVE_HIGH`. RTC/GPS-UART/battery/LoRa/BLE/sleep identical; schema unchanged (4). Board added as `hardware/iteration3`; pin map updated.** | **current — 🆕 built, awaiting first bench run on the new board. Verify: `[CFG-BOARD]` banner, GPS powers on (active-HIGH), accel reads over SPI (WHO_AM_I 0x33), and a clean battery-only sleep-floor PPK (expected near v7's 34 µA now the Grove parasitic is gone). See `../production/v17/README.md`.** |
| **v16** | **v15 + DURABLE LONG-EXCURSION LoRa BACKLOG. Animals roam out of repeater range for days; GPS keeps fixing, and the un-ACK'd fixes are held in the flash-persisted `pending[]` backlog and replayed newest-first when the link returns. v15 kept only 5 slots (~10 h @ 2 h) and dropped the rest, so a multi-day excursion lost most of its track. v16 raises `PENDING_SLOTS` 5→84 (= 1 week @ 2 h, ~2 KB flash placed above the accel ring; `static_assert`s prove the two can't collide and both fit the ~132 KB budget), and drains a big backlog on a shorter `BACKLOG_GAP_SEC`=15 s so a week replays in minutes (deep-sleep gaps → wall-clock cost, not battery). Newest-first + stop-at-miss + persistence make an interrupted catch-up resume next pass. Transport/BLE/GPS/sleep byte-identical to v15; schema 3→4.** | **✅ BENCH-VALIDATED (prior stable, PCB iteration2; 2026-08-03, repeater off→on): `pending` climbed 1→42 (past v15's cap of 5), survived every drone-pass reboot, and on reconnect drained newest-first 42→1 → `delivered=42 pending=0 undelivered=0`, ALL 42 fixes, zero loss/zero out-of-order; accel ring shared the flash with 0 bad. Flash-budget math compile-checked (accel ends 0x9E00, backlog 0xA000–0xA7E0, 512 B guard). Power capture was USB-attached (floor ~1.67 mA = artifact) → one clean battery-only run still to do. See `../production/v16/logs/BACKLOG_TEST_ANALYSIS.md`.** |
| **v15** | **v14 + GPS backup-cell (MS621FE) HEALTH + CHARGE-ON-COLD. Measured: a charged cell hot-starts (~4 s) even after 1 h off; a flat cell cold-starts (~35 s) or fails. v15 infers health from TTFF: HOT (fast) → power off; LOW (slow) → keep GPS on to `GPS_CHARGE_SEC`=180 s to recharge (the patient window IS the charge); DEAD (non-hot ×4 despite charging) → flag + stop charging. Every LoRa packet carries `TTFF=<s>,CELL=<OK\|LOW\|DEAD>` (repeater relays raw; reference LoRa receiver parses it). Self-regulating; worst case ~9–12 mo on a 9 Ah 26500.** | **✅ FIELD-VALIDATED (prior stable; outdoor 2026-08-02, `SIM_FIX=0`): a drained cell was detected (110 s cold fix → `CELL=LOW`) and self-charged into hot starts (`TTFF` 110→35→14→9 s, `CELL=OK`) within two 180 s holds — the whole thesis on real hardware. Zero regression: BLE 7 rec/0 bad/no freeze, LoRa delivery-guarantee backfilled a 33 min repeater outage (lost only 2 oldest to the 5-slot cap), two-timer accel + real-GPS RTC + flash persistence intact. See `../production/v15/logs/OUTDOOR_ANALYSIS.md`.** |
| **v14** | **v13 + COLD / FIRST-FIX GPS strategy: a fix is COLD when the clock was lost (`!rtcSynced`), we've never fixed (`lastFixUnix==0`), it's been a long time since the last fix (stale / NEW CITY), or we just left a no-sky streak — and a COLD fix gets a patient `COLD_FIX_MAX_SEC` (180 s) window with NO early no-sky abort, while normal wakes keep a frugal budget. `lastFixUnix` persisted in flash so the new-city case is caught whether the battery stayed on (stale gap) or was lost (`!rtcSynced`). Fixes the outdoor cold-start silence.** | **current — ✅ cold-fix VALIDATED (overnight: real SV=9/11 fixes, first fix cold-locked, no hang). Tuned from that run: WARM `NO_SKY_ABORT_SEC` 25→45 s (was killing warm reacquisitions), `COLD_AFTER_NOFIX` 2→1. See `../production/v14/logs/`.** |
| v13 | v12 + (1) TWO INDEPENDENT TIMERS — accel collection on its own `ACCEL_PERIOD_HOURS` cadence, decoupled from the GNSS/LoRa cadence and the drone pass; each pass still sends the WHOLE ring accumulated since the last pass; bounded ring drops oldest when full (default 64, sized for a 2-wk/6-h deployment). (2) the per-record TIMESTAMP restored to the wire as its own `T <ts>` line. Transport byte-identical to the proven v12.** | **✅ VERIFIED (bench 2026-07-29): accel on its own 324 s timer (not every GNSS cycle), each pass sends everything since last + clears, per-record ts cross-checks, bad=0, no freeze; LoRa delivery-guarantee handled a repeater outage. See `../production/v13/logs/`.** |

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
> (`SIMULATE_FIX`, `BACKOFF_BENCH_MIN`) let several run indoors — see the v17/v16 READMEs.

| Pri | Test | Goal | How | Receiver |
|---|---|---|---|---|
| ✅ | ~~**v17 iteration3 validation**~~ **DONE (2026-08-03):** both hardware ports confirmed (GPS active-HIGH TPS22918, accel over SPI), hot GPS fixes, backlog drained 10/10 zero-loss, RTC/sleep/battery/flash good. PPK floor ~190 µA (opt #1). `../production/v17/logs/`. | — | — |
| **1** | **Final battery-only PPK + P1-floor fix (opt #1)** | Book the true floor with USB detached and confirm the ~190 µA → ~35–45 µA P1-crowbar fix. | Park P1.03/P1.01/P1.04 input buffers before sleep; battery-only PPK. | — |
| 2 | **1 h-cadence endurance / hot-fix hold** | Confirm the backup cell holds ephemeris across the 1 h gap so fixes stay hot (`CELL=OK`) — the single biggest lifespan lever. | Real 1 h cadence outdoors, watch `TTFF`/`CELL`. | ON |
| 3 | **Strategy B backoff** | K consecutive no-fix → cadence stretches, snaps back on first fix. | `SIMULATE_FIX=0`, `NOFIX_BACKOFF_AFTER=3`, `BACKOFF_BENCH_MIN=3`; needs no-fix cycles. | ON |
| 4 | **Strategy A no-sky abort** | Confirm early abort when `SV<4` (energy save in dens/canopy). | `SIMULATE_FIX=0` in a genuine no-sky interior; watch `(no-sky abort)`. | optional |
| 5 | **Real TX current** | Confirm true LoRa TX ≈ **90 mA** (10 sps under-samples the ms-scale burst). | Higher-rate power capture during a TX. | — |
| 6 | **AS3933 WUR real wake + BLE field integration** | See **§7 — handed to the ISL lab engineers.** | — | — |

### Robustness checklist (still to close before deployment)
- Brownout / blackout mid-cycle (power yanked during GPS or TX) → flash integrity + resume.
- `>68 min` cadence **endurance** (item 2) — single 2 h wake already proven.
- Battery reading under real load on battery (not USB back-feed).

---

## 5. Open design questions
- ~~**Retention depth vs. archive replay.**~~ **RESOLVED in v16:** `PENDING_SLOTS`
  raised **5 → 84** (= 1 week @ 2 h), so a multi-day out-of-range excursion now replays
  every fix newest-first on return (bench-validated: 42 fixes, zero loss). The
  `undelivered` archive remains the fire-and-forget overflow only beyond a week.
- **AS3933 WUR enable + BLE field integration** — gated OFF (`ENABLE_WUR_WAKE 0`)
  until validated on hardware; owned by the ISL lab engineers (see §7).

## 6. Deep-sleep floor — ✅ SOLVED (was "unexplained ~150 µA")
**Root cause found & fixed in production v7.** A step-by-step deep-sleep teardown
proved the ~120 µA overage was a single line: **`pinMode(P0.31, INPUT)`** on the
battery-sense pin. P0.31 (AIN7) floats at the **1 MΩ divider midpoint ≈ VDD/2**; a
connected digital input buffer there conducts ~**118 µA** of shoot-through
("crowbar") current.
- **Evidence (modules attached):** minimal sketch = **32–37 µA**;
  additive sweep pinned the jump to the GPIO-parking step (34 → 152 µA); the
  park-breakdown pinned it to **one pin** — batt-sense OFF = 34 µA, all other
  parked pins = 152 µA. Wire/RTC/trickle-charge/timer/wake-pin all cost ~0.
- **Fix (v7):** keep P0.31's input buffer **DISCONNECTED** except during the SAADC
  sample (`battPinDisconnect()` → `PIN_CNF[31]=2`). The ADC reads via its analog
  mux, so battery reading is unaffected. **Floor: ~155 → 34 µA (~4.5× idle life).**
- **✅ VERIFIED** on battery: 34 µA floor, sane battery read, TX/ACK intact
  (`../production/v7/logs/PowerProfile_v7_battery.csv`).
- The old "raise the divider" and "maybe it's the LDO/modules" theories are both
  disproven — it was our own `pinMode`, not hardware. The RT9080 LDO is fine.
- Any *further* squeeze (32 µA → single digits) would be a low-Iq-LDO schematic-v3
  item, worth far less than this firmware win. Future schematic revs: follow the
  one-authoritative-schematic rule in `../hardware/README.md`.

### iteration3 (v17) power profile — measured 2026-08-03 (`../production/v17/logs/ppk_v17_iter3_summary.txt`)
- **Floor ~190 µA** on iteration3 (34.3 min capture; 119 s continuous sleep run).
  Per-op: hot GPS fix ~8 s @ ~36 mA, LoRa TX 105 mA/50 ms, accel 10 s @ ~3.6 mA,
  BLE offload ~120 s/pass. **Lifespan model** (9600 mAh, GNSS/1 h, accel/3 h, BLE/2 wk,
  hot fixes): **~3.3 yr as-is; ~7 yr if the floor is parked.** Sensitivity: if fixes go
  cold (180 s charge), 10 % cold ⇒ ~2.0 yr, 25 % ⇒ ~1.3 yr — so keeping fixes hot (v15
  charge-on-cold) is the biggest lever.
- **Optimization #1 — floor ~190 µA → ~35–45 µA.** Same crowbar class as v7, on the
  iteration3 **P1** pins: `accelPinsPark()` disconnects only the P0 SPI pins, leaving
  **P1.03 (accel INT1), P1.01 (accel INT2), P1.04 (WuR wake)** as connected `INPUT`
  buffers that crowbar when floating. Fix: set `NRF_P1->PIN_CNF[1]/[3]/[4] = 2` before
  sleep (guard P1.04 so it stays armed when `ENABLE_WUR_WAKE=1`). Low-risk, v7-consistent.
- **Optimization #2 — `TTFF`/`CELL` on backlogged packets** read transmit-time state
  (`formatPacket()` reads them from globals). Position/`SV`/ts/vbat are per-packet and
  always correct; only the two *health* fields can read a false `LOW` after a backlog.
  Optional per-packet fix stamps them at stage time (flash schema 4→5). Both items are
  detailed in `../production/v17/README.md` → *Pending optimizations*.

---

## 7. Handoff to ISL lab engineers — the two items to integrate (WUR + BLE)
The tracking data path is finished and field-validated through **v17**. The two
**on-demand** items below are **NOT integrated** in the delivered firmware and are owned by
the **ISL lab engineers**. Full context in the root [`../HANDOVER.md`](../HANDOVER.md).

### 7a. AS3933 wake-up receiver (WUR) — **never integrated in any version**
- **Today:** the drone pass that triggers the BLE offload is **faked with a timer**
  (`SIMULATE_WUR_HOURS`, default 0.25 h); the real wake pin is disarmed
  (`ENABLE_WUR_WAKE = 0`). The AS3933 is proven only at the SPI/config/RC-cal level
  (`../tests/ISL_WUR_AS3933/`); it is wired to **P1.04** (wake IRQ), SPI shared with the accel.
- **Remaining:** trigger a **real LF wake** from a transmitter
  (`../reference/AS3933_wakeup/WuTx*` — 433 MHz, 19 kHz OOK/Manchester, pattern `0x9669`),
  confirm the **P1.04 rising-edge IRQ + R13 pattern match**, then set `SIMULATE_WUR_HOURS = 0`
  and `ENABLE_WUR_WAKE = 1` so a real beacon drives the offload — and validate it end-to-end.

### 7b. BLE offload protocol — **to be updated by the ISL team**
- **Today:** the collar advertises as **`Custodia-Tracker`** and streams the accel flash
  ring over the proven fire-and-forget NUS path (bench-verified: no-freeze, 0 bad/0 corrupt).
  Reference emitter/receiver: `../production/v17/ISL_v17_Drone_Receiver`,
  `../tests/ISL_BLE_CustomOpen/`, `../tests/Accelerometer/`.
- **Remaining:** integrate the **updated BLE protocol already developed by Omar Khalfa**
  (pending), replacing the current offload path, then validate the drone-side end to end with
  the WUR-triggered pass from 7a. (During iteration3 field runs the current path occasionally
  dropped a record and a collar reset disconnected other BLE peers — the updated protocol is
  expected to address this.)

Everything needed to continue both is in this repo: pin map (`ISL_Pinout.md`), the bring-up
tests, the reference emitters/receivers, and the firmware hooks in `../production/v17`.

---

### Quick pointers
- Pin map of record: **`ISL_Pinout.md`** · Deep-sleep rules: **`ISL_DeepSleep_Notes.md`**
- GNSS field findings + strategy origin: **`GNSS_FieldStrategy.md`**
- Current/final firmware + knobs: **`../production/v17/README.md`**
