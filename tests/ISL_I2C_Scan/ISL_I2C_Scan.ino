/*
 * ISL Board (RAK_feather) - I2C bus scan  [bring-up test #0]  (v2: USB serial)
 *
 * NOTE ON DEBUG PORT: this board is flashed/monitored over native USB-C (the
 * nRF52840 USB CDC), which in RUI3 is `Serial` - NOT `Serial0`/RAKDAP. If you
 * open the monitor and see nothing, that was the bug. v2 prints to `Serial`.
 *
 *   Serial (USB-C CDC) -> PC  (115200)
 *   Wire   -> I2C   SDA=P0.13, SCL=P0.14   (RTC RV-3028 expected at 0x52)
 *
 * After a reset the USB port may re-enumerate - reopen the monitor on the same
 * COM port if needed.
 */

#include <Wire.h>

void setup()
{
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 3000) delay(10);   // wait for USB host to open
  delay(500);

  Serial.println("ISL Board - I2C scan v2 (SDA=P0.13, SCL=P0.14)  [USB serial]");
  Serial.println("------------------------------------------------------");
  Wire.begin();
}

void loop()
{
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++)
  {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0)
    {
      Serial.printf("  device found at 0x%02X%s\r\n",
                    addr, addr == 0x52 ? "  <- RV-3028 RTC (expected)" : "");
      found++;
    }
  }
  if (found == 0) Serial.println("  no I2C devices found (check pull-ups / wiring)");
  Serial.printf("scan done: %u device(s)\r\n\r\n", found);
  delay(3000);
}
