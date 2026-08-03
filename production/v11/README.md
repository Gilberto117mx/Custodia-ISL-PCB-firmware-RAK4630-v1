# ISL Board — Production firmware v11 (COLLAR, BLE offload data-integrity fix)

> ## ✅ THIS IS THE LAST SUCCESSFUL / KNOWN-GOOD COLLAR BUILD
> Bench-verified end to end on 2026-07-28 (`logs/`): 3 drone passes, **20 unique
> accel records received, 0 bad / 0 corrupt**, the collar **never froze**, and LoRa
> stayed 100 %. If in doubt, flash **v11**. `v12` builds on this to send only *new*
> data each pass, but v11 is the reference that is proven correct.

**v11 = v10 + the BLE offload data-integrity fix.** v10 is kept in the repo as
reference. Everything else (GNSS A/B/C, LoRa P2P + ACK + delivery guarantee, 34 µA
sleep, accel flash ring, WUR/`SIMULATE_WUR`, reboot-to-sleep) is unchanged.

### Full behaviour summary (what v11 does, start to finish)

1. **Boot** → heartbeat, load flash state, one-shot GPS time-sync (seed RTC).
2. **Every `GNSS_PERIOD_MIN`** (deep-sleep wake): a **COLLECT** cycle — battery read,
   GNSS fix (Strategy A SV-gated adaptive timeout / B backoff / C RTC re-sync, or
   `SIMULATE_FIX`), **collect 10 s of LIS3DHTR accel → append to the flash ring**,
   stage the position packet.
3. **LoRa pass** — transmit the position packet P2P, wait for ACK, hold/re-send
   un-ACK'd real fixes newest-first (delivery guarantee). LoRa carries position only.
4. **Drone pass** (every `SIMULATE_WUR_HOURS`, or a real AS3933 WUR wake): bring up
   BLE and **blast the whole accel ring** (see transport below), then **reboot-to-sleep**
   to clear BLE deterministically.
5. **Deep sleep** at the 34 µA floor (battery, headless) until the next wake.

The ring is **kept** after a blast (`OFFLOAD_DELETE_AFTER_SEND = 0`): the drone
dedups by `seq`, so nothing is ever lost — but it means **each pass re-sends the
entire ring** (the receiver logs the already-seen records as `dup`). If you want each
pass to carry only *new* data instead, that is exactly what **v12** is for.

## What the v10 run proved (see `../v10/logs/`)

The overnight-style bench run was, in the ways that matter most, a **success**:

- ✅ **The collar never froze.** ~10 full GNSS→accel→LoRa→sleep cycles and **6+
  drone passes**, every one recovered cleanly (fire-and-forget + reboot-to-sleep).
  The freeze that plagued the `api.ble.custom` path is gone for good.
- ✅ **LoRa was 100 %** — every position packet (seq 58–77) ACKed, RSSI −37…−60 dBm.
- ✅ The receiver **connected, subscribed, and saw the framing** — the collar id and
  all 16 record headers arrived in order with correct seq/timestamps.
- ❌ **But zero records *validated*.** The receiver logged `new=0 dup=0 bad=0` and a
  garbage sample-count (`n=4294952064`) on every record.

### Root cause

`api.ble.uart` fragments each write into ~20-byte NUS notifications and gives the
app **no flow control**, so on a lossy/unencrypted link a notification can be
dropped. v10's per-record header —

```
R 051 42 1785165983 100 <sum>      (30+ bytes = TWO notifications)
```

— split across two packets. The **first** (`seq`, `ts`) always arrived; the
**second** (`count`, `checksum`) was routinely lost, so the receiver parsed a
garbage sample count and could never complete/verify a record. Same mechanism
dropped most sample lines too.

## The v11 fix

