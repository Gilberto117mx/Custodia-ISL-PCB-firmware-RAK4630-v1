# ISL Board — Production firmware v8

**v8 = v7 (unchanged tracker) + accelerometer-over-BLE.**

Everything v7 does is preserved byte-for-byte — GNSS strategy A/B/C, calibrated
battery, LoRa P2P + ACK + delivery guarantee, flash persistence, RV-3028 clock,
and the **34 µA deep-sleep floor**. v8 only *adds* two things, both modelled on
the proven `tests/Accelerometer/` emitter:

1. **5 s accelerometer collect on every GNSS fix.** After a position fix settles
   (and before the LoRa pass), the Grove **LIS3DHTR** is read over **bit-bang I2C
   on P0.24/P0.25** for 5 s at 10 Hz (100 Hz / ±2 g / high-res, raw counts).
   No-fix cycles skip the accelerometer entirely.
2. **BLE transmit of that burst, after the LoRa pass.** LoRa still carries **only
   the tracker (position) packet** (unchanged `doTransmitPass`). Then, on a fix
   cycle, BLE comes up (RUI3 `api.ble.uart` = Nordic UART Service), advertises as
   `Custodia-Tracker`, streams the buffer, and is **torn down** so the deep-sleep
   floor is untouched.

### Per-cycle flow

```
wake ─▶ GNSS acquire (strategy A) ─▶ [fix] collect 5 s accel
     ─▶ LoRa TX tracker packet (+ ACK/delivery, v7) ─▶ [fix] BLE TX accel burst
     ─▶ deep sleep (GNSS_PERIOD_MIN, 34 µA floor)
```

BLE payload (identical to the committed emitter):

```
ACC cycle=<n> count=<M>
s01 <x>,<y>,<z>
 …
sMM <x>,<y>,<z>
END
```

### Bench result so far (1st run) + the reboot-to-sleep fix

The **accel → LoRa → BLE path is confirmed working**: a `SIMULATE_FIX` run
collected 49 samples, sent the tracker packet over LoRa, and the receiver
discovered `Custodia-Tracker` by name, subscribed, and printed the full
`ACC/sNN/END` burst. ✅

But the tracker then **stalled at the first deep sleep** and never cycled again.
Cause: RUI3 re-advertises on the BLE disconnect, the receiver reconnects *during*
`api.system.sleep.all()`, and a Just-Works pairing attempt wedges the SoftDevice
so the RTC/backstop wake never fires (`api.ble.stop()` does not hold while a
central is connected). v7 never hit this because it only *stopped* BLE at boot,
never used it.

**Fix (in this version):** a fix cycle (BLE used) does **not** deep-sleep
directly — it persists the intended sleep to flash and `api.system.reboot()`s.
The reset fully clears BLE, and the fresh boot performs the queued sleep in a
clean, v7-identical, BLE-free state (`doQueuedSleepIfAny` → `deepSleep`).
No-fix cycles never touch BLE and keep the exact v7 `deepSleep()` path. Cost:
one extra ~2–3 s re-init per fix cycle (negligible against a 2 h period).

## ⚠ Two board packages — flash the right one to each board

| Sketch | Board | Arduino board package |
|--------|-------|-----------------------|
| `ISL_Production_v8/` | ISL tracker (RAK4630) | **RAKwireless RUI3** |
| `ISL_v8_BLE_Receiver/` | receiver nRF52840 | **Adafruit / WisBlock BSP** (`bluefruit.h`) |

**Why the receiver differs from your committed one:** the RUI3 tracker's
`api.ble.uart` advertises the **name only** — it does *not* put the Nordic UART
Service UUID into the advertising packet. Your committed receiver filters on that
UUID, so it can't discover a RUI3 board. `ISL_v8_BLE_Receiver` is the same
receiver but matches on the **complete local name** `Custodia-Tracker` instead
(then discovers the NUS and subscribes to TXD exactly as before).

## Wiring (added vs v7)

Grove **LIS3DHTR** → ISL board, bit-bang I2C:

| LIS3DHTR | ISL pin |
|----------|---------|
| SDA | P0.24 |
| SCL | P0.25 |
| VCC | 3V3 |
| GND | GND |

Address `0x19`, `WHO_AM_I` (0x0F) = `0x33`.

## Config knobs (top of `ISL_Production_v8.ino`)

