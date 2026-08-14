# Handover — ISL sealed-collar tracking system (first pilot, medium scale)

_Snapshot: 2026-08-11._ This repository is the **handover artifact for the first
medium-scale pilot** of the Custodia sealed-collar animal-tracking system. `main`
is the delivery branch — everything needed to build, flash, deploy, and validate the
pilot is here.

> ## ⚠️ Read first — two items are NOT integrated and are owned by the ISL lab team
>
> **1. The AS3933 wake-up receiver (WUR) has never been integrated — in any firmware
> version.** In the shipped collar firmware the "drone pass" that triggers the BLE
> offload is **faked with a timer** (`SIMULATE_WUR_HOURS`, default 0.25 h) and the real
> wake pin is disarmed (`ENABLE_WUR_WAKE = 0`). The AS3933 SPI comms, register config,
> and RC-oscillator calibration are proven on the bench (`tests/ISL_WUR_AS3933/`), and
> the collar is wired for it (wake on **P1.04**), but a **live LF wake event → P1.04 IRQ
> → real drone-pass offload must be integrated and validated by the ISL lab engineers.**
>
> **2. The BLE offload protocol is to be updated by the ISL team.** The firmware here
> uses the proven fire-and-forget NUS path (bench-verified `bad=0` across many runs), but
> the **updated BLE protocol has already been developed by Omar Khalfa and is pending
> integration** by the ISL team. Treat the current BLE path as a working placeholder that
> the ISL team will replace with Omar Khalfa's protocol.
>
> Neither item blocks the tracking + LoRa + backlog data path, which is field-validated
> end-to-end (below). Both are the on-demand *accelerometer-offload* path.

---

## What's in the box

The system has **four matched pieces** (all in this repo) plus one **cloud API**
(external). Paths below are the exact code + validation evidence.

### 1. Collar firmware — [`production/v17/`](production/v17/) ★ (the pilot build)
- **Runs on:** ISL PCB **iteration3** (schematic [`hardware/iteration3/`](hardware/iteration3/)) —
  RAK4630 (nRF52840 + SX1262) + RV-3028 RTC + L76K GNSS + AS3933 wake-up radio +
  **onboard LIS3DHTR accelerometer** (new in iteration3; replaces the external Grove
  module used through iteration2).
- **Feature set** (v16 feature-complete + the iteration3 hardware port):
  - GNSS with cold/first-fix strategy (v14) and MS621FE backup-cell health +
    charge-on-cold (v15): every LoRa packet carries `TTFF=<s>,CELL=<OK|LOW|DEAD>`.
  - LoRa P2P with the **delivery guarantee**: fixes that miss an ACK are held in a
    **flash-persisted 84-slot pending queue (~1 week @ 2 h cadence)**, then replayed
    newest-first when the link returns; older fixes overflow to a write-only archive
    (bounded loss, never corruption).
  - Two independent timers (accel collection vs drone pass); accel data is stored in a
    64-record flash ring and offloaded over BLE (fire-and-forget NUS, ≤20 B lines,
    checksummed, blast ×2). **← this offload is what the WUR/BLE items above concern.**
  - iteration3 port: accel driver rewritten I²C → **bit-bang SPI (mode 3)** on the shared
    WuR bus (accel CS P0.28); GPS power via `GPS_EN_ACTIVE_HIGH` (iteration3 TPS22918 =
    active-HIGH).
- **Validation:** [`production/v17/logs/`](production/v17/logs/)
  - `VALIDATION.md` — iteration3 bench validation (open-sky hot fixes; accel over SPI;
    LoRa/backlog/BLE/RTC/sleep/battery/flash all pass).
  - `REALWORLD_BACKLOG.md` — **end-to-end field proof** of the durable backlog: a real
    2-day out-of-range trip → on first gateway contact an **83-packet burst drained
    newest-first (seq 194→94), `emit=83 bad=0`**, ~15 s/packet.
  - `ppk_v17_iter3_summary.txt` + [`POWER_PROFILE.md`](production/v17/POWER_PROFILE.md)
    — measured power profile (table + current-vs-time graph).

