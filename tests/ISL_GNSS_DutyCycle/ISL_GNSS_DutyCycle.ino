/*
 * ISL Board (RAK_feather) - GNSS duty cycle + deep sleep  [test #6]  (alive-first)
 *
 * Cycle:  GPS ON -> search up to GPS_SEARCH_MS (or until a fix) -> GPS OFF
 *         -> deep-sleep SLEEP_SECONDS (RTC wake on P0.21) -> repeat.
 *
 * Fuses two validated pieces:
 *   - GNSS on Serial0/UART1 (P0.19/P0.20), EN=P1.02 active-LOW, 9600 RAK_CUSTOM_MODE
 *   - Deep-sleep path from ISL_DeepSleep_Baseline (api.ble.stop + FPU off, clearFPU,
 *     RV-3028 periodic timer, api.system.sleep.all).  Baseline floor = 157 uA @ 3.6 V.
 *
 * POWER NOTE: GPS power (P1.02) is CUT during sleep, so each search is a COLD start
 * (J6 exposes no backup/reset line). Cold TTFF outdoors ~30 s, so GPS_SEARCH_MS=30 s
 * is workable but tight - raise it if you don't get fixes. Expected current shape:
 * ~30 s at ~25-40 mA (GPS searching) then 60 s at ~157 uA (sleep), repeating.
 *
 * USB NOTE: native USB `Serial` drops in deep sleep. You'll see the first cycle on
 * USB, then the port disappears at the first [SLEEP]; measure the rest on battery.
 * All USB prints are guarded with if(Serial) so a detached port never blocks.
 *
 * Vbat for deep-sleep measurements = 3.6 V (battery nominal).
 */

#include <stdint.h>
#include <stddef.h>
#include <Arduino.h>
#include <Wire.h>
#include <TinyGPSPlus.h>
#include "Melopero_RV3028.h"

// ---- ISL pin map (schematic v2) ----
#define RTC_INT_PIN     P0_21     // RV-3028 ~INT wake
#define GPS_EN_PIN      P1_02     // L76K_EN, active-LOW -> LOW = ON, HIGH = OFF
#define GPS_UART_RX_PIN P0_19     // UART1 RX (Serial0) - drive LOW before sleep
#define GPS_UART_TX_PIN P0_20     // UART1 TX (Serial0) - drive LOW before sleep
#define WUR_CLK         P0_03
#define WUR_MOSI        P0_30
#define WUR_MISO        P0_29
#define WUR_CS          P0_26
#define WUR_WAKE        P1_04
#define BATT_ADC_PIN    P0_31

// ---- behaviour ----
#define GPS_SEARCH_MS       30000UL       // search window per cycle (or until fix)
#define SLEEP_SECONDS       60UL          // deep sleep between searches
#define GPS_BOOT_MS         500UL         // let the L76K power up before reading
#define GPS_POWERDOWN_MS    250UL         // let the TX line go quiet after cutting power
#define GPS_SETTLE_MS       1500UL        // require this much continuous valid fix
#define GPS_BAUD            9600
#define SLEEP_BACKSTOP_MS   3600000UL
#define WARMUP_MS           3000

// ---- RV-3028 periodic-timer regs (from production v2) ----
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

Melopero_RV3028 rtc;
static volatile bool rtcWoke = false;
void onRtcWake() { rtcWoke = true; }

void clearFPU()
{
    __set_FPSCR(__get_FPSCR() & ~0x0000009Fu);
    (void)__get_FPSCR();
    NVIC_ClearPendingIRQ(FPU_IRQn);
}

// USB print helpers (guarded so a detached port never blocks)
void say(const char *s) { if (Serial) { Serial.println(s); Serial.flush(); } }

