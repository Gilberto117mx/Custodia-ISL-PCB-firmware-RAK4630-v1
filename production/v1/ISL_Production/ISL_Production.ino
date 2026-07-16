/*
 * ============================================================================
 * ISL Board (RAK_feather) - PRODUCTION firmware v1  (PRELIMINARY, first port)
 * ============================================================================
 *
 * Full node firmware for the ISL board: operate loop, tunable knobs, packet/ACK
 * protocol, and flash persistence, built around the ISL board's hardware
 * (pins, serials, GPS power topology).
 *
 *   wake -> read battery (board quiet) -> try GNSS fix -> build packet
 *        -> add to BUFFER -> if BUFFER full, TX + wait ACK
 *        -> ACK ok -> mark delivered
 *        -> ACK miss -> keep in PENDING, deep-sleep TX_RETRY_MS, retry
 *        -> after MAX_TX_RETRIES -> archive to UNDELIVERED list
 *        -> deep-sleep GNSS_PERIOD_MS, next cycle
 *
 * ============================================================================
 * ISL HARDWARE SPECIFICS
 * ============================================================================
 *   Debug          native USB-C `Serial` (COM50), NOT Serial0/RAKDAP.
 *                  USB DROPS in deep sleep - you see each awake phase only if
 *                  the port re-enumerates; all prints are if(Serial)-guarded.
 *                  Power measurements: on battery @ 3.6 V (project convention).
 *   GPS UART       Serial0 = UART1 = P0.19/P0.20.
 *   GPS power      single L76K_EN P1.02, ACTIVE-LOW P-FET. No reset pin:
 *                  the external L76X keeps hot start on its own backup battery,
 *                  so there is no cold-reset dance and no coldBoot flag.
 *   GPS teardown   validated in tests #6 (docs/ISL_DeepSleep_Notes.md): cut EN
 *                  -> 250 ms quiet -> Serial0.end() -> drive P0.19+P0.20 LOW
 *                  (isolate the module; a high UART pin phantom-powers it ~440 uA).
 *   RTC wake       P0.21 (schematic v2). Validated test #5.
 *   Extra pins     AS3933 WUR parked (SPI pins low, WAKE input). Arming P1.04 as
 *                  a second wake source comes after the WUR real-wake test passes
 *                  (see ENABLE_WUR_WAKE below).
 *   Sleep floor    157 uA @ 3.6 V (test #5/#6) - divider-dominated.
 *   Structure      alive-first (board quirk): tiny setup(), heartbeat, then the
 *                  real init a few seconds into loop(). Heavy init in setup()
 *                  wedges the native-USB app.
 *
 * FIRST-FLASH NOTE: if the board is in LoRaWAN mode (factory default), the LoRa
 * init switches it to P2P and REBOOTS ONCE automatically. That's expected - the
 * second boot runs normally.
 *
 * Companion receiver: the project's LoRa ACK receiver (external to this repo -
 * it echoes the devID it hears, so DEVICE_ID below works without receiver changes).
 *
 * On-air TX payload (6 fields):  "<devID3>,<seq>,<lat.6>,<lon.6>,<vbatV.2>,<ts>"
 * ACK payload:                   "ACK,<devID3>,<seq>"
 * ============================================================================
 */

#include <stdint.h>    // force fixed-width types first (RUI3 <time.h> can knock these out)
#include <stddef.h>
#include <Arduino.h>
#include <Wire.h>
#include <TinyGPSPlus.h>
#include "Melopero_RV3028.h"

// ============================================================================
// USER CONFIGURATION
// ============================================================================
constexpr uint32_t GNSS_PERIOD_MS       = 60000UL;    // normal cadence (1 min TEST; deployment ~2 h)
constexpr uint8_t  TX_BUFFER_SIZE  = 1;          // packets to accumulate before TX (max 3)
constexpr uint32_t TX_RETRY_MS          = 300000UL;   // 5 min between TX retries
constexpr uint8_t  MAX_TX_RETRIES       = 3;          // TX attempts before -> undelivered
constexpr uint32_t GNSS_FIX_TIMEOUT_MS  = 60000UL;    // 1 min max per fix attempt
constexpr uint8_t  MAX_GNSS_RETRIES     = 2;          // fix attempts before timestamp-only packet
constexpr uint32_t GNSS_RETRY_WAIT_MS   = 300000UL;   // 5 min sleep between fix retries
constexpr uint32_t POST_FIX_SETTLE_MS   = 3000UL;     // continuous valid readings required
constexpr uint32_t ACK_TIMEOUT_MS       = 8000UL;     // wait for ACK after TX done (v2 as-run)
constexpr uint16_t DEVICE_ID            = 51;         // "051" - ISL node over-air ID

