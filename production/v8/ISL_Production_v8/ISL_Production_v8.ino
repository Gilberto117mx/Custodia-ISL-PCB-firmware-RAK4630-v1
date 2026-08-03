/*
 * ============================================================================
 * ISL Board (RAK_feather) - PRODUCTION firmware v8
 * ============================================================================
 * v8 = v7 (UNCHANGED tracker) + ACCELEROMETER-over-BLE. The entire v7 pipeline
 * - GNSS strategy A/B/C, calibrated battery, LoRa P2P + ACK + delivery
 * guarantee, flash persistence, RV-3028 clock, and the 34 uA deep-sleep floor -
 * is preserved byte-for-byte. Two things are ADDED, both modelled on the proven
 * tests/Accelerometer/ pair:
 *
 *   1) ACCEL COLLECT (5 s) ON EVERY GNSS FIX. Right after a position fix settles
 *      (and before the LoRa pass), we read the Grove LIS3DHTR over BIT-BANG I2C
 *      on P0.24/P0.25 (pure GPIO - works under RUI3 despite Wire1 being
 *      unsupported) for ACCEL_COLLECT_MS at ACCEL_PERIOD_MS. Same 100 Hz / +-2 g
 *      / high-res config and raw-count sample format as the committed emitter.
 *      No-fix cycles skip the accelerometer entirely.
 *
 *   2) BLE TX OF THE ACCEL BURST, AFTER THE LoRa PASS. LoRa still carries ONLY
 *      the tracker (position) packet - unchanged v7 doTransmitPass. Once that
 *      pass is done, if this was a fix cycle we bring BLE up (RUI3 api.ble.uart =
 *      Nordic UART Service), advertise as "Custodia-Tracker", stream the buffer
 *      as "ACC cycle=N count=M\n" / "sNN x,y,z\n" / "END\n", then STOP BLE so the
 *      deep-sleep floor is untouched.
 *
 *  >> FRAMEWORK NOTE (why the receiver is a SEPARATE, different sketch):
 *     The committed tests/Accelerometer emitter is Adafruit/Bluefruit BSP, whose
 *     advertising carries the NUS *service UUID*; RUI3's api.ble.uart advertises
 *     the *name* only. So the RUI3 tracker cannot be discovered by a receiver
 *     that filters on the service UUID. v8 ships ISL_v8_BLE_Receiver, a Bluefruit
 *     central that matches by NAME ("Custodia-Tracker") instead - see that sketch
 *     and this folder's README. The RUI3<->Bluefruit BLE link is THE thing to
 *     verify on the bench (advertising discovery + notification subscribe, incl.
 *     any Just-Works pairing) - see the README test plan.
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
// v8 - ACCELEROMETER (Grove LIS3DHTR, bit-bang I2C on P0.24/P0.25) + BLE TX
//   Matches the committed tests/Accelerometer emitter: 100 Hz / +-2 g / high-res,
//   raw 16-bit counts, 5 s of samples per fix (max 60). Collected only on a fix.
// ----------------------------------------------------------------------------
#define ENABLE_ACCEL_BLE     1     // master switch for the whole v8 accel+BLE path
constexpr uint32_t ACCEL_COLLECT_MS = 5000;   // seconds of motion buffered per fix (5 s)
constexpr uint32_t ACCEL_PERIOD_MS  = 100;    // 10 Hz sample cadence (matches the emitter)
constexpr uint8_t  ACCEL_MAX_SAMPLES = 60;    // buffer cap (5 s @ 10 Hz = 50; 60 = margin)

// BLE transmit window. RUI3's api.ble.uart cannot report "central connected", so
// we advertise, wait BLE_SETTLE_MS for the receiver to connect + subscribe to
// notifications, then stream the buffer paced BLE_LINE_GAP_MS/line, hold, stop.
// >> If the receiver connects but misses the first lines, raise BLE_SETTLE_MS. <<
constexpr uint32_t BLE_SETTLE_MS    = 5000;   // wait for the central to connect+subscribe
constexpr uint32_t BLE_LINE_GAP_MS  = 50;     // per-line pacing (emitter uses 50)
constexpr uint32_t BLE_HOLD_MS      = 500;    // let the last notification flush before stop
constexpr uint32_t BLE_ADV_MAX_SEC  = 20;     // hard cap on the advertise/TX window (safety)

// ----------------------------------------------------------------------------
// STRATEGY A - SV-gated adaptive GPS timeout
// ----------------------------------------------------------------------------
constexpr uint16_t SV_MIN             = 4;    // satellites-in-view that make the long wait "worth it"
constexpr uint32_t NO_SKY_ABORT_SEC   = 25;   // if fewer than SV_MIN sats are seen by this time -> abort (no sky),
                                              //   send a timestamp-only packet. (Give GSV time to populate: >=20 s.)
constexpr uint32_t FIX_MAX_SEC        = 120;  // hard cap once SV_MIN+ satellites ARE visible (extend the wait to here)

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
#define FLASH_SCHEMA_VER    2
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
    uint8_t  bufferCount;
    uint8_t  pendingCount;
    uint8_t  reserved[2];
};  // 32 bytes

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

// v8 accel buffer (raw LIS3DHTR counts) - filled by collectAccel() on a fix,
// drained by bleSendAccel() after the LoRa pass.
struct AccelSample { int16_t x, y, z; };
static AccelSample   accelBuf[ACCEL_MAX_SAMPLES];
static uint8_t       accelCount = 0;
static bool          haveAccel  = false;   // set on a fix cycle after collectAccel()
static bool          bleUsed    = false;   // set when BLE was brought up this cycle -> reboot-to-sleep
static uint32_t      bleCycle   = 0;        // increments each BLE burst (goes in the header line)

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
        say("[FLASH] Fresh init (schema v2)");
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
    api.system.sleep.all(seconds * 1000UL);
}

// ============================================================================
// v8 REBOOT-TO-SLEEP (only used after a BLE/fix cycle - see SleepCmd comment).
//   queueSleepAndReboot(): persist the intended sleep, then reset. The reset is
//   what fully clears the BLE/SoftDevice state that would otherwise wedge sleep.
//   doQueuedSleepIfAny(): at boot, if a sleep was queued, clear it and perform it
//   in a clean (BLE never started this session) state - identical to v7 sleep.
// ============================================================================
void queueSleepAndReboot(uint32_t seconds)
{
    SleepCmd sc = { SLEEPCMD_MAGIC, seconds };
    flashWrite(FLASH_OFF_SLEEPCMD, &sc, sizeof(sc));
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
void gpsAcquire(FixResult &r)
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

    while ((millis() - start) < FIX_MAX_MS) {
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
        // short window, there's no sky - stop now and save the GPS burst.
        if ((millis() - start) >= NO_SKY_ABORT_MS && r.peakInView < SV_MIN) {
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
    DBG("[GPS] SIMULATED FIX  used=%u  IN-VIEW=%u  lat=%ld lon=%ld  ts=%lu\r\n",
        r.sats, r.peakInView, (long)r.latE6, (long)r.lonE6, (unsigned long)r.fixUnix);
    return;
#endif
    gpsPowerOn();
    gpsAcquire(r);
    DBG("[GPS] %s in %lu ms  used=%u  IN-VIEW peak=%u  %s  lat=%ld lon=%ld\r\n",
        r.haveFix ? "FIX" : "no fix",
        (unsigned long)r.ttff_ms, r.sats, r.peakInView,
        r.haveFix ? "" : (r.noSky ? "(no-sky abort)" : "(sky, no fix)"),
        (long)r.latE6, (long)r.lonE6);
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
    return snprintf(buf, bufSize,
                    "%03u,%lu,%s%lu.%06lu,%s%lu.%06lu,%u.%02u,%lu,SV=%lu",
                    (unsigned)DEVICE_ID,
                    (unsigned long)p.seq,
                    latSign, (unsigned long)(laA / 1000000UL), (unsigned long)(laA % 1000000UL),
                    lonSign, (unsigned long)(loA / 1000000UL), (unsigned long)(loA % 1000000UL),
                    vInt, vCen,
                    (unsigned long)p.timestamp,
                    (unsigned long)p.satsInView);
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

// Collect ACCEL_COLLECT_MS of motion into accelBuf[]. Sets haveAccel on success.
void collectAccel()
{
    haveAccel  = false;
    accelCount = 0;
    if (!lisInit()) { say("[ACCEL] LIS3DHTR not found (WHO_AM_I != 0x33) - skipping"); return; }

    uint32_t t0 = millis();
    while ((millis() - t0) < ACCEL_COLLECT_MS && accelCount < ACCEL_MAX_SAMPLES) {
        int16_t x, y, z;
        lisReadXYZ(x, y, z);
        accelBuf[accelCount].x = x;
        accelBuf[accelCount].y = y;
        accelBuf[accelCount].z = z;
        accelCount++;
        delay(ACCEL_PERIOD_MS);
    }
    haveAccel = (accelCount > 0);
    DBG("[ACCEL] %u samples gathered\r\n", accelCount);
}

// ============================================================================
// v8 - BLE accel TX (RUI3 api.ble.uart = Nordic UART Service).
//   Same payload/flow as the committed emitter's bleTX(). RUI3 advertises the
//   NAME only (no service UUID) -> use ISL_v8_BLE_Receiver (matches by name).
//   BLE is fully torn down at the end so the deep-sleep floor is unaffected.
// ============================================================================
void bleSendAccel()
{
    if (!haveAccel || accelCount == 0) return;

    char nm[] = BLE_BROADCAST_NAME;
    api.ble.settings.broadcastName.set(nm, sizeof(nm) - 1);
    api.ble.uart.start(0);                       // start NUS + advertise (name only)
    DBG("[BLE] advertising '%s', settling %lu ms for the receiver...\r\n",
        nm, (unsigned long)BLE_SETTLE_MS);

    // No connection callback in RUI3 api.ble.uart: give the central time to scan,
    // connect, and enable TXD notifications before we stream.
    uint32_t settle = BLE_SETTLE_MS;
    if (settle > BLE_ADV_MAX_SEC * 1000UL) settle = BLE_ADV_MAX_SEC * 1000UL;
    delay(settle);

    char line[64];
    int  n = snprintf(line, sizeof(line), "ACC cycle=%lu count=%u\n",
                      (unsigned long)bleCycle, accelCount);
    api.ble.uart.write((uint8_t *)line, n);
    delay(BLE_LINE_GAP_MS);

    for (uint8_t i = 0; i < accelCount; i++) {
        n = snprintf(line, sizeof(line), "s%02u %d,%d,%d\n",
                     (unsigned)(i + 1), accelBuf[i].x, accelBuf[i].y, accelBuf[i].z);
        api.ble.uart.write((uint8_t *)line, n);
        delay(BLE_LINE_GAP_MS);
    }
    api.ble.uart.write((uint8_t *)"END\n", 4);
    delay(BLE_HOLD_MS);                          // let the last notification flush

    // NOTE: we do NOT api.ble.stop() here. On RUI3 a stop while a central is
    // connected re-advertises on the disconnect; the central reconnects and a
    // Just-Works pairing attempt wedges the next deep sleep. Instead we flag the
    // cycle so loop() does a reboot-to-sleep, which clears BLE deterministically.
    bleUsed = true;
    bleCycle++;
    haveAccel = false;
    DBG("[BLE] accel burst sent (%u samples) - will reboot-to-sleep\r\n", accelCount);
}
#else
void collectAccel() {}
void bleSendAccel() {}
#endif  // ENABLE_ACCEL_BLE

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

    // v8: on a real fix, collect 5 s of accelerometer BEFORE staging/TX (fresh
    // motion at the fix moment). No-fix cycles skip it (haveAccel stays false).
    haveAccel = false;
    if (r.haveFix) {
        DBG("== ACCEL collect (%lu ms @ %lu ms) ==\r\n",
            (unsigned long)ACCEL_COLLECT_MS, (unsigned long)ACCEL_PERIOD_MS);
        collectAccel();
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
    say("ISL Board - PRODUCTION v7  (v6 + deep-sleep floor fix: AIN7 crowbar ~155->~35 uA)");
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
}

void setup()
{
    Serial.begin(115200);                       // native USB-C CDC
    { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
    delay(300);
    say("ISL Production v7 boot. Heartbeat, then init...");
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

    // ==== OPERATE: one full cycle per pass ====
    // v8: if the previous (BLE) cycle queued a sleep before rebooting, perform it
    // now - in this fresh, BLE-clean session - then fall through to a normal cycle.
    bleUsed = false;
    doQueuedSleepIfAny();

    doCollect();

    if (header.bufferCount > 0 || header.pendingCount > 0) {
        doTransmitPass();
    }

    // v8: AFTER the LoRa pass (which carries ONLY the tracker packet), stream the
    // accelerometer burst over BLE. Only fires on a fix cycle (haveAccel set).
    // BLE is torn down inside bleSendAccel(), so the deep-sleep floor is unchanged.
    if (haveAccel) {
        DBG("== BLE accel TX ==\r\n");
        bleSendAccel();
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
    // v8: a fix cycle brought BLE up -> reboot-to-sleep so the reset clears BLE
    // and the queued sleep runs clean on the next boot. No-fix cycles never
    // touched BLE, so they take the proven v7 deepSleep() path directly.
    if (bleUsed) {
        DBG("== IDLE (reboot-to-sleep) %lu s ==\r\n", (unsigned long)(sleepMs / SECONDS));
        queueSleepAndReboot(sleepMs / SECONDS);   // never returns
    } else {
        DBG("== IDLE deep-sleep %lu s ==\r\n", (unsigned long)(sleepMs / SECONDS));
        deepSleep(sleepMs / SECONDS);
    }
}
