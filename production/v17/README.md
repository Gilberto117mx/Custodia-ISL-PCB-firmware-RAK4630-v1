# ISL Board — Production firmware v17 (COLLAR, ported to PCB iteration3 / onboard accel)

**v17 = v16, ported to the new ISL PCB (iteration3 — the "onboard accelerometer"
board).** The feature set is byte-for-byte v16 (durable 1-week LoRa backlog, GPS
backup-cell health + charge-on-cold, cold/first-fix budget, two accel/BLE timers, the
proven no-freeze BLE offload, delivery guarantee, reboot-to-sleep). **Only the hardware
interface changed**, in the two places the iteration3 schematic introduced.

> ✅ **Status: VALIDATED on iteration3 (2026-08-03).** Open-sky run: **6+ hot GPS fixes,
> TTFF 6–7 s, 21–22 sats, `CELL=OK`**; onboard **accel over SPI** collecting clean
> records (`bad=0`); **LoRa + the v16 durable backlog proven with *real* fixes** — 9
> fixes buffered while the repeater was absent, then drained newest-first on reconnect,
> `delivered=10 pending=0 undelivered=0`, zero loss; RTC/deep-sleep/battery/flash all
> good. (An earlier "no-fix" scare was just no sky at that spot — the same module gave
> 22 sats once outdoors.) No feature or flash-layout change vs v16 (schema still 4).
>
> Two **optimization-only** items surfaced (neither is a blocker — see *Pending
> optimizations* below): the **deep-sleep floor** measured **~190 µA** (a fixable P1
> input-buffer crowbar) and the **TTFF/CELL fields on backlogged packets** read
> transmit-time state. WUR + BLE-reset integration and a final battery-only PPK remain
> the other engineer's / next step.

## What changed vs v16 (hardware ports only)

### 1. Accelerometer: Grove-over-I²C → **onboard LIS3DHTR over SPI**
The external Grove LIS3DHTR on I²C (P0.24/P0.25) is replaced by the **onboard LIS3DHTR
(U5)** on a **4-wire SPI bus shared with the AS3933 WuR** (each device has its own CS):

| Signal | Pin | Note |
|---|---|---|
| SPI CLK | **P0.03** | shared with WuR |
| SPI MOSI | **P0.30** | shared |
| SPI MISO | **P0.29** | shared |
| **Accel CS** | **P0.28** | active-LOW (LIS3DHTR) |
| INT1 / INT2 | P1.03 / P1.01 | available; unused (collection is timer-based) |

The driver was rewritten from bit-bang I²C to **bit-bang SPI (mode 3)** — same silicon
(`WHO_AM_I`=0x33), same registers, same 100-sample records, same flash ring. Because
it's the **bare chip** (not the Grove module), the old **~296 µA parasitic is gone**;
firmware still power-downs it (`CTRL1=0x00`, ~0.5 µA) after each collect.

### 2. GPS power: active-LOW P-FET → **active-HIGH TPS22918 load switch**
`L76K_EN` stays on **P1.02**, but the switch changed from an active-LOW P-FET
(iteration2) to a **TPS22918 load switch whose `ON` pin is active-HIGH** (`VOUT` =
`L76K_SUPPLY`). So **HIGH = GPS ON** now. Guarded by:

```c
#define GPS_EN_ACTIVE_HIGH  1   // iteration3 TPS22918; set 0 if your board still has the P-FET
```

> ⚠️ This one is easy to miss — the board "looks the same." If GPS never fixes
> (`SV=0`, no `SET FROM GPS`), the polarity is the first thing to check.

**Unchanged from iteration2** (so untouched in firmware): RTC (P0.13/14 + INT P0.21),
GPS UART (`Serial0` P0.19/P0.20), battery (P0.31/AIN7), LoRa, BLE, the whole sleep path.

## What to check on the bench (new-board bring-up)

1. Boot prints `[CFG-BOARD] PCB iteration3  accel=ONBOARD LIS3DHTR/SPI(CS P0.28...)  GPS_EN=active-HIGH TPS22918`.
2. **GPS powers on:** `[GPS] VCC ON`, then (outdoors, `SIM_FIX=0`) a real `SET FROM GPS`
   and a fix. If it stays `SV=0` forever → check `GPS_EN_ACTIVE_HIGH`.
3. **Accel reads over SPI:** `[ACCEL] 100 samples in 10000 ms` and `[RING] stored …`
   with sane checksums on the drone receiver (`bad=0`). If `[ACCEL] LIS3DHTR not found
   (WHO_AM_I != 0x33)` → SPI wiring/mode issue.
4. **Sleep floor:** with a **clean battery-only PPK run (no USB)**, the floor should now
   sit near the v7 **34 µA** (no Grove parasitic) — this is the board's payoff and the
   number still owed from the v16 test (which was USB-attached). See v16 logs.
5. Backlog / cell-health / BLE behave exactly as v16 (unchanged) — a quick repeat of the
   v16 repeater-off→on backlog check confirms the port didn't disturb anything.

## Config knobs (v17 additions)