// Arm the AS3933 WUR (P1.04 rising edge) as a SECOND deep-sleep wake source.
// Leave 0 until the WUR real-wake test passes (an unvalidated wake source can
// cause spurious wakes and wreck the duty cycle). RTC wake works either way.
#define ENABLE_WUR_WAKE     0

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
// ISL PIN MAP (schematic v2, hardware-verified in tests #0-#6)
// ============================================================================
#define RTC_INT_PIN         P0_21     // RV-3028 ~INT (wake) - validated test #5
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
#define GPS_BOOT_DELAY_MS   500UL     // ISL-validated power-up settle (tests #3/#6)
#define GPS_POWERDOWN_MS    250UL     // let the TX line go quiet after cutting EN

// ADC - 12-bit, AR_INTERNAL = 2.4 V FS, divider 2.0
#define ADC_SAMPLES         31
#define SAT_REJECT          4090
#define ADC_MIN_VALID       12

// RV-3028 registers/bits
#define TIME_SET_FLAG       0x5A
#define REG_TIMER_VALUE_0   0x0A
#define REG_TIMER_VALUE_1   0x0B
#define REG_STATUS          0x0E
#define REG_CONTROL_1       0x0F
#define REG_CONTROL_2       0x10
#define STATUS_TF           0x08
#define CTRL1_TE            0x04
#define CTRL1_TD_1HZ        0x02
#define CTRL2_TIE           0x10
#define SLEEP_BACKSTOP_MS   3600000UL     // 1 h safety (RTC INT normally wakes us far sooner)

// Flash layout
#define FLASH_MAGIC         0xA5A5CAFEUL
#define FLASH_SCHEMA_VER    1
#define FLASH_OFF_HEADER    0x0000
#define FLASH_OFF_BUFFER    0x0040
#define FLASH_OFF_PENDING   0x0080
#define FLASH_OFF_UNDELIV   0x0100
#define MAX_UNDELIVERED     8

#define WARMUP_MS           3000      // alive-first heartbeat before init

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

static uint8_t  rxBuf[256];
static uint8_t  rxLen  = 0;
static int16_t  rxRssi = 0;
static int8_t   rxSnr  = 0;

// ============================================================================
// Debug over native USB - guarded: a detached/asleep port must never block.
// ============================================================================
#define DBG(...)  do { if (Serial) { Serial.printf(__VA_ARGS__); Serial.flush(); } } while (0)
void say(const char *s) { if (Serial) { Serial.println(s); Serial.flush(); } }

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
// RTC (RV-3028) - wake pin = P0.21
// ============================================================================
void rtcInit()
{
    Wire.begin();
    rtc.initI2C();
    rtc.set24HourMode();
    rtc.writeToRegister(0x35, 0x00);    // backup-domain config (validated tests #1/#5)
    rtc.writeToRegister(0x37, 0x1C);
    if (rtc.readFromRegister(USER_RAM1_ADDRESS) != TIME_SET_FLAG) {
        // >>> EDIT to the correct current time before the first flash <<<
        rtc.setTime(2026, 7, 8, 3, 12, 0, 0);   // (year, month, date, weekday, hh, mm, ss)
        rtc.writeToRegister(USER_RAM1_ADDRESS, TIME_SET_FLAG);
        say("[RTC] Time SET (first boot)");
    }
    rtc.writeToRegister(REG_CONTROL_2, rtc.readFromRegister(REG_CONTROL_2) | CTRL2_TIE);
    pinMode(RTC_INT_PIN, INPUT);        // external 10k pull-up (R7)
}

// Single-shot periodic timer: fire in `seconds`. 12-bit @ 1 Hz -> max 4095 s
// (~68 min); production 2 h cadence needs the 1/60 Hz tick mode (same note as v2).
void rtcSetNextWake(uint32_t seconds)
{
    if (seconds < 1)    seconds = 1;
    if (seconds > 4095) seconds = 4095;
    rtc.writeToRegister(REG_CONTROL_1, rtc.readFromRegister(REG_CONTROL_1) & ~CTRL1_TE);
    rtc.writeToRegister(REG_TIMER_VALUE_0, seconds & 0xFF);
    rtc.writeToRegister(REG_TIMER_VALUE_1, (seconds >> 8) & 0x0F);
    rtc.writeToRegister(REG_STATUS, rtc.readFromRegister(REG_STATUS) & ~STATUS_TF);
    rtc.writeToRegister(REG_CONTROL_1, CTRL1_TE | CTRL1_TD_1HZ);
}

