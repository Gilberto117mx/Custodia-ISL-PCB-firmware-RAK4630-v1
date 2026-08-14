# SolarNodeSystem v1 — Repeater, Gateway, XIAO uploader

Matched set of three firmwares that carry ISL collar packets into
the Custodia cloud through a star topology.

```
    ISL collars  (RAK4630 / RUI3 - ISL_Board/production/vN, dev id 051)
                          on-air payload:  "051,7,lat,lon,v,ts[,SV=n,TTFF=s,CELL=OK|LOW|DEAD]"
             |  LoRa P2P  915 MHz / BW 250 / SF7 / CR4/5 / preamble 8 / sync 0x12
   ┌─────────┼─────────┐
   ▼         ▼         ▼
Repeater  Repeater  ...           <-- SolarNode_Repeater.ino
                                       (SenseCAP Solar Node P1-Pro:
                                        XIAO nRF52840 Plus + Wio-SX1262, RadioLib)
   └─────────┬─────────┘
             │   verbatim relay on the SAME LoRa channel
             ▼
         Gateway                   <-- SolarNode_Gateway.ino
                                       (same board as the repeater)
             │   Grove UART  115200 8N1, one JSON line per frame
             ▼
        XIAO ESP32S3               <-- XIAO_ESP32S3_Uploader.ino  (Arduino ESP32 core)
             │   WiFi + HTTPS, batched POSTs to
             │   https://custodia.world/api/locations
             ▼
       Cloud / dashboard           <-- dedup + policy live HERE
```

## Design principle — **collars & repeaters stay dumb, cloud stays smart**

The tracker collars are deployed on animals; the repeaters are solar-powered
outdoor gear. Neither is easy to touch. Anything that might need tuning —
duplicate collapsing, staleness, RSSI weighting — is deliberately pushed to
the cloud so we can iterate policy without pushing firmware to field devices.

Concrete firmware consequences:

- **Repeater** does the classic Meshtastic-style managed flood: RX → ACK
  (50 ms turnaround guard) → re-transmit the collar payload VERBATIM on the
  same channel. It keeps a 16-entry `(id, seq)` dedup ring so it doesn't
  re-relay a packet on a collar re-send or on another repeater's relay of the
  same packet — that dedup is a **functional necessity** (prevents on-air
  loops), not a policy knob.
- **Gateway** parses the same payload but does NOT re-transmit. It ACKs
  direct-heard collars (jittered so it doesn't collide with a repeater's ACK)
  and emits one JSON line per accepted frame on `Serial1`. **No dedup at the
  gateway** — if two repeaters both relay the same packet, the cloud gets two
  uploads with the same `(device_id, timestamp)` and collapses them in the
  dashboard.
- **Collar firmware is UNCHANGED** — the existing ISL `ISL_Production` firmware (dev
  id 051) works untouched.

## On-air formats (all ASCII, one channel)

- **Collar → air** *(from ISL production/vN, unchanged)*
  `"<id>,<seq>,<lat.6>,<lon.6>,<vbatV.2>,<ts>[,SV=<n>,TTFF=<s>,CELL=<OK|LOW|DEAD>]"`
  Also accepted: `"CUST,<id>,<seq>,…"` mesh form.
- **Repeater relay → air** — bytes are **identical** to the collar frame.
  Range extension only, not a new frame format.
- **ACK → collar** *(unchanged)* — `"ACK,<id>,<seq>"`. Repeater sends after
  a fixed 50 ms; gateway adds `rand(0..60) ms` on top so its ACK doesn't
  collide with a co-hearing repeater's ACK. Collar accepts the first valid
  ACK and stops retrying.

## Gateway → XIAO (Grove UART, 115200 8N1, `\n` terminated)

One JSON line per accepted frame:

```json
{"devID":51,"seq":7,"lat":24.713600,"lon":46.675301,
 "vbat":3.60,"ts":1751328000,
 "gwID":60,"rssi":-95,"snr":8,
 "sv":9,"ttff":32,"cell":"OK"}
```

`sv` / `ttff` / `cell` only appear when the collar sent them (ISL v15+). The
XIAO passes the mandatory fields (`devID`/`ts`/`lat`/`lon`/`vbat`) straight
through to Custodia as JSON numbers — no float reparse.

## Cloud POST (Custodia — see `docs/GatewayAPI.txt`)

XIAO **batches** up to 20 packets (or every 15 s idle) into one:

```
POST https://custodia.world/api/locations
Content-Type: application/json
Authorization: Bearer YOUR_CLOUD_BEARER_TOKEN

{
  "repeater_id": "RPT-001",
  "timestamp":   1699123456,
  "packets": [
    {"device_id":51,"timestamp":1751328000,"latitude":24.713600,"longitude":46.675301,"voltage":3.60},
    ...
  ]
}
```

