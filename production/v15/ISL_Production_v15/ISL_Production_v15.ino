/*
 * ============================================================================
 * ISL Board (RAK_feather) - PRODUCTION firmware v15  (COLLAR)
 * ============================================================================
 * v15 = v14 + GPS BACKUP-CELL HEALTH & CHARGE-ON-COLD + TTFF/CELL telemetry.
 *
 * The L76K's MS621FE backup cell preserves ephemeris for HOT starts and only charges
 * while the GPS is powered. Measured (tests/ISL_GPS_Backup_CellTest): a charged cell
 * gives ~3-5 s hot fixes even after 1 h off; a drained cell gives ~35 s cold starts
 * (or fails if the almanac is lost). Our ~5 s/cycle on-time can't offset the ~1 h
 * backup drain, so the cell slowly depletes -> cold starts return (the v14 overnight
 * run). v15 makes the collar SELF-REGULATING:
 *   - Infer cell health from TTFF: a fix within HOT_TTFF_SEC => cell holding (HOT).
 *   - On a slow/missed fix (cell low), KEEP THE GPS ON to GPS_CHARGE_SEC total to
 *     recharge the cell - the patient window IS the charge - so the next wake is hot.
 *   - On a HOT fix, power off immediately (no charge) - cheap.
 *   - If it stays non-hot for CELL_DEAD_AFTER cycles despite charging, flag the cell
 *     DEAD and stop the futile charge (still fix, just slower).
 * Every position packet carries "TTFF=<s>,CELL=<OK|LOW|DEAD>" so cell health is
 * visible in the LoRa telemetry (the repeater relays the raw string). Worst case
 * (dead cell -> 3 min/cycle) is still ~9-12 months on a 9000 mAh 26500 LiSOCl2.
 * Operationally: pre-charge the cell (module powered ~30 min) before deployment; the
 * firmware then maintains it and tells you if it ever stops holding.
 *
 * v15 keeps v14's COLD / FIRST-FIX budget selection unchanged (below).
 * ----------------------------------------------------------------------------
 * v14 = v13 (bench-verified) + a COLD / FIRST-FIX strategy so the GPS gets a long,
 * patient window exactly when it needs one, without wasting energy on normal wakes.
 *
 * WHY: a cold GPS (fresh power, or moved far while away - a "new city") can take
 * minutes and won't even report satellites for the first 30-60 s. v13's 25 s no-sky
 * abort therefore killed the cold first fix before the receiver had woken up (this is
 * what silenced the outdoor headless run - see v13/logs/OUTDOOR_ANALYSIS.md).
 *
 * WHAT v14 DOES: it classifies each fix as WARM or COLD and picks the budget:
 *   - WARM (the common case): today's frugal 25 s no-sky abort / 120 s cap.
 *   - COLD: a patient COLD_FIX_MAX_SEC window (default 180 s = your "3 minutes") with
 *     NO early no-sky abort, so a cold receiver has time to wake up and lock.
 * A fix is COLD when ANY of these hold:
 *   - !rtcSynced                    (RTC lost its clock -> MAIN POWER was lost: fresh
 *                                    flash / battery reconnect / literally first ever);
 *   - header.lastFixUnix == 0       (never had a POSITION fix yet);
 *   - now - lastFixUnix >= STALE     (long time since the last fix: long gap / NEW CITY
 *                                    with the battery still connected);
 *   - consecutiveNoFix >= COLD_AFTER_NOFIX  (just emerged from a no-sky stretch, i.e.
 *                                    boxed/in-transit -> the arrival fix is cold).
 * `lastFixUnix` is persisted in the flash header (survives deep sleep, reboots AND
 * power loss), so the "new city" case is caught whether or not the battery stayed on:
 * battery kept -> stale gap; battery lost -> !rtcSynced. On a successful fix we record
 * lastFixUnix, so the NEXT fix is warm/short again.
 *
 * How to tell COLD apart from a normal wake: a normal deep-sleep wake resumes with
 * rtcSynced==true AND a recent lastFixUnix; anything else (clock lost / no recent fix /
 * no-fix streak) is the cold path. The [CFG-GPS] boot line and the per-cycle
 * "[GPS] COLD-fix budget ... reason=..." line make it visible in the log.
 *
 * Everything else is byte-identical to v13, below.
 * ----------------------------------------------------------------------------
 * v13 = v12 (bench-verified: new-data-per-pass, 0 bad/0 corrupt) + TWO additions:
 *
 *  (1) TWO INDEPENDENT TIMERS for the accelerometer path. The accel collection is
 *      now on its OWN cadence, decoupled from the GNSS/LoRa cadence and from the
 *      drone pass:
 *        * ACCEL_PERIOD_HOURS = how often a 10 s accel record is collected + stored;
 *        * SIMULATE_WUR_HOURS = how often the drone passes (real deployment: the
 *          AS3933 WUR wake instead) -> offload the WHOLE ring of everything stored
 *          since the last pass, then clear it (v12 new-data-per-pass).
 *      So e.g. accel every 6 h -> 4 records/day -> ~40 over 10 days, and the next
 *      drone pass sends all 40 (the drone waits for the full transfer -- the BLE
 *      engineer's concern; we just stream it reliably as v11/v12 proved). The ring is
 *      a bounded flash ring that DROPS THE OLDEST record when full, so the device can
 *      never exhaust flash (size ACCEL_RING_RECORDS for the drone interval).
 *
 *  (2) The per-record absolute TIMESTAMP is back on the BLE wire. v11 dropped `ts`
 *      from the header when it shortened every line to <= 20 B; v13 sends it as its
 *      OWN short line "T <ts>" (13 B, one notification) right after the record header,
 *      so motion bursts are time-referenced again without breaking the "one line =
 *      one notification" rule that made v11/v12 reliable.
 *
 * From v12 (unchanged): every drone pass transmits ONLY NEW accel data
 * (OFFLOAD_DELETE_AFTER_SEND = 1 -> clear-after-send); on-collar buffering/replay is
 * intentionally the other engineer's job. Set it to 0 for v11's keep-and-dedup.
 *
 * TRANSPORT (unchanged from v11/v12 -- still cannot freeze):
 *  - RUI3 `api.ble.uart`, FIRE-AND-FORGET (the bounded sum of delay()s is the
 *    "chrono"); advertise by NAME "Custodia-Tracker"; fixed BLE_SETTLE_MS wait; write
 *    each line paced BLE_LINE_GAP_MS; hold BLE_HOLD_MS; reboot-to-sleep clears BLE
 *    (never api.ble.stop()); blasted BLE_BLAST_REPEATS times for loss resilience.
 *
 * WIRE FORMAT (every line <= 20 B = one notification; the "T" line is the v13 add):
 *        "I <dev> <nrecs>"      opening announce (DEVICE_ID sent here, once)
 *        "R <seq> <n> <ck16>"   per-record header (16-bit checksum keeps it <=20 B)
 *        "T <ts>"               <-- v13: per-record unix timestamp (its own line)
 *        "<x>,<y>,<z>"          a sample
 *        "E <dev> <nrecs>"      end
 *
 * NOTE: the drone/receiver is another engineer's board (different code). "Pairing
 * procedure fail" on the collar is cosmetic on this RUI3 build - notifications flow
 * unencrypted and validate fine (proven in the v11/v12 runs).
 * ----------------------------------------------------------------------------
 * v9 = v8 tracker + DRONE-PASS BULK ACCEL OFFLOAD. The v7 tracker (GNSS A/B/C,
 * calibrated battery, LoRa P2P + ACK + delivery guarantee, flash, RV-3028 clock,
 * 34 uA floor) and v8's reboot-to-sleep BLE teardown are preserved. What changes
 * is WHEN and HOW the accelerometer data leaves the node:
 *
 *   1) COLLECT 10 s OF ACCEL, STORE IT IN A FLASH RING (not sent immediately).
 *      Every ACCEL_EVERY_N_FIXES-th GNSS fix we read the LIS3DHTR (bit-bang I2C
 *      on P0.24/P0.25) for ACCEL_SECS at ACCEL_HZ and append a fixed-size record
 *      {seq, ts, samples[]} to a ring of ACCEL_RING_RECORDS in api.system.flash.
 *      LoRa still carries ONLY the position packet, every fix (unchanged).
 *
 *   2) OFFLOAD THE WHOLE RING OVER BLE ONLY ON A "DRONE PASS". A drone carrying
 *      the BLE receiver flies over ~every 2 weeks; its AS3933 WUR wake (P1.04)
 *      triggers the offload. On a pass we run a REQUEST/VERIFY handshake:
 *        tracker -> "REQ recs=N samples=S sum=SUM"
 *        drone   -> "GO"
 *        tracker -> per record: "REC seq ts n" then n "x,y,z" lines
 *        tracker -> "END"
 *        drone   -> "ACK recs=N sum=SUM"   (only if it got ALL records intact)
 *      The ring is CLEARED only on a matching ACK; otherwise everything is kept
 *      for the next pass. This guarantees the drone received the whole transfer.
 *
 *   3) SIMULATE_WUR_HOURS (bench): with no real drone/WUR, set this to fake a
 *      drone pass every N hours (RTC-time based) so the store->offload->verify
 *      cycle is testable indoors. 0 = use the real AS3933 WUR wake (P1.04).
 *
 *   >> Storage reality: raw 3-axis @ 10 Hz is ~600 B / 10 s record. A full 2-week
 *      ring is a few KB. RUI3 flash is MEASURED at ~132 KB usable
 *      (tests/ISL_Flash_Probe), so storage is NOT a limit - a 2-week raw ring fits
 *      easily (10 s @ 10 Hz = 612 B/record -> ~220 records). The only low-level
 *      cap is <=255 B per api.system.flash call, handled by chunking each record.
 *      ACCEL_RING_RECORDS + ACCEL_EVERY_N_FIXES tune cadence/capacity.
 *
 *   >> Receiver: ISL_v9_Drone_Receiver (Bluefruit central, matches by NAME) runs
 *      the drone side of the handshake and verifies the transfer. RUI3 advertises
 *      the name only, not the NUS UUID - hence the name match. BLE cycles end with
 *      a reboot-to-sleep (v8) so the deep-sleep floor is untouched.
 * ----------------------------------------------------------------------------
 * v7 = v6 + THE DEEP-SLEEP FLOOR FIX. A step-by-step teardown found the whole
 * ~120 uA of "unexplained" sleep-floor overage was ONE line: pinMode(P0.31,
 * INPUT) on the battery-sense pin. P0.31 (AIN7) floats at the 1M/1M divider
 * midpoint (~VDD/2), and a connected DIGITAL input buffer there conducts ~118 uA
 * of shoot-through ("crowbar") current. Proof: with the modules attached, the
 * floor moved 34 uA (pin disconnected) <-> 152 uA (pin as INPUT), and NOTHING
 * else in the init (Wire, RV-3028 config, trickle-charge, timer, wake-pin setup)
 * changed it. v7 leaves that input buffer DISCONNECTED (see battPinDisconnect())
 * except during the actual SAADC sample - the ADC reads the analog voltage
 * through its own mux, so the battery reading is unaffected. Expected floor with
 * GNSS + WUR attached: ~35 uA (was ~155). That is ~4-5x longer idle battery life.
 * Nothing else changes from v6 - all of A/B/C + the delivery guarantee are as-is.
 *
 * v6 = v5 + the GNSS FIELD STRATEGY (see docs/GNSS_FieldStrategy.md), built from
 * the v3->v5 outdoor runs. Four behaviours change; everything else (GPS
 * teardown/isolation, calibrated battery, alive-first structure, LoRa P2P +
 * ACK, flash persistence, deep sleep) is inherited unchanged.
 *
 *  A) SV-GATED ADAPTIVE GPS TIMEOUT (strategy A - the centrepiece).
 *     Instead of a fixed GPS window we watch satellites-IN-VIEW live:
 *       - Fix (hot ~7 s / warm ~30 s)                    -> done immediately.
 *       - Fewer than SV_MIN sats seen by NO_SKY_ABORT_SEC -> ABORT (no sky),
 *         emit a timestamp-only packet. Saves ~60-100 s of ~45 mA every blind
 *         cycle (dens / dense canopy / the 24 h sealed transport).
 *       - SV_MIN+ sats visible but no fix yet            -> EXTEND to FIX_MAX_SEC
 *         (satellites are up, the wait is worth it).
 *
 *  B) NO-SKY BACKOFF (strategy B).
 *     After NOFIX_BACKOFF_AFTER (K) consecutive no-fix cycles, STRETCH the
 *     GPS cadence to BACKOFF_PERIOD_HOURS (e.g. 2 h -> 6 h) so a long no-sky
 *     stretch becomes a few short probes instead of dozens of wasted GPS bursts.
 *     Snaps straight back to the normal cadence on the first successful fix.
 *
 *  C) RTC RE-SYNC ON EVERY REAL FIX (strategy C - fixes the 11-day-behind bug).
 *     v5 synced the RTC ONCE and could seed it from the module's unreliable
 *     backup-RTC time when there was no position fix. v6 re-syncs the RV-3028
 *     from the GPS UTC of EVERY real position fix (a position fix => the time is
 *     genuine satellite time, not the module's backup). The packet timestamp on
 *     a fix cycle is taken directly from that fix; no-fix cycles extrapolate
 *     from this GNSS-disciplined clock. => every timestamp is GNSS-derived, and
 *     RV-3028 drift is corrected for free on each fix. (The two-sample time-only
 *     seed is kept only to give a plausible clock BEFORE the first-ever fix.)
 *
 *  #5) SUCCESSFUL FIXES ARE NEVER ABANDONED (delivery guarantee).
 *     A real fix that gets no ACK is NOT dropped - it is kept in `pending` and
 *     re-sent on later cycles. Ordering follows "newest matters most":
 *       - Send the freshly-collected packet FIRST.
 *       - ONLY if it ACKs (link proven up) do we drain the older un-ACK'd real
 *         fixes, newest -> oldest, each TX_PULSE_GAP_SEC (>=30 s) apart so their
 *         ACK windows never collide. Stop at the first failure (link dropped) -
 *         the rest stay pending for next time.
 *       - If the freshest packet itself fails to ACK, the link is down: we don't
 *         waste energy blasting the backlog, we just retain everything.
 *     (D, the low-battery GPS lockout, is intentionally NOT implemented: the
 *     LiSOCl2 primary cell has a near-flat discharge curve, so a voltage
 *     threshold can't tell "healthy" from "nearly dead" - strategy A already
 *     bounds the wasted GPS energy. The MS621FE GPS backup is rechargeable;
 *     strategy A + the "fix ASAP at sealing" procedure cover it.)
 *
 * Bench facts to remember:
 *  - USB-C attached => sleep floor ~1.78 mA (nRF USB peripheral active while
 *    VBUS present). The ~155 uA floor exists only on battery. Measure headless.
 *  - [BAT] reads high (~3.9 V) with USB attached (VBUS back-feeds the rail);
 *    on battery it reads the true cell voltage.
 * ============================================================================
 *
 *   Debug  = native USB `Serial` (drops in deep sleep; prints if(Serial)-guarded).
 *   GPS    = Serial0/UART1 (P0.19/20), EN=P1.02 active-low; isolation teardown.
 *   RTC    = RV-3028, ~INT wake on P0.21; VBACKUP has NO battery (3.3 V rail).
 *   Sleep  = api.ble.stop + FPU off + clearFPU + api.system.sleep.all.
 * ============================================================================
 */

