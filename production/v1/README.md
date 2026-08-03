# ISL Board — Production Firmware **v1** ✅ (validated on hardware, ~3.6 h run)

Full node firmware for the ISL board: the operate loop, tunable knobs, 6-field
packet + `ACK,<id>,<seq>` protocol, and flash persistence layout. It talks to the
project's LoRa ACK receiver (external to this repo — it echoes whatever devID it
hears).

> **Status: VALIDATED.** Two hardware runs:
> - **Run 1** (~3.6 h, indoor): receiver confirmed packets the whole run (first
>   on-air validation of the ISL SX1262); sleep floor stable at ~155 µA. §Power profile.
> - **Run 2** (min/sec variant, 15-min period): **first GNSS FIX on the ISL board**
>   — full happy-path cycle: boot → fix in ~21 s → TX → ACK → 15-min sleep at the
>   baseline floor. §Run 2.
>
> **Two sketches, same firmware:**
> - `ISL_Production/` — original, knobs in milliseconds (run-1 as-run).
> - **`ISL_Production_units/` — knobs in MINUTES/SECONDS, auto-converted to ms.
>   Identical logic** (run-2 as-run, with `GNSS_PERIOD_MIN=15`, `GNSS_FIX_TIMEOUT_SEC=90`,
>   `POST_FIX_SETTLE_SEC=5`). *(The min/sec knob style carried into v2–v7; the current
>   deployment build is `../v17/`.)*

```
wake → read battery (quiet) → GNSS fix (retries) → build packet → BUFFER
     → buffer full or PENDING? → TX + ACK (retries, then UNDELIVERED archive)
     → deep-sleep GNSS_PERIOD_MS → repeat
```
(The state machine is the one sketched above.)

## The knobs
| Constant | Value | Role |
|---|---|---|
| `GNSS_PERIOD_MS` | 60 s | idle deep-sleep between cycles (deployment target ~2 h) |
| `TRACKER_BUFFER_SIZE` | 1 | packets buffered before a TX pass (max 3) |
| `GNSS_FIX_TIMEOUT_MS` | 60 s | max listen per fix attempt |
| `POST_FIX_SETTLE_MS` | 3 s | fix must hold this long to be accepted |
| `MAX_GNSS_RETRIES` | 2 | attempts before a timestamp-only packet |
| `GNSS_RETRY_WAIT_MS` | 300 s | deep sleep between fix attempts |
| `ACK_TIMEOUT_MS` | 8000 ms | ACK RX window |
| `TX_RETRY_MS` | 300 s | deep sleep between TX retries |
| `MAX_TX_RETRIES` | 3 | TX attempts before UNDELIVERED |
| `DEVICE_ID` | **51** | `"051"` — the ISL node's over-air ID |
| `ENABLE_WUR_WAKE` | **0** | arm AS3933 P1.04 as 2nd wake source — flip to 1 **after** the WUR real-wake test passes |

Radio: 915 MHz · BW 250 kHz · SF7 · CR 4/5 · preamble 8 · 14 dBm.

## ISL hardware specifics baked into this build
- **Debug** = native USB `Serial` — **drops in deep sleep**; every print is
  `if (Serial)`-guarded.
- **GPS UART** = **`Serial0` (UART1, P0.19/20)**, opened `RAK_CUSTOM_MODE`.
- **GPS power** = single **`L76K_EN` P1.02, active-LOW** P-FET high-side switch.
- **GPS start** = **always hot start** — the external L76X has its own backup
  battery; no reset pin, no `coldBoot` flag.
