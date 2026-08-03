# v9 run 1 — offload triggers, then BLE pairing freeze

Bench run, `SIMULATE_FIX 1`, `GNSS_PERIOD_MIN 4`, short `SIMULATE_WUR` (offload
fired after 1 cycle). Accelerometer connected; **USB-C connected**; power profiler
in series with the LiSOCl₂ cell.

## What worked ✅ (most of v9)

- **Accel → flash ring:** `[ACCEL] 100 samples in 10000 ms` → `[RING] stored seq=5
  ts=… n=100 -> slot 4 (5/16)`. 10 s @ 10 Hz collected and persisted; ring carried
  4 prior records + this one = 5.
- **Fix + LoRa TX + ACK:** `051,6,…` sent, `[RX] ACK OK RSSI=-42 dBm SNR=12` (strong,
  close range); the LoRa repeater received seq 6 (`RSSI=-65`) and ACK'd/relayed.
- **Deep sleep + RTC wake:** `IDLE deep-sleep 240 s` → `[WAKE] RTC P0.21 after ~240002 ms`.
- **Drone pass fired:** `== DRONE PASS: offloading 5 records ==` → advertised
  `Custodia-Tracker` → `sent REQ, waiting for GO`. The BLE receiver **found it,
  connected, subscribed, and received `REQ recs=5 samples=5`.** The offload path is
  almost end-to-end.

## The failure ❌ — tracker freezes on the BLE handshake

Tracker's last two lines then **nothing** (frozen, needs manual reset):
```
[BLE] sent REQ, waiting for GO...
Pairing procedure fail.
```

**Two compounding causes:**
1. **Wrong receiver.** The BLE receiver banner is `=== ISL v8 Receiver ===` — the
   old v8 sketch, which just prints incoming data and **never sends `GO`/`ACK`**.
   So the tracker waited for a `GO` that could never arrive. Use
   **`ISL_v9_Drone_Receiver`** (implements REQ→GO→…→END→ACK).
2. **RUI3 pairing/bonding failure wedges the SoftDevice.** The receiver was
   reflashed several times (banners 16:40 / 17:14 / 17:36), so the tracker held a
   **stale Just-Works bond**; the new link's pairing failed → `Pairing procedure
   fail` → the RUI3 BLE stack hung the whole tracker. (Earlier runs logged
   "Pairing success" and did *not* freeze — same fragility, opposite outcome.)

## Power (this run) — floor NOT measurable here

USB-C was connected, so the sleep floor is the **USB-attached ~1.9 mA** (nRF USB
peripheral), not the true battery floor. `PowerProfile_USB.csv`: min 1.9 mA, mean
10.5 mA, **max ~90 mA**. The 90 mA peak is the real **LoRa-TX current** — finally
captured properly (earlier 10 sps runs under-sampled it to ~40 mA). For a real
34 µA floor number, measure **headless (no USB)** as in the v7/v8 battery runs.

## Fixes applied in the firmware (this commit)

- Corrected the runtime banners `PRODUCTION v7` → **v9** (they were inherited from
  the v7 copy).
- **Freeze-recovery pre-arm:** before a drone-pass offload, the firmware now
  advances `lastDumpUnix` and **persists a queued sleep to flash first**. If the
  BLE offload hangs and the board is reset (by hand, or a future watchdog), the
  next boot performs the queued sleep and resumes normally instead of immediately
  re-attempting the offload and re-freezing.

## Still recommended (root-cause fix)

The pre-arm makes a reset *recover*, but the hang itself still needs a manual
reset. To PREVENT the freeze, remove BLE pairing entirely: reimplement the NUS
offload on RUI3's **custom BLE service with `RAK_SET_OPEN`** (no pairing/bonding),
keeping the same Nordic-UART UUIDs so `ISL_v9_Drone_Receiver` still works. A
hardware watchdog would add automatic recovery on top. See the v9 README.

## Next test

1. Flash **`ISL_v9_Drone_Receiver`** to the drone board (not the v8 receiver).
2. Clear any stale bond (fresh-flash both, or power-cycle) before the run.
3. `SIMULATE_FIX 1`, `GNSS_PERIOD_MIN 5`, small `SIMULATE_WUR_HOURS` — watch for
   `REQ → GO → REC… → END → ACK` on both sides and `DRONE PASS done (VERIFIED)` →
   `[RING] cleared` on the tracker.
4. For power, run **without USB**.