#include <stdint.h>    // force fixed-width types first (RUI3 <time.h> can knock these out)
#include <stddef.h>
#include <Arduino.h>
#include <nrf.h>       // SAADC offset calibration (battery reader conditioning)
#include <Wire.h>
#include <TinyGPSPlus.h>
#include "Melopero_RV3028.h"

// ============================================================================
// USER CONFIGURATION  -->  MINUTES / SECONDS / counts
// ============================================================================
constexpr uint32_t GNSS_PERIOD_MIN       = 120;  // NORMAL idle deep-sleep between cycles (DEPLOYMENT = 120 = 2 h; set to 2 for a quick bench check)
constexpr uint8_t  TRACKER_BUFFER_SIZE   = 1;    // fresh packets staged before a TX pass (max 3; keep 1)
constexpr uint32_t POST_FIX_SETTLE_SEC   = 5;    // a fix must stay valid this long before it's accepted
constexpr uint32_t ACK_TIMEOUT_SEC       = 8;    // RX window for the ACK after each TX
constexpr uint16_t DEVICE_ID             = 51;   // "051" - ISL node over-air ID

// ----------------------------------------------------------------------------
// v9 - ACCELEROMETER (Grove LIS3DHTR, bit-bang I2C on P0.24/P0.25) -> FLASH RING
//   100 Hz / +-2 g / high-res config; raw 16-bit counts. A 10 s record is
//   appended to a flash ring on the ACCEL timer (below); the whole ring is
//   offloaded over BLE only on a drone pass (WUR / SIMULATE_WUR).
// ----------------------------------------------------------------------------
#define ENABLE_ACCEL_BLE       1    // master switch for the whole v9 accel path
constexpr uint32_t ACCEL_SECS          = 10;  // seconds of motion per record
constexpr uint32_t ACCEL_HZ            = 10;  // effective sample rate (period = 1000/HZ ms)
constexpr uint16_t ACCEL_SAMPLES_PER_REC = ACCEL_SECS * ACCEL_HZ;   // 100 samples (6 B each)

// v13 - TWO INDEPENDENT TIMERS (this is the v13 feature). The accelerometer now has
// its OWN collection cadence, decoupled from the GNSS/LoRa cadence AND from the drone
// pass:
//   * ACCEL_PERIOD_HOURS  = how often a 10 s accel record is collected + stored.
//   * SIMULATE_WUR_HOURS  = how often the drone passes (below) -> offload the WHOLE
//                           ring of everything accumulated since the last pass.
// Example: ACCEL_PERIOD_HOURS = 6 -> 4 records/day -> ~40 records over 10 days; the
// next drone pass sends all 40 (the drone waits for the full transfer -- that is the
// BLE engineer's concern; we just stream it reliably, as now).
//
// The accel timer is evaluated on each GNSS wake off the GNSS-disciplined RTC, so for
// an EXACT accel cadence keep ACCEL_PERIOD_HOURS a whole multiple of the wake period
// (GNSS_PERIOD_MIN/60). It is collected on the first wake at/after each accel-period
// boundary, on fix OR no-fix cycles. For a quick bench, use a small value (e.g. 0.05
// = 3 min). Deployment example: 6.
constexpr double   ACCEL_PERIOD_HOURS  = 6.0;
constexpr uint32_t ACCEL_PERIOD_SEC    = (uint32_t)(ACCEL_PERIOD_HOURS * 3600.0 + 0.5);

// v9 POWER (the big one): the Grove LIS3DHTR MODULE draws ~296 uA on always-on
// 3V3 (measured: floor 335 uA with it wired, 39 uA with it unplugged). Its own
// pull-ups/LED/regulator can't be shut off in firmware. FIX = power the module
// VCC from a GPIO-switched rail (direct GPIO high-side if the draw stays under a
// few mA, else a load-switch / P-FET). Set ACCEL_PWR_PIN to that pin and the
// firmware powers the module ONLY during the 10 s collection. -1 = not wired yet
// (module on 3V3 -> the elevated sleep floor stays; see README power note).
constexpr int      ACCEL_PWR_PIN     = -1;    // e.g. a free Pxx once rewired; HIGH = module ON
constexpr uint32_t ACCEL_PWR_BOOT_MS = 20;    // module settle time after power-on

// Flash ring capacity. Record = 12 B header + SAMPLES*6 B (100 -> 612 B). RUI3
// flash is ~132 KB usable (MEASURED, tests/ISL_Flash_Probe) -> up to ~200 records
// @10 Hz. SIZE THIS FOR THE DEPLOYMENT: it must cover the records that accumulate
// between drone passes = ceil(drone_interval / ACCEL_PERIOD). 2-week drone + 6 h accel
// = 14*24/6 = 56 records; 64 gives margin. When the ring is FULL it drops the OLDEST
// record to keep the device sane (see accelRingAppend) -- so it can never exhaust
// flash; it just loses the oldest data if a pass is missed long enough to overflow.
constexpr uint16_t ACCEL_RING_RECORDS  = 64;

// Drone pass = when the whole ring is offloaded over BLE.
//   SIMULATE_WUR_HOURS > 0  -> fake a pass every N hours (RTC-time based) for the
//                              bench (set e.g. 0.25 = 15 min; deployment ~= 336 = 2 wk).
//   SIMULATE_WUR_HOURS == 0 -> use the real AS3933 WUR wake on P1.04 (needs
//                              ENABLE_WUR_WAKE = 1 so the pin is armed in sleep).
constexpr double   SIMULATE_WUR_HOURS  = 0.25;

// BLE offload pacing -- v11 keeps v10's PROVEN no-freeze transport (RUI3
// `api.ble.uart`, FIRE-AND-FORGET: advertise by NAME, fixed BLE_SETTLE_MS wait,
// then `api.ble.uart.write()` each line paced BLE_LINE_GAP_MS apart, hold, reboot-
// to-sleep) and fixes the DATA-INTEGRITY problem the v10 run exposed.
//
// WHAT v10 SHOWED: the collar never froze (good) and the receiver connected +
// subscribed + saw the framing, but ZERO records validated. Root cause: api.ble.uart
// fragments each write into ~20-byte NUS notifications and gives us NO flow control,
// so on a lossy/unencrypted link a notification can be dropped. v10's per-record
// header "R 051 42 1785165983 100 <sum>" is 30+ bytes = 2 notifications; the FIRST
// (seq, ts) always arrived intact, the SECOND (count, checksum) was routinely lost,
// so the receiver parsed a garbage sample-count and could never complete a record.
//
// v11 FIX: keep every protocol line SHORT ENOUGH TO FIT IN ONE NOTIFICATION (<=20 B),
// so a line either arrives whole (parses) or is lost whole (that record just retries
// next pass) -- a dropped packet can no longer corrupt a neighbour. The DEVICE_ID is
// sent once (I/E lines), the per-record header drops it and uses a 16-bit checksum,
// and we BLAST THE RING BLE_BLAST_REPEATS times so a single loss gets another chance
// (the receiver dedups by seq, so repeats are free). Still fire-and-forget, still
// cannot freeze.
//
// v12 CHANGE (the ONLY behavioural difference from v11): CLEAR-AFTER-SEND. Every
// drone pass transmits ONLY NEW accel data -- the records collected since the
// previous pass -- and then the ring is cleared, so a pass NEVER re-sends records the
// last pass already carried. (In v11 this was 0: the ring was kept and every pass
// re-sent the whole ring, with the drone deduping by seq.)
//
// This intentionally drops on-collar buffering/replay: with fire-and-forget there is
// no ACK, so if a pass is missed those records are gone. That is BY DESIGN for v12 --
// the other engineer takes over buffering/retention with a different BLE approach;
// our job here is only "each transmission is fresh data." Set back to 0 to restore
// v11's keep-and-dedup behaviour.
#define OFFLOAD_DELETE_AFTER_SEND  1

constexpr uint32_t BLE_SETTLE_MS       = 6000;  // fixed wait for the central to connect+subscribe before streaming
constexpr uint32_t BLE_LINE_GAP_MS     = 22;    // per-line pacing (>= conn interval) so the NUS TX buffer keeps up
constexpr uint32_t BLE_HOLD_MS         = 500;   // let the last notification flush before we reboot
// Safety ceiling on the whole blast. Fire-and-forget is a bounded sum of delays so
// it can NEVER hang regardless; this only caps a runaway. It must comfortably exceed
// a FULL ring at the current pacing, or a big deployment blast would be truncated:
//   worst case ~= ACCEL_RING_RECORDS * (2 + ACCEL_SAMPLES_PER_REC) * BLE_LINE_GAP_MS
//                 * BLE_BLAST_REPEATS  (64 recs -> ~290 s). 600 s gives margin.
constexpr uint32_t BLE_STREAM_MAX_MS   = 600000; // 10 min ceiling (covers a full 64-record ring x2)
constexpr uint8_t  BLE_BLAST_REPEATS   = 2;     // send the whole ring N times for loss resilience (receiver dedups)

// ----------------------------------------------------------------------------
// STRATEGY A - SV-gated adaptive GPS timeout
// ----------------------------------------------------------------------------
constexpr uint16_t SV_MIN             = 4;    // satellites-in-view that make the long wait "worth it"
constexpr uint32_t NO_SKY_ABORT_SEC   = 45;   // WARM no-sky abort. Raised 25->45 after the v14 overnight run: a
                                              //   warm reacquisition after an hour asleep can need 30-60 s to see
                                              //   >=4 sats, so 25 s was killing good cycles (seq 3/4 SV=1/0). 45 s
                                              //   gives GSV time to populate before we give up. (See v14/logs.)
constexpr uint32_t FIX_MAX_SEC        = 120;  // hard cap once SV_MIN+ satellites ARE visible (extend the wait to here)

// ----------------------------------------------------------------------------
// v14 - COLD / FIRST-FIX strategy. A cold receiver (fresh power, or moved far while
// away - a "new city") can take minutes and won't even report satellites for 30-60 s,
// so the 25 s no-sky abort above would kill it. On a COLD fix we therefore give the
// GPS a long, patient window with NO early abort (grace = the full window), and only
// on cold fixes - normal wakes keep the frugal 25 s/120 s budget above. A fix counts
// as COLD when ANY of these hold (see tryOneFix):
//   * !rtcSynced                       -> the RTC lost its clock = MAIN POWER was lost
//                                         (fresh flash / battery reconnect / first ever)
//   * header.lastFixUnix == 0          -> we have never had a POSITION fix yet
//   * now - lastFixUnix >= STALE       -> long time since the last fix (long gap / NEW
//                                         CITY with the battery still connected)
//   * consecutiveNoFix >= COLD_AFTER_NOFIX -> just emerged from a no-sky stretch
//                                         (boxed/in-transit) - the arrival fix is cold
constexpr uint32_t COLD_FIX_MAX_SEC        = 180;  // >= your "3 minutes": cold-fix patient window (no early abort)
constexpr uint32_t COLD_AFTER_NOFIX        = 1;    // consecutive no-fix cycles -> next fix is COLD. 2->1 after the
                                                  //   v14 run so the FIRST miss flips the next cycle to the patient
                                                  //   window (no two warm misses in a row before cold engages).
constexpr uint32_t EPHEMERIS_STALE_HOURS   = 12;   // a fix older than this -> COLD. Keep it > GNSS_PERIOD so normal
                                                   //   wakes stay "warm"; catches long transports / power-off gaps.

// ----------------------------------------------------------------------------
// v15 - GPS BACKUP-CELL HEALTH + CHARGE-ON-COLD
// ----------------------------------------------------------------------------
// The L76K's MS621FE backup cell (5.5 mAh) preserves ephemeris for HOT starts, and
// it only charges WHILE THE GPS IS POWERED. Measured: a charged cell -> ~3-5 s hot
// fix even after 1 h off; a drained cell -> ~35 s cold start (or a failure if the
// almanac is lost). Our per-cycle on-time (~5 s) does NOT offset the ~1 h backup
// drain, so the cell slowly depletes -> cold starts return (the overnight v14 run).
//
// STRATEGY (self-regulating, the patient window IS the charge):
//   * Infer cell health from TTFF: a fix within HOT_TTFF_SEC => cell is holding (HOT).
//   * When a fix is SLOW / missed (cell low), KEEP THE GPS ON until total on-time
//     reaches GPS_CHARGE_SEC, to recharge the cell. Next cycle is hot again.
//   * When HOT, power off immediately (no charge needed) - cheap.
//   * If it stays non-hot for CELL_DEAD_AFTER cycles even after charging, the cell
//     can't hold -> flag it DEAD (telemetry) and STOP the futile charge (just fix).
// Every position packet carries "TTFF=<s>" and "CELL=<OK|LOW|DEAD>" so cell health
// is visible remotely. Worst case (dead cell -> 3 min/cycle) is still ~9-12 months
// on a 9000 mAh 26500 LiSOCl2.
constexpr uint32_t HOT_TTFF_SEC    = 15;   // fix within this => cell healthy (hot start); slower => cell low
constexpr uint32_t GPS_CHARGE_SEC  = 180;  // when the cell is low, hold GPS ON this long TOTAL to charge it (>= COLD window)
constexpr uint32_t CELL_DEAD_AFTER = 4;    // consecutive non-hot fixes despite charging => flag cell DEAD, stop charging

