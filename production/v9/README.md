# ISL Board — Production firmware v9

**v9 = v8 tracker + drone-pass bulk accelerometer offload.**

The v7 tracker (GNSS A/B/C, calibrated battery, LoRa P2P + ACK + delivery
guarantee, flash, RV-3028 clock, 34 µA floor) and v8's reboot-to-sleep BLE
teardown are preserved. What changes is **when/how** accel data leaves the node.

## Behaviour

1. **Collect + store (no BLE).** Every `ACCEL_EVERY_N_FIXES`-th GNSS fix, read
   the LIS3DHTR (bit-bang I2C, P0.24/P0.25) for `ACCEL_SECS` at `ACCEL_HZ` and
   append a fixed-size record `{seq, ts, samples[]}` to a **flash ring** of
   `ACCEL_RING_RECORDS`. LoRa still sends only the position packet, every fix.
2. **Offload on a drone pass.** A drone carrying the receiver flies over
   (~every 2 weeks). Its AS3933 WUR wake (P1.04) — or `SIMULATE_WUR_HOURS` on the
   bench — triggers a **one-directional blast** of the whole ring over BLE:
   ```
   tracker → (per record) "R <seq> <ts> <n> <sum>"  +  n × "<x>,<y>,<z>"
   tracker → "E <nRecs>"
   ```
   framed into fixed 20-byte notifications (`[len][payload]`). **No GO/ACK** — so
   nothing can hang — and the ring is **never cleared on-device**. Reliability
   comes from the tracker re-sending its whole ring each pass and the drone
   **deduping by record `seq`** (and per-record checksum), exactly like the LoRa
   repeater dedups `<id,seq>`. The ring self-bounds by rolling off the oldest, so
   nothing is lost while `ACCEL_RING_RECORDS` covers more than one drone interval.
   BLE cycles end with v8's reboot-to-sleep.

   **Transport = `api.ble.custom` + `RAK_SET_OPEN` (NO pairing).** This is the key
   lesson from the BLE bring-up: `api.ble.uart` gates notifications behind
   Just-Works pairing, which fails/**hangs** ("Pairing procedure fail"); the
   custom service with open permission notifies with no pairing (proven in
   `../../tests/ISL_BLE_CustomOpen/`). It advertises the **NUS service UUID** (the
   name truncates to "Cust"), so the receiver discovers it **by service UUID**,
   not by name.

## ⚠ Power finding — CONFIRMED: the accel module needs a switchable rail

Two battery Power-Profiler runs (no USB) settled it:

| Condition | Deep-sleep floor |
|-----------|------------------|
| Grove LIS3DHTR **connected** (v8) | **~335 µA** |
| Grove LIS3DHTR **unplugged** | **~39 µA** (≈ v7's 34 µA — base tracker intact) |

So the **module itself draws ~296 µA** on the always-on 3V3 rail (its pull-ups /
LED / regulator — `3.3 V / 296 µA ≈ 11 kΩ`). The LIS3DH *chip* powers down to
~0.5 µA, but the module overhead **cannot be shut off in firmware** while its VCC
is hard-wired to 3V3. At a 2 h cycle this ~296 µA is ~**8–10× the idle drain** and
would dominate battery life.

**THE FIX IS HARDWARE — power the accel module from a GPIO-switched rail** (like
GPS on P1.02): route the module VCC through a free GPIO — directly if its draw
stays under a few mA, else a load-switch / P-FET — so it's fully off during the
2 h sleep and on only for the 10 s collection.

**Firmware is ready for it:** set **`ACCEL_PWR_PIN`** (top of the sketch) to that
pin. `collectAccelToRing()` then drives it HIGH (+`ACCEL_PWR_BOOT_MS` settle)
before the collect and LOW after, and `accelPinsPark()` leaves P0.24/P0.25
input-disconnect so nothing back-feeds the powered-off module. `-1` (default) =
not wired yet → module stays on 3V3 and the ~335 µA floor remains.

Also added (help a little even on 3V3, but they do NOT remove the module draw):
LIS3DH `CTRL1=0x00` power-down after each collect; `accelPinsPark()` before sleep.

**Next step:** rewire the module VCC to a free GPIO, set `ACCEL_PWR_PIN`, and
re-profile — the floor should return to ~39 µA.

## ⚠ Two board packages

| Sketch | Board | Package |
|--------|-------|---------|
| `ISL_Production_v9/` | ISL tracker (RAK4630) | **RAKwireless RUI3** |
| `ISL_v9_Drone_Receiver/` | drone nRF52840 | **Adafruit / WisBlock BSP** |

---

## Data-rate / link-budget / storage analysis

**Assumptions:** raw 3-axis int16 = 6 B/sample; 10 s record; drone every 2 weeks
(336 h). Per-record size by rate: 10 Hz→600 B, 25 Hz→1500 B, 50 Hz→3000 B.

**BLE throughput** (the gate): proven text+`delay(50)`/line ≈ 120 B/s (very
slow); light pacing (`delay(8)`, used here) ≈ 0.6–1 KB/s; binary ≈ 2–5 KB/s.

**Drone contact window:** straight flyover ≈ 15–30 s; circling/hover ≈ 1–5 min.
BLE is short-range — at 50–100 m the link budget (+8 dBm TX, −95 dBm sens,
~80 dB path loss @100 m) leaves only ~10–20 dB margin, eaten fast by antenna
orientation and the animal's body. **Plan for the drone to dwell/circle; consider
LE Coded PHY (Long Range) for margin.**

**The binding constraint is transmission window, not storage** (RUI3 flash is
ample — see below):

| Cadence | Records/2 wk | Store @10 Hz | TX @0.6 KB/s | TX @2 KB/s |
|---------|--------------|--------------|--------------|-----------|
| 1 h | 336 | 202 KB | 5.6 min | 100 s |
| 3 h | 112 | 67 KB | 112 s | 34 s |
| **6 h** | **56** | **34 KB** | **56 s** | **17 s** |
| 12 h | 28 | 17 KB | 28 s | 8 s |

**Recommendation:** collect **10 s @ 10 Hz every ~6 h** (56 records, ~34 KB),
offload on the pass. Transmission is comfortable even for a short dwell.

**Flash: MEASURED ~132 KB usable** (`../../tests/ISL_Flash_Probe/probe_result.txt`
— writes succeed through 0x20000, fail at 0x21000). That easily holds a full
2-week raw ring: at 10 s @ 10 Hz (612 B/record) it fits **220 records ≈ 55 days**.
So storage is NOT a limit — the earlier "may not fit" worry was wrong. The only
low-level constraint is the **≤255 B per-call** cap on `api.system.flash.set/get`,
which v9 already handles by chunking each record (`flashWriteLong`).

`ACCEL_RING_RECORDS × 612 B` must fit ~132 KB → up to ~215 records at 10 Hz. The
default ring = 16 is a **bench** size; raise it to cover your real 2-week gap
(e.g. 56–64) with huge margin.

---

## Config knobs (top of `ISL_Production_v9.ino`)

| Knob | Default | Meaning |
|------|---------|---------|
| `ACCEL_SECS` / `ACCEL_HZ` | 10 / 10 | Record length and sample rate (100 samples/record). |
| `ACCEL_EVERY_N_FIXES` | 1 | Collect a record every Nth fix. |
| `ACCEL_RING_RECORDS` | 16 | Flash ring capacity. Flash is ~132 KB (measured) → up to ~215 records @10 Hz; raise to 56–64 for a real 2-week deployment. |
| `SIMULATE_WUR_HOURS` | 0.25 | >0 = fake a drone pass every N h (bench); 0 = real AS3933 WUR (P1.04). |
| `GNSS_PERIOD_MIN` | 120 | Deep-sleep between fixes (set 2 for a fast bench test). |
| `SIMULATE_FIX` | 0 | 1 = fabricate fixes so it runs indoors. |
| `BLE_LINE_GAP_MS` | 8 | Per-line pacing during the bulk stream (throughput knob). |

Fast bench recipe: `SIMULATE_FIX 1`, `GNSS_PERIOD_MIN 2`, `SIMULATE_WUR_HOURS 0.1`
(≈ every 6 min → a few records, then an offload).

---

## What to test to verify full functionality

### A. Build & flash
1. `ISL_Production_v9` compiles under **RUI3**; `ISL_v9_Drone_Receiver` under the
   **WisBlock BSP**.

### B. Collect → ring (indoors, `SIMULATE_FIX 1`)
2. Each collection logs `[ACCEL] 100 samples …` then `[RING] stored seq=… slot …
   (k/16)`. Ring count climbs each collection.
3. **Flash:** measured ~132 KB usable (`../../tests/ISL_Flash_Probe/`), so
   `[FLASH WRITE FAILED]` should never appear at sane ring sizes (up to ~215
   records @10 Hz). No-fix cycles never store.

### C. Drone pass / offload — **the key new path (custom-open, one-directional)**
4. When the interval elapses: tracker logs `== DRONE PASS: offloading k records ==`
   → `custom-open NUS up (no pairing), advertising … Waiting for subscribe...`.
   There must be **no "Pairing procedure fail"** anywhere.
5. Receiver logs `Found tracker (NUS service)! Connecting...` → `Connected` →
   `subscribed - receiving blast:`, then per record `[REC] seq=… n=…` and
   `seq=… OK (… samples, checksum match)`, ending with `== END of blast: k
   records … new=/dup=/bad= ==`.
6. Tracker logs `blasted k records (one-directional …)` → `DRONE PASS done
   (blasted)` → reboot-to-sleep. The ring is **kept** (count unchanged) — the
   drone dedups, so a second pass shows the same records as `dup`.
7. **Dedup check:** let two passes happen without resetting the receiver — the
   second should log the records as `(dup, skipping)`, `new=0`.
8. **No-subscriber path:** run a pass with the receiver OFF → tracker logs
   `no subscriber - keeping data for next pass`, no hang, cycles continue.

### D. Cadence & power
9. Normal collect cycles take the plain v7 `deepSleep()` (no reboot); only drone
   passes reboot-to-sleep. Confirm the floor is still ~34 µA between cycles.
10. Confirm passes recur at `SIMULATE_WUR_HOURS` and the ring re-accumulates
    between them.

### E. Real WUR (deployment, later)
11. Set `SIMULATE_WUR_HOURS 0` + `ENABLE_WUR_WAKE 1`. The offload then triggers on
    a real P1.04 wake. **TODO before deployment:** the AS3933 must be configured
    and issued CLEAR_WAKE after each pass (the WUR bring-up, test #4) — until then
    `WUR_COOLDOWN_SEC` rate-limits repeats. Real-WUR wake was only partially
    validated (see `tests/README.md` #4).
