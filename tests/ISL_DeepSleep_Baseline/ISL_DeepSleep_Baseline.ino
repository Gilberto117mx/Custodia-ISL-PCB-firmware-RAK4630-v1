/*
 * ISL Board (RAK_feather) - DEEP-SLEEP CURRENT BASELINE  [test #5]  (alive-first)
 *
 * Purpose: establish the ISL board's deep-sleep floor. Mirrors:
 *   - api.ble.stop() + NVIC_DisableIRQ(FPU_IRQn)      (setup)
 *   - clearFPU() immediately before sleeping
 *   - all peripherals driven to a defined low-power idle
 *   - RV-3028 periodic-timer wake via external INT (falling edge)
 *   - api.system.sleep.all() as the actual sleep call
 *
 * ISL baseline contributors:
 *   + Battery divider (1 MOhm/1 MOhm + C17) draws ~1.8 uA from 3.6 V, always-on
 *     (NOTE: an earlier diagram wrongly showed 10k/10k - see docs correction).
 *   + AS3933 WUR sits powered on 3.3 V listening (~2.7 uA typ) - left in its default
 *     state here (not disabled), so it's included in the baseline.
 *   + RT9080-33 LDO quiescent current (nRF + SX1262 run off the 3.3 V rail).
 *   Measured floor here was ~157 uA @ 3.6 V; later traced to an AIN7 GPIO crowbar
 *   (our own pinMode), NOT the divider/LDO - fixed in production v7 (34 uA). See
 *   docs/ISL_DeepSleep_Notes.md.
 *
 * ISL pin map (schematic v2):
 *   RTC ~INT (wake) = P0.21   (was P1.03 in v1)   <- this test also VALIDATES it
 *   L76K_EN (GPS)   = P1.02, active-LOW -> HIGH = GPS OFF
 *   AS3933 SPI      = CLK P0.03 / MOSI P0.30 / MISO P0.29 / CS P0.26 / WAKE P1.04
 *   BATT sense      = P0.31 (AIN7)
 *   Debug           = native USB-C `Serial` (DROPS in deep sleep - see below)
 *
 * =====================  HOW TO MEASURE  =====================
 * 1) Flash, open the USB monitor. You will see the heartbeat + STEP lines up to
 *    "[SLEEP] ..." and then the COM port DISAPPEARS. That is correct: native USB
 *    powers down in deep sleep. (This board logs over native USB only, which drops
 *    in sleep; measure on battery.)
 * 2) Disconnect USB. Power the board from the BATTERY connector through a DC
 *    current meter / power analyzer (Nordic PPK2, Otii, or a good uA meter) in
 *    series with the battery +.
 * 3) Read the floor. With SLEEP_SECONDS below, you'll see a brief wake blip every
 *    ~N seconds (confirms the P0.21 RTC wake works) and the flat floor between
 *    blips = the deep-sleep baseline. Quote it WITH the battery voltage.
 *    (Set SLEEP_SECONDS large, e.g. 300, for a long flat trace.)
 * ============================================================
 */

#include <stdint.h>    // fixed-width types first (RUI3 <time.h> can knock these out)
#include <stddef.h>
#include <Arduino.h>
#include <Wire.h>
#include "Melopero_RV3028.h"

// ---- ISL pin map (schematic v2) ----
#define RTC_INT_PIN     P0_21     // RV-3028 ~INT wake
#define GPS_EN_PIN      P1_02     // L76K_EN, active-LOW -> HIGH = OFF
#define WUR_CLK         P0_03
#define WUR_MOSI        P0_30
#define WUR_MISO        P0_29
#define WUR_CS          P0_26
#define WUR_WAKE        P1_04
#define BATT_ADC_PIN    P0_31

// ---- timing ----
#define SLEEP_SECONDS       30UL          // wake cadence (raise for a flatter trace)
#define SLEEP_BACKSTOP_MS   3600000UL     // 1 h safety (RTC INT wakes us far sooner)
#define WARMUP_MS           3000

// ---- RV-3028 periodic-timer regs (identical to production v2) ----
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

void say(const char *s) { if (Serial) { Serial.println(s); Serial.flush(); } }