| Knob | Default | Meaning |
|------|---------|---------|
| `ENABLE_ACCEL_BLE` | `1` | Master switch for the whole accel+BLE path (0 = pure v7). |
| `ACCEL_COLLECT_MS` | `5000` | Seconds of motion buffered per fix. |
| `ACCEL_PERIOD_MS` | `100` | Sample cadence (10 Hz). |
| `ACCEL_MAX_SAMPLES` | `60` | Buffer cap (5 s @ 10 Hz = 50; 60 = margin). |
| `BLE_SETTLE_MS` | `5000` | Wait for the central to connect + subscribe before streaming. |
| `BLE_LINE_GAP_MS` | `50` | Per-line pacing. |
| `BLE_HOLD_MS` | `500` | Flush the last notification before stopping BLE. |
| `GNSS_PERIOD_MIN` | `120` | Deep-sleep between cycles (set `2` for a fast bench test). |
| `SIMULATE_FIX` | `0` | `1` = fabricate a fix (no sky) so accel+BLE run indoors. |

For a quick indoor bring-up: `SIMULATE_FIX 1` and `GNSS_PERIOD_MIN 2`.

---

## What to test to verify full functionality

Ordered so each step de-risks the next. The **BLE link is the one genuinely new,
unproven piece** — everything else is inherited from validated v7.

### A. Build & flash
1. `ISL_Production_v8` compiles under **RUI3** and boots (heartbeat → init).
2. `ISL_v8_BLE_Receiver` compiles under the **WisBlock BSP** and prints
   `Scanning for 'Custodia-Tracker' by name...`.

### B. Accelerometer (indoors, `SIMULATE_FIX 1`)
3. On each simulated fix: `[ACCEL] N samples gathered`, N ≈ 50 for 5 s @ 10 Hz.
4. If `[ACCEL] LIS3DHTR not found` → re-seat SDA=P0.24 / SCL=P0.25 (WHO_AM_I≠0x33).

### C. BLE link — ✅ CONFIRMED on the 1st run, re-verify after the fix
5. Receiver prints `Found Custodia-Tracker (by name)! Connecting...` →
   `Connected!` → `Discovered.` → `Subscribed to incoming data stream.` ✅
6. Receiver then prints the burst: `ACC cycle=… count=…`, `sNN x,y,z` lines,
   `END`. ✅ (1st run delivered all 49 samples.)
7. If a future run connects but no data arrives → Just-Works pairing (add a
   security callback to the receiver); the 1st run did **not** hit this.
8. If it never discovers the tracker → scan with nRF Connect for
   `Custodia-Tracker` during the BLE phase; raise `BLE_SETTLE_MS` if timing is tight.

### D. Cycling — **the fix to re-verify (was the stall)**
9. On a fix cycle the tracker now ends with `== IDLE (reboot-to-sleep) N s ==`,
   resets, and the next boot prints `== BOOT: performing queued N s deep-sleep
   (BLE-clean) ==` then sleeps and **wakes into the next cycle**. Confirm it now
   runs **cycle after cycle** instead of stalling after the first BLE TX.
10. Serial order on a fix cycle: GNSS fix → `ACCEL collect` → `TX pass`
    (LoRa tracker packet) → `BLE accel TX` → `reboot-to-sleep` → (boot) queued
    `deep-sleep` → next cycle.
11. A **no-fix** cycle skips accel+BLE and takes the plain v7 `deepSleep()` (no
    reboot): `== IDLE deep-sleep N s ==`.

### E. LoRa unchanged
12. The tracker (position) packet is still received exactly as in v7.
    **Note:** v7 waits for an `ACK,<dev>,<seq>` from the receiver. If your LoRa
    receiver is fire-and-forget (no ACK), the packet still transmits, but the
    delivery guarantee treats it as un-ACK'd and retries it on later cycles.
    Decide whether the LoRa receiver should ACK, or whether to relax the ACK
    requirement for deployment.

### F. Power — confirm the floor survived
13. On **battery, headless**, confirm the deep-sleep floor is still **~34 µA**
    during the queued sleep. Because that sleep runs on a fresh boot where BLE was
    never started, the state is identical to v7 — the floor should match v7. If it
    rose, check the reboot-to-sleep actually happened (look for the `BOOT:
    performing queued …` line) rather than a direct sleep with BLE still up.
14. Confirm the reboot doesn't disturb LoRa: after the reset, `loraConfigureOnce`
    should find P2P already set (no second reboot) and the next cycle transmits
    normally.

### G. Endurance
15. Full **2 h cadence** over several cycles: RTC wake on P0.21, clock held,
    fix → accel → LoRa → BLE each cycle, floor stable.