| Knob | Default | Meaning |
|------|---------|---------|
| `GPS_EN_ACTIVE_HIGH` | 1 | iteration3 TPS22918 (HIGH=ON). Set 0 for an iteration2 P-FET board. |
| `ACCEL_SPI_DELAY_US` | 4 | bit-bang SPI half-bit delay for the accel bus. |

(All v16/v15/v14 knobs unchanged: `PENDING_SLOTS=84`, `BACKLOG_GAP_SEC=15`,
`HOT_TTFF_SEC=15`, `GPS_CHARGE_SEC=180`, `COLD_FIX_MAX_SEC=180`, `NO_SKY_ABORT_SEC=45`, …)

## Power & lifespan (measured 2026-08-03, `ppk20260803T143349`)

Deep-sleep floor **~190 µA** (119 s continuous run confirms it). Per-op from the capture:
hot GPS fix ~8 s @ ~36 mA, LoRa TX 105 mA/50 ms, accel 10 s @ ~3.6 mA, BLE offload
~120 s/pass. Modelled at the deployment cadence **(GNSS/1 h, accel/3 h, BLE/2 wk) on a
9600 mAh LiSOCl₂ cell**, assuming hot fixes:

| Case | mAh/day | Life (85% usable) |
|---|---|---|
| **As-is (floor 190 µA)** | ~6.8 | **~3.3 years** |
| If floor fixed (~40 µA) — see opt #1 | ~3.2 | **~7 years** |

Sleep is ~67 % of the as-is budget, so the floor dominates. **Caveat — the single biggest
lever is GPS hot-vs-cold:** the table assumes ~8 s hot fixes. If the L76K backup cell
can't hold ephemeris across the 1 h gap and fixes go cold (180 s charge, `CELL=LOW`),
GPS energy explodes — even **10 % cold ⇒ ~2 yr, 25 % ⇒ ~1.3 yr**. The v15 charge-on-cold
strategy is what keeps fixes hot; confirm hot fixes hold at the real 1 h cadence.

## Pending — OPTIMIZATION ONLY (consult before the seal; none are blockers)

The validated behaviour above is correct as-is. These are improvements to *consider*;
the firmware works and ships without them.

1. **Deep-sleep floor ~190 µA → target ~35–45 µA (≈2× battery life).**
   Root cause (confirmed in code): `accelPinsPark()` disconnects only the **P0** SPI pins;
   the iteration3 **P1** pins — **P1.03 (accel INT1), P1.01 (accel INT2), P1.04 (WuR wake)** —
   are left as **connected `INPUT` buffers** and crowbar when floating (P1.04 floats with
   the WuR unplugged; the two INTs are unused). Same class as the v7 AIN7 crowbar.
   **Fix:** before sleep, set `NRF_P1->PIN_CNF[1]/[3]/[4] = 2` (input-disconnect), matching
   AIN7/SPI — guard P1.04 so it stays armed when `ENABLE_WUR_WAKE=1`. Low-risk, v7-consistent.
   *Impact:* ~3.3 yr → ~7 yr at the deployment cadence.

2. **`TTFF`/`CELL` on *backlogged* LoRa packets show transmit-time state, not fix-time.**
   `formatPacket()` reads `TTFF`/`CELL` from **globals** (`g_lastTTFF_s`/`g_cellStatus`);
   `SV`/lat/lon/ts/vbat are per-packet. A fix buffered during an out-of-range/indoor
   stretch drains stamped with the collar's *current* cell state (observed: seq 9 staged
   `TTFF=7,CELL=OK`, delivered `TTFF=120,CELL=LOW`). **Position data is always correct;**
   only the two *health* fields can read a false `LOW` after a backlog. **Fix:** add
   `ttff`+`cell` to `PacketSlot` (24→28 B), stamp at stage time — a **flash-schema bump
   (4→5)** = one-time re-init. Cosmetic/telemetry only; positions unaffected either way.

3. **GPS hot-start margin at 1 h cadence.** Verify the backup cell holds ephemeris for the
   full hour (fixes stay `CELL=OK`, ~7 s). If it drifts cold, either shorten the GNSS
   period, raise `GPS_CHARGE_SEC`, or add a small periodic top-up wake. Biggest lifespan
   lever (see caveat above).

## Deferred to the other engineer / next step (not ours)
- **WUR (AS3933) real LF wake** + arming P1.04 as the wake source (`ENABLE_WUR_WAKE`).
- **BLE integration robustness:** the offload occasionally dropped a record and a reset
  disconnected other BLE peers — the second engineer owns the BLE-central/WUR side.
- **Final battery-only PPK** (USB detached) to book the true floor + per-op charge, and a
  real 1 h-cadence endurance run to confirm the hot-fix assumption.

## Notes
- INT1/INT2 are wired (P1.03/P1.01) but unused — collection stays timer-based. They
  enable a future motion-triggered wake without a board change (see opt #1 re: parking).
- Committed source uses deployment defaults; set quick-bench values (see v16 README) for
  a fast check, and restore them before sealing.
- The two receivers (`ISL_v17_LoRa_Receiver`, `ISL_v17_Drone_Receiver`) are byte-identical
  to v16 apart from the version label — the LoRa/BLE wire formats did not change.