- **GPS sleep teardown** = cut EN → wait for the TX line to go quiet →
  `Serial0.end()` → **drive P0.19 + P0.20 LOW** (module isolation; a high UART pin
  phantom-powers the dead module ~440 µA — test #6).
- **RTC wake pin** = **P0.21** (schematic v2; validated test #5).
- **Extra wake** = AS3933 WUR on P1.04, behind `ENABLE_WUR_WAKE` (default off until validated).
- **Structure** = **alive-first**: heartbeat 3 s, then init in `loop()` (native-USB wedge quirk).
- **Sleep floor** = **157 µA @ 3.6 V** here — later traced to an AIN7 input-buffer
  crowbar (our own `pinMode`), **not** the divider/LDO, and fixed in **v7 → 34 µA**
  (see `../../docs/ISL_DeepSleep_Notes.md`).

Flash layout/magic, packet formatting (integer-only math), queue/pending/undelivered
logic, RTC timer flow, unix-time algorithm, and LoRa config/callbacks are all part
of this firmware.

## How to test (first run)
1. **Set the RTC time** in `rtcInit()` (`rtc.setTime(...)`) before flashing, if
   the RTC was never set (test boards from tests #1/#5 already have it set).
2. Close the serial monitor, flash, reopen COM50, reset.
3. **First flash only:** if the board is in LoRaWAN mode you'll see
   `[LORA] switching to P2P and rebooting...` and one automatic reboot — normal.
4. Expected serial (first cycle, before USB drops at `[SLEEP]`):
   `[alive]…` → banner → `[FLASH] Fresh init` → `[CFG] id=051…` → `== COLLECT ==`
   → `[BAT] … mV` → `[GPS] attempt 1/2` → fix or timeout → `[BUF] added seq=1` →
   `== TX (attempt 1/3) ==` → `[TX] seq=1: 051,1,…` → `[RX] ACK OK …` →
   `== IDLE deep-sleep 60 s ==` → port drops.
5. Run the LoRa ACK receiver on the receiver board — it should log the 6-field
   packet and `>> ACK sent (after 50 ms)`.
6. On the power analyzer (battery, 3.6 V): GPS burst → short TX/ACK burst →
   60 s floor at ~157 µA, repeating.

## Power profile (`PowerProfile_Production.csv`, 216 min, 10 ms samples, 3.6 V, indoor)

| Phase | This run |
|---|---|
| **Deep-sleep floor** | **155.3 µA median** (σ 3.2 µA) across **80 sleep plateaus** — 156.2 µA at the start → 154.7 µA at the end. **= the 157 µA bring-up baseline.** |
| GNSS acquiring | ~38 mA, **54 bursts, ~60 s each** (indoor → never fixed → burned the full `GNSS_FIX_TIMEOUT`, then the 2-attempt retry with 300 s / 60 s sleeps between) |
| LoRa TX + ACK | brief bursts in the 1–6 mA band (~51 s total over the run); receiver confirmed delivery |
| Duty split | sleep 74.4 % · GPS 25.1 % · TX/idle 0.5 % |
| Overall mean | **~9.6 mA** — this is the **indoor worst case**: GPS is on 25 % of the time because it never fixes and runs the full 60 s × 2 timeout every cycle |

**Reading it:** the sleep floor holds at ~155 µA for the entire 3.6 h — identical
to tests #5/#6 — so the deep-sleep, GPS-isolation, and RTC-wake mechanisms all
hold under the real operate loop, for hours, without drift or hangs. The ~9.6 mA
*average* is dominated entirely by GPS-on time; a **hot-start fix outdoors**
(<30 s, the module has its own backup battery) collapses that on-time, and a
longer `GNSS_PERIOD_MS` drops it further. (This 157 µA floor was later cut to
34 µA in v7 — see the deep-sleep notes.)

Consistency check: 80 plateaus, medians 154.7–157.8 µA (2 % spread over 3.6 h);
cycle structure (60 s idle + 300 s fix-retry + 60 s GPS attempts) matches the
operate loop with these knobs exactly.

## Run 2 — min/sec variant, **first GNSS fix** (`PowerProfile_run2_15min.csv`, 41 s, 3.6 V)

Config as-run (`ISL_Production_units`): period **15 min**, fix timeout **90 s**,
settle **5 s**, rest unchanged. The capture is short (operator's laptop closed,
board kept running on power), but it contains one complete cycle:

| Time | Phase | Reading |
|---|---|---|
| 1.6–5.6 s | boot + alive-first init | ~3.4 mA |
| 5.7–26.9 s | **GPS: 21.3 s @ ~42 mA** | stopped **far below the 90 s timeout** |
| ~27–28 s | LoRa TX + ACK | ~1 s in the 2–10 mA band |
| 28.1 s → end | deep sleep (15 min) | **161.7 µA median** (σ 3.0) — baseline floor |

**Why this proves a fix:** in this firmware the only path where the GPS runs
*once* for 21 s and goes *straight to TX* is a successful fix — a no-fix attempt
burns the full 90 s, deep-sleeps 5 min, and retries before any TX. So the burst
ending at 21.3 s = **TTFF ≈ 16 s + 5 s settle → fix accepted**, packet
transmitted, ACK'd (operator confirmed correct sequence numbers at the
receiver), then a clean 15-min sleep. First GNSS fix recorded on this board;
the earlier "outdoor fix" open item is closed.

## Validated so far (runs 1 + 2)
Deep sleep ~155–162 µA @ 3.6 V (hours-stable) · RTC wake (P0.21) · GPS duty-cycle
+ isolation teardown · **GNSS fix (~21 s incl. settle)** · flash persistence
across cycles · **LoRa TX + ACK delivery** (receiver-confirmed, both runs) ·
full state machine stable for hours · min/sec config variant.

## ⚠ Robustness findings still needed (untested subset)
Both runs exercised only the **happy path** (every packet ACK'd on the first
try, every cycle clean). The failure/robustness branches exist in the code but
have **never fired on this board** in v1 and need deliberate fault-injection runs
before deployment (most are addressed and validated by v6–v16 — see those READMEs):

| Area | What must be exercised | How to test |
|---|---|---|
| **ACK loss / TX retry** | ACK-timeout path: packet → PENDING, `TX_RETRY` deep sleep, retry, delivery counters | run with the receiver OFF for a few cycles, then turn it on; check `pending` drains and `delivered` catches up |
| **The lists** (PENDING / UNDELIVERED ring / buffer > 1) | queue rotation, flash slot bounds, the 8-slot undelivered ring wraparound, `TRACKER_BUFFER_SIZE` 2–3 batching | receiver off past `MAX_TX_RETRIES` → packets must land in UNDELIVERED; also run a multi-packet buffer config |
| **Brownout** | battery sag during the 42 mA GPS burst / TX on a weak cell: does the nRF brown-out mid-cycle corrupt flash state? does `nextSeq` stay monotonic? | weak/CR-limited supply or bench supply with current limit; check state after recovery |
| **Blackout** | total power loss mid-cycle → reboot recovery from flash (header/magic intact, no seq reuse, pending survives) | yank power at different cycle phases; verify `[FLASH] Loaded` state each time |
| **Serial-log evidence** | a matched delivered/undelivered count (both runs so far were receiver-eyeball only) | log node + receiver serial to files for a multi-hour run |
| **Long cadence** | single sleep ≤ 4095 s (~68 min, RV-3028 12-bit @ 1 Hz); the 2 h deployment target needs the 1/60 Hz tick mode | implement + verify a >68 min period |
| ~~Battery calibration~~ | **FIXED in `../v2`** — v1 reads ~30 % low (2.4 V-FS assumption); v2 uses the calibrated `raw×1795/1000` (≤25 mV). v1 kept as the as-run validated build. | done in v2 |
| **WUR second wake** | AS3933 real wake → `ENABLE_WUR_WAKE 1` → wake-on-demand cycle | after the LF-transmitter test passes (ISL lab engineers) |

**v1 is the validated baseline record.** The current deployment build is `../v17/`
(ported to PCB iteration3); the acceptance checklist above is carried forward and
mostly closed across v6–v16.