void rtcClearTF()
{
    rtc.writeToRegister(REG_STATUS, rtc.readFromRegister(REG_STATUS) & ~STATUS_TF);
}

// Unix seconds, integer math only (H. Hinnant civil-calendar algorithm; no <time.h>).
uint32_t rtcNowUnix()
{
    int Y = rtc.getYear();
    int M = rtc.getMonth();
    int D = rtc.getDate();
    int h = rtc.getHour();
    int m = rtc.getMinute();
    int s = rtc.getSecond();

    int      y   = Y - (M <= 2 ? 1 : 0);
    int      era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long     days = (long)era * 146097 + (long)doe - 719468;

    return (uint32_t)(((days * 24L + h) * 60L + m) * 60L + s);
}

// ============================================================================
// DEEP SLEEP driver (rules: docs/ISL_DeepSleep_Notes.md; floor 157 uA @ 3.6 V)
// ============================================================================
void deepSleep(uint32_t seconds)
{
    DBG("[SLEEP] %lu s\r\n", (unsigned long)seconds);
    rtcSetNextWake(seconds);
    rtcWokeUs = false;
    clearFPU();
    uint32_t before = millis();
    api.system.sleep.all(SLEEP_BACKSTOP_MS);
    uint32_t slept  = millis() - before;
    rtcClearTF();
    DBG("[WAKE] %s after ~%lu ms\r\n",
        rtcWokeUs ? "RTC P0.21" : "other/backstop", (unsigned long)slept);
}

// ============================================================================
// BATTERY (AIN7/P0.31) - median+reject flow.
// ISL's low-impedance divider doesn't strictly need it, but it is harmless and
// keeps the firmware identical. Read while the board is quiet.
// ============================================================================
static int cmp_int(const void *a, const void *b) { return (*(const int *)a) - (*(const int *)b); }

uint16_t readVbat_mV()
{
    pinMode(BATT_ADC_PIN, INPUT);
    analogReadResolution(12);
    analogReference(AR_INTERNAL);
    int valid[ADC_SAMPLES]; int m = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int r = analogRead(BATT_ADC_PIN);
        if (r > 0 && r < SAT_REJECT) valid[m++] = r;
        delay(2);
    }
    if (m < ADC_MIN_VALID) return 0;
    qsort(valid, m, sizeof(int), cmp_int);
    int med = valid[m/2];
    // Vbat_mV = med * 2.4 * 2.0 * 1000 / 4095 = med * 4800 / 4095  (integer only)
    uint32_t vbat_mV = ((uint32_t)med * 4800UL) / 4095UL;
    return (uint16_t)vbat_mV;
}

// ============================================================================
// GPS - Serial0/UART1, EN=P1.02 active-low. Teardown validated in test #6.
// The external L76X hot-starts from its own backup battery: no reset pin dance.
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
    // Order matters (tests #6 v1->v3, docs/ISL_DeepSleep_Notes.md rules 3+4):
    digitalWrite(GPS_EN_PIN, HIGH);                                       // 1. cut power first
    delay(GPS_POWERDOWN_MS);                                              // 2. TX line quiets
    Serial0.end();                                                        // 3. release UART1
    pinMode(GPS_UART_RX_PIN, OUTPUT); digitalWrite(GPS_UART_RX_PIN, LOW); // 4. isolate module:
    pinMode(GPS_UART_TX_PIN, OUTPUT); digitalWrite(GPS_UART_TX_PIN, LOW); //    no phantom power
    say("[GPS] VCC OFF, UART pins driven LOW (module isolated)");
}

