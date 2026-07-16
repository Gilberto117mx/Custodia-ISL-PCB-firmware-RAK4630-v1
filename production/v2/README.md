# ISL Board — Production Firmware **v2** (calibrated battery)

Same node firmware as **v1** (validated on hardware — see `../v1/README.md`), with
one change: the **battery reader is now calibrated**. Knobs are in human units
(minutes/seconds, auto-converted to ms), exactly like v1's `ISL_Production_units`.

> **The only delta v1 → v2:** `readVbat_mV()`. Everything else — the operate loop,
> deep-sleep path, GPS teardown, RTC wake, LoRa TX/ACK, flash persistence, packet
> format, all knobs — is byte-for-byte v1.

## What changed: battery `readVbat_mV()`
v1 assumed `analogReference(AR_INTERNAL)` = 2.4 V full-scale
and read the battery **~30 % low** with a false "cliff" near the RT9080 LDO
dropout. Root cause (full write-up in `../../tests/ISL_Battery_ADC/README.md`):

- On **this** RUI3 core, `AR_INTERNAL` is actually **~3.67 V FS** (0.6 V band-gap
  ref, gain 1/6), not 2.4 V.
- **Direct-register SAADC access returns 0** (the core's driver owns the SAADC).

**v2 reader** (validated over two 6-point sweeps, 3.2–3.7 V):
```c
batteryAdcInit();                  // once at boot: SAADC offset calibration
                                   // (conditions the internal ref, no cliff)
raw = median-of-31 analogRead(AR_INTERNAL, P0_31);
Vbat_mV = raw * 1795 / 1000;       // through-origin single-constant cal
```
**Accuracy ≤25 mV (<0.8 %) across 3.2–3.7 V.** The packet's `vbat` field is now
trustworthy (v1 was reporting it ~30 % low).

## Knobs (min/sec)
Set in `ISL_Production/ISL_Production.ino`:
`GNSS_PERIOD_MIN`, `TX_RETRY_MIN`, `GNSS_RETRY_WAIT_MIN` (minutes);
`GNSS_FIX_TIMEOUT_SEC`, `POST_FIX_SETTLE_SEC`, `ACK_TIMEOUT_SEC` (seconds);
`TX_BUFFER_SIZE`, `MAX_GNSS_RETRIES`, `MAX_TX_RETRIES`, `DEVICE_ID` (counts).
Auto-converted to ms by a "do not edit" block. `[CFG]` prints them in those units.

## Inherited hardware layer (unchanged from v1)
| | Value |
|---|---|
| GPS | `Serial0`/UART1 (P0.19/20), EN=P1.02 active-low, isolation teardown (test #6) |
| RTC wake | P0.21 (test #5) |
| Deep-sleep floor | ~155 µA @ 3.6 V (cause unexplained — not the 1 MΩ divider; see `../../docs/ISL_DeepSleep_Notes.md`) |
| Debug | native USB `Serial` (drops in sleep; `if(Serial)`-guarded) |
| Start | always hot (external L76X has its own backup battery) |
| `DEVICE_ID` | 51 (`"051"`) |
| `ENABLE_WUR_WAKE` | 0 (arm P1.04 after the AS3933 real-wake test) |

Receiver: the project's LoRa ACK receiver (external to this repo).

## Status & test plan
**Validated (inherited from v1, same code):** deep sleep 155 µA, RTC wake, GPS
duty-cycle + fix (~21 s), LoRa TX + ACK delivery, flash persistence, hours-stable.
**New in v2:** calibrated battery — bench-validated on the ADC test; **first job
on a v2 run is to confirm the `[BAT]` line now reads the true supply** (e.g. a
3.6 V cell should log ~3.60 V, not ~2.53 V).

**Still the robustness checklist from `../v1/README.md`** applies unchanged
(ACK-loss/TX-retry, PENDING/UNDELIVERED lists, brownout, blackout, serial-log
delivered/undelivered evidence, >68 min cadence, WUR second wake). v2 does not
change any of those paths — it only fixes the battery number.

## Files
- `ISL_Production/ISL_Production.ino` — node firmware v2 (calibrated battery).
- (receiver: the project's LoRa ACK receiver, external to this repo.)