// ---- RTC bring-up + single-shot periodic timer (from production v2) ----
void rtcInit()
{
    Wire.begin();
    rtc.initI2C();
    rtc.set24HourMode();
    rtc.writeToRegister(0x35, 0x00);           // CR1220-safe backup config
    rtc.writeToRegister(0x37, 0x1C);
    if (rtc.readFromRegister(USER_RAM1_ADDRESS) != TIME_SET_FLAG) {
        rtc.setTime(2026, 7, 8, 3, 12, 0, 0);
        rtc.writeToRegister(USER_RAM1_ADDRESS, TIME_SET_FLAG);
    }
    rtc.writeToRegister(REG_CONTROL_2, rtc.readFromRegister(REG_CONTROL_2) | CTRL2_TIE);
    pinMode(RTC_INT_PIN, INPUT);               // external 10k pull-up (R7)
}

void rtcSetNextWake(uint32_t seconds)
{
    if (seconds < 1)    seconds = 1;
    if (seconds > 4095) seconds = 4095;        // 12-bit @ 1 Hz -> max ~68 min
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

// Park every peripheral in a defined low-power state.
void idlePeripherals()
{
    // GPS OFF: active-LOW enable -> drive HIGH so the Q1 P-FET is off.
    pinMode(GPS_EN_PIN, OUTPUT); digitalWrite(GPS_EN_PIN, HIGH);

    // AS3933 MCU-side SPI pins to defined states (module left in default listening
    // mode; its ~2.7 uA is part of this baseline). Avoids floating-input leakage.
    pinMode(WUR_CLK,  OUTPUT); digitalWrite(WUR_CLK,  LOW);
    pinMode(WUR_MOSI, OUTPUT); digitalWrite(WUR_MOSI, LOW);
    pinMode(WUR_CS,   OUTPUT); digitalWrite(WUR_CS,   LOW);   // active-HIGH CS idle = LOW
    pinMode(WUR_MISO, INPUT);
    pinMode(WUR_WAKE, INPUT);

    // Battery divider is always-on hardware; P0.31 is just the sense tap.
    pinMode(BATT_ADC_PIN, INPUT);
}

enum Phase { WARMUP, INIT, SLEEP };
Phase phase = WARMUP;

void setup()
{
    Serial.begin(115200);
    { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
    delay(300);
    say("ISL deep-sleep baseline (#5). Heartbeat, then init + sleep...");
}

void loop()
{
    static uint32_t t0 = millis();
    static uint32_t lastHb = 0;

    if (phase == WARMUP)
    {
        if (millis() - lastHb >= 500) {
            lastHb = millis();
            if (Serial) { Serial.printf("[alive] %lu ms\r\n", (unsigned long)(millis() - t0)); Serial.flush(); }
        }
        if (millis() - t0 >= WARMUP_MS) phase = INIT;
        return;
    }

    if (phase == INIT)
    {
        say("STEP: sleep-floor prerequisites (api.ble.stop, FPU IRQ off)...");
        api.ble.stop();
        NVIC_DisableIRQ(FPU_IRQn);

        say("STEP: RTC init (wake INT on P0.21) + park peripherals...");
        rtcInit();
        idlePeripherals();

        say("STEP: arm deep-sleep wake = RTC INT falling edge on P0.21...");
        api.system.sleep.setup(RUI_WAKEUP_FALLING_EDGE, RTC_INT_PIN);
        api.system.sleep.registerWakeupCallback(onRtcWake);

        say("READY. Entering periodic deep sleep. NOTE: USB drops now - to measure,");
        say("power from BATTERY via a current meter and read the floor (see header).");
        phase = SLEEP;
        return;
    }

    // SLEEP: reprogram RTC, drop to deep sleep, wake on P0.21, repeat.
    if (Serial) { Serial.printf("[SLEEP] %lu s ...\r\n", (unsigned long)SLEEP_SECONDS); Serial.flush(); }
    rtcSetNextWake(SLEEP_SECONDS);
    rtcWoke = false;
    clearFPU();
    uint32_t before = millis();
    api.system.sleep.all(SLEEP_BACKSTOP_MS);
    uint32_t slept = millis() - before;
    rtcClearTF();
    if (Serial) {
        Serial.printf("[WAKE] %s after ~%lu ms\r\n",
                      rtcWoke ? "RTC P0.21" : "backstop", (unsigned long)slept);
        Serial.flush();
    }
}
