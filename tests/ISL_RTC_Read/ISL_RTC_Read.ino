/*
 * ISL Board (RAK_feather) - RV-3028 RTC  [test #1 - v2 ALIVE-FIRST diagnostic]
 *
 * The app boots (RUI3 prints "Current Work Mode: LoRaWAN") but then faults during
 * RTC init and the board drops back into the SDFU bootloader (COM50). This version
 * proves the app is alive first (heartbeat 3 s), then steps through each RTC call
 * with a flushed marker so the LAST line tells us exactly which call crashes.
 *
 *   Serial (USB-C CDC, COM50) -> PC (115200)
 *   Wire -> I2C SDA=P0.13, SCL=P0.14  (RV-3028 @ 0x52, confirmed by test #0)
 *
 * Open COM50, press reset, and report the LAST line you see.
 */

#include <Wire.h>
#include "Melopero_RV3028.h"

Melopero_RV3028 rtc;
#define TIME_SET_FLAG   0x5A

void say(const char *s) { Serial.println(s); Serial.flush(); }

enum Phase { WARMUP, INIT, RUN };
Phase phase = WARMUP;

void setup()
{
  Serial.begin(115200);
  { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
  delay(300);
  say("ISL RTC v2 (alive-first). Boot OK. Heartbeat 3 s, then RTC init...");
}

void loop()
{
  static uint32_t t0 = millis();
  static uint32_t lastHb = 0;

  if (phase == WARMUP)
  {
    if (millis() - lastHb >= 500)
    {
      lastHb = millis();
      Serial.printf("[alive] %lu ms\r\n", (unsigned long)(millis() - t0));
      Serial.flush();
    }
    if (millis() - t0 >= 3000) phase = INIT;
    return;
  }

  if (phase == INIT)
  {
    say("STEP a: Wire.begin() ...");
    Wire.begin();
    say("  a ok");

    say("STEP b: rtc.initI2C() ...");
    rtc.initI2C();
    say("  b ok");

    say("STEP c: rtc.set24HourMode() ...");
    rtc.set24HourMode();
    say("  c ok");

    say("STEP d: read USER_RAM1 flag ...");
    uint8_t flag = rtc.readFromRegister(USER_RAM1_ADDRESS);
    Serial.printf("  d ok, flag=0x%02X\r\n", flag); Serial.flush();

    if (flag != TIME_SET_FLAG)
    {
      say("STEP e: rtc.setTime() ...");
      rtc.setTime(2026, 7, 6, 7, 12, 0, 0);   // (year, month, date, weekday, hh, mm, ss)
      rtc.writeToRegister(USER_RAM1_ADDRESS, TIME_SET_FLAG);
      say("  e ok (time set)");
    }
    else say("  time already set - keeping");

    say("INIT complete. Ticking time every second:");
    phase = RUN;
    return;
  }

  // RUN
  static uint32_t last = 0;
  if (millis() - last >= 1000)
  {
    last = millis();
    Serial.printf("%04d-%02d-%02d %02d:%02d:%02d\r\n",
                  rtc.getYear(), rtc.getMonth(), rtc.getDate(),
                  rtc.getHour(), rtc.getMinute(), rtc.getSecond());
    Serial.flush();
  }
}