// ----------------------------------------------------------------------------
// STRATEGY B - no-sky backoff  (the two knobs you asked to keep at the top)
// ----------------------------------------------------------------------------
constexpr uint32_t NOFIX_BACKOFF_AFTER  = 3;  // K: this many CONSECUTIVE no-fix cycles trips the backoff
constexpr uint32_t BACKOFF_PERIOD_HOURS = 6;  // STRETCHED cadence (hours) while backed off; snaps back to
                                              //   GNSS_PERIOD_MIN on the first real fix.
// >> BENCH TIP: indoors you get no fix, so after K cycles the node sleeps
//    BACKOFF_PERIOD_HOURS and will look "frozen". For a quick indoor test raise
//    NOFIX_BACKOFF_AFTER (e.g. 999) or drop BACKOFF_PERIOD_HOURS. <<

// ----------------------------------------------------------------------------
// #5 - delivery guarantee: gap between successive TX pulses in one pass
// ----------------------------------------------------------------------------
constexpr uint32_t TX_PULSE_GAP_SEC   = 30;   // >=30 s between packets when draining the backlog, so one
                                              //   packet's ACK window can't collide with the next TX.

// GPS->RTC time seeding (hardened; used only for the pre-first-fix bootstrap - a
// real fix re-syncs the clock directly via strategy C).
constexpr uint32_t BOOT_TIME_SYNC_TIMEOUT_SEC = 120;  // boot-time time-only GPS session limit
constexpr uint16_t MIN_VALID_YEAR             = 2025; // reject the module's pre-sync default date
constexpr uint16_t MAX_VALID_YEAR             = 2044; // reject corrupt-sentence future dates
constexpr uint32_t TIME_CONFIRM_GAP_S         = 2;    // 2nd sane reading must be >= this much later
constexpr int32_t  TIME_CONFIRM_TOL_S         = 2;    // ...and advanced consistently within +/- this

// Arm the AS3933 WUR (P1.04 rising edge) as a SECOND deep-sleep wake source.
// Leave 0 until the WUR real-wake test passes. RTC wake works either way.
#define ENABLE_WUR_WAKE     0

// ----------------------------------------------------------------------------
// BENCH TEST HOOKS - all default OFF; a deployment build leaves them at 0/OFF.
// ----------------------------------------------------------------------------
// BACKOFF_BENCH_MIN: override strategy B's stretched cadence with a value in
//   MINUTES so backoff is testable indoors in minutes instead of hours.
//   0 = use BACKOFF_PERIOD_HOURS (the real deployment value). For tonight try 3.
constexpr uint32_t BACKOFF_BENCH_MIN = 0;

// SIMULATE_FIX: fabricate a GPS position fix (no sky needed) so the delivery
//   guarantee (#5) and the strategy-C RTC re-sync can be validated on the bench.
//   Skips powering the GPS; every COLLECT returns a canned fix. Set to 0 for any
//   real GPS test. On the FIRST simulated fix the RTC is seeded from SIM_UTC_*
//   (so you see "[RTC] SET FROM GPS"); afterwards the packet timestamp advances
//   off that clock. Toggle the receiver on/off between cycles to watch un-ACK'd
//   fixes get held in `pending` and re-sent newest-first, >=30 s apart.
#define SIMULATE_FIX        0
constexpr int32_t  SIM_LAT_E6 = 22528600;   // ~22.528600 N (canned bench position)
constexpr int32_t  SIM_LON_E6 = 113940480;  // ~113.940480 E
constexpr uint16_t SIM_SV     = 12;         // canned satellites-in-view to report
constexpr int      SIM_UTC_Y  = 2026, SIM_UTC_MO = 7, SIM_UTC_D = 14;  // canned seed date
constexpr int      SIM_UTC_H  = 20,   SIM_UTC_MI = 0, SIM_UTC_S = 0;   // canned seed time (UTC)

// ---------------------------------------------------------------------------
// UNIT CONVERSION (do not edit) - everything below works in milliseconds.
// ---------------------------------------------------------------------------
constexpr uint32_t SECONDS = 1000UL;
constexpr uint32_t MINUTES = 60UL * SECONDS;

constexpr uint32_t GNSS_PERIOD_MS       = GNSS_PERIOD_MIN      * MINUTES;
constexpr uint32_t BACKOFF_PERIOD_MIN   = BACKOFF_BENCH_MIN ? BACKOFF_BENCH_MIN
                                                            : BACKOFF_PERIOD_HOURS * 60UL;
constexpr uint32_t BACKOFF_PERIOD_MS    = BACKOFF_PERIOD_MIN   * MINUTES;
constexpr uint32_t NO_SKY_ABORT_MS      = NO_SKY_ABORT_SEC     * SECONDS;
constexpr uint32_t FIX_MAX_MS           = FIX_MAX_SEC          * SECONDS;
constexpr uint32_t COLD_FIX_MAX_MS      = COLD_FIX_MAX_SEC     * SECONDS;   // v14
constexpr uint32_t EPHEMERIS_STALE_SEC  = EPHEMERIS_STALE_HOURS * 3600UL;   // v14
constexpr uint32_t HOT_TTFF_MS          = HOT_TTFF_SEC        * SECONDS;    // v15
constexpr uint32_t GPS_CHARGE_MS        = GPS_CHARGE_SEC      * SECONDS;    // v15
constexpr uint32_t POST_FIX_SETTLE_MS   = POST_FIX_SETTLE_SEC  * SECONDS;
constexpr uint32_t ACK_TIMEOUT_MS       = ACK_TIMEOUT_SEC      * SECONDS;
constexpr uint32_t TX_PULSE_GAP_MS      = TX_PULSE_GAP_SEC     * SECONDS;
constexpr uint32_t BOOT_TIME_SYNC_MS    = BOOT_TIME_SYNC_TIMEOUT_SEC * SECONDS;

// ============================================================================
// RADIO - must match the receiver exactly
// ============================================================================
constexpr double   LORA_FREQ_HZ         = 915000000.0;
constexpr uint16_t LORA_SF              = 7;
constexpr uint8_t  LORA_BW              = 1;          // 250 kHz
constexpr uint8_t  LORA_CR              = 0;          // 4/5
constexpr uint8_t  LORA_PREAMBLE        = 8;
constexpr uint8_t  LORA_TX_POWER_DBM    = 14;

// ============================================================================
// ISL PIN MAP (schematic v2, hardware-verified in tests #0-#7)
// ============================================================================
#define RTC_INT_PIN         P0_21
#define GPS_EN_PIN          P1_02     // L76K_EN, ACTIVE-LOW (P-FET) - LOW = GPS ON
#define GPS_UART_RX_PIN     P0_19     // UART1 RX (Serial0) - drive LOW in sleep
#define GPS_UART_TX_PIN     P0_20     // UART1 TX (Serial0) - drive LOW in sleep
#define WUR_CLK_PIN         P0_03
#define WUR_MOSI_PIN        P0_30
#define WUR_MISO_PIN        P0_29
#define WUR_CS_PIN          P0_26     // AS3933 CS is ACTIVE-HIGH -> park LOW
#define WUR_WAKE_PIN        P1_04     // AS3933 WAKE (rising on detect)
#define BATT_ADC_PIN        P0_31     // AIN7, 1M/1M divider + C17. v7: input buffer kept
                                      //   DISCONNECTED in sleep (mid-rail crowbar = ~118 uA)
#define GPS_BAUD            9600

// v8: LIS3DHTR accelerometer - bit-bang I2C on the exposed secondary pins.
// (RUI3 4.2.4 does not drive hardware Wire1; bit-bang is pure GPIO and works.)
#define ACCEL_SDA_PIN       P0_24
#define ACCEL_SCL_PIN       P0_25
#define ACCEL_I2C_DELAY_US  5
#define LIS_ADDR            0x19     // Grove default; WHO_AM_I(0x0F) -> 0x33
#define LIS_REG_WHOAMI      0x0F
#define LIS_REG_CTRL1       0x20
#define LIS_REG_CTRL4       0x23
#define LIS_REG_OUT_X_L     0x28
#define BLE_BROADCAST_NAME  "Custodia-Tracker"

#define GPS_BOOT_DELAY_MS   500UL
#define GPS_POWERDOWN_MS    250UL
#define WARMUP_MS           3000UL
// Deep-sleep backstop is DYNAMIC: requested sleep + margin (see deepSleep).
constexpr uint32_t SLEEP_BACKSTOP_MARGIN_MS = 5UL * MINUTES;

// Battery ADC - CALIBRATED (test #2 final): Vbat_mV = raw * 1795 / 1000
#define ADC_SAMPLES         31
#define VBAT_CAL_NUM        1795UL
#define VBAT_CAL_DEN        1000UL

// RV-3028 registers/bits
#define TIME_SET_FLAG       0x5A      // USER_RAM1: "RTC has been GPS-synced"
#define REG_TIMER_VALUE_0   0x0A
#define REG_TIMER_VALUE_1   0x0B
#define REG_STATUS          0x0E
#define REG_CONTROL_1       0x0F
#define REG_CONTROL_2       0x10
#define STATUS_TF           0x08
#define CTRL1_TE            0x04
#define CTRL1_TD_1HZ        0x02      // TD=10 -> 1 Hz tick    (max 4095 s ~ 68 min)
#define CTRL1_TD_1_60HZ     0x03      // TD=11 -> 1/60 Hz tick (max 4095 min ~ 2.8 days)
#define CTRL2_TIE           0x10

// Flash layout (v6: SCHEMA_VER bumped to 2 - offsets widened so buffer[] can no
// longer run into the pending region, and pending[] enlarged to PENDING_SLOTS).
#define FLASH_MAGIC         0xA5A5CAFEUL
#define FLASH_SCHEMA_VER    3         // v14: added FlashHeader.lastFixUnix -> old flash re-inits
#define FLASH_OFF_HEADER    0x0000    // 32 B header
#define FLASH_OFF_BUFFER    0x0040    // buffer[3]  = 72 B  (ends 0x88)
#define FLASH_OFF_PENDING   0x0100    // pending[5] = 120 B (ends 0x178)
#define FLASH_OFF_UNDELIV   0x0200    // undelivered ring (8 * 24 B, ends 0x02C0)
#define FLASH_OFF_SLEEPCMD  0x0300    // v8: queued-sleep command (8 B) - see queueSleepAndReboot
#define PENDING_SLOTS       5         // un-ACK'd real fixes kept for retry (newest-first)
#define MAX_UNDELIVERED     8         // overflow archive (oldest real fixes rolled off pending)

// v8: after a BLE (fix) cycle we cannot deep-sleep directly - RUI3 re-advertises
// on the BLE disconnect and the central reconnects mid-sleep, and a Just-Works
// pairing attempt then wedges the SoftDevice so the wake never fires. Instead we
// persist the intended sleep here and api.system.reboot(): the reset fully clears
// BLE, and the fresh boot performs this sleep in a clean, v7-identical state.
#define SLEEPCMD_MAGIC      0xA5510EEDUL
struct SleepCmd { uint32_t magic; uint32_t seconds; };  // 8 bytes

// v9: accelerometer flash ring (records collected between drone passes).
#define FLASH_OFF_ACCEL_META  0x0400   // ring metadata (magic, head, count, nextSeq, lastDumpUnix, lastAccelUnix)
#define FLASH_OFF_ACCEL_DATA  0x0500   // record i at DATA + i * sizeof(AccelRecord)
#define ACCEL_META_MAGIC      0xACCE1D0AUL   // v13: bumped (added lastAccelUnix) -> old meta re-inits
struct AccelRecord {
    uint32_t seq;
    uint32_t ts;                                    // unix time of collection start
    uint16_t count;                                 // samples actually stored
    uint16_t _pad;
    int16_t  xyz[ACCEL_SAMPLES_PER_REC][3];         // raw counts
};                                                  // 12 + SAMPLES*6 bytes
struct AccelMeta {
    uint32_t magic;
    uint16_t head;          // index of the oldest record in the ring
    uint16_t count;         // records currently stored (<= ACCEL_RING_RECORDS)
    uint32_t nextSeq;       // next record sequence number
    uint32_t lastDumpUnix;  // unix time of the last successful drone offload (0 = none yet)
    uint32_t lastAccelUnix; // v13: unix time of the last accel collection (0 = none yet) - the accel timer
};
// v13: the ring must fit the RUI3 flash budget (~132 KB usable, measured).
static_assert(FLASH_OFF_ACCEL_DATA + (uint32_t)ACCEL_RING_RECORDS * sizeof(AccelRecord) <= 130000UL,
              "ACCEL_RING_RECORDS too large for the flash budget - reduce it");

// ============================================================================
// TYPES (fixed sizes, no float)
// ============================================================================
struct FlashHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t deviceID;
    uint32_t nextSeq;
    uint32_t deliveredCount;
    uint32_t undeliveredCount;
    uint32_t lastDeliveredSeq;
    uint32_t lastFixUnix;   // v14: unix time of the last successful POSITION fix (0 = never) - cold-fix detector
    uint8_t  bufferCount;
    uint8_t  pendingCount;
    uint8_t  reserved[2];
};  // 36 bytes (fits the 64 B header slot before FLASH_OFF_BUFFER=0x40)

struct PacketSlot {
    uint32_t seq;
    uint32_t timestamp;
    int32_t  latE6;
    int32_t  lonE6;
    uint16_t vbat_mV;
    uint8_t  hasFix;
    uint8_t  txAttempts;
    uint32_t satsInView;   // peak satellites-in-view (GSV) this cycle - reception diag
};  // 24 bytes

