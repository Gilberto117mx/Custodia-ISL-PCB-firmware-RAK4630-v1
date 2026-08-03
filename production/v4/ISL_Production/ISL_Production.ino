/*
 * ============================================================================
 * ISL Board (RAK_feather) - PRODUCTION firmware v4
 * ============================================================================
 * v4 = the HARDENED v3 (which the last bench run was NOT using - the
 * "2066-31-19" garbage seed cannot pass this build's guard) + the bench-tested
 * knob values (period 5 min / fix timeout 60 s / tx_retries 2) + a cycle
 * counter in the COLLECT/TX prints so USB-CDC log replays after wake (a
 * cosmetic re-enumeration artifact - the receiver proved no real double-TX)
 * are instantly distinguishable from real repeats.
 *
 * Time-seed guard (this build): full range check (year 2025-2044, month 1-12,
 * day 1-31, h/m/s) + TWO-SAMPLE confirmation (2nd sane reading >=2 s later
 * must advance consistently with wall time) + boot self-heal (insane stored
 * RTC time clears the sync flag and forces a GPS re-seed).
 *
 * Bench facts to remember (from the v3 5-min run):
 *  - USB-C attached => sleep floor ~1.78 mA (nRF USB peripheral active while
 *    VBUS present). The 155 uA floor exists only on battery. Measure headless.
 *  - [BAT] reads ~3.9 V with USB attached (VBUS back-feeds the battery rail);
 *    on battery it reads the true cell voltage.
 * ============================================================================
 *
 * Same firmware as v2 (validated operate loop + CALIBRATED battery reader), plus:
 *
 *  1) RTC TIME FROM GNSS (test #7). At boot, if the RTC was never GPS-synced
 *     (USER_RAM flag absent - true after any full power loss, since VBACKUP is
 *     tied to the 3.3 V rail on this board), run a TIME-ONLY GPS session: wait
 *     for a valid+sane UTC date/time (year >= MIN_VALID_YEAR, age < 2 s - GPS
 *     time arrives BEFORE a position fix, usually seconds) and set the RV-3028.
 *     If it fails (indoors), the node still operates and re-seeds automatically
 *     during the normal COLLECT fix sessions until it succeeds. All wake-ups and
 *     packet timestamps then run off that GPS-derived UTC clock.
 *     Also fixes the Melopero setTime() argument order: (year, month, WEEKDAY,
 *     DATE, hh, mm, ss) - the old hardcoded calls had weekday/date swapped.
 *
 *  2) LONG WAKE PERIODS (>68 min, e.g. 2 h) - robust dual-mode RTC timer.
 *     The RV-3028 countdown timer is 12-bit (max preset 4095):
 *        1 Hz tick    -> up to 4095 s  (~68 min), 1 s granularity
 *        1/60 Hz tick -> up to 4095 min (~2.8 days), 1 MINUTE granularity
 *     rtcSetNextWake() now auto-selects the tick: <= 4095 s uses 1 Hz; longer
 *     uses 1/60 Hz (seconds rounded to the nearest minute; first period can be
 *     up to one tick (~60 s) short/long - inherent to the countdown timer and
 *     irrelevant at multi-hour cadence). The deep-sleep BACKSTOP now scales
 *     with the requested sleep (period + 5 min margin) instead of a fixed 1 h,
 *     so a 2 h sleep is no longer cut short by the backstop.
 *     Default GNSS_PERIOD_MIN = 120 (the 2 h deployment cadence).
 *
 * Everything else - operate loop, GPS teardown/isolation, LoRa TX/ACK, flash
 * persistence, calibrated battery (raw*1795/1000), alive-first structure - is
 * unchanged from v2.
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
constexpr uint32_t GNSS_PERIOD_MIN       = 5;    // idle deep-sleep between cycles (bench; deployment ~120)
constexpr uint8_t  TRACKER_BUFFER_SIZE   = 1;    // packets to accumulate before a TX pass (max 3)
constexpr uint32_t TX_RETRY_MIN          = 5;    // deep sleep between TX retries
constexpr uint8_t  MAX_TX_RETRIES        = 2;    // TX attempts before a packet is archived UNDELIVERED
constexpr uint32_t GNSS_FIX_TIMEOUT_SEC  = 60;   // max listen time per fix attempt
constexpr uint8_t  MAX_GNSS_RETRIES      = 2;    // fix attempts before a timestamp-only packet
constexpr uint32_t GNSS_RETRY_WAIT_MIN   = 5;    // deep sleep between fix attempts
constexpr uint32_t POST_FIX_SETTLE_SEC   = 5;    // fix must stay valid this long before it's accepted
constexpr uint32_t ACK_TIMEOUT_SEC       = 8;    // RX window for the ACK after each TX
constexpr uint16_t DEVICE_ID             = 51;   // "051" - ISL node over-air ID

// GPS->RTC time seeding (test #7, hardened after v3 bring-up caught month=31/year=2088
// garbage NMEA passing the old year-only guard)
constexpr uint32_t BOOT_TIME_SYNC_TIMEOUT_SEC = 120;  // boot-time time-only GPS session limit
constexpr uint16_t MIN_VALID_YEAR             = 2025; // reject the module's pre-sync default date
constexpr uint16_t MAX_VALID_YEAR             = 2044; // reject corrupt-sentence future dates
constexpr uint32_t TIME_CONFIRM_GAP_S         = 2;    // 2nd sane reading must be >= this much later
constexpr int32_t  TIME_CONFIRM_TOL_S         = 2;    // ...and advanced consistently within +/- this

// Arm the AS3933 WUR (P1.04 rising edge) as a SECOND deep-sleep wake source.
// Leave 0 until the WUR real-wake test passes. RTC wake works either way.
#define ENABLE_WUR_WAKE     0

// ---------------------------------------------------------------------------
// UNIT CONVERSION (do not edit) - everything below works in milliseconds.
// ---------------------------------------------------------------------------
constexpr uint32_t SECONDS = 1000UL;
constexpr uint32_t MINUTES = 60UL * SECONDS;

constexpr uint32_t GNSS_PERIOD_MS       = GNSS_PERIOD_MIN      * MINUTES;
constexpr uint32_t TX_RETRY_MS          = TX_RETRY_MIN         * MINUTES;
constexpr uint32_t GNSS_FIX_TIMEOUT_MS  = GNSS_FIX_TIMEOUT_SEC * SECONDS;
constexpr uint32_t GNSS_RETRY_WAIT_MS   = GNSS_RETRY_WAIT_MIN  * MINUTES;
constexpr uint32_t POST_FIX_SETTLE_MS   = POST_FIX_SETTLE_SEC  * SECONDS;
constexpr uint32_t ACK_TIMEOUT_MS       = ACK_TIMEOUT_SEC      * SECONDS;
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
#define BATT_ADC_PIN        P0_31     // AIN7, 10k/10k divider
#define GPS_BAUD            9600

#define GPS_BOOT_DELAY_MS   500UL
#define GPS_POWERDOWN_MS    250UL
#define WARMUP_MS           3000UL
// Deep-sleep backstop is DYNAMIC in v3: requested sleep + margin (see deepSleep).
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

// Flash layout (identical to v1/v2)
#define FLASH_MAGIC         0xA5A5CAFEUL
#define FLASH_SCHEMA_VER    1
#define FLASH_OFF_HEADER    0x0000
#define FLASH_OFF_BUFFER    0x0040
#define FLASH_OFF_PENDING   0x0080
#define FLASH_OFF_UNDELIV   0x0100
#define MAX_UNDELIVERED     8

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
    uint32_t reserved;
};  // 24 bytes

// ============================================================================
// GLOBAL STATE
// ============================================================================
Melopero_RV3028 rtc;
FlashHeader     header;
PacketSlot      buffer[3];
PacketSlot      pending[3];

static volatile bool rtcWokeUs = false;
static volatile bool txDone    = false;
static volatile bool rxDone    = false;
static bool          rtcSynced = false;   // USER_RAM flag cached (GPS-synced?)
static uint32_t      cycleNum  = 0;       // increments each COLLECT (disambiguates CDC log replays)

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
static bool     utcSane(int Y, int M, int D, int h, int m, int s);
static uint32_t unixFromUtc(int Y, int M, int D, int h, int m, int s);

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
        say("[FLASH] Fresh init");
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
//   <= 4095 s          : 1 Hz tick, 1 s granularity
//   >  4095 s          : 1/60 Hz tick, preset in MINUTES (rounded), 1 min
//                        granularity, max 4095 min (~2.8 days). First period may
//                        be short/long by up to one tick (~60 s) - inherent to
//                        the countdown timer, irrelevant at multi-hour cadence.
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

// If not yet synced, try to seed the RTC from this TinyGPSPlus. HARDENED:
// v3 bring-up caught a corrupted-but-parsed RMC ("2088-31-19") passing the old
// year-only guard - TinyGPSPlus checksums sentences but does NOT range-check
// fields. Defenses, in order:
//   1) FULL range check (year window 2025-2044, month 1-12, day 1-31, h/m/s).
//   2) TWO-SAMPLE CONFIRMATION: a second sane reading >= TIME_CONFIRM_GAP_S
//      later must have advanced consistently with wall time (+/- tolerance).
//      Garbage doesn't tick coherently; a real GPS clock does.
// Called opportunistically inside every GPS session (boot + COLLECT).
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
// DEEP SLEEP - dynamic backstop: requested sleep + margin (a fixed 1 h backstop
// would cut a 2 h RTC sleep short; the backstop must only catch a MISSED INT).
// ============================================================================
void deepSleep(uint32_t seconds)
{
    DBG("[SLEEP] %lu s\r\n", (unsigned long)seconds);
    rtcSetNextWake(seconds);
    rtcWokeUs = false;
    clearFPU();
    uint32_t backstop = seconds * 1000UL + SLEEP_BACKSTOP_MARGIN_MS;
    uint32_t before = millis();
    api.system.sleep.all(backstop);
    uint32_t slept  = millis() - before;
    rtcClearTF();
    DBG("[WAKE] %s after ~%lu ms\r\n",
        rtcWokeUs ? "RTC P0.21" : "other/backstop", (unsigned long)slept);
}

// ============================================================================
// BATTERY (AIN7/P0.31, 10k/10k) - CALIBRATED (test #2 final).
// ============================================================================
static int cmp_int(const void *a, const void *b) { return (*(const int *)a) - (*(const int *)b); }

static bool saadcWaitEvt(volatile uint32_t *evt) {
    for (uint32_t n = 0; n < 2000000UL; n++) { if (*evt) { *evt = 0; return true; } }
    return false;
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

    pinMode(BATT_ADC_PIN, INPUT);
    analogReadResolution(12);
    analogReference(AR_INTERNAL);
}

uint16_t readVbat_mV()
{
    int v[ADC_SAMPLES]; int m = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) { v[m++] = analogRead(BATT_ADC_PIN); delay(2); }
    qsort(v, m, sizeof(int), cmp_int);
    uint16_t raw = v[m/2];
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

// Acquire fix with settling; ALSO seeds the RTC opportunistically (v3).
bool gpsAcquire(uint32_t timeout_ms, int32_t *latE6, int32_t *lonE6, uint16_t *sats, uint32_t *ttff_ms)
{
    TinyGPSPlus gps;
    uint32_t start   = millis();
    uint32_t firstAt = 0;
    bool     validSeen = false;

    while ((millis() - start) < timeout_ms) {
        while (Serial0.available()) gps.encode(Serial0.read());

        maybeSeedRtc(gps);   // v3: free re-seed whenever time is valid & not yet synced

        bool ok = gps.location.isValid() && gps.location.age() < 2000 &&
                  gps.date.isValid()     && gps.time.isValid();
        if (ok) {
            if (!validSeen) { validSeen = true; firstAt = millis(); }
            if ((millis() - firstAt) >= POST_FIX_SETTLE_MS) {
                *latE6   = (int32_t)(gps.location.lat() * 1e6);
                *lonE6   = (int32_t)(gps.location.lng() * 1e6);
                *sats    = gps.satellites.isValid() ? gps.satellites.value() : 0;
                *ttff_ms = millis() - start;
                return true;
            }
        } else if (validSeen) {
            validSeen = false;
        }
    }
    *ttff_ms = millis() - start;
    return false;
}

bool tryOneFix(int32_t *latE6, int32_t *lonE6, uint16_t *sats, uint32_t *ttff_ms)
{
    gpsPowerOn();
    bool ok = gpsAcquire(GNSS_FIX_TIMEOUT_MS, latE6, lonE6, sats, ttff_ms);
    DBG("[GPS] %s in %lu ms  sats=%u  lat=%ld lon=%ld\r\n",
        ok ? "FIX" : "no fix",
        (unsigned long)*ttff_ms, *sats, (long)*latE6, (long)*lonE6);
    gpsPowerOff();
    return ok;
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
                    "%03u,%lu,%s%lu.%06lu,%s%lu.%06lu,%u.%02u,%lu",
                    (unsigned)DEVICE_ID,
                    (unsigned long)p.seq,
                    latSign, (unsigned long)(laA / 1000000UL), (unsigned long)(laA % 1000000UL),
                    lonSign, (unsigned long)(loA / 1000000UL), (unsigned long)(loA % 1000000UL),
                    vInt, vCen,
                    (unsigned long)p.timestamp);
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

bool trySendPacket(PacketSlot &p)
{
    char payload[128];
    int len = formatPacket(p, payload, sizeof(payload));
    DBG("[TX] seq=%lu: %s\r\n", (unsigned long)p.seq, payload);
    if (!loraSend((uint8_t *)payload, (uint8_t)len)) return false;
    p.txAttempts++;
    return loraWaitAck(p.seq);
}

// ============================================================================
// QUEUE HELPERS
// ============================================================================
void queuePush(PacketSlot *q, uint8_t &n, const PacketSlot &p, uint8_t maxN)
{
    if (n < maxN) { q[n++] = p; return; }
    for (uint8_t i = 0; i < maxN - 1; i++) q[i] = q[i + 1];
    q[maxN - 1] = p;
}

// ============================================================================
// PHASES
// ============================================================================
void doCollect()
{
    cycleNum++;
    DBG("== COLLECT (cycle %lu, up %lu s) ==\r\n", (unsigned long)cycleNum, (unsigned long)(millis()/1000));

    uint16_t vbat = readVbat_mV();
    DBG("[BAT] %u mV\r\n", vbat);

    int32_t  latE6 = 0, lonE6 = 0;
    uint16_t sats  = 0;
    uint32_t ttff  = 0;
    bool     haveFix = false;

    for (uint8_t attempt = 1; attempt <= MAX_GNSS_RETRIES; attempt++) {
        DBG("[GPS] attempt %u/%u\r\n", attempt, MAX_GNSS_RETRIES);
        if (tryOneFix(&latE6, &lonE6, &sats, &ttff)) { haveFix = true; break; }
        if (attempt < MAX_GNSS_RETRIES) {
            DBG("[GPS] failed, deep-sleeping %lu s before retry\r\n",
                (unsigned long)(GNSS_RETRY_WAIT_MS / SECONDS));
            deepSleep(GNSS_RETRY_WAIT_MS / SECONDS);
        }
    }

    PacketSlot p;
    memset(&p, 0, sizeof(p));
    p.seq        = header.nextSeq++;
    p.timestamp  = rtcNowUnix();
    p.latE6      = haveFix ? latE6 : 0;
    p.lonE6      = haveFix ? lonE6 : 0;
    p.vbat_mV    = vbat;
    p.hasFix     = haveFix ? 1 : 0;
    p.txAttempts = 0;

    queuePush(buffer, header.bufferCount, p, TRACKER_BUFFER_SIZE);
    saveBuffer();
    saveHeader();
    DBG("[BUF] added seq=%lu (%s), buf=%u/%u\r\n",
        (unsigned long)p.seq, haveFix ? "fix" : "no-fix",
        header.bufferCount, TRACKER_BUFFER_SIZE);
}

void doTransmitPass()
{
    DBG("[TX pass] cycle=%lu pending=%u buffer=%u\r\n", (unsigned long)cycleNum, header.pendingCount, header.bufferCount);
    loraConfigureOnce();

    PacketSlot leftover[6];
    uint8_t    lcount = 0;

    for (uint8_t i = 0; i < header.pendingCount; i++) {
        if (trySendPacket(pending[i])) {
            header.deliveredCount++;
            header.lastDeliveredSeq = pending[i].seq;
        } else if (lcount < 6) {
            leftover[lcount++] = pending[i];
        }
    }
    for (uint8_t i = 0; i < header.bufferCount; i++) {
        if (trySendPacket(buffer[i])) {
            header.deliveredCount++;
            header.lastDeliveredSeq = buffer[i].seq;
        } else if (lcount < 6) {
            leftover[lcount++] = buffer[i];
        }
    }

    api.lora.precv(0);   // radio idle before sleep

    header.pendingCount = 0;
    header.bufferCount  = 0;
    uint8_t copy = lcount < 3 ? lcount : 3;
    for (uint8_t i = 0; i < copy; i++) pending[i] = leftover[i];
    header.pendingCount = copy;

    for (uint8_t i = 3; i < lcount; i++) appendUndelivered(leftover[i]);

    saveBuffer();
    savePending();
    saveHeader();
}

void doTransmitWithRetries()
{
    for (uint8_t attempt = 1; attempt <= MAX_TX_RETRIES; attempt++) {
        DBG("== TX (attempt %u/%u) ==\r\n", attempt, MAX_TX_RETRIES);
        doTransmitPass();
        if (header.pendingCount == 0) break;
        if (attempt < MAX_TX_RETRIES) {
            DBG("[TX] pending=%u, sleeping %lu s before retry\r\n",
                header.pendingCount, (unsigned long)(TX_RETRY_MS / SECONDS));
            deepSleep(TX_RETRY_MS / SECONDS);
        }
    }
    if (header.pendingCount > 0) {
        DBG("[TX] %u still pending after %u tries -> UNDELIVERED\r\n",
            header.pendingCount, MAX_TX_RETRIES);
        for (uint8_t i = 0; i < header.pendingCount; i++) appendUndelivered(pending[i]);
        header.pendingCount = 0;
        savePending();
        saveHeader();
    }
}

// ============================================================================
// setup / loop  (alive-first: init deferred into loop - ISL board quirk)
// ============================================================================
enum Phase { WARMUP, INIT, OPERATE };
Phase phase = WARMUP;

void initEverything()
{
    say("========================================================");
    say("ISL Board - PRODUCTION v4  (hardened seed + bench knobs)");
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

    // v3: seed the RTC from GNSS UTC on cold boots (VBACKUP has no battery, so
    // any full power loss resets the clock; GPS restores it in seconds).
    if (!rtcSynced) bootTimeSync();

    DBG("[CFG] id=%03u  period=%lu min%s  fix_timeout=%lu s  retry_wait=%lu min  "
        "tx_retry=%lu min  ack=%lu s  settle=%lu s  buffer=%u  gnss_retries=%u  "
        "tx_retries=%u  WUR_wake=%d  rtcSynced=%d\r\n",
        (unsigned)DEVICE_ID,
        (unsigned long)GNSS_PERIOD_MIN,
        (GNSS_PERIOD_MIN * 60UL > 4095UL) ? " (1/60Hz tick)" : " (1Hz tick)",
        (unsigned long)GNSS_FIX_TIMEOUT_SEC,
        (unsigned long)GNSS_RETRY_WAIT_MIN,
        (unsigned long)TX_RETRY_MIN,
        (unsigned long)ACK_TIMEOUT_SEC,
        (unsigned long)POST_FIX_SETTLE_SEC,
        TRACKER_BUFFER_SIZE, MAX_GNSS_RETRIES, MAX_TX_RETRIES,
        ENABLE_WUR_WAKE, rtcSynced ? 1 : 0);
}

void setup()
{
    Serial.begin(115200);                       // native USB-C CDC
    { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
    delay(300);
    say("ISL Production v4 boot. Heartbeat, then init...");
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
    doCollect();

    bool triggerTx = (header.bufferCount >= TRACKER_BUFFER_SIZE) || (header.pendingCount > 0);
    if (triggerTx) {
        doTransmitWithRetries();
    }

    DBG("== IDLE deep-sleep %lu s ==\r\n", (unsigned long)(GNSS_PERIOD_MS / SECONDS));
    deepSleep(GNSS_PERIOD_MS / SECONDS);
}
