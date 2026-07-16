/*
 * ISL Board (RAK_feather) - Seed the RTC from GNSS UTC time  [test #7]  (alive-first)
 *
 * Goal: prove we can set the RV-3028's FIRST time from the L76K's acquired UTC
 * timestamp instead of a hardcoded compile-time date. Flow:
 *   power GPS -> read NMEA (TinyGPSPlus) -> wait for a VALID + SANE UTC date/time
 *   -> rtc.setTime(that UTC) -> power GPS off -> RTC ticks on its own, matching GPS.
 *
 * WHY it can be fast: GPS decodes time from the satellite nav message, usually
 * BEFORE a full position fix - so we require date+time valid, NOT a location fix.
 * SANITY GUARD: require year >= 2025 and age < 2 s, to reject the module's
 * power-on default date (e.g. 2016/1980) before it is really time-synced.
 *
 * Time is UTC (no timezone / no leap offset) - the RTC holds UTC, which is what
 * the production unix-epoch packet field wants. RTC init latency is < ~1 s.
 *
 * Production integration (not done here): on the FIRST GPS session, if the RTC
 * was never GPS-synced (USER_RAM flag), call this once; optionally re-sync every
 * N cycles to trim RV-3028 drift (GPS = master clock).
 *
 *   Serial (USB-C CDC) -> PC (115200)
 *   GPS = Serial0/UART1 (P0.19 RX / P0.20 TX), EN=P1.02 active-low, 9600
 *   RTC = RV-3028 on I2C (SDA P0.13 / SCL P0.14) @ 0x52
 */

#include <stdint.h>
#include <stddef.h>
#include <Arduino.h>
#include <Wire.h>
#include <TinyGPSPlus.h>
#include "Melopero_RV3028.h"

#define GPS_EN_PIN          P1_02
#define GPS_UART_RX_PIN     P0_19
#define GPS_UART_TX_PIN     P0_20
#define GPS_BAUD            9600
#define GPS_BOOT_DELAY_MS   500UL
#define GPS_POWERDOWN_MS    250UL
#define TIME_TIMEOUT_MS     180000UL   // give up seeking GPS time after 3 min
#define MIN_VALID_YEAR      2025        // reject module default date before sync
#define WARMUP_MS           3000UL

// force a fresh GPS-set every run for testing (production would gate on this flag)
#define FORCE_SET_EACH_RUN  1
#define TIME_SET_FLAG       0x5A

Melopero_RV3028 rtc;
TinyGPSPlus gps;

void say(const char *s) { Serial.println(s); Serial.flush(); }

// Day of week (0=Sunday) via Sakamoto - cosmetic for the RV-3028 (timekeeping
// doesn't depend on it), but set it correctly anyway.
static uint8_t dow(int y, int m, int d)
{
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    return (uint8_t)((y + y/4 - y/100 + y/400 + t[m-1] + d) % 7);
}

// Unix seconds from a UTC Y/M/D h:m:s, integer only (H. Hinnant civil algorithm).
static uint32_t toUnix(int Y,int M,int D,int h,int m,int s)
{
    int y = Y - (M <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y-399) / 400;
    unsigned yoe = (unsigned)(y - era*400);
    unsigned doy = (153*(M + (M>2 ? -3 : 9)) + 2)/5 + D - 1;
    unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
    long days = (long)era*146097 + (long)doe - 719468;
    return (uint32_t)(((days*24L + h)*60L + m)*60L + s);
}

void gpsPowerOn()
{
    pinMode(GPS_EN_PIN, OUTPUT); digitalWrite(GPS_EN_PIN, LOW);   // active-low ON
    delay(GPS_BOOT_DELAY_MS);
    Serial0.begin(GPS_BAUD, RAK_CUSTOM_MODE);
}
void gpsPowerOff()
{
    digitalWrite(GPS_EN_PIN, HIGH);
    delay(GPS_POWERDOWN_MS);
    Serial0.end();
    pinMode(GPS_UART_RX_PIN, OUTPUT); digitalWrite(GPS_UART_RX_PIN, LOW);
    pinMode(GPS_UART_TX_PIN, OUTPUT); digitalWrite(GPS_UART_TX_PIN, LOW);
}