// Result of one GPS acquisition session (strategy A / C outputs).
struct FixResult {
    bool     haveFix;      // real position fix, settled
    bool     noSky;        // aborted early: < SV_MIN sats by NO_SKY_ABORT_SEC
    int32_t  latE6, lonE6;
    uint16_t sats;         // satellites used in the fix
    uint16_t peakInView;   // peak satellites-in-view this session (diagnostic)
    uint32_t ttff_ms;      // time from power-on to fix (or to abort/timeout)
    uint32_t fixUnix;      // GPS UTC of the fix, unix seconds (0 if no fix)
};

// ============================================================================
// GLOBAL STATE
// ============================================================================
Melopero_RV3028 rtc;
FlashHeader     header;
PacketSlot      buffer[3];
PacketSlot      pending[PENDING_SLOTS];   // index 0 = NEWEST un-ACK'd real fix

static volatile bool rtcWokeUs = false;
static volatile bool txDone    = false;
static volatile bool rxDone    = false;
static bool          rtcSynced = false;   // USER_RAM flag cached (GPS-synced?)
static uint32_t      cycleNum  = 0;       // increments each COLLECT (disambiguates CDC log replays)
static uint32_t      consecutiveNoFix = 0;// strategy B: consecutive cycles without a real fix
// v15 GPS backup-cell health (RAM; survives normal deep-sleep wakes, resets on a
// reboot-to-sleep - which is fine, it re-learns over the next few cycles).
static uint16_t      g_lastTTFF_s = 0;     // last fix's time-to-fix in seconds (0 = none yet) - telemetry
static uint32_t      coldStreak   = 0;     // consecutive non-HOT fixes -> drives the DEAD-cell flag
static uint8_t       g_cellStatus = 0;     // 0=OK(hot), 1=LOW(charging), 2=DEAD(not holding) - telemetry

// v9 accel state: one working record + the flash-ring metadata.
static AccelMeta     accelMeta;             // loaded from flash at boot
static AccelRecord   accelRec;              // scratch for the current collection / dump read
static bool          bleUsed    = false;   // set when BLE was brought up this cycle -> reboot-to-sleep
static uint32_t      fixCounter = 0;        // real fixes seen this session (for ACCEL_EVERY_N_FIXES)

static uint8_t  rxBuf[256];
static uint8_t  rxLen  = 0;
static int16_t  rxRssi = 0;
static int8_t   rxSnr  = 0;

// ============================================================================
// Debug over native USB - guarded: a detached/asleep port must never block.
// ============================================================================
#define DBG(...)  do { if (Serial) { Serial.printf(__VA_ARGS__); Serial.flush(); } } while (0)
void say(const char *s) { if (Serial) { Serial.println(s); Serial.flush(); } }

// forward declarations (used before their definitions)
static bool        utcSane(int Y, int M, int D, int h, int m, int s);
static uint32_t    unixFromUtc(int Y, int M, int D, int h, int m, int s);
void               rtcSetFromGps(int Y, int M, int D, int h, int m, int s);
static inline void battPinDisconnect();   // v7: used in deepSleep/napSleep, defined in the battery section
static inline void accelPinsPark();        // v9: park P0.24/P0.25 in sleep (defined below)

// ============================================================================
// FPU + wake callbacks
// ============================================================================
void clearFPU()
{
    __set_FPSCR(__get_FPSCR() & ~0x0000009Fu);
    (void)__get_FPSCR();
    NVIC_ClearPendingIRQ(FPU_IRQn);
}

void onRtcWake()   { rtcWokeUs = true; }
void onTxDone()    { txDone    = true; }

void onRxDone(rui_lora_p2p_recv_t data)
{
    rxLen  = (data.BufferSize < sizeof(rxBuf)) ? data.BufferSize : sizeof(rxBuf);
    memcpy(rxBuf, data.Buffer, rxLen);
    rxRssi = data.Rssi;
    rxSnr  = data.Snr;
    rxDone = true;
}

// ============================================================================
// FLASH helpers
// ============================================================================
static bool flashWrite(uint32_t off, const void *data, uint8_t len)
{
    return api.system.flash.set(off, (uint8_t *)data, len);
}
static bool flashRead(uint32_t off, void *out, uint8_t len)
{
    return api.system.flash.get(off, (uint8_t *)out, len);
}

// v9: api.system.flash.set/get length is bounded (<=255 B), but an AccelRecord is
// ~612 B - so write/read it in <=128 B chunks.
static bool flashWriteLong(uint32_t off, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    bool ok = true;
    while (len) {
        uint8_t n = (len > 128) ? 128 : (uint8_t)len;
        ok = flashWrite(off, p, n) && ok;
        off += n; p += n; len -= n;
    }
    return ok;
}
static bool flashReadLong(uint32_t off, void *out, uint32_t len)
{
    uint8_t *p = (uint8_t *)out;
    bool ok = true;
    while (len) {
        uint8_t n = (len > 128) ? 128 : (uint8_t)len;
        ok = flashRead(off, p, n) && ok;
        off += n; p += n; len -= n;
    }
    return ok;
}

void saveHeader()  { flashWrite(FLASH_OFF_HEADER,  &header,  sizeof(header));  }
void saveBuffer()  { flashWrite(FLASH_OFF_BUFFER,  buffer,   sizeof(buffer));  }
void savePending() { flashWrite(FLASH_OFF_PENDING, pending,  sizeof(pending)); }

void appendUndelivered(const PacketSlot &p)
{
    uint32_t idx = header.undeliveredCount % MAX_UNDELIVERED;
    uint32_t off = FLASH_OFF_UNDELIV + idx * sizeof(PacketSlot);
    flashWrite(off, &p, sizeof(PacketSlot));
    header.undeliveredCount++;
    saveHeader();
}

void flashLoadAll()
{
    flashRead(FLASH_OFF_HEADER, &header, sizeof(header));
    if (header.magic != FLASH_MAGIC || header.version != FLASH_SCHEMA_VER) {
        memset(&header, 0, sizeof(header));
        memset(buffer,  0, sizeof(buffer));
        memset(pending, 0, sizeof(pending));
        header.magic    = FLASH_MAGIC;
        header.version  = FLASH_SCHEMA_VER;
        header.deviceID = DEVICE_ID;
        header.nextSeq  = 1;
        saveHeader();
        saveBuffer();
        savePending();
        say("[FLASH] Fresh init (schema v3)");
        return;
    }
    flashRead(FLASH_OFF_BUFFER,  buffer,  sizeof(buffer));
    flashRead(FLASH_OFF_PENDING, pending, sizeof(pending));
    DBG("[FLASH] Loaded: nextSeq=%lu delivered=%lu undelivered=%lu buf=%u pend=%u\r\n",
        (unsigned long)header.nextSeq,
        (unsigned long)header.deliveredCount,
        (unsigned long)header.undeliveredCount,
        header.bufferCount, header.pendingCount);
}

// ============================================================================
// RTC (RV-3028) - wake pin P0.21; time comes from GPS (no hardcoded date).
// ============================================================================
void rtcInit()
{
    Wire.begin();
    rtc.initI2C();
    rtc.set24HourMode();
    rtc.writeToRegister(0x35, 0x00);    // backup-domain config
    rtc.writeToRegister(0x37, 0x1C);    // (note: VBACKUP has no battery on this board)
    rtcSynced = (rtc.readFromRegister(USER_RAM1_ADDRESS) == TIME_SET_FLAG);
    // SELF-HEAL: distrust the flag if the stored time itself is insane (e.g. a
    // past bad seed like "2088-31-19") - clear it and force a GPS re-seed.
    if (rtcSynced && !utcSane(rtc.getYear(), rtc.getMonth(), rtc.getDate(),
                              rtc.getHour(), rtc.getMinute(), rtc.getSecond())) {
        rtc.writeToRegister(USER_RAM1_ADDRESS, 0x00);
        rtcSynced = false;
        say("[RTC] stored time INSANE -> cleared sync flag, will re-seed from GNSS");
    }
    DBG("[RTC] %s  current: %04d-%02d-%02d %02d:%02d:%02d\r\n",
        rtcSynced ? "GPS-synced flag present" : "NOT GPS-synced (will seed from GNSS)",
        rtc.getYear(), rtc.getMonth(), rtc.getDate(),
        rtc.getHour(), rtc.getMinute(), rtc.getSecond());
    rtc.writeToRegister(REG_CONTROL_2, rtc.readFromRegister(REG_CONTROL_2) | CTRL2_TIE);
    pinMode(RTC_INT_PIN, INPUT);        // external 10k pull-up (R7)
}

// Robust single-shot countdown wake in `seconds` - AUTO tick selection:
//   <= 4095 s : 1 Hz tick, 1 s granularity
//   >  4095 s : 1/60 Hz tick, preset in MINUTES (rounded), max 4095 min (~2.8 d).
void rtcSetNextWake(uint32_t seconds)
{
    uint32_t preset;
    uint8_t  tick;
    if (seconds < 1) seconds = 1;
    if (seconds <= 4095UL) {
        preset = seconds;
        tick   = CTRL1_TD_1HZ;
    } else {
        preset = (seconds + 30UL) / 60UL;           // round to nearest minute
        if (preset > 4095UL) preset = 4095UL;       // ~2.8 days hard max
        tick   = CTRL1_TD_1_60HZ;
    }
    rtc.writeToRegister(REG_CONTROL_1, rtc.readFromRegister(REG_CONTROL_1) & ~CTRL1_TE);
    rtc.writeToRegister(REG_TIMER_VALUE_0, preset & 0xFF);
    rtc.writeToRegister(REG_TIMER_VALUE_1, (preset >> 8) & 0x0F);
    rtc.writeToRegister(REG_STATUS, rtc.readFromRegister(REG_STATUS) & ~STATUS_TF);
    rtc.writeToRegister(REG_CONTROL_1, CTRL1_TE | tick);   // single-shot (no TRPT)
    DBG("[RTC] timer: preset=%lu %s\r\n", (unsigned long)preset,
        tick == CTRL1_TD_1HZ ? "s (1 Hz tick)" : "min (1/60 Hz tick)");
}

void rtcClearTF()
{
    rtc.writeToRegister(REG_STATUS, rtc.readFromRegister(REG_STATUS) & ~STATUS_TF);
}

// Unix seconds from UTC Y/M/D h:m:s, integer math only (H. Hinnant algorithm).
static uint32_t unixFromUtc(int Y, int M, int D, int h, int m, int s)
{
    int      y   = Y - (M <= 2 ? 1 : 0);
    int      era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long     days = (long)era * 146097 + (long)doe - 719468;

    return (uint32_t)(((days * 24L + h) * 60L + m) * 60L + s);
}

uint32_t rtcNowUnix()
{
    return unixFromUtc(rtc.getYear(), rtc.getMonth(), rtc.getDate(),
                       rtc.getHour(), rtc.getMinute(), rtc.getSecond());
}

// Full-range sanity on a UTC datetime (year window + every field in range).
static bool utcSane(int Y, int M, int D, int h, int m, int s)
{
    return Y >= MIN_VALID_YEAR && Y <= MAX_VALID_YEAR &&
           M >= 1 && M <= 12 && D >= 1 && D <= 31 &&
           h <= 23 && m <= 59 && s <= 59;
}

// Day of week (0=Sunday), Sakamoto.
static uint8_t dow(int y, int m, int d)
{
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    return (uint8_t)((y + y/4 - y/100 + y/400 + t[m-1] + d) % 7);
}

// Set the RV-3028 from a GPS UTC datetime. CORRECT Melopero arg order:
// setTime(year, month, WEEKDAY, DATE, hh, mm, ss) - weekday BEFORE date!
void rtcSetFromGps(int Y, int M, int D, int h, int m, int s)
{
    rtc.setTime((uint16_t)Y, (uint8_t)M, dow(Y, M, D), (uint8_t)D,
                (uint8_t)h, (uint8_t)m, (uint8_t)s);
    rtc.writeToRegister(USER_RAM1_ADDRESS, TIME_SET_FLAG);
    rtcSynced = true;
    DBG("[RTC] SET FROM GPS: %04d-%02d-%02d %02d:%02d:%02d UTC (unix~%lu)\r\n",
        Y, M, D, h, m, s, (unsigned long)rtcNowUnix());
}

// PRE-FIRST-FIX bootstrap only: if never synced, try to seed the RTC from the
// GPS time-before-fix. HARDENED (v3 caught a corrupted-but-parsed "2088-31-19"):
//   1) FULL range check.
//   2) TWO-SAMPLE CONFIRMATION: a 2nd sane reading >= TIME_CONFIRM_GAP_S later
//      must have advanced consistently with wall time (garbage doesn't tick).
// Once a real position fix lands, strategy C (rtcSetFromGps in gpsAcquire) owns
// the clock and this becomes a no-op (rtcSynced == true).
void maybeSeedRtc(TinyGPSPlus &g)
{
    static uint32_t candUnix = 0;   // first sane reading (candidate)
    static uint32_t candMs   = 0;

    if (rtcSynced) return;
    if (!(g.date.isValid() && g.time.isValid() &&
          g.date.age() < 2000 && g.time.age() < 2000)) return;

    int Y = g.date.year(), M = g.date.month(), D = g.date.day();
    int h = g.time.hour(), m = g.time.minute(), s = g.time.second();
    if (!utcSane(Y, M, D, h, m, s)) { candUnix = 0; return; }   // garbage: drop candidate

    uint32_t nowUnix = unixFromUtc(Y, M, D, h, m, s);
    if (candUnix == 0) { candUnix = nowUnix; candMs = millis(); return; }

    uint32_t wall_s = (millis() - candMs) / 1000UL;
    if (wall_s < TIME_CONFIRM_GAP_S) return;                    // wait for separation

    int32_t drift = (int32_t)(nowUnix - candUnix) - (int32_t)wall_s;
    if (drift < -TIME_CONFIRM_TOL_S || drift > TIME_CONFIRM_TOL_S) {
        candUnix = nowUnix; candMs = millis();                  // inconsistent: new candidate
        return;
    }
    rtcSetFromGps(Y, M, D, h, m, s);                            // confirmed: seed from 2nd reading
    candUnix = 0;
}

