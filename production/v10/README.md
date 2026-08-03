# ISL Board — Production firmware v10 (COLLAR, final BLE offload)

**v10 = v9 + the finished collar-side BLE offload.** Everything in v7–v9 is
preserved (GNSS A/B/C, LoRa, 34 µA sleep, accel flash ring, WUR/`SIMULATE_WUR`,
reboot-to-sleep). The only change is *how the accel ring leaves the collar*, now
locked to the proven, receiver-friendly transport.

## The offload (collar side is all that matters here)

**v10 reverts the transport to the one that ran 5 clean cycles in v8** — the
`api.ble.custom` + `notify()` path kept **freezing the collar** (a `notify()` on a
link the drone had just dropped blocks *forever* inside the RUI3 stack; even an
application-level watchdog can't fire because the CPU is stuck in the vendor code).
The v8 transport has no such call, so it simply cannot hang.

- **Transport: RUI3 `api.ble.uart` (Nordic UART Service), FIRE-AND-FORGET.**
  The collar `api.ble.uart.start(0)`, advertises by **NAME** (`Custodia-Tracker`),
  waits a **fixed** `BLE_SETTLE_MS` for a central to connect + subscribe, then just
  `api.ble.uart.write()`s each line paced `BLE_LINE_GAP_MS` apart, holds, and
  **reboots-to-sleep** (which is what clears BLE — we never `api.ble.stop()`).
  `api.ble.uart.write()` pushes into the NUS TX buffer and returns whether or not a
  central is listening — **there is no connect/subscribe callback, no `notify()`
  on a live link, and no drone→collar handshake to block on.** The whole stage is a
  bounded sum of `delay()`s, so **the collar can never freeze on BLE.** This *is*
  the "chrono": time, not link state, ends the stage.
- **Discovery is by NAME** (api.ble.uart advertises the name, not the NUS UUID), so
  the reference receiver matches `Custodia-Tracker`, then discovers the standard
  NUS and subscribes to TXD — exactly like the v8 receiver.
- **One-directional, ID-tagged text stream** (newline-delimited over the NUS byte
  stream):
  ```
  ID <dev> recs=<N>
  R <dev> <seq> <ts> <n> <sum>
  <x>,<y>,<z>   × n
  E <dev> <N>
  ```
  Every line carries the collar `DEVICE_ID`, so a drone servicing several animals
  never confuses whose data is whose. The receiver dedups by `seq` and verifies a
  per-record checksum.
- **No ACK (fire-and-forget), so the ring is KEPT by default**
  (`OFFLOAD_DELETE_AFTER_SEND 0`): the collar can't know if the drone actually got
  the blast, so it keeps the records and the drone dedups by `seq` — **no data is
  lost** when no drone was really there, and the ring self-bounds by rolling off the
  oldest. Set to `1` to clear after every blast (assumes delivery; loss acceptable).

> The **drone/receiver is another engineer's board** (different code). The
> `ISL_v10_Drone_Receiver` here is only a **reference** so you can bench-verify the
> collar. Connection stability for a long blast is tuned on the central (drone)
> side — out of scope for the collar; the collar just advertises + fire-and-forget
> streams, and can never freeze doing so.

## Config knobs (top of `ISL_Production_v10.ino`)

| Knob | Default | Meaning |
|------|---------|---------|
| `DEVICE_ID` | 51 | Collar id, tagged on every line (`051`). |
| `OFFLOAD_DELETE_AFTER_SEND` | 0 | 0 = keep ring + drone dedups by seq (no data loss); 1 = clear after every blast. |
| `SIMULATE_WUR_HOURS` | 0.25 | >0 = fake a drone pass every N h; 0 = real AS3933 WUR (P1.04). |
| `GNSS_PERIOD_MIN` | 120 | Deep-sleep between fixes (set small for bench). |
| `SIMULATE_FIX` | 0 | 1 = fabricate fixes to run indoors. |
| `ACCEL_RING_RECORDS` | 16 | Flash ring capacity (flash ~132 KB; raise for deployment). |
| `BLE_SETTLE_MS` | 5000 | Fixed wait for the central to connect+subscribe before streaming. |
| `BLE_LINE_GAP_MS` | 20 | Per-line pacing so the NUS TX buffer keeps up. |
| `BLE_STREAM_MAX_MS` | 60000 | Safety cap on the whole blast (pure delay-bounded; can't hang). |

Fast bench recipe: `SIMULATE_FIX 1`, `GNSS_PERIOD_MIN 5`, `SIMULATE_WUR_HOURS 0.1`.

## What to check on the bench (collar)

1. Collar boots `PRODUCTION v10`, collects `[ACCEL] 100 samples → [RING] stored`.
2. On a drone pass: `api.ble.uart advertising 'Custodia-Tracker' … settling 5000 ms
   (fire-and-forget)` — **no "Pairing procedure fail"** anywhere.
3. Reference receiver (or the future drone): `Found Custodia-Tracker (by name)` →
   `subscribed - receiving blast` → `Collar id=051 announcing k records`, then per
   record `id=051 seq=… OK (100 samples, checksum match)`, ending `END id=051`.
4. Collar logs `blasted k records as id=051 (fire-and-forget) - reboot-to-sleep
   clears BLE` → `blasted, kept for dedup` → reboot-to-sleep → resumes normal
   GNSS/LoRa cycling.
5. **The collar always leaves the BLE stage on time**, whether the receiver
   connected, never connected, or connected and dropped mid-blast — the stage is a
   fixed sum of delays (`BLE_SETTLE_MS` + per-line gaps + `BLE_HOLD_MS`), with no
   call that can block on link state. **It can never stay stuck in BLE.**

## Known / deferred
- **Real AS3933 WUR wake** (drop-everything-on-wake) still pending the LF
  transmitter — `SIMULATE_WUR` stands in until then. On real WUR, a fast-path
  (advertise ASAP, skip GNSS/LoRa init) should be added to fit the fly-by window.
- **Accel module sleep current** (~296 µA on 3V3) — hardware: switchable rail on
  the next PCB (`ACCEL_PWR_PIN` hook is ready).