// Acquire fix with settling (on Serial0).
bool gpsAcquire(uint32_t timeout_ms, int32_t *latE6, int32_t *lonE6, uint16_t *sats, uint32_t *ttff_ms)
{
    TinyGPSPlus gps;
    uint32_t start   = millis();
    uint32_t firstAt = 0;
    bool     validSeen = false;

    while ((millis() - start) < timeout_ms) {
        while (Serial0.available()) gps.encode(Serial0.read());

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
            validSeen = false;   // fix dropped - reset settling window
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
// LoRa P2P - TX + ACK RX
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

// 6-field on-air string, integer math only.
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
    api.lora.precv(0);   // ensure radio leaves RX
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
    say("== COLLECT ==");

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
                (unsigned long)(GNSS_RETRY_WAIT_MS / 1000));
            deepSleep(GNSS_RETRY_WAIT_MS / 1000);
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

    queuePush(buffer, header.bufferCount, p, TX_BUFFER_SIZE);
    saveBuffer();
    saveHeader();
    DBG("[BUF] added seq=%lu (%s), buf=%u/%u\r\n",
        (unsigned long)p.seq, haveFix ? "fix" : "no-fix",
        header.bufferCount, TX_BUFFER_SIZE);
}

void doTransmitPass()
{
    DBG("[TX pass] pending=%u  buffer=%u\r\n", header.pendingCount, header.bufferCount);
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

    api.lora.precv(0);   // radio idle before sleep (deep-sleep rule 1)

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
                header.pendingCount, (unsigned long)(TX_RETRY_MS / 1000));
            deepSleep(TX_RETRY_MS / 1000);
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
    say("ISL Board - PRODUCTION v1");
    say("========================================================");

    // Sleep-floor prerequisites (docs/ISL_DeepSleep_Notes.md rule 1)
    api.ble.stop();
    NVIC_DisableIRQ(FPU_IRQn);

    // Peripherals
    rtcInit();

    // GPS to safe idle: EN off, both UART pins LOW (isolation - rule 4)
    pinMode(GPS_EN_PIN, OUTPUT);      digitalWrite(GPS_EN_PIN, HIGH);
    pinMode(GPS_UART_RX_PIN, OUTPUT); digitalWrite(GPS_UART_RX_PIN, LOW);
    pinMode(GPS_UART_TX_PIN, OUTPUT); digitalWrite(GPS_UART_TX_PIN, LOW);

    // WUR pins parked (rule 5); module keeps listening on its own
    pinMode(WUR_CLK_PIN,  OUTPUT); digitalWrite(WUR_CLK_PIN,  LOW);
    pinMode(WUR_MOSI_PIN, OUTPUT); digitalWrite(WUR_MOSI_PIN, LOW);
    pinMode(WUR_CS_PIN,   OUTPUT); digitalWrite(WUR_CS_PIN,   LOW);
    pinMode(WUR_MISO_PIN, INPUT);
    pinMode(WUR_WAKE_PIN, INPUT);

    // Wake source(s)
    api.system.sleep.setup(RUI_WAKEUP_FALLING_EDGE, RTC_INT_PIN);   // RTC (validated)
#if ENABLE_WUR_WAKE
    api.system.sleep.setup(RUI_WAKEUP_RISING_EDGE, WUR_WAKE_PIN);   // WUR (after real-wake test)
#endif
    api.system.sleep.registerWakeupCallback(onRtcWake);

    // Persistent state
    flashLoadAll();

    // LoRa early so a first-flash mode switch reboots BEFORE we start collecting.
    loraConfigureOnce();

    DBG("[CFG] id=%03u  GNSS period=%lu s  buffer=%u  TX retry=%lu s  fix timeout=%lu s  WUR wake=%d\r\n",
        (unsigned)DEVICE_ID,
        (unsigned long)(GNSS_PERIOD_MS / 1000),
        TX_BUFFER_SIZE,
        (unsigned long)(TX_RETRY_MS / 1000),
        (unsigned long)(GNSS_FIX_TIMEOUT_MS / 1000),
        ENABLE_WUR_WAKE);
}

void setup()
{
    Serial.begin(115200);                       // native USB-C CDC
    { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
    delay(300);
    say("ISL Production v1 boot. Heartbeat, then init...");
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
    // 1) Collect (battery + GNSS + build packet)
    doCollect();

    // 2) TX + ACK cycle if the buffer is full or retries are pending
    bool triggerTx = (header.bufferCount >= TX_BUFFER_SIZE) || (header.pendingCount > 0);
    if (triggerTx) {
        doTransmitWithRetries();
    }

    // 3) Deep-sleep until the next GNSS period (RTC P0.21 wakes us)
    DBG("== IDLE deep-sleep %lu s ==\r\n", (unsigned long)(GNSS_PERIOD_MS / 1000));
    deepSleep(GNSS_PERIOD_MS / 1000);
}
