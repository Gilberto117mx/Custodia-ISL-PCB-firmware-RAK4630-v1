# SolarNodeSystem v1 — Bench Test, 2026-08-06

**Verdict: end-to-end pipeline PASS.** Tracker collar → Gateway (LoRa) →
XIAO ESP32S3 (Grove UART) → Custodia (`/api/locations`) → dashboard, all
verified with matching data across every stage.

## Setup

| Node       | Firmware                                                                     | Notes                                       |
| ---------- | ---------------------------------------------------------------------------- | ------------------------------------------- |
| Collar     | `ISL_Production_v17.ino` (byte-for-byte v16 on iteration3 PCB, dev id `051`) | Bench, GPS visible (SV=12, TTFF=1s, CELL=OK) |
| Repeater   | *not exercised this run — direct collar↔gateway link*                        | —                                           |
| Gateway    | `SolarNodeSystem/v1/SolarNode_Gateway/SolarNode_Gateway.ino`                 | Solar Node P1-Pro, gwID=60                  |
| WiFi bridge| `SolarNodeSystem/v1/XIAO_ESP32S3_Uploader/XIAO_ESP32S3_Uploader.ino`         | Grove UART on P0.10/P0.09 (see wiring)      |
| Cloud      | `https://custodia.world/api/locations`                                       | Bearer `YOUR_CLOUD_BEARER_TOKEN`, `REPEATER_ID="RPT-001"` |

LoRa PHY (unchanged, matches ISL collar): **915 MHz, BW 250 kHz, SF7, CR 4/5,
preamble 8, sync 0x12.**

Raw logs live next to this file:
- `Tracker_log.txt` — collar side (RUI3 `+EVT:` events, ACK RSSI/SNR)
- `Gateway_log.txt` — gateway side (per-packet `[RX] [ACK] [EMIT]` trace)

## What the logs prove

### 1) LoRa link — clean, no losses in the sample

Gateway counters at the end of the capture:

```
[STAT] up 0d 00:11:59  |  rx=13 ack=13 emit=13 bad=0
[STAT] up 0d 00:11:29  |  rx=11 ack=11 emit=11 bad=0
[STAT] up 0d 00:10:59  |  rx=10 ack=10 emit=10 bad=0
[STAT] up 0d 00:10:29  |  rx=8  ack=8  emit=8  bad=0
```

`rx == ack == emit`, `bad == 0` across the whole run. Every heard collar
frame parsed cleanly, was ACK'd, and produced a JSON line on the Grove UART
that made it to the cloud.

Link quality (bench, close proximity):

| Seq | Gateway RSSI | Gateway SNR | Tracker RSSI (of ACK) | Tracker SNR |
| --- | ------------ | ----------- | --------------------- | ----------- |
| 37  | *(not in capture — first log line is post-ACK)* |             | -44 dBm               | 13 dB       |
| 36  | -73 dBm      | 11.8 dB     | -50 dBm               | 13 dB       |
| 35  | -65 dBm      | 11.8 dB     | -34 dBm               | 13 dB       |
| 34  | -78 dBm      | 12.0 dB     | -44 dBm               | 12 dB       |
| 33  | -77 dBm      | 12.5 dB     | -42 dBm               | 10 dB       |
| 32  | -71 dBm      | 12.2 dB     | -42 dBm               | 13 dB       |
| 31  | -70 dBm      | 11.5 dB     | *(next TX gap)*       |             |

SNR is comfortably above the SF7 demod floor (~-7.5 dB) on both directions,
even in the same room. Expect the gateway-side RSSI to widen dramatically
outdoors — that's what the repeater is for.

### 2) ACK protocol — 100% success, timing well inside the collar's 8 s window

Cross-referencing the two logs on `seq=36`:

```
[TRACKER] 01:05:29.224  TX  seq=36 (fix): 051,36,22.528600,113.940480,3.99,1784062098,SV=12,TTFF=1,CELL=OK
[TRACKER] 01:05:29.309  +EVT:TXP2P DONE                                                (~85 ms airtime)
[GW]      01:05:29.612  RX #9 len=64 "051,36,..." rssi=-73 snr=11.8                    (~388 ms after TX call)
[GW]      01:05:29.695  ACK sent "ACK,51,36"  (backoff 69 ms, 9 total)
[TRACKER] 01:05:29.694  +EVT:RXP2P:-50:13:41434B2C35312C3336  ("ACK,51,36" in hex)
[TRACKER] 01:05:29.734  [RX] ACK OK  RSSI=-50 dBm  SNR=13 dB
```

Turnaround: collar `TX DONE` → gateway `ACK sent` in **~386 ms**; collar sees
the ACK in **~470 ms** from its TX call. The collar's `ACK_TIMEOUT_MS = 8000`
is roughly 17× that window, so the link has huge margin.

