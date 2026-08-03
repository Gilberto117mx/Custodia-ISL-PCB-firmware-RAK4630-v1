# ISL Board — Production firmware v17 (COLLAR, ported to PCB iteration3 / onboard accel)

**v17 = v16, ported to the new ISL PCB (iteration3 — the "onboard accelerometer"
board).** The feature set is byte-for-byte v16 (durable 1-week LoRa backlog, GPS
backup-cell health + charge-on-cold, cold/first-fix budget, two accel/BLE timers, the
proven no-freeze BLE offload, delivery guarantee, reboot-to-sleep). **Only the hardware
interface changed**, in the two places the iteration3 schematic introduced.

> 🆕 **Status: built, awaiting the first bench run on the new board.** No firmware
> feature or flash-layout change (schema still 4). The risk surface is exactly the two
> hardware ports below — verify both on the bench before trusting a long run.

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

## Notes / deferred
- INT1/INT2 are wired (P1.03/P1.01) but unused — collection stays timer-based. They
  enable a future motion-triggered wake without a board change.
- The bench values in the committed source (e.g. `GNSS_PERIOD_MIN`, `SIMULATE_WUR_HOURS`,
  `SIMULATE_FIX`) are deployment defaults; set the quick-bench values (see v16 README)
  for a fast check, and restore deployment values before sealing.
- The two receivers (`ISL_v17_LoRa_Receiver`, `ISL_v17_Drone_Receiver`) are byte-identical
  to v16 apart from the version label — the LoRa/BLE wire formats did not change.