// ============================================================================
// DEEP SLEEP - dynamic backstop: requested sleep + margin (a fixed backstop
// would cut a multi-hour RTC sleep short; the backstop must only catch a MISSED
// INT). Arms the RV-3028 countdown as the wake source.
// ============================================================================
void deepSleep(uint32_t seconds)
{
    DBG("[SLEEP] %lu s\r\n", (unsigned long)seconds);
    rtcSetNextWake(seconds);
    rtcWokeUs = false;
    clearFPU();
    battPinDisconnect();   // v7: kill the AIN7 input-buffer crowbar before the long sleep
    accelPinsPark();       // v9: park P0.24/P0.25 (no MCU current via the accel pull-ups)
    uint32_t backstop = seconds * 1000UL + SLEEP_BACKSTOP_MARGIN_MS;
    uint32_t before = millis();
    api.system.sleep.all(backstop);
    uint32_t slept  = millis() - before;
    rtcClearTF();
    DBG("[WAKE] %s after ~%lu ms\r\n",
        rtcWokeUs ? "RTC P0.21" : "other/backstop", (unsigned long)slept);
}

// SHORT nap between TX pulses (#5). Does NOT arm the RV-3028 countdown - it just
// sleeps at the battery floor for `seconds` and wakes on the sleep backstop, so
// the inter-pulse gap costs almost no energy. Radio is already idle here.
void napSleep(uint32_t seconds)
{
    DBG("[GAP] %lu s at floor (ACK-window guard)\r\n", (unsigned long)seconds);
    clearFPU();
    battPinDisconnect();   // v7: same crowbar fix for the inter-pulse nap
    accelPinsPark();       // v9: park the accel I2C lines for the nap too
    api.system.sleep.all(seconds * 1000UL);
}

// ============================================================================
// v8 REBOOT-TO-SLEEP (only used after a BLE/fix cycle - see SleepCmd comment).
//   queueSleepAndReboot(): persist the intended sleep, then reset. The reset is
//   what fully clears the BLE/SoftDevice state that would otherwise wedge sleep.
//   doQueuedSleepIfAny(): at boot, if a sleep was queued, clear it and perform it
//   in a clean (BLE never started this session) state - identical to v7 sleep.
// ============================================================================
// Persist the intended sleep to flash WITHOUT rebooting. Used to pre-arm recovery
// before a risky BLE offload: if that hangs and the board is reset, the next boot
// finds this and sleeps cleanly instead of re-attempting the offload.
void queueSleepCmd(uint32_t seconds)
{
    SleepCmd sc = { SLEEPCMD_MAGIC, seconds };
    flashWrite(FLASH_OFF_SLEEPCMD, &sc, sizeof(sc));
}

void queueSleepAndReboot(uint32_t seconds)
{
    queueSleepCmd(seconds);
    DBG("== REBOOT-to-sleep: queued %lu s, resetting to clear BLE ==\r\n",
        (unsigned long)seconds);
    delay(50);
    api.system.reboot();
    // never returns
}

void doQueuedSleepIfAny()
{
    SleepCmd sc;
    flashRead(FLASH_OFF_SLEEPCMD, &sc, sizeof(sc));
    if (sc.magic != SLEEPCMD_MAGIC) return;
    // Clear FIRST so a reset during the sleep can't relatch it into a sleep loop.
    sc.magic = 0;
    flashWrite(FLASH_OFF_SLEEPCMD, &sc, sizeof(sc));
    DBG("== BOOT: performing queued %lu s deep-sleep (BLE-clean) ==\r\n",
        (unsigned long)sc.seconds);
    deepSleep(sc.seconds);
}

// ============================================================================
// BATTERY (AIN7/P0.31, 1M/1M + C17) - CALIBRATED (test #2 final; ratio 2.0).
// ============================================================================
static int cmp_int(const void *a, const void *b) { return (*(const int *)a) - (*(const int *)b); }

static bool saadcWaitEvt(volatile uint32_t *evt) {
    for (uint32_t n = 0; n < 2000000UL; n++) { if (*evt) { *evt = 0; return true; } }
    return false;
}

// v7 FIX (the big one): keep P0.31's DIGITAL input buffer DISCONNECTED. The AIN7
// pin floats at the 1M/1M divider midpoint (~VDD/2); a *connected* input buffer
// there conducts ~118 uA of shoot-through ("crowbar") current - which was the
// entire unexplained sleep-floor overage. The deep-sleep teardown proved it:
// parking this one pin as INPUT moved the floor 34 -> 152 uA; leaving it
// disconnected drops it back to ~34 uA. The SAADC reads the analog voltage through
// its own mux, so analogRead()/battery reading is UNAFFECTED. Nordic explicitly
// recommends leaving ADC pins disconnected for exactly this reason.
static inline void battPinDisconnect()
{
    NRF_P0->PIN_CNF[31] = 0x00000002UL;   // P0.31 (AIN7): DIR=Input, INPUT=Disconnect, no pull
}

// v9: park the bit-bang I2C lines (P0.24 SDA / P0.25 SCL) as INPUT with the input
// buffer DISCONNECTED before sleep, so the MCU sources/sinks NO current on them
// (the external module pull-ups just hold them high). Same idea as the AIN7 fix -
// guarantees the MCU adds nothing on these pins during the long idle sleep.
static inline void accelPinsPark()
{
    NRF_P0->PIN_CNF[24] = 0x00000002UL;   // P0.24 (SDA)
    NRF_P0->PIN_CNF[25] = 0x00000002UL;   // P0.25 (SCL)
}

void batteryAdcInit()
{
    NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos;
    NRF_SAADC->CH[0].CONFIG =
        (SAADC_CH_CONFIG_GAIN_Gain1_6    << SAADC_CH_CONFIG_GAIN_Pos)   |
        (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
        (SAADC_CH_CONFIG_TACQ_40us       << SAADC_CH_CONFIG_TACQ_Pos)   |
        (SAADC_CH_CONFIG_MODE_SE         << SAADC_CH_CONFIG_MODE_Pos);
    NRF_SAADC->EVENTS_CALIBRATEDONE = 0;
    NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
    saadcWaitEvt(&NRF_SAADC->EVENTS_CALIBRATEDONE);
    NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos;

    // v7: do NOT pinMode(BATT_ADC_PIN, INPUT) - that connected input buffer is the
    // ~118 uA leak. Leave it disconnected; analogRead() still samples via the SAADC.
    battPinDisconnect();
    analogReadResolution(12);
    analogReference(AR_INTERNAL);
}

uint16_t readVbat_mV()
{
    int v[ADC_SAMPLES]; int m = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) { v[m++] = analogRead(BATT_ADC_PIN); delay(2); }
    qsort(v, m, sizeof(int), cmp_int);
    uint16_t raw = v[m/2];
    battPinDisconnect();   // v7: re-disconnect the input buffer analogRead may have connected
    return (uint16_t)(((uint32_t)raw * VBAT_CAL_NUM) / VBAT_CAL_DEN);
}

// ============================================================================
// GPS - Serial0/UART1, EN=P1.02 active-low. Isolation teardown (test #6).
// ============================================================================
void gpsPowerOn()
{
    pinMode(GPS_EN_PIN, OUTPUT);
    digitalWrite(GPS_EN_PIN, LOW);          // active-low: GPS ON
    delay(GPS_BOOT_DELAY_MS);
    Serial0.begin(GPS_BAUD, RAK_CUSTOM_MODE);
    say("[GPS] VCC ON (P1.02 LOW, hot start via module backup battery)");
}

void gpsPowerOff()
{
    digitalWrite(GPS_EN_PIN, HIGH);                                       // cut power first
    delay(GPS_POWERDOWN_MS);                                              // TX line quiets
    Serial0.end();                                                        // release UART1
    pinMode(GPS_UART_RX_PIN, OUTPUT); digitalWrite(GPS_UART_RX_PIN, LOW); // isolate module
    pinMode(GPS_UART_TX_PIN, OUTPUT); digitalWrite(GPS_UART_TX_PIN, LOW);
    say("[GPS] VCC OFF, UART pins driven LOW (module isolated)");
}

// Boot-time TIME-ONLY GPS session (no position fix required). Returns true if
// the RTC got seeded. On failure the node proceeds; COLLECT sessions re-try.
bool bootTimeSync()
{
    DBG("[TIME] boot GPS time-sync (up to %lu s, no fix needed)...\r\n",
        (unsigned long)(BOOT_TIME_SYNC_MS / SECONDS));
    gpsPowerOn();
    TinyGPSPlus g;
    uint32_t start = millis(), lastLog = 0;
    while ((millis() - start) < BOOT_TIME_SYNC_MS && !rtcSynced) {
        while (Serial0.available()) g.encode(Serial0.read());
        maybeSeedRtc(g);
        if (Serial && (millis() - lastLog) >= 5000) {
            lastLog = millis();
            DBG("   ...%lus  sats=%d  timeValid=%d year=%d\r\n",
                (unsigned long)((millis() - start) / 1000),
                g.satellites.isValid() ? (int)g.satellites.value() : 0,
                g.time.isValid() ? 1 : 0,
                g.date.isValid() ? (int)g.date.year() : 0);
        }
    }
    gpsPowerOff();
    if (!rtcSynced) say("[TIME] no GPS time yet - will keep trying in COLLECT sessions");
    return rtcSynced;
}

// STRATEGY A + C: adaptive, SV-gated acquisition. Watches satellites-in-view
// live; aborts early with no sky, extends when sky is present, and RE-SYNCS the
// RTC from the fix's genuine GPS time the moment a fix settles.
// v14: budgets are passed in so tryOneFix can pick a WARM (short, frugal) or COLD
// (long, patient, no early abort) window. noSkyMs == maxMs means "never no-sky-abort".
void gpsAcquire(FixResult &r, uint32_t noSkyMs, uint32_t maxMs)
{
    memset(&r, 0, sizeof(r));

    TinyGPSPlus gps;
    // satellites-IN-VIEW per constellation (GSV field 3 = total in view).
    // atoi("") == 0, so unseen talkers contribute 0. Sum = total in view.
    TinyGPSCustom gpV(gps, "GPGSV", 3), bdV(gps, "BDGSV", 3),
                  glV(gps, "GLGSV", 3), gaV(gps, "GAGSV", 3);
    uint32_t start   = millis();
    uint32_t firstAt = 0;
    bool     validSeen = false;

    while ((millis() - start) < maxMs) {
        while (Serial0.available()) gps.encode(Serial0.read());

        maybeSeedRtc(gps);   // pre-first-fix time bootstrap only (no-op once synced)

        uint16_t inview = (uint16_t)(atoi(gpV.value()) + atoi(bdV.value()) +
                                     atoi(glV.value()) + atoi(gaV.value()));
        if (inview > r.peakInView) r.peakInView = inview;

        bool ok = gps.location.isValid() && gps.location.age() < 2000 &&
                  gps.date.isValid()     && gps.time.isValid();
        if (ok) {
            if (!validSeen) { validSeen = true; firstAt = millis(); }
            if ((millis() - firstAt) >= POST_FIX_SETTLE_MS) {
                r.haveFix = true;
                r.latE6   = (int32_t)(gps.location.lat() * 1e6);
                r.lonE6   = (int32_t)(gps.location.lng() * 1e6);
                r.sats    = gps.satellites.isValid() ? gps.satellites.value() : 0;
                r.ttff_ms = millis() - start;
                // STRATEGY C: a position fix => genuine GPS time. Re-sync the RTC
                // (corrects drift, fixes the 11-day bug) and take the packet
                // timestamp straight from this fix.
                int Y = gps.date.year(),  M = gps.date.month(),  D = gps.date.day();
                int h = gps.time.hour(),  mn = gps.time.minute(), s = gps.time.second();
                if (utcSane(Y, M, D, h, mn, s)) {
                    rtcSetFromGps(Y, M, D, h, mn, s);
                    r.fixUnix = unixFromUtc(Y, M, D, h, mn, s);
                } else {
                    r.fixUnix = rtcNowUnix();   // shouldn't happen on a real fix
                }
                return;
            }
        } else if (validSeen) {
            validSeen = false;
        }

        // STRATEGY A: no-sky early abort. If SV_MIN sats never appeared by the
        // (budget-dependent) window, there's no sky - stop now and save the GPS burst.
        // On a COLD fix noSkyMs == maxMs, so this never fires early (patient).
        if ((millis() - start) >= noSkyMs && r.peakInView < SV_MIN) {
            r.noSky   = true;
            r.ttff_ms = millis() - start;
            return;
        }
    }
    // Timed out WITH satellites visible but no fix (weak/marginal sky).
    r.ttff_ms = millis() - start;
}