// ---- RTC ----
void rtcInit()
{
    Wire.begin();
    rtc.initI2C();
    rtc.set24HourMode();
    rtc.writeToRegister(0x35, 0x00);
    rtc.writeToRegister(0x37, 0x1C);
    if (rtc.readFromRegister(USER_RAM1_ADDRESS) != TIME_SET_FLAG) {
        rtc.setTime(2026, 7, 8, 3, 12, 0, 0);
        rtc.writeToRegister(USER_RAM1_ADDRESS, TIME_SET_FLAG);
    }
    rtc.writeToRegister(REG_CONTROL_2, rtc.readFromRegister(REG_CONTROL_2) | CTRL2_TIE);
    pinMode(RTC_INT_PIN, INPUT);
}
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
void rtcClearTF() { rtc.writeToRegister(REG_STATUS, rtc.readFromRegister(REG_STATUS) & ~STATUS_TF); }

// Park non-GPS peripherals low (GPS handled per cycle).
void idlePeripherals()
{
    pinMode(WUR_CLK,  OUTPUT); digitalWrite(WUR_CLK,  LOW);
    pinMode(WUR_MOSI, OUTPUT); digitalWrite(WUR_MOSI, LOW);
    pinMode(WUR_CS,   OUTPUT); digitalWrite(WUR_CS,   LOW);
    pinMode(WUR_MISO, INPUT);
    pinMode(WUR_WAKE, INPUT);
    pinMode(BATT_ADC_PIN, INPUT);
}

// ---- GPS session: power on, search up to timeout or until a settled fix ----
bool gpsSession()
{
    if (Serial) { Serial.printf("== GPS session: power ON, searching up to %lu s ==\r\n",
                                (unsigned long)(GPS_SEARCH_MS / 1000)); Serial.flush(); }
    pinMode(GPS_EN_PIN, OUTPUT); digitalWrite(GPS_EN_PIN, LOW);   // active-low ON
    delay(GPS_BOOT_MS);
    Serial0.begin(GPS_BAUD, RAK_CUSTOM_MODE);

    TinyGPSPlus gps;
    uint32_t start = millis();
    uint32_t firstValidAt = 0;
    bool     validSeen = false, gotFix = false;
    uint32_t lastLog = 0;

    while ((millis() - start) < GPS_SEARCH_MS) {
        while (Serial0.available()) gps.encode(Serial0.read());

        bool ok = gps.location.isValid() && gps.location.age() < 2000 &&
                  gps.date.isValid() && gps.time.isValid();
        if (ok) {
            if (!validSeen) { validSeen = true; firstValidAt = millis(); }
            if ((millis() - firstValidAt) >= GPS_SETTLE_MS) { gotFix = true; break; }
        } else if (validSeen) {
            validSeen = false;   // fix dropped, reset settle window
        }

        // progress ping every 5 s
        if (Serial && (millis() - lastLog) >= 5000) {
            lastLog = millis();
            Serial.printf("   ...%lus  sats=%d  %s\r\n",
                          (unsigned long)((millis() - start) / 1000),
                          gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                          validSeen ? "acquiring" : "searching");
            Serial.flush();
        }
    }

    if (Serial) {
        if (gotFix) {
            int32_t latE6 = (int32_t)(gps.location.lat() * 1e6);
            int32_t lonE6 = (int32_t)(gps.location.lng() * 1e6);
            const char *ls = latE6 < 0 ? "-" : "", *os = lonE6 < 0 ? "-" : "";
            uint32_t la = latE6 < 0 ? -latE6 : latE6, lo = lonE6 < 0 ? -lonE6 : lonE6;
            Serial.printf("   >>> FIX in %lu ms  sats=%d  lat=%s%lu.%06lu lon=%s%lu.%06lu\r\n",
                          (unsigned long)(millis() - start),
                          gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                          ls, (unsigned long)(la / 1000000UL), (unsigned long)(la % 1000000UL),
                          os, (unsigned long)(lo / 1000000UL), (unsigned long)(lo % 1000000UL));
        } else {
            Serial.printf("   >>> NO FIX after %lu s (cold start / indoors?)\r\n",
                          (unsigned long)(GPS_SEARCH_MS / 1000));
        }
        Serial.flush();
    }

    // Power the GPS off, in the RIGHT ORDER, and ISOLATE the module for sleep.
    // Two separate lessons from the ISL deep-sleep tests:
    //   (a) v4 wake bug: GPS streams on its VCC cap for a few ms after power-off;
    //       edges on the UART RX pin wake sleep.all() instantly -> cut power first,
    //       let the TX line go quiet, THEN release the UART.
    //   (b) leakage: the GPS is an EXTERNAL module with its OWN backup battery, so
    //       once L76K_EN is off its main VCC is fully dead. Any UART pin left HIGH
    //       (INPUT_PULLUP on RX, or TX left driven high by end()) sources current
    //       through the module's I/O ESD clamps into that dead rail - phantom-
    //       powering/charging it (~440 uA tail). Driving BOTH UART lines LOW
    //       isolates the module (drive both UART control pins LOW). Driving RX low
    //       also kills the UART-RX pin-sense,
    //       so it fixes the instant-wake too.
    digitalWrite(GPS_EN_PIN, HIGH);                                    // cut power first
    delay(GPS_POWERDOWN_MS);                                           // let TX go quiet
    Serial0.end();                                                    // release UART1
    pinMode(GPS_UART_RX_PIN, OUTPUT); digitalWrite(GPS_UART_RX_PIN, LOW);  // no back-feed via RX
    pinMode(GPS_UART_TX_PIN, OUTPUT); digitalWrite(GPS_UART_TX_PIN, LOW);  // no back-feed via TX
    return gotFix;
}