### 2. LoRa repeater — [`SolarNodeSystem/v1/SolarNode_Repeater/`](SolarNodeSystem/v1/SolarNode_Repeater/)
- **Runs on:** SenseCAP Solar Node P1-Pro (XIAO nRF52840 Plus + Wio-SX1262).
- **Behaviour:** RX collar packet → **50 ms ACK-turnaround guard** → send ACK →
  **verbatim relay** on the same channel; 16-entry `<id,seq>` **dedup ring** prevents
  on-air loops. Relays raw bytes, so the collar's `TTFF`/`CELL` fields flow through
  unchanged (it also logs a `[GPS-CELL]` line per packet).

### 3. LoRa gateway — [`SolarNodeSystem/v1/SolarNode_Gateway/`](SolarNodeSystem/v1/SolarNode_Gateway/)
- **Runs on:** the same SenseCAP Solar Node P1-Pro hardware.
- **Behaviour:** RX → **ACK (jittered so it doesn't collide with a repeater ACK)** →
  parse → **emit one JSON line per accepted frame on Grove UART** (P0.10/P0.09, 115200
  8N1). No re-transmit, no field-side dedup — duplicates collapse in the cloud.

### 4. WiFi uploader — [`SolarNodeSystem/v1/XIAO_ESP32S3_Uploader/`](SolarNodeSystem/v1/XIAO_ESP32S3_Uploader/)
- **Runs on:** Seeed XIAO ESP32-S3.
- **Behaviour:** reads the gateway's JSON from Grove UART, batches, and POSTs to the
  Custodia cloud over WiFi + HTTPS. API contract:
  [`SolarNodeSystem/v1/docs/GatewayAPI.txt`](SolarNodeSystem/v1/docs/GatewayAPI.txt).
  > 🔐 **Before flashing:** fill in `config.h` — WiFi SSID/password and
  > `CLOUD_AUTH_BEARER` (the bearer token is **redacted to `YOUR_CLOUD_BEARER_TOKEN`** in
  > this repo; get the real value from the Custodia cloud admin). Give each deployed
  > gateway a unique `REPEATER_ID`.

### End-to-end bench validation
[`SolarNodeSystem/v1/docs/BenchTest_v1_20260806/`](SolarNodeSystem/v1/docs/BenchTest_v1_20260806/)
— collar → repeater → gateway → XIAO → cloud, `rx == ack == emit`, `bad == 0`, matching
data in the Custodia dashboard.

---

## Pilot readiness at a glance

| Piece | State for the pilot |
|---|---|
| Collar GNSS + RTC + battery + deep sleep | ✅ validated (bench + field) |
| LoRa delivery guarantee + 1-week backlog | ✅ **field-proven** (2-day trip, 83-packet drain, zero loss) |
| Repeater / gateway / cloud upload | ✅ bench-validated end-to-end |
| Power / lifespan | ✅ measured: ~190 µA floor → **~3.3 yr** on 9600 mAh (see [`POWER_PROFILE.md`](production/v17/POWER_PROFILE.md)) |
| **AS3933 WUR integration** | ⚠️ **NOT integrated in any version — drone pass faked by timer.** ISL lab to integrate + validate. |
| **BLE offload protocol** | ⚠️ current path is a placeholder; **Omar Khalfa's updated protocol pending integration** by the ISL team. |

---

## Power / lifespan (deployment estimate)

From the measured PPK capture ([`production/v17/logs/ppk_v17_iter3_summary.txt`](production/v17/logs/ppk_v17_iter3_summary.txt))
at a realistic deployment cadence (GNSS/1 h, accel/3 h, BLE/2 weeks, hot fixes):

| Case | mAh/day | Life on 9600 mAh ER26500 (85 % usable) |
|---|---:|---:|
| **As delivered** (sleep floor ~190 µA) | ~6.8 | **~3.3 years** |
| **If sleep-floor opt applied** (~40 µA — see below) | ~3.2 | **~7 years** |

**Biggest lifespan lever = keeping GPS fixes hot** (the v15 backup-cell strategy). If a
fraction go cold (180 s charge window), GPS energy climbs sharply: 10 % cold → ~2 yr,
25 % cold → ~1.3 yr on the delivered floor. Full breakdown:
[`production/v17/POWER_PROFILE.md`](production/v17/POWER_PROFILE.md).

---

## Pending — OPTIMIZATION ONLY (firmware-only, not blockers)

Detailed in [`production/v17/README.md`](production/v17/README.md) → *Pending
optimizations*. None prevents the pilot; the delivered firmware works as-is.

1. **Sleep-floor P1 input-buffer crowbar → ~40 µA (roughly 2× battery life).**
   `accelPinsPark()` disconnects only the P0 SPI pins; the iteration3 **P1** pins
   (P1.03 accel INT1, P1.01 accel INT2, P1.04 WuR wake) are left as connected `INPUT`
   and crowbar when floating. Fix: `NRF_P1->PIN_CNF[1/3/4] = 2` before sleep
   (v7-consistent). *(Guard P1.04 so it stays armed once the WUR is integrated.)*
2. **`TTFF`/`CELL` staleness on backlogged packets.** `formatPacket()` reads these two
   fields from globals, not per-packet. **Position data is always per-packet-correct**;
   only the two health fields can read a false `LOW` after a backlog. Fix: add
   `ttff`+`cell` to `PacketSlot` (24→28 B, flash-schema bump 4→5).
3. **Confirm GPS hot-start margin at the real deployment cadence** — the biggest lifespan
   lever (see Power).

---

## The two ISL-lab work items, spelled out

### A. Integrate + validate the AS3933 WUR (on-demand drone-pass wake)
- **Today:** the drone pass is simulated (`SIMULATE_WUR_HOURS` timer); the AS3933 is only
  proven at the SPI/config/RC-cal level (`tests/ISL_WUR_AS3933/`).
- **To do:** drive a real LF wake from a transmitter
  ([`reference/AS3933_wakeup/WuTx*`](reference/AS3933_wakeup/) — 433 MHz, 19 kHz
  OOK/Manchester, pattern `0x9669`), confirm the **P1.04 rising-edge IRQ + pattern
  match**, then set `SIMULATE_WUR_HOURS = 0` and `ENABLE_WUR_WAKE = 1` so a real drone
  beacon triggers the offload. Validate the offload fires on a real pass and the collar
  returns to the sleep floor cleanly.

### B. Integrate Omar Khalfa's updated BLE protocol
- **Today:** fire-and-forget NUS offload (bench-verified, but during field runs the drone
  side occasionally dropped a record and a collar reset disconnected other BLE peers).
- **To do:** integrate the **updated BLE protocol already developed by Omar Khalfa**
  (pending), replacing the current offload path, and validate the drone-side central end
  to end with the WUR-triggered pass from item A.

---

## Where to look for anything

| Looking for… | Path |
|---|---|
| Collar firmware (pilot build) | `production/v17/` |
| Schematic of record | `hardware/iteration3/` |
| Authoritative pin map | `docs/ISL_Pinout.md` |
| Status & what's pending | `docs/ROADMAP.md` |
| Power profile (table + graph) | `production/v17/POWER_PROFILE.md` |
| Collar field/bench validation | `production/v17/logs/` |
| Repeater / gateway / uploader | `SolarNodeSystem/v1/` |
| Cloud API contract | `SolarNodeSystem/v1/docs/GatewayAPI.txt` |
| End-to-end bench validation | `SolarNodeSystem/v1/docs/BenchTest_v1_20260806/` |
| Test status by subsystem × board | `tests/README.md` |
| WUR bring-up (SPI/config/RC-cal) | `tests/ISL_WUR_AS3933/` |