void tryOneFix(FixResult &r)
{
#if SIMULATE_FIX
    // BENCH: fabricate a fix without powering the GPS (no sky needed) so the
    // delivery guarantee (#5) and strategy-C re-sync can be exercised indoors.
    memset(&r, 0, sizeof(r));
    r.haveFix    = true;
    r.latE6      = SIM_LAT_E6;
    r.lonE6      = SIM_LON_E6;
    r.sats       = SIM_SV;
    r.peakInView = SIM_SV;
    r.ttff_ms    = 1234;
    if (!rtcSynced) {                       // seed once so timestamps advance sensibly
        rtcSetFromGps(SIM_UTC_Y, SIM_UTC_MO, SIM_UTC_D, SIM_UTC_H, SIM_UTC_MI, SIM_UTC_S);
    }
    r.fixUnix = rtcNowUnix();
    g_lastTTFF_s = 1; coldStreak = 0; g_cellStatus = 0;   // v15: sim = healthy/hot
    DBG("[GPS] SIMULATED FIX  used=%u  IN-VIEW=%u  lat=%ld lon=%ld  ts=%lu\r\n",
        r.sats, r.peakInView, (long)r.latE6, (long)r.lonE6, (unsigned long)r.fixUnix);
    return;
#endif
    // v14 COLD / FIRST-FIX decision. Cold => a long, patient window with NO early
    // no-sky abort (noSkyMs == maxMs); warm => today's frugal 25 s / 120 s budget.
    uint32_t nowU     = rtcSynced ? rtcNowUnix() : 0;
    bool     everFix  = (header.lastFixUnix != 0);
    bool     stale    = rtcSynced && everFix &&
                        ((uint32_t)(nowU - header.lastFixUnix) >= EPHEMERIS_STALE_SEC);
    bool     coldFix  = (!rtcSynced)                           // power lost / fresh flash / first ever
                     || (!everFix)                             // never had a POSITION fix
                     || stale                                  // long gap / NEW CITY (battery kept)
                     || (consecutiveNoFix >= COLD_AFTER_NOFIX);// just left a no-sky stretch (in transit)
    uint32_t noSkyMs  = coldFix ? COLD_FIX_MAX_MS : NO_SKY_ABORT_MS;  // cold: no early abort
    uint32_t maxMs    = coldFix ? COLD_FIX_MAX_MS : FIX_MAX_MS;
    const char *why   = !rtcSynced ? "power-lost/first"
                      : !everFix   ? "never-fixed"
                      : stale      ? "stale/new-city"
                      : (consecutiveNoFix >= COLD_AFTER_NOFIX) ? "no-fix-streak" : "";
    DBG("[GPS] %s-fix budget (no-sky<%lus, max<%lus)%s%s  [rtcSynced=%d everFix=%d noFixStreak=%lu]\r\n",
        coldFix ? "COLD" : "warm",
        (unsigned long)(noSkyMs / SECONDS), (unsigned long)(maxMs / SECONDS),
        coldFix ? "  reason=" : "", why,
        rtcSynced ? 1 : 0, everFix ? 1 : 0, (unsigned long)consecutiveNoFix);

    gpsPowerOn();
    uint32_t gpsOnAt = millis();               // v15: total GPS-on clock (for the charge-hold)
    gpsAcquire(r, noSkyMs, maxMs);
    DBG("[GPS] %s in %lu ms  used=%u  IN-VIEW peak=%u  %s  lat=%ld lon=%ld\r\n",
        r.haveFix ? "FIX" : "no fix",
        (unsigned long)r.ttff_ms, r.sats, r.peakInView,
        r.haveFix ? "" : (r.noSky ? "(no-sky abort)" : "(sky, no fix)"),
        (long)r.latE6, (long)r.lonE6);

    // ---- v15: GPS backup-cell HEALTH + CHARGE-ON-COLD ----
    // A fast fix means the MS621FE is holding ephemeris (HOT). A slow/missed fix means
    // it's low; keep the GPS ON to GPS_CHARGE_SEC total to recharge it (the patient
    // window IS the charge) - unless it stays non-hot for CELL_DEAD_AFTER cycles, in
    // which case the cell can't hold -> flag DEAD and stop the futile charge.
    g_lastTTFF_s = r.haveFix ? (uint16_t)((r.ttff_ms + 500) / 1000)
                             : (uint16_t)((millis() - gpsOnAt) / 1000);
    bool hot = r.haveFix && (r.ttff_ms < HOT_TTFF_MS);
    if (hot) coldStreak = 0;
    else if (coldStreak < 60000UL) coldStreak++;
    bool cellDead = (coldStreak >= CELL_DEAD_AFTER);
    g_cellStatus = hot ? 0 : (cellDead ? 2 : 1);          // 0=OK 1=LOW 2=DEAD

    if (!hot && !cellDead) {                               // cell low but chargeable -> charge it
        if ((millis() - gpsOnAt) < GPS_CHARGE_MS)
            DBG("[GPS] cell LOW (ttff=%us) -> charging to %lus total on...\r\n",
                (unsigned)g_lastTTFF_s, (unsigned long)GPS_CHARGE_SEC);
        while ((millis() - gpsOnAt) < GPS_CHARGE_MS) {
            while (Serial0.available()) Serial0.read();    // drain NMEA (discard) so the UART FIFO stays clean
            delay(20);
        }
    }
    DBG("[GPS] CELL=%s  lastTTFF=%us  coldStreak=%lu  (gps-on %lus)\r\n",
        g_cellStatus == 0 ? "OK" : (g_cellStatus == 1 ? "LOW" : "DEAD"),
        (unsigned)g_lastTTFF_s, (unsigned long)coldStreak,
        (unsigned long)((millis() - gpsOnAt) / 1000));
    gpsPowerOff();
}

// ============================================================================
// LoRa P2P - TX + ACK RX (unchanged)
// ============================================================================
void loraConfigureOnce()
{
    if (api.lora.nwm.get() != 0) {
        say("[LORA] switching to P2P and rebooting...");
        api.lora.nwm.set();
        api.system.reboot();
    }
    api.lora.pfreq.set(LORA_FREQ_HZ);
    api.lora.psf.set(LORA_SF);
    api.lora.pbw.set(LORA_BW);
    api.lora.pcr.set(LORA_CR);
    api.lora.ppl.set(LORA_PREAMBLE);
    api.lora.ptp.set(LORA_TX_POWER_DBM);
    api.lora.registerPSendCallback(onTxDone);
    api.lora.registerPRecvCallback(onRxDone);
}

int formatPacket(const PacketSlot &p, char *buf, int bufSize)
{
    int32_t la = p.hasFix ? p.latE6 : 0;
    int32_t lo = p.hasFix ? p.lonE6 : 0;
    const char *latSign = (la < 0) ? "-" : "";
    const char *lonSign = (lo < 0) ? "-" : "";
    uint32_t laA = (la < 0) ? (uint32_t)(-la) : (uint32_t)la;
    uint32_t loA = (lo < 0) ? (uint32_t)(-lo) : (uint32_t)lo;
    uint16_t vInt = p.vbat_mV / 1000;
    uint16_t vCen = (p.vbat_mV / 10) % 100;
    // v15: append TTFF (s) + GPS backup-cell health so it is visible in the LoRa
    // telemetry (the repeater relays the raw string). OK=hot cell, LOW=charging,
    // DEAD=cell won't hold -> replace/check it.
    const char *cell = (g_cellStatus == 0) ? "OK" : (g_cellStatus == 1) ? "LOW" : "DEAD";
    return snprintf(buf, bufSize,
                    "%03u,%lu,%s%lu.%06lu,%s%lu.%06lu,%u.%02u,%lu,SV=%lu,TTFF=%u,CELL=%s",
                    (unsigned)DEVICE_ID,
                    (unsigned long)p.seq,
                    latSign, (unsigned long)(laA / 1000000UL), (unsigned long)(laA % 1000000UL),
                    lonSign, (unsigned long)(loA / 1000000UL), (unsigned long)(loA % 1000000UL),
                    vInt, vCen,
                    (unsigned long)p.timestamp,
                    (unsigned long)p.satsInView,
                    (unsigned)g_lastTTFF_s, cell);
}

bool loraSend(const uint8_t *payload, uint8_t len)
{
    txDone = false;
    if (!api.lora.psend(len, (uint8_t *)payload)) {
        say("[TX] psend queue FAILED");
        return false;
    }
    uint32_t start = millis();
    while (!txDone && (millis() - start) < 5000UL) delay(2);
    if (!txDone) { say("[TX] tx-done timeout"); return false; }
    return true;
}

bool loraWaitAck(uint32_t seq)
{
    rxDone = false;
    rxLen  = 0;
    if (!api.lora.precv(ACK_TIMEOUT_MS)) {
        say("[RX] precv FAILED");
        return false;
    }
    uint32_t start = millis();
    while (!rxDone && (millis() - start) < (ACK_TIMEOUT_MS + 1000UL)) delay(2);
    api.lora.precv(0);
    if (!rxDone) return false;

    char tmp[257];
    uint8_t n = rxLen < 256 ? rxLen : 256;
    memcpy(tmp, rxBuf, n); tmp[n] = '\0';
    unsigned dev = 0; unsigned long ackSeq = 0;
    if (sscanf(tmp, "ACK,%u,%lu", &dev, &ackSeq) != 2) {
        DBG("[RX] not-ACK (\"%s\")\r\n", tmp);
        return false;
    }
    if (dev != DEVICE_ID || ackSeq != seq) {
        DBG("[RX] mismatch (dev=%u seq=%lu vs expected dev=%u seq=%lu)\r\n",
            dev, ackSeq, (unsigned)DEVICE_ID, (unsigned long)seq);
        return false;
    }
    DBG("[RX] ACK OK  RSSI=%d dBm  SNR=%d dB\r\n", (int)rxRssi, (int)rxSnr);
    return true;
}

// Send one packet and wait for its ACK. Returns true only on a matched ACK.
bool trySendPacket(PacketSlot &p)
{
    char payload[128];
    int len = formatPacket(p, payload, sizeof(payload));
    DBG("[TX] seq=%lu (%s): %s\r\n",
        (unsigned long)p.seq, p.hasFix ? "fix" : "no-fix", payload);
    if (!loraSend((uint8_t *)payload, (uint8_t)len)) return false;
    p.txAttempts++;
    return loraWaitAck(p.seq);
}

void markDelivered(const PacketSlot &p)
{
    header.deliveredCount++;
    header.lastDeliveredSeq = p.seq;
}

// ============================================================================
// PENDING backlog (#5) - newest-first stack of un-ACK'd REAL fixes.
// ============================================================================
void pendingPushNewest(const PacketSlot &p)
{
    // Full: roll the OLDEST fix off to the flash archive (physical bound; we
    // keep the NEWEST fixes because recent position matters most).
    if (header.pendingCount >= PENDING_SLOTS) {
        appendUndelivered(pending[header.pendingCount - 1]);
        header.pendingCount = PENDING_SLOTS - 1;
    }
    for (int i = header.pendingCount; i > 0; i--) pending[i] = pending[i - 1];
    pending[0] = p;
    header.pendingCount++;
}

void pendingRemoveFront()
{
    for (uint8_t i = 0; i + 1 < header.pendingCount; i++) pending[i] = pending[i + 1];
    if (header.pendingCount) header.pendingCount--;
}

// ============================================================================
// v8 - LIS3DHTR accelerometer over BIT-BANG I2C (P0.24/P0.25)
//   Ported verbatim in behaviour from the committed tests/Accelerometer emitter
//   (pinMode/digitalWrite/delayMicroseconds are all available under RUI3).
// ============================================================================
#if ENABLE_ACCEL_BLE
static void accI2cStart()
{
    pinMode(ACCEL_SDA_PIN, OUTPUT); pinMode(ACCEL_SCL_PIN, OUTPUT);
    digitalWrite(ACCEL_SDA_PIN, HIGH); digitalWrite(ACCEL_SCL_PIN, HIGH);
    delayMicroseconds(ACCEL_I2C_DELAY_US);
    digitalWrite(ACCEL_SDA_PIN, LOW);  delayMicroseconds(ACCEL_I2C_DELAY_US);
    digitalWrite(ACCEL_SCL_PIN, LOW);  delayMicroseconds(ACCEL_I2C_DELAY_US);
}
static void accI2cStop()
{
    pinMode(ACCEL_SDA_PIN, OUTPUT);
    digitalWrite(ACCEL_SDA_PIN, LOW);  digitalWrite(ACCEL_SCL_PIN, HIGH);
    delayMicroseconds(ACCEL_I2C_DELAY_US);
    digitalWrite(ACCEL_SDA_PIN, HIGH); delayMicroseconds(ACCEL_I2C_DELAY_US);
}
static void accI2cWrite(uint8_t b)
{
    pinMode(ACCEL_SDA_PIN, OUTPUT);
    for (int i = 7; i >= 0; i--) {
        digitalWrite(ACCEL_SDA_PIN, (b >> i) & 1); delayMicroseconds(ACCEL_I2C_DELAY_US);
        digitalWrite(ACCEL_SCL_PIN, HIGH);         delayMicroseconds(ACCEL_I2C_DELAY_US);
        digitalWrite(ACCEL_SCL_PIN, LOW);          delayMicroseconds(ACCEL_I2C_DELAY_US);
    }
    pinMode(ACCEL_SDA_PIN, INPUT);                 // ACK clock pulse
    digitalWrite(ACCEL_SCL_PIN, HIGH); delayMicroseconds(ACCEL_I2C_DELAY_US);
    digitalWrite(ACCEL_SCL_PIN, LOW);  delayMicroseconds(ACCEL_I2C_DELAY_US);
    pinMode(ACCEL_SDA_PIN, OUTPUT);
}
static uint8_t accI2cRead(bool ack)
{
    uint8_t b = 0; pinMode(ACCEL_SDA_PIN, INPUT);
    for (int i = 7; i >= 0; i--) {
        digitalWrite(ACCEL_SCL_PIN, HIGH); delayMicroseconds(ACCEL_I2C_DELAY_US);
        b = (b << 1) | digitalRead(ACCEL_SDA_PIN);
        digitalWrite(ACCEL_SCL_PIN, LOW);  delayMicroseconds(ACCEL_I2C_DELAY_US);
    }
    pinMode(ACCEL_SDA_PIN, OUTPUT);
    digitalWrite(ACCEL_SDA_PIN, ack ? LOW : HIGH); delayMicroseconds(ACCEL_I2C_DELAY_US);
    digitalWrite(ACCEL_SCL_PIN, HIGH);             delayMicroseconds(ACCEL_I2C_DELAY_US);
    digitalWrite(ACCEL_SCL_PIN, LOW);              delayMicroseconds(ACCEL_I2C_DELAY_US);
    return b;
}
static uint8_t lisReadReg(uint8_t reg)
{
    accI2cStart(); accI2cWrite((LIS_ADDR << 1) | 0); accI2cWrite(reg);
    accI2cStart(); accI2cWrite((LIS_ADDR << 1) | 1);
    uint8_t v = accI2cRead(false); accI2cStop(); return v;
}
static void lisWriteReg(uint8_t reg, uint8_t val)
{
    accI2cStart(); accI2cWrite((LIS_ADDR << 1) | 0); accI2cWrite(reg); accI2cWrite(val); accI2cStop();
}
static bool lisInit()
{
    // idle the bus HIGH and let the sensor settle, then retry WHO_AM_I (a single
    // first-transaction read on a floating bus is unreliable).
    pinMode(ACCEL_SDA_PIN, OUTPUT); pinMode(ACCEL_SCL_PIN, OUTPUT);
    digitalWrite(ACCEL_SDA_PIN, HIGH); digitalWrite(ACCEL_SCL_PIN, HIGH);
    delay(20);
    bool found = false;
    for (int t = 0; t < 10 && !found; t++) {
        if (lisReadReg(LIS_REG_WHOAMI) == 0x33) found = true; else delay(20);
    }
    if (!found) return false;
    lisWriteReg(LIS_REG_CTRL1, 0x57);   // 100 Hz, normal mode, X/Y/Z enabled
    lisWriteReg(LIS_REG_CTRL4, 0x08);   // high resolution, +-2 g
    delay(10);
    return true;
}
static void lisReadXYZ(int16_t &x, int16_t &y, int16_t &z)
{
    uint8_t d[6];
    accI2cStart(); accI2cWrite((LIS_ADDR << 1) | 0); accI2cWrite(LIS_REG_OUT_X_L | 0x80);
    accI2cStart(); accI2cWrite((LIS_ADDR << 1) | 1);
    for (int i = 0; i < 6; i++) d[i] = accI2cRead(i < 5);
    accI2cStop();
    x = (int16_t)((d[1] << 8) | d[0]);
    y = (int16_t)((d[3] << 8) | d[2]);
    z = (int16_t)((d[5] << 8) | d[4]);
}

