# v16 backlog test — analysis (2026-08-03, bench, repeater off→on)

Raw logs: [`Tracker_v16_backlog.log`](Tracker_v16_backlog.log) (collar),
[`BLE_Receiver_v16_backlog.log`](BLE_Receiver_v16_backlog.log) (drone), power summary
[`ppk_v16_backlog_summary.txt`](ppk_v16_backlog_summary.txt).

**Test:** bench run with the recommended backlog config (`SIM_FIX=1`, `GNSS_PERIOD_MIN=2`,
`SIMULATE_WUR_HOURS=0.25`, `PENDING_SLOTS=84`, `BACKLOG_GAP_SEC=15`). The **repeater was
powered OFF** to emulate an out-of-range excursion, fixes were allowed to pile up, then the
**repeater was powered back ON** to watch the backlog drain. **Verdict: the v16 durable
backlog works exactly as designed — 42 fixes buffered through a simulated outage and every
one delivered on reconnect, zero loss.**

## Headline: 42 fixes buffered out-of-range, all 42 delivered on return

| Phase | What the log shows |
|---|---|
| **Outage (repeater off)** | Every 2 min a SIM fix stages and TXes; each gets `RXP2P RECEIVE TIMEOUT`. `pending` climbs **1 → 42**, far past v15's old cap of 5. `undelivered` stays **0** the whole time (never hit the 84 cap). |
| **Persistence** | The backlog survived **every drone-pass reboot**: `[FLASH] Loaded: … pend=7 … 13 … 19 … 25 … 31 … 37`, each reload matching the pre-reboot count. |
| **Reconnect (repeater on)** | At seq 42 the fresh fix `ACK OK RSSI=-45`; the link is proven up and the backlog **drains newest→oldest: 42, 41, 40, … 3, 2, 1**, each ACK'd, **15 s apart** (`[GAP] 15 s`, i.e. `BACKLOG_GAP_SEC`). |
| **Result** | `[TX pass] done: delivered=42 lastSeq=1 pending=0 undelivered=0`. Next boot confirms `Loaded: nextSeq=43 delivered=42 undelivered=0 pend=0`. |

**42 fixes, 42 delivered, 0 lost, 0 out-of-order** (strict newest-first). The whole ~1.4 h
"excursion" replayed in a single pass of ~42 packets × 15 s ≈ **10.5 min**, each packet's
gap a deep-sleep nap. This is precisely the behaviour v15 could **not** do (it would have
kept only the last 5 and dropped ~37).

## Shared flash proven in practice (the earlier concern)

While the position backlog grew to 42 deep, the **accel ring ran concurrently** in the same
flash and offloaded over BLE every drone pass. The drone receiver logged accel records
**seq 20–41 = 22 records, `bad=0`**, every checksum matched, and the ×2 blast's duplicates
were correctly skipped (`dup` tracks `new`). So the accel ring (`0x0500…`) and the growing
position backlog (`0xA000…`) coexisted with **zero corruption** — the compile-time
non-collision asserts hold in practice, not just on paper.

(Note: position seq restarted at 1 because the flash schema bump 3→4 re-inited the header;
the accel ring kept its own sequence at 20 via its separate magic — both expected.)

## Everything else intact

- **RTC ← real GPS at boot:** even indoors the time-only `bootTimeSync` got UTC in ~25 s
  (`SET FROM GPS 2026-08-03 04:19:29`), so all timestamps are genuine.
- **BLE offload:** 8 drone passes, all clean (connect → blast ×2 → ring cleared →
  reboot-to-sleep), **never a freeze**; `new` climbed 1→22, `bad=0` throughout.
- **Battery reads:** 3859 → ~3977 mV, stable across the whole run (no brownout under the
  drain pass).
- **Telemetry:** every packet carried `SV=12,TTFF=1,CELL=OK` (SIM values — cell logic isn't
  exercised in sim; that was validated separately in the v15 outdoor run).

## Power capture — read the caveat

The PPK trace is **behavioural only, not a battery-life number**, for two reasons the setup
forces:

1. **USB-C was attached** for logging *and* a battery was in series with the PPK at the same
   time. With VBUS present the nRF52840 USB peripheral stays clocked, so the **deep-sleep
   floor reads ~1.67 mA here, not the proven battery-only 34 µA (v7)** — and USB backfeeds
   the rail, so absolute currents aren't deployment-representative.
2. **`SIM_FIX=1`**, so the periodic real-GPS burst (~40 mA for seconds) is absent.

What it *does* validate: the **shape** is healthy — ~88 % of time at the (USB-inflated)
floor, ~12 % in BLE-settle / LoRa-TX / boot-GPS bursts, peaks to ~95 mA on LoRa TX, and
**every burst returns to the floor with no stuck-awake anomaly**. The 42-packet drain shows
as a run of ~42 TX spikes ~15 s apart, each dropping back to the floor between.
**For a real battery-life figure, re-run battery-only (no USB), `SIM_FIX=0`.**

## Bottom line

v16 is **functionally validated**: the durable 1-week LoRa backlog buffered a full simulated
out-of-range excursion (42 fixes), persisted it across every reboot, and delivered **all 42
newest-first on reconnect with zero loss and zero corruption**, while the accel ring shared
the flash cleanly. The only follow-up is a clean **battery-only power run** for the life
number (this capture was USB-attached, so its ~1.67 mA floor is a measurement artifact, not
the device's real sleep current).