// Wait for a valid+sane UTC time from GPS. Returns true and fills the fields.
bool gpsAcquireTime(int *Y,int *M,int *D,int *h,int *m,int *s,uint16_t *sats,uint32_t *elapsed)
{
    uint32_t start = millis(), lastLog = 0;
    while ((millis() - start) < TIME_TIMEOUT_MS) {
        while (Serial0.available()) gps.encode(Serial0.read());

        // FULL range check (hardened): TinyGPSPlus checksums sentences but does
        // NOT range-check fields - a corrupted-but-parsed RMC can yield e.g.
        // "2088-31-19" (seen live in production v3 bring-up). Production v3 also
        // requires two consistent readings 2 s apart before seeding.
        bool timeOk = gps.date.isValid() && gps.time.isValid() &&
                      gps.date.age() < 2000 && gps.time.age() < 2000 &&
                      gps.date.year() >= MIN_VALID_YEAR && gps.date.year() <= 2044 &&
                      gps.date.month() >= 1 && gps.date.month() <= 12 &&
                      gps.date.day()   >= 1 && gps.date.day()   <= 31 &&
                      gps.time.hour() <= 23 && gps.time.minute() <= 59 &&
                      gps.time.second() <= 59;
        if (timeOk) {
            *Y = gps.date.year(); *M = gps.date.month(); *D = gps.date.day();
            *h = gps.time.hour(); *m = gps.time.minute(); *s = gps.time.second();
            *sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
            *elapsed = millis() - start;
            return true;
        }
        if (Serial && (millis() - lastLog) >= 3000) {
            lastLog = millis();
            Serial.printf("   ...%lus  sats=%d  timeValid=%d dateYear=%d (need >=%d)\r\n",
                          (unsigned long)((millis()-start)/1000),
                          gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                          gps.time.isValid() ? 1 : 0,
                          gps.date.isValid() ? (int)gps.date.year() : 0,
                          MIN_VALID_YEAR);
            Serial.flush();
        }
    }
    *elapsed = millis() - start;
    return false;
}

enum Phase { WARMUP, INIT, ACQUIRE, RUN };
Phase phase = WARMUP;

void setup()
{
    Serial.begin(115200);
    { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
    delay(300);
    say("ISL RTC<-GNSS time-set (test #7). Heartbeat, then seek GPS UTC time...");
}

void loop()
{
    static uint32_t t0 = millis();
    static uint32_t lastHb = 0;

    if (phase == WARMUP) {
        if (millis() - lastHb >= 500) { lastHb = millis(); Serial.printf("[alive] %lu ms\r\n", (unsigned long)(millis()-t0)); Serial.flush(); }
        if (millis() - t0 >= WARMUP_MS) phase = INIT;
        return;
    }

    if (phase == INIT) {
        say("STEP: RTC init ...");
        Wire.begin();
        rtc.initI2C();
        rtc.set24HourMode();
        rtc.writeToRegister(0x35, 0x00);
        rtc.writeToRegister(0x37, 0x1C);

        uint8_t flag = rtc.readFromRegister(USER_RAM1_ADDRESS);
        Serial.printf("  RTC flag=0x%02X  current RTC time: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                      flag, rtc.getYear(), rtc.getMonth(), rtc.getDate(),
                      rtc.getHour(), rtc.getMinute(), rtc.getSecond());
        Serial.flush();

        if (!FORCE_SET_EACH_RUN && flag == TIME_SET_FLAG) {
            say("  RTC already GPS-set (flag present) - skipping GPS, going to RUN.");
            phase = RUN; return;
        }
        say("STEP: power GPS, seeking a valid+sane UTC time (no position fix needed)...");
        gpsPowerOn();
        phase = ACQUIRE;
        return;
    }

    if (phase == ACQUIRE) {
        int Y,M,D,h,m,s; uint16_t sats; uint32_t el;
        if (gpsAcquireTime(&Y,&M,&D,&h,&m,&s,&sats,&el)) {
            Serial.printf("  >>> GPS UTC acquired in %lu ms  sats=%u : %04d-%02d-%02d %02d:%02d:%02d\r\n",
                          (unsigned long)el, sats, Y,M,D,h,m,s);
            gpsPowerOff();

            uint8_t wd = dow(Y,M,D);
            // Melopero_RV3028::setTime order is (year, month, WEEKDAY, DATE, hh, mm, ss)
            // - weekday BEFORE date. (The old hardcoded calls had these swapped, which
            //   is why the day-of-month came out wrong; harmless there, fatal here.)
            rtc.setTime((uint16_t)Y, (uint8_t)M, wd, (uint8_t)D, (uint8_t)h, (uint8_t)m, (uint8_t)s);
            rtc.writeToRegister(USER_RAM1_ADDRESS, TIME_SET_FLAG);
            Serial.printf("  >>> RTC SET from GPS (weekday=%u). unix=%lu\r\n",
                          wd, (unsigned long)toUnix(Y,M,D,h,m,s));
            Serial.flush();
        } else {
            Serial.printf("  >>> NO GPS time after %lu s (indoors? cold start?) - RTC left as-is.\r\n",
                          (unsigned long)(el/1000));
            gpsPowerOff();
        }
        say("Now ticking the RTC every second (should match the GPS time + keep counting):");
        phase = RUN;
        return;
    }

    // RUN: show the RTC advancing on its own (GPS off), with unix epoch.
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    int Y=rtc.getYear(),M=rtc.getMonth(),D=rtc.getDate();
    int h=rtc.getHour(),m=rtc.getMinute(),s=rtc.getSecond();
    Serial.printf("%04d-%02d-%02d %02d:%02d:%02d UTC   unix=%lu\r\n",
                  Y,M,D,h,m,s, (unsigned long)toUnix(Y,M,D,h,m,s));
    Serial.flush();
}