// ---------------------------------------------------------------------------
// Accel flash ring (fixed-size records; head = oldest, wraps when full).
// ---------------------------------------------------------------------------
void accelRingLoad()
{
    flashRead(FLASH_OFF_ACCEL_META, &accelMeta, sizeof(accelMeta));
    if (accelMeta.magic != ACCEL_META_MAGIC) {
        memset(&accelMeta, 0, sizeof(accelMeta));
        accelMeta.magic   = ACCEL_META_MAGIC;
        accelMeta.nextSeq = 1;
        flashWrite(FLASH_OFF_ACCEL_META, &accelMeta, sizeof(accelMeta));
        say("[RING] fresh accel ring");
    }
}
void accelRingSaveMeta() { flashWrite(FLASH_OFF_ACCEL_META, &accelMeta, sizeof(accelMeta)); }

void accelRingClear()
{
    accelMeta.head = 0; accelMeta.count = 0;
    accelRingSaveMeta();
    DBG("[RING] cleared (delivered)\r\n");
}

// Append accelRec (seq/ts/count/xyz already filled). Overwrites the oldest when full.
void accelRingAppend()
{
    uint16_t slot;
    if (accelMeta.count < ACCEL_RING_RECORDS) {
        slot = (accelMeta.head + accelMeta.count) % ACCEL_RING_RECORDS;
        accelMeta.count++;
    } else {
        slot = accelMeta.head;                                   // full: drop oldest
        accelMeta.head = (accelMeta.head + 1) % ACCEL_RING_RECORDS;
    }
    accelRec.seq = accelMeta.nextSeq++;
    uint32_t off = FLASH_OFF_ACCEL_DATA + (uint32_t)slot * sizeof(AccelRecord);
    bool ok = flashWriteLong(off, &accelRec, sizeof(AccelRecord));
    accelRingSaveMeta();
    DBG("[RING] stored seq=%lu ts=%lu n=%u -> slot %u (%u/%u)%s\r\n",
        (unsigned long)accelRec.seq, (unsigned long)accelRec.ts, accelRec.count,
        slot, accelMeta.count, ACCEL_RING_RECORDS, ok ? "" : "  [FLASH WRITE FAILED]");
}

// Read the logical record (0 = oldest) into accelRec. Returns false if out of range.
bool accelRingGet(uint16_t logical)
{
    if (logical >= accelMeta.count) return false;
    uint16_t slot = (accelMeta.head + logical) % ACCEL_RING_RECORDS;
    uint32_t off  = FLASH_OFF_ACCEL_DATA + (uint32_t)slot * sizeof(AccelRecord);
    return flashReadLong(off, &accelRec, sizeof(AccelRecord));
}

// ---------------------------------------------------------------------------
// Collect one 10 s record and append it to the ring.
// ---------------------------------------------------------------------------
void collectAccelToRing(uint32_t ts)
{
    if (ACCEL_PWR_PIN >= 0) {                     // v9: power the module rail on for the collect
        pinMode(ACCEL_PWR_PIN, OUTPUT);
        digitalWrite(ACCEL_PWR_PIN, HIGH);
        delay(ACCEL_PWR_BOOT_MS);
    }
    if (!lisInit()) {
        say("[ACCEL] LIS3DHTR not found (WHO_AM_I != 0x33) - skipping");
        if (ACCEL_PWR_PIN >= 0) digitalWrite(ACCEL_PWR_PIN, LOW);
        accelPinsPark();
        return;
    }
    memset(&accelRec, 0, sizeof(accelRec));
    accelRec.ts = ts;
    const uint32_t periodMs = 1000UL / ACCEL_HZ;
    uint16_t i = 0;
    uint32_t t0 = millis();
    while (i < ACCEL_SAMPLES_PER_REC) {
        uint32_t due = millis();
        int16_t x, y, z;
        lisReadXYZ(x, y, z);
        accelRec.xyz[i][0] = x; accelRec.xyz[i][1] = y; accelRec.xyz[i][2] = z;
        i++;
        while (millis() - due < periodMs) { /* pace to ACCEL_HZ */ }
    }
    accelRec.count = i;
    DBG("[ACCEL] %u samples in %lu ms\r\n", i, (unsigned long)(millis() - t0));
    // v9 POWER: put the LIS3DH into power-down (CTRL1=0x00, ~0.5 uA vs ~11 uA at
    // 100 Hz) and park the I2C lines - so the sensor adds ~nothing during the long
    // sleep that follows. (The Grove MODULE's own quiescent draw - LED/regulator/
    // pull-ups - is a HARDWARE item; see the README power note.)
    lisWriteReg(LIS_REG_CTRL1, 0x00);
    if (ACCEL_PWR_PIN >= 0) digitalWrite(ACCEL_PWR_PIN, LOW);   // v9: cut the module rail
    accelPinsPark();
    accelRingAppend();
}

// ============================================================================
// BLE drone offload - v10's PROVEN no-freeze transport + v11 data-integrity fix.
// RUI3 `api.ble.uart` (NUS), FIRE-AND-FORGET:
//   1) advertise by NAME (api.ble.uart advertises the name, not the service UUID
//      -> the receiver matches by name "Custodia-Tracker");
//   2) wait a FIXED BLE_SETTLE_MS for a central to connect + subscribe to TXD;
//   3) blindly `api.ble.uart.write()` each text line, paced BLE_LINE_GAP_MS apart;
//   4) hold BLE_HOLD_MS, then reboot-to-sleep (which is what clears BLE).
// No connect/subscribe callback, no notify()-on-a-live-link, no handshake -> the
// whole stage is a bounded sum of delays and CANNOT freeze. We never api.ble.stop()
// (a stop while connected re-advertises and the reconnect can wedge the next sleep).
//
// v11 protocol (every line <= 20 B so it fits in ONE NUS notification -> a dropped
// packet loses a whole line, never corrupts a neighbour; the v10 30-byte header
// spanned 2 notifications and its 2nd half (count+checksum) was routinely dropped):
//   "I <dev> <nrecs>"          opening announce (DEVICE_ID sent here, once)
//   "R <seq> <n> <ck16>"       per-record header: seq, sample-count, 16-bit checksum
//   "T <ts>"                   v13: per-record unix timestamp (its own <=20 B line)
//   "<x>,<y>,<z>"              a sample
//   "E <dev> <nrecs>"          end
// The whole ring is blasted BLE_BLAST_REPEATS times for loss resilience; the
// receiver dedups by seq so repeats cost nothing but a second chance.
// ============================================================================

// Stream one newline-terminated text line over NUS, paced. api.ble.uart.write is
// non-blocking wrt the link (fire-and-forget), so this can never hang.
static void bleWriteLine(const char *line, int n)
{
    api.ble.uart.write((uint8_t *)line, n);
    delay(BLE_LINE_GAP_MS);
}

// Blast the whole ring one-directionally (fire-and-forget). Always returns true
// (a blast was attempted); with no ACK we cannot know if the drone got it.
bool bleDroneOffload()
{
    bleUsed = true;                              // -> loop() reboots-to-sleep after this
    uint16_t nRecs = accelMeta.count;

    char nm[] = BLE_BROADCAST_NAME;
    api.ble.settings.broadcastName.set(nm, sizeof(nm) - 1);
    api.ble.uart.start(0);                       // start NUS + advertise (name only)
    DBG("[BLE] api.ble.uart advertising '%s', %u records x%u, settling %lu ms (fire-and-forget)...\r\n",
        nm, nRecs, (unsigned)BLE_BLAST_REPEATS, (unsigned long)BLE_SETTLE_MS);

    // No connect callback in RUI3 api.ble.uart: give the central a fixed window to
    // scan, connect, and enable TXD notifications before we stream.
    delay(BLE_SETTLE_MS);

    uint32_t streamStart = millis();
    char line[40];
    int  n;
    bool capped = false;

    for (uint8_t rep = 0; rep < BLE_BLAST_REPEATS && !capped; rep++) {
        // opening announce: who I am + how many records (repeated each pass)
        n = snprintf(line, sizeof(line), "I %u %u\n", (unsigned)DEVICE_ID, nRecs);
        bleWriteLine(line, n);

        for (uint16_t r = 0; r < nRecs; r++) {
            // Safety cap on the whole blast (pure delay-bounded; still can't hang).
            if ((int32_t)(millis() - streamStart) > (int32_t)BLE_STREAM_MAX_MS) {
                DBG("[BLE] stream cap hit (rep %u, rec %u)\r\n", rep, r);
                capped = true; break;
            }
            if (!accelRingGet(r)) continue;
            // 16-bit checksum: keeps the header <= 20 B (one notification). The
            // receiver masks its own running sum to 16 bits and compares.
            uint16_t ck = 0;
            for (uint16_t i = 0; i < accelRec.count; i++)
                ck += (uint16_t)accelRec.xyz[i][0] + (uint16_t)accelRec.xyz[i][1] + (uint16_t)accelRec.xyz[i][2];
            n = snprintf(line, sizeof(line), "R %lu %u %u\n",
                         (unsigned long)accelRec.seq, accelRec.count, (unsigned)ck);
            bleWriteLine(line, n);
            // v13: per-record absolute TIMESTAMP as its OWN short line (fits one
            // notification: "T 1784059200\n" = 13 B). This restores the time
            // reference that v11 dropped when it shortened the header. Sent right
            // after "R" so the receiver associates it with THIS record before the
            // samples arrive. Not part of the checksum.
            n = snprintf(line, sizeof(line), "T %lu\n", (unsigned long)accelRec.ts);
            bleWriteLine(line, n);
            for (uint16_t i = 0; i < accelRec.count; i++) {
                n = snprintf(line, sizeof(line), "%d,%d,%d\n",
                             accelRec.xyz[i][0], accelRec.xyz[i][1], accelRec.xyz[i][2]);
                bleWriteLine(line, n);
            }
        }
        n = snprintf(line, sizeof(line), "E %u %u\n", (unsigned)DEVICE_ID, nRecs);
        bleWriteLine(line, n);
    }
    delay(BLE_HOLD_MS);                          // let the last notification flush

    DBG("[BLE] blasted %u records as id=%u x%u reps (fire-and-forget) - reboot-to-sleep clears BLE\r\n",
        nRecs, (unsigned)DEVICE_ID, (unsigned)BLE_BLAST_REPEATS);
    return true;                                 // attempted; no ACK to confirm
}
#else
void accelRingLoad() {}
void accelRingSaveMeta() {}
void accelRingClear() {}
void collectAccelToRing(uint32_t) {}
bool bleDroneOffload() { return false; }
#endif  // ENABLE_ACCEL_BLE

// Real-WUR only: ignore repeat triggers within this window (the AS3933 WAKE line
// stays HIGH until cleared; this rate-limits until the WUR bring-up adds a proper
// CLEAR_WAKE). In SIMULATE_WUR mode the interval itself provides the spacing.
constexpr uint32_t WUR_COOLDOWN_SEC = 600;

// Is this wake a "drone pass" (offload the ring)? SIMULATE_WUR_HOURS>0 -> fake it
// on RTC time; ==0 -> the real AS3933 WAKE pin (P1.04) held HIGH on detection.
bool isDronePass()
{
#if ENABLE_ACCEL_BLE
    if (accelMeta.count == 0) return false;          // nothing to offload
    if (SIMULATE_WUR_HOURS > 0.0) {
        if (!rtcSynced) return false;                // need a real clock for timing
        uint32_t now = rtcNowUnix();
        if (accelMeta.lastDumpUnix == 0) {           // anchor on first data, don't dump yet
            accelMeta.lastDumpUnix = now; accelRingSaveMeta(); return false;
        }
        uint32_t interval = (uint32_t)(SIMULATE_WUR_HOURS * 3600.0 + 0.5);
        return (now - accelMeta.lastDumpUnix) >= interval;
    } else {
        if (rtcSynced && accelMeta.lastDumpUnix != 0 &&
            (rtcNowUnix() - accelMeta.lastDumpUnix) < WUR_COOLDOWN_SEC) return false;
        return (digitalRead(WUR_WAKE_PIN) == HIGH);
    }
#else
    return false;
#endif
}

