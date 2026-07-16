# ISL_GNSS_Serial0 — GNSS-only raw NMEA monitor (test #3)

Powers the L76K (EN=P1.02), opens `Serial0`/UART1 @ 9600 `RAK_CUSTOM_MODE`, and
echoes raw NMEA to USB with a byte counter. No sleep, no LoRa, no RTC — pure GNSS.
Use it to check reception (GSV satellites-in-view + SNR, GGA fix quality, RMC
status). **Keep USB connected** to read the dump; take it to open sky.

## Outdoor result — hardware confirmed EXCELLENT (`outdoor_nmea_capture.txt`)
Open sky, continuous power: **3D fix, 12 sats used / ~19 in view, SNR 33–42 dB-Hz,
HDOP 2.0**, real position (~22.5286 N, 113.9405 E). This is the proof that the
production "no fix" was **cold-start × short power-cycled windows**, not the
antenna/hardware — see `../../docs/GNSS_FieldStrategy.md`.

## How to read the key sentences
- `$GxGSV,…,NN,…` — `NN` = satellites in view; each entry ends in **SNR (dB-Hz)**
  (30+ good, teens/blank = weak).
- `$GxGGA,…,Q,SS,…` — `Q` fix quality (0=none, 1=fix), `SS` sats used.
- `$GxRMC,…,A/V,…` — **A**=valid fix, **V**=void.

Talker is **GN** (combined GPS+BeiDou) for position sentences; GSV is per
constellation (GP/BD). TinyGPSPlus parses GN + the NMEA-4.1 nav-status field fine.