## Files

| Path                                              | Board                      | Notes                                                                    |
| ------------------------------------------------- | -------------------------- | ------------------------------------------------------------------------ |
| `SolarNode_Repeater/SolarNode_Repeater.ino`       | XIAO nRF52840 Plus + SX1262 | Validated field firmware. RX → ACK → verbatim relay. 16-entry dedup.     |
| `SolarNode_Gateway/SolarNode_Gateway.ino`         | XIAO nRF52840 Plus + SX1262 | Same PHY & board. RX → ACK (jittered) → JSON on `Serial1`. No dedup.     |
| `XIAO_ESP32S3_Uploader/XIAO_ESP32S3_Uploader.ino` | XIAO ESP32S3                | Batches JSON lines from `Serial1`, POSTs to Custodia.                    |
| `XIAO_ESP32S3_Uploader/config.h`                  | XIAO ESP32S3                | WiFi + endpoint + token + `REPEATER_ID` — edit before flashing.          |
| `docs/GatewayAPI.txt`                             |                            | Custodia `/api/locations` spec (endpoint, auth, body, responses).        |

## Build

- **Repeater / Gateway** (both): Arduino IDE with the Seeeduino nRF52 core +
  RadioLib. FQBN `Seeeduino:nrf52:xiaonRF52840Plus`. Sketch folder name must
  match the `.ino` name.
  ```
  arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Plus SolarNode_Repeater
  arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Plus SolarNode_Gateway
  ```
- **XIAO ESP32S3**: Arduino IDE with the ESP32 core. Board = "XIAO_ESP32S3".

## Before flashing

1. **Repeater** — set `REPEATER_ID` unique per unit. Nothing else.
2. **Gateway** — set `GATEWAY_ID` if `60` clashes with anything on air.
3. **XIAO** `config.h` — fill in `WIFI_SSID`, `WIFI_PASSWORD`, set
   `REPEATER_ID` to whatever the dashboard expects for this gateway
   (`"RPT-001"`, `"RPT-002"`, …). `CLOUD_URL` and `CLOUD_AUTH_BEARER` are
   pre-loaded from the API spec. For production, replace `client.setInsecure()`
   with a pinned CA cert and set `INSECURE_TLS 0`.

## Grove UART wiring (P1-Pro gateway ↔ XIAO ESP32S3)

The Solar Node P1-Pro's Grove port is **not** on the standard XIAO Serial1 pins
(D6/D7). Per the SenseCAP block diagram, Grove goes through an NMOS level-shift
to two P0 GPIOs:

| Grove wire     | P1-Pro pin                         | Role on P1-Pro   | XIAO ESP32S3 pin |
| -------------- | ---------------------------------- | ---------------- | ---------------- |
| Yellow (SIG1)  | **Grove D0** = P0.10 (via level-shift) | TX  (drives XIAO RX) | **D7** = GPIO 44 (RX)  |
| White  (SIG2)  | **Grove D1** = P0.09 (via level-shift) | RX  (from XIAO TX)   | **D6** = GPIO 43 (TX)  |
| Red    (VCC)   | 3V3                                | power            | 3V3 (optional)   |
| Black  (GND)   | GND                                | ground           | GND (mandatory)  |

Because these pins aren't on the XIAO header, the Arduino core's `Serial1`
cannot address them — the gateway sets up **UARTE1 at register level** in the
`GroveUart` namespace inside `SolarNode_Gateway.ino`.

**One-time UICR fix:** P0.09/P0.10 are the nRF52840 NFC pins and default to
NFC mode on a factory-fresh chip. `GroveUart::factoryDisableNfcPinsOnce()`
writes `NRF_UICR->NFCPINS = 0xFFFFFFFE` and takes effect on the next reboot.
SenseCAP factory firmware already did this — safe no-op on a real Solar Node.

## Bench-test order

1. Repeater + collar only — collar sees the ACK (`[RX] ACK OK` in tracker
   log; tracker marks packet delivered). Repeater log shows `[RELAY]`.
2. Gateway alone (no XIAO yet) — loop its Grove UART into a USB-serial
   adapter and watch JSON lines appear on every `[RX]`.
3. XIAO on the gateway's Grove UART — the XIAO's USB serial prints
   `[UART] {...}` for each line.
4. XIAO joins WiFi — watch for `[HTTP] OK 200` and Custodia's
   `{"success": true, "inserted": N, ...}` after each batch.
5. Second repeater — cloud gets **two** posts per collar packet. Design
   choice; dashboard collapses them on `(device_id, timestamp)`.