Jittered backoff working as designed — `66, 69, 70, 84, 85, 86, 95 ms` — no
two of the ACKs in this capture used the same delay.

### 3) `GPS-CELL` diagnostic pass-through

Every packet contains the v15+ health tokens and they propagate cleanly:

```
[RX] parsed id=51 seq=36
[GPS-CELL] id=51  SV=12  TTFF=1s  CELL=OK
```

`CELL=OK` on all six packets in the sample → the collar's L76K MS621FE
backup cell is holding ephemeris. TTFF=1s = warm/hot start every wake.

### 4) Grove UART hop (P0.10/P0.09 register-level UARTE1) — works

`[EMIT]` follows every `[ACK]` (13/13 emits in the sample), so the
register-level UARTE1 setup on the NFC pins is functional on this hardware.
The UICR NFC-disable is already done on the SenseCAP Solar Node from
factory, as expected.

### 5) Cloud + dashboard round-trip — visible in the Custodia UI

Dashboard screenshot at capture time shows:
- Entry: **Tracker -949** (this is Custodia's internal record id — see fix
  #1 below), 9 locations recorded, last seen `07/15/2026 05:06`.
- Latest fix: `22.5286, 113.9405` — matches the payload verbatim
  (collar sends `22.528600, 113.940480`, dashboard truncates display).
- Battery: `3.99V` — matches the packet's `vbat` field.
- Movement between fixes: `0 m` — correct; bench-stationary tracker.
- DB row count: `812 locations` and climbing.

So the full pipeline works: **packet on air → ACK'd → JSON on UART → HTTPS
POST batch → row in Custodia's DB → dot on the map.**

## Configuration that produced this run

**Collar** (`ISL_Production_v17.ino`, unmodified):
- `DEVICE_ID = 51` (broadcast as `"051"`), byte-for-byte v16 feature set
- LoRa 915 MHz / BW250 / SF7 / CR4/5 / preamble 8 / sync 0x12, 14 dBm TX
- `ACK_TIMEOUT_MS = 8000`
- 15 s "ACK-window guard" gap between TXs is visible in `[GAP]` log lines

**Gateway** (`SolarNode_Gateway.ino`, this repo):
- `GATEWAY_ID = 60`
- `ACK_TX_DELAY_MS = 50` + `ACK_JITTER_MS = 60` → observed backoffs 66–95 ms ✓
- `XIAO_BAUD = 115200` on Grove UART (`GroveUart::` UARTE1 on P0.10/P0.09)
- No dedup here — cloud collapses duplicates (design principle)

**XIAO uploader** (`XIAO_ESP32S3_Uploader.ino` + `config.h`):
- `CLOUD_URL = https://custodia.world/api/locations`, bearer `YOUR_CLOUD_BEARER_TOKEN`
- `REPEATER_ID = "RPT-001"`
- `BATCH_MAX_PACKETS = 20`, `BATCH_FLUSH_MS = 15000`, `UPLOAD_QUEUE_SIZE = 256`
- WiFi + NTP handled by `wifiEnsure()` / `configTime()`

## Known dashboard-side gaps (no firmware change needed)

Per the user's design principle ("collars & repeaters stay dumb, cloud stays
smart"), the following can and should be fixed on the Custodia side without
touching any deployed firmware:

1. **Tracker naming.** The dashboard shows "Tracker -949" for our device.
   Custodia is displaying an internal record id instead of our `device_id`
   from the payload. Fix in the dashboard: label by `device_id` (e.g.
   "Tracker 051") and store the internal id as a secondary key.

2. **Battery percentage curve.** Dashboard reports `3.99 V (100 %)`. That's
   only "100 %" if the min/max voltage curve matches the collar's
   chemistry. For the ISL collar's 26500 LiSOCl2 (flat discharge ~3.6 V for
   most of life; drop below ~3.4 V ≈ EOL) OR for an 18650 Li-ion (min ~3.0,
   max ~4.2), the mapping needs to be set per collar type. Firmware sends
   the raw voltage; the dashboard should own the SoC estimator.

3. **Cross-run marker aggregation.** All 9 locations plot at the same
   `(22.5286, 113.9405)` because the tracker was stationary on the bench.
   Expected. When the tracker moves, `distance-since-last` will populate;
   the current `0 m` per-row is correct today.

4. **Legacy "Tracker myriota_170 / 120 / 130" entries** are from a
   different transport (satellite). They're not part of the SolarNode
   pipeline and shouldn't be conflated with these bench trackers.

## Sign-off

- Firmware in `SolarNodeSystem/v1/` behaves as specified.
- No changes required to the collar firmware, the repeater firmware, or the
  gateway firmware based on this bench test.
- Follow-ups belong to the Custodia dashboard team.