// ---- deep sleep SLEEP_SECONDS ----
void deepSleep(uint32_t seconds)
{
    if (Serial) { Serial.printf("[SLEEP] %lu s ...\r\n", (unsigned long)seconds); Serial.flush(); }
    rtcSetNextWake(seconds);
    rtcWoke = false;
    clearFPU();
    uint32_t before = millis();
    api.system.sleep.all(SLEEP_BACKSTOP_MS);
    uint32_t slept = millis() - before;
    rtcClearTF();
    if (Serial) { Serial.printf("[WAKE] %s after ~%lu ms\r\n",
                                rtcWoke ? "RTC P0.21" : "backstop", (unsigned long)slept); Serial.flush(); }
}

enum Phase { WARMUP, INIT, CYCLE };
Phase phase = WARMUP;

void setup()
{
    Serial.begin(115200);
    { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
    delay(300);
    say("ISL GNSS duty-cycle (#6): 30 s search / 60 s sleep. Heartbeat, then start...");
}

void loop()
{
    static uint32_t t0 = millis();
    static uint32_t lastHb = 0;

    if (phase == WARMUP) {
        if (millis() - lastHb >= 500) {
            lastHb = millis();
            if (Serial) { Serial.printf("[alive] %lu ms\r\n", (unsigned long)(millis() - t0)); Serial.flush(); }
        }
        if (millis() - t0 >= WARMUP_MS) phase = INIT;
        return;
    }

    if (phase == INIT) {
        say("STEP: sleep-floor prerequisites + RTC + park peripherals...");
        api.ble.stop();
        NVIC_DisableIRQ(FPU_IRQn);
        rtcInit();
        idlePeripherals();
        pinMode(GPS_EN_PIN, OUTPUT); digitalWrite(GPS_EN_PIN, HIGH);   // GPS starts OFF
        api.system.sleep.setup(RUI_WAKEUP_FALLING_EDGE, RTC_INT_PIN);
        api.system.sleep.registerWakeupCallback(onRtcWake);
        say("READY. Starting duty cycle. (USB drops at first [SLEEP].)");
        phase = CYCLE;
        return;
    }

    // CYCLE: one GPS search + one deep sleep per loop pass.
    gpsSession();
    deepSleep(SLEEP_SECONDS);
}