// ============================================================================
// PHASES
// ============================================================================
void doCollect()
{
    cycleNum++;
    DBG("== COLLECT (cycle %lu, up %lu s, noFixStreak=%lu) ==\r\n",
        (unsigned long)cycleNum, (unsigned long)(millis()/1000),
        (unsigned long)consecutiveNoFix);

    uint16_t vbat = readVbat_mV();
    DBG("[BAT] %u mV\r\n", vbat);

    FixResult r;
    tryOneFix(r);

    // Strategy B bookkeeping.
    if (r.haveFix) consecutiveNoFix = 0;
    else           consecutiveNoFix++;

    // v14: record the time of the last successful POSITION fix (persisted). This is
    // the cold-fix detector's memory: a recent lastFixUnix => next fix is "warm"; a
    // stale/absent one => "cold" (patient). Saved by the saveHeader() below.
    if (r.haveFix) header.lastFixUnix = r.fixUnix;

    // v13 TWO-TIMER: the accelerometer runs on its OWN cadence (ACCEL_PERIOD_HOURS),
    // decoupled from the GNSS/LoRa cadence and from the drone pass. We time-gate off
    // the GNSS-disciplined RTC, so a record is collected whenever the accel period has
    // elapsed -- on fix OR no-fix cycles alike. (Needs a synced clock; before the very
    // first sync there is no time base, so we simply wait for it.) The record is
    // appended to the flash ring and offloaded later, whole, on a drone pass. BLE is
    // NOT used here - only LoRa (the position packet) transmits this cycle.
    if (rtcSynced) {
        if (r.haveFix) fixCounter++;
        uint32_t now = rtcNowUnix();
        if (accelMeta.lastAccelUnix == 0 ||
            (uint32_t)(now - accelMeta.lastAccelUnix) >= ACCEL_PERIOD_SEC) {
            DBG("== ACCEL collect %lu s -> ring (accel timer, every %lu s) ==\r\n",
                (unsigned long)ACCEL_SECS, (unsigned long)ACCEL_PERIOD_SEC);
            accelMeta.lastAccelUnix = now;          // persisted by the append's saveMeta
            collectAccelToRing(now);
        }
    }

    PacketSlot p;
    memset(&p, 0, sizeof(p));
    p.seq        = header.nextSeq++;
    // Every timestamp is GNSS-derived: a fix uses the fix's exact GPS UTC; a
    // no-fix cycle extrapolates from the GNSS-disciplined RTC (never the
    // module's backup RTC).
    p.timestamp  = r.haveFix ? r.fixUnix : rtcNowUnix();
    p.latE6      = r.haveFix ? r.latE6 : 0;
    p.lonE6      = r.haveFix ? r.lonE6 : 0;
    p.vbat_mV    = vbat;
    p.hasFix     = r.haveFix ? 1 : 0;
    p.txAttempts = 0;
    p.satsInView = r.peakInView;

    // stage into buffer (normally exactly one fresh packet)
    if (header.bufferCount < TRACKER_BUFFER_SIZE) {
        buffer[header.bufferCount++] = p;
    } else {
        for (uint8_t i = 0; i < TRACKER_BUFFER_SIZE - 1; i++) buffer[i] = buffer[i + 1];
        buffer[TRACKER_BUFFER_SIZE - 1] = p;
    }
    saveBuffer();
    saveHeader();
    DBG("[BUF] staged seq=%lu (%s), buf=%u/%u  pend=%u\r\n",
        (unsigned long)p.seq, r.haveFix ? "fix" : "no-fix",
        header.bufferCount, TRACKER_BUFFER_SIZE, header.pendingCount);
}

// #5 delivery policy. Newest-first; only drain the backlog if the link is proven
// up by the freshest packet's ACK; real fixes that miss an ACK are retained.
void doTransmitPass()
{
    DBG("[TX pass] cycle=%lu buffer=%u pending=%u\r\n",
        (unsigned long)cycleNum, header.bufferCount, header.pendingCount);
    loraConfigureOnce();

    bool linkUp = false;

    // (1) Freshest first. buffer[bufferCount-1] is the newest staged packet.
    for (int i = (int)header.bufferCount - 1; i >= 0; i--) {
        if (trySendPacket(buffer[i])) {
            markDelivered(buffer[i]);
            linkUp = true;
            if (i > 0) napSleep(TX_PULSE_GAP_SEC);   // gap before next fresh packet (rare)
        } else {
            // Link is down. Preserve the un-sent REAL fixes, drop no-fix
            // heartbeats (best-effort), then stop - don't blast a dead link.
            // Push oldest-first so the newest un-sent fix ends up at pending[0].
            for (int j = 0; j <= i; j++) {
                if (buffer[j].hasFix) pendingPushNewest(buffer[j]);
            }
            linkUp = false;
            break;
        }
    }
    header.bufferCount = 0;

    // (2) Link confirmed up -> drain the real-fix backlog, newest -> oldest,
    // TX_PULSE_GAP_SEC apart. Stop at the first miss (link dropped): keep the rest.
    if (linkUp) {
        while (header.pendingCount > 0) {
            napSleep(TX_PULSE_GAP_SEC);              // >=30 s so ACK windows can't collide
            if (trySendPacket(pending[0])) {         // pending[0] = newest backlog fix
                markDelivered(pending[0]);
                pendingRemoveFront();
            } else {
                break;                               // link dropped mid-drain
            }
        }
    }

    api.lora.precv(0);   // radio idle before sleep
    saveBuffer();
    savePending();
    saveHeader();

    DBG("[TX pass] done: delivered=%lu lastSeq=%lu pending=%u undelivered=%lu\r\n",
        (unsigned long)header.deliveredCount, (unsigned long)header.lastDeliveredSeq,
        header.pendingCount, (unsigned long)header.undeliveredCount);
}

// ============================================================================
// setup / loop  (alive-first: init deferred into loop - ISL board quirk)
// ============================================================================
enum Phase { WARMUP, INIT, OPERATE };
Phase phase = WARMUP;

void initEverything()
{
    say("========================================================");
    say("ISL Board - PRODUCTION v15 (COLLAR: v14 + GPS backup-cell health + charge-on-cold, TTFF/CELL telemetry)");
    say("========================================================");

    api.ble.stop();
    NVIC_DisableIRQ(FPU_IRQn);

    rtcInit();
    batteryAdcInit();   // one-time SAADC offset cal (conditions the battery-ADC ref)

    // GPS to safe idle: EN off, both UART pins LOW (isolation)
    pinMode(GPS_EN_PIN, OUTPUT);      digitalWrite(GPS_EN_PIN, HIGH);
    pinMode(GPS_UART_RX_PIN, OUTPUT); digitalWrite(GPS_UART_RX_PIN, LOW);
    pinMode(GPS_UART_TX_PIN, OUTPUT); digitalWrite(GPS_UART_TX_PIN, LOW);

    // WUR pins parked
    pinMode(WUR_CLK_PIN,  OUTPUT); digitalWrite(WUR_CLK_PIN,  LOW);
    pinMode(WUR_MOSI_PIN, OUTPUT); digitalWrite(WUR_MOSI_PIN, LOW);
    pinMode(WUR_CS_PIN,   OUTPUT); digitalWrite(WUR_CS_PIN,   LOW);
    pinMode(WUR_MISO_PIN, INPUT);
    pinMode(WUR_WAKE_PIN, INPUT);

    api.system.sleep.setup(RUI_WAKEUP_FALLING_EDGE, RTC_INT_PIN);   // RTC wake (validated)
#if ENABLE_WUR_WAKE
    api.system.sleep.setup(RUI_WAKEUP_RISING_EDGE, WUR_WAKE_PIN);   // WUR wake (after real-wake test)
#endif
    api.system.sleep.registerWakeupCallback(onRtcWake);

    flashLoadAll();
    accelRingLoad();          // v9: load the accelerometer flash-ring metadata
    loraConfigureOnce();

    // seed the RTC from GNSS UTC on cold boots (VBACKUP has no battery, so any
    // full power loss resets the clock; GPS restores it in seconds).
    if (!rtcSynced) bootTimeSync();

    DBG("[CFG] id=%03u  period=%lu min%s  SV_MIN=%u  no_sky_abort=%lu s  fix_max=%lu s  "
        "settle=%lu s  ack=%lu s  backoff=K%lu/%lumin  tx_gap=%lu s  buffer=%u  "
        "pend_slots=%u  WUR_wake=%d  SIM_FIX=%d  rtcSynced=%d\r\n",
        (unsigned)DEVICE_ID,
        (unsigned long)GNSS_PERIOD_MIN,
        (GNSS_PERIOD_MIN * 60UL > 4095UL) ? " (1/60Hz tick)" : " (1Hz tick)",
        SV_MIN, (unsigned long)NO_SKY_ABORT_SEC, (unsigned long)FIX_MAX_SEC,
        (unsigned long)POST_FIX_SETTLE_SEC, (unsigned long)ACK_TIMEOUT_SEC,
        (unsigned long)NOFIX_BACKOFF_AFTER, (unsigned long)BACKOFF_PERIOD_MIN,
        (unsigned long)TX_PULSE_GAP_SEC, TRACKER_BUFFER_SIZE, PENDING_SLOTS,
        ENABLE_WUR_WAKE, SIMULATE_FIX, rtcSynced ? 1 : 0);
    // v13: the two accel/BLE timers + the ring capacity.
    DBG("[CFG-ACCEL] accel_collect_every=%lu s  drone_pass_every=%lu s (SIMULATE_WUR)  "
        "ring=%u records  clear_after_send=%d  ring_now=%u\r\n",
        (unsigned long)ACCEL_PERIOD_SEC,
        (unsigned long)(uint32_t)(SIMULATE_WUR_HOURS * 3600.0 + 0.5),
        ACCEL_RING_RECORDS, OFFLOAD_DELETE_AFTER_SEND, accelMeta.count);
    // v14: the cold/first-fix budget + the persisted last-fix memory.
    DBG("[CFG-GPS] warm=no-sky<%lus/max<%lus  COLD=<%lus (no early abort)  cold_if: "
        "!synced | never-fixed | gap>=%luh | noFixStreak>=%lu   lastFix=%s\r\n",
        (unsigned long)NO_SKY_ABORT_SEC, (unsigned long)FIX_MAX_SEC, (unsigned long)COLD_FIX_MAX_SEC,
        (unsigned long)EPHEMERIS_STALE_HOURS, (unsigned long)COLD_AFTER_NOFIX,
        header.lastFixUnix ? "present" : "NONE (first fix will be COLD)");
    // v15: cell-health + charge-on-cold knobs.
    DBG("[CFG-CELL] hot_if_ttff<%lus  charge_when_low_to=%lus_on  dead_after=%lu non-hot  "
        "-> packet carries TTFF=<s>,CELL=<OK|LOW|DEAD>\r\n",
        (unsigned long)HOT_TTFF_SEC, (unsigned long)GPS_CHARGE_SEC, (unsigned long)CELL_DEAD_AFTER);
}

void setup()
{
    Serial.begin(115200);                       // native USB-C CDC
    { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
    delay(300);
    say("ISL Production v15 boot. Heartbeat, then init...");
}

void loop()
{
    static uint32_t t0 = millis();
    static uint32_t lastHb = 0;

    if (phase == WARMUP) {
        if (millis() - lastHb >= 500) {
            lastHb = millis();
            DBG("[alive] %lu ms\r\n", (unsigned long)(millis() - t0));
        }
        if (millis() - t0 >= WARMUP_MS) phase = INIT;
        return;
    }

    if (phase == INIT) {
        initEverything();
        phase = OPERATE;
        return;
    }

    // ==== OPERATE: one action per pass ====
    // If the previous (BLE) cycle queued a sleep before rebooting, perform it now
    // in this fresh, BLE-clean session, then fall through.
    bleUsed = false;
    doQueuedSleepIfAny();

    // v9: DRONE PASS? -> offload the whole ring over BLE (request/verify). This is
    // the ONLY thing that uses BLE.
    if (isDronePass()) {
        DBG("== DRONE PASS: offloading %u records ==\r\n", accelMeta.count);

        // ROBUSTNESS (v9): RUI3's BLE pairing can HANG the SoftDevice on a failed
        // Just-Works/bonding attempt (e.g. a reflashed receiver -> "Pairing
        // procedure fail" -> freeze). Before we touch BLE we therefore (a) advance
        // lastDumpUnix so we won't re-trigger a pass immediately, and (b) PERSIST a
        // queued sleep to flash. If the offload then hangs and the board is reset
        // (by hand, or a watchdog), the next boot performs the queued sleep and
        // resumes normally instead of re-freezing on another offload attempt.
        accelMeta.lastDumpUnix = rtcSynced ? rtcNowUnix() : accelMeta.lastDumpUnix;
        accelRingSaveMeta();
        queueSleepCmd(GNSS_PERIOD_MS / SECONDS);   // pre-arm recovery (write only, no reboot)

        bool sent = bleDroneOffload();             // fire-and-forget blast (cannot hang)

        // v12: CLEAR-AFTER-SEND (OFFLOAD_DELETE_AFTER_SEND=1). Fire-and-forget has no
        // ACK so `sent` is always true; we clear the ring after every blast so the
        // NEXT pass carries only records collected since this one -- new data every
        // time, never a re-send. (v11 kept the ring here and re-sent it, deduped by
        // seq.) Missed-pass retention is intentionally dropped -> the other engineer.
        if (OFFLOAD_DELETE_AFTER_SEND && sent) accelRingClear();
        DBG("== DRONE PASS done (%s) -> reboot-to-sleep ==\r\n",
            OFFLOAD_DELETE_AFTER_SEND ? "blasted+cleared (next pass = new data)" : "blasted, kept for dedup");
        queueSleepAndReboot(GNSS_PERIOD_MS / SECONDS);   // never returns
    }

    doCollect();

    if (header.bufferCount > 0 || header.pendingCount > 0) {
        doTransmitPass();          // LoRa position packet only (unchanged)
    }

    // STRATEGY B: stretch the cadence after K consecutive no-fix cycles; the
    // first fix (consecutiveNoFix==0) snaps straight back to the normal period.
    uint32_t sleepMs = GNSS_PERIOD_MS;
    if (consecutiveNoFix >= NOFIX_BACKOFF_AFTER) {
        sleepMs = BACKOFF_PERIOD_MS;
        DBG("== BACKOFF: %lu consecutive no-fix >= K=%lu -> stretch to %lu h ==\r\n",
            (unsigned long)consecutiveNoFix, (unsigned long)NOFIX_BACKOFF_AFTER,
            (unsigned long)BACKOFF_PERIOD_HOURS);
    }
    // v9: a normal collect cycle never touches BLE (offload only happens on a
    // drone pass, above), so it takes the proven v7 deepSleep() path directly.
    DBG("== IDLE deep-sleep %lu s ==\r\n", (unsigned long)(sleepMs / SECONDS));
    deepSleep(sleepMs / SECONDS);
}