1. **Every line fits in ONE notification (≤ 20 B).** A dropped packet now loses a
   *whole line*, never corrupts its neighbour. The `DEVICE_ID` is sent once, the
   per-record header drops it and uses a **16-bit checksum**:
   ```
   I <dev> <nrecs>          opening announce (id sent here, once)
   R <seq> <n> <ck16>       per-record header (seq, sample-count, 16-bit checksum)
   <x>,<y>,<z>              a sample
   E <dev> <nrecs>          end of a pass
   ```
2. **Double-blast (`BLE_BLAST_REPEATS = 2`).** The whole ring is sent twice; the
   receiver dedups by `seq`, so a single loss gets a second chance for free.
3. **Receiver hardening.** A sample-count sanity guard (`0 < n ≤ 200`) means a
   corrupt header can never wedge accumulation, and the receiver now **accepts
   Just-Works pairing** (IO caps = None, no MITM) so the collar's central-initiated
   security request can complete and *encrypt/stabilise* the link.

**Transport is otherwise identical to v10 and still cannot freeze** — advertise by
name, fixed `BLE_SETTLE_MS` wait, paced `api.ble.uart.write()`, hold, reboot-to-sleep
(never `api.ble.stop()`). No ACK, so `OFFLOAD_DELETE_AFTER_SEND` defaults to `0`
(keep ring + drone dedups by seq; no data lost when no drone was there).

### On "Pairing procedure fail"

The v10 collar logged `Connected.` → `Pairing procedure fail.` on the passes where
the receiver connected — yet notifications still flowed. On this RUI3 build the
central-initiated pairing is **cosmetic** (the NUS TX characteristic is not
encryption-required), so the data loss was framing/fragmentation, **not** the
pairing. v11's receiver accepting Just-Works is a best-effort attempt at a stable
*encrypted* link; it is not required for data to arrive. The **real drone board**
(another engineer's) should likewise either accept Just-Works or ignore the request.

## ✅ Bench result — 2026-07-28 (`logs/`, `SIMULATE_FIX=1`, period 2 min, `FIX_MAX_SEC=60`)

**The offload works.** Across **3 drone passes** the reference receiver validated
**20 unique records, 0 bad checksums, 0 corrupt** — every record that arrived read
`OK (100 samples, checksum match)`.

- Pass 1: `new=16 dup=0 bad=0` on the 1st blast (all 16 records validated on the
  **first** try, clean link), then the 2nd blast = all 16 `dup` → `new=16 dup=16`.
- Pass 2 (ring rotated): seq 63–76 dup, 77–78 new → `new=18 … bad=0`.
- Pass 3: seq 65–78 dup, 79–80 new → `new=20 … bad=0`.

This **confirms the root-cause fix**: v10's `n=4294952064` garbage is gone entirely.
Short single-notification lines parse cleanly; the 16-bit checksum matches on every
record. The collar never froze, LoRa stayed 100 % (seq 78–83 all ACKed), and
reboot-to-sleep cycled the whole run.

Notes worth keeping:
- **The 2nd blast being all `dup` is the redundancy working, not a bug.** On a clean
  bench link the 1st blast already delivers everything; `BLE_BLAST_REPEATS=2` is
  insurance for a real, lossy, moving fly-by. If you want to shorten the on-air time
  for a tight fly-by window, drop it to 1 (accepting less loss margin).
- **Blast duration:** ~22 ms/line × 101 lines ≈ **2.2 s/record** → ~35 s for 16
  records, ~70 s for the x2 blast. That fits under `BLE_STREAM_MAX_MS=90 s` today,
  but it means the drone must hold the link ~70 s. For deployment, tune
  `BLE_LINE_GAP_MS` down and/or `BLE_BLAST_REPEATS` to 1 to fit the fly-by window,
  and raise `BLE_STREAM_MAX_MS` if you grow `ACCEL_RING_RECORDS`.
- **`Pairing procedure fail` still prints on the collar and is still cosmetic** — the
  receiver's `pairing_complete` callback never fired (the RUI3 security request isn't
  a standard Just-Works the central answers), yet **all data flowed and validated**.
  So encryption is not required for correctness; the real drone board can ignore the
  request too.
- **Power capture:** the PPK2 ran the full **27 min without freezing this time**
  (rig fix helped). But it was USB-powered with the accel module attached, so "sleep"
  reads ~1.9 mA (a USB-CDC artifact) with GPS/TX peaks to ~86 mA — this is **not** the
  34 µA battery floor (already validated in v7) and shouldn't be quoted as such.
  Measure the true floor **battery-only, headless** (no USB).

## Config knobs (top of `ISL_Production_v11.ino`)

| Knob | Default | Meaning |
|------|---------|---------|
| `DEVICE_ID` | 51 | Collar id, sent in the `I`/`E` lines (`051`). |
| `OFFLOAD_DELETE_AFTER_SEND` | 0 | 0 = keep ring + drone dedups by seq (no loss); 1 = clear after every blast. |
| `BLE_BLAST_REPEATS` | 2 | How many times the whole ring is sent per pass (loss resilience). |
| `BLE_SETTLE_MS` | 6000 | Fixed wait for the central to connect+subscribe before streaming. |
| `BLE_LINE_GAP_MS` | 22 | Per-line pacing (≥ conn interval) so the NUS TX buffer keeps up. |
| `BLE_STREAM_MAX_MS` | 90000 | Safety cap on the whole blast (pure delay-bounded; can't hang). |
| `SIMULATE_WUR_HOURS` | 0.25 | >0 = fake a drone pass every N h; 0 = real AS3933 WUR (P1.04). |
| `GNSS_PERIOD_MIN` | 120 | Deep-sleep between fixes (set small for bench). |
| `SIMULATE_FIX` | 0 | 1 = fabricate fixes to run indoors. |
| `ACCEL_RING_RECORDS` | 16 | Flash ring capacity (flash ~132 KB; raise for deployment). |

Fast bench recipe: `SIMULATE_FIX 1`, `GNSS_PERIOD_MIN 5`, `SIMULATE_WUR_HOURS 0.1`.

## What to check on the bench (collar + reference receiver)

1. Collar boots `PRODUCTION v11`, collects `[ACCEL] 100 samples → [RING] stored`.
2. On a drone pass: `api.ble.uart advertising 'Custodia-Tracker', N records x2 …
   (fire-and-forget)`.
3. Receiver: `Found Custodia-Tracker (by name)` → `subscribed - receiving blast` →
   `Collar id=051 announcing k records`, then per record
   **`seq=… OK (100 samples, checksum match)`**, ending `END … new=k dup=… bad=0`.
   (On the 2nd blast pass every record should read `(dup, skipping)` — that is the
   redundancy working.)
4. Collar always leaves the BLE stage on time and reboots-to-sleep, whether the
   receiver connected, never connected, or dropped mid-blast. **It can never freeze.**

## Bench-rig notes (from the v10 run)

These are **test-rig**, not firmware, issues seen during the v10 run:

- **The PPK2 power profiler froze ~11.6 min in** (its readout flat-lined at a
  constant 1.92 mA). The collar kept running the whole time — only the capture died.
  No valid endurance/power number from that run; re-capture with the PPK on its own
  USB port (not shared through a hub with the receiver), and short/known-good cables.
- **The receiver board misbehaved** (interfered with a nearby BT speaker/mouse, RST
  sometimes unresponsive). "RST not responding" is the classic signature of a **USB
  brown-out** — feed the receiver from a powered hub / a different port, and keep it
  physically a little away from the collar so the two 2.4 GHz radios don't desense
  each other during the blast.

## Known / deferred (unchanged from v10)

- **Real AS3933 WUR wake** (drop-everything-on-wake) still pending the LF
  transmitter — `SIMULATE_WUR` stands in. On real WUR, add a fast-path (advertise
  ASAP, skip GNSS/LoRa) to fit the fly-by window.
- **Accel module sleep current** (~296 µA on 3V3) — hardware: switchable rail on the
  next PCB (`ACCEL_PWR_PIN` hook is ready).
