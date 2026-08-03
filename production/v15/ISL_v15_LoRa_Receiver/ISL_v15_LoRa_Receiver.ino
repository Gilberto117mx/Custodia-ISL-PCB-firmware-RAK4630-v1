/*
 * ISL v15 LoRa Receiver / ACK node  (reference)
 * =============================================================================
 * Listens for the collar's LoRa position packets, ACKs them (so the tracker's
 * delivery guarantee marks them delivered), and prints them - including the v15
 * GPS backup-cell health fields.
 *   Board: XIAO nRF52840 + SX1262, RadioLib.
 *
 * NOTE ON THE DEPLOYED REPEATER: the real relay is the separate "SolarNode LoRa
 * Repeater" (its own firmware). It relays the RAW payload and parses only the
 * leading id/seq for dedup/ACK, so the appended v15 fields need NO change there -
 * they already show up in its log. This sketch is the in-repo reference receiver,
 * updated to *parse and highlight* the health fields.
 *
 * v15 packet (variable length; first 6 are fixed, the rest are key=val):
 *   "<id>,<seq>,<lat>,<lon>,<vbat>,<ts>,SV=<n>,TTFF=<s>,CELL=<OK|LOW|DEAD>"
 * We split on commas, ACK with fields[0]/[1], and pull SV / TTFF / CELL out for a
 * one-line health summary. CELL=DEAD / a large TTFF => that collar's GPS backup
 * cell is not holding ephemeris (check/replace it).
 *
 * ACK: "ACK,<id>,<seq>". Reply is delayed ACK_TX_DELAY_MS so the node is in RX
 * (fixes the TX->RX turnaround race).
 * =============================================================================
 */

#include <Arduino.h>
#include <RadioLib.h>

// ---- pins (same board as tracker) ----
constexpr int PIN_LORA_DIO1  = 1;
constexpr int PIN_LORA_BUSY  = 3;
constexpr int PIN_LORA_NSS   = 4;
constexpr int PIN_SHARED_RST = 6;
constexpr int LEDS[] = {11, 12, 13};
constexpr uint32_t ACK_TX_DELAY_MS = 50;   // wait for node TX->RX turnaround before ACK

SX1262 sx = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_SHARED_RST, PIN_LORA_BUSY);
volatile bool packetReady = false;
void onPacket() { packetReady = true; }
void ledsOff() { for (int pin : LEDS) digitalWrite(pin, HIGH); }

// Split a CSV string into up to maxN fields. Returns the count.
static int splitCSV(const String &raw, String out[], int maxN) {
  int n = 0, start = 0;
  for (int i = 0; i <= (int)raw.length() && n < maxN; i++) {
    if (i == (int)raw.length() || raw[i] == ',') {
      out[n++] = raw.substring(start, i);
      start = i + 1;
    }
  }
  return n;
}

// Pull the value of a "KEY=" token from the field list ("" if absent).
static String tokenVal(String fields[], int n, const char *key) {
  String k = String(key);
  for (int i = 0; i < n; i++)
    if (fields[i].startsWith(k)) return fields[i].substring(k.length());
  return "";
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 4000) delay(10);

  for (int pin : LEDS) pinMode(pin, OUTPUT);
  ledsOff();
  NRF_QSPI->ENABLE = 0;

  Serial.println("=== ISL v15 LoRa Receiver (ACK node, shows GPS CELL health) ===");
  int state = sx.begin();
  if (state != RADIOLIB_ERR_NONE) { Serial.println("[ERROR] radio init " + String(state)); while (true) delay(1000); }

  sx.setFrequency(915.0);
  sx.setBandwidth(250.0);
  sx.setSpreadingFactor(7);
  sx.setCodingRate(5);
  sx.setPreambleLength(8);
  sx.setSyncWord(0x12);
  sx.setOutputPower(14);
  sx.setPacketReceivedAction(onPacket);

  state = sx.startReceive();
  if (state != RADIOLIB_ERR_NONE) { Serial.println("[ERROR] startReceive " + String(state)); while (true) delay(1000); }

  Serial.println("[RADIO] Listening. 915 | BW250 | SF7 | CR5 | pre8 | sync0x12");
  Serial.println("---------------------------------------------------");
}

void loop() {
  ledsOff();
  if (!packetReady) { delay(5); return; }
  packetReady = false;

  String rx = "";
  int state = sx.readData(rx);
  if (state != RADIOLIB_ERR_NONE) { Serial.println("[ERROR] readData " + String(state)); sx.startReceive(); return; }

  float rssi = sx.getRSSI(), snr = sx.getSNR();

  String f[12];
  int n = splitCSV(rx, f, 12);

  if (n >= 2) {
    String ack = "ACK," + f[0] + "," + f[1];        // id, seq
    delay(ACK_TX_DELAY_MS);
    int tx = sx.transmit(ack);
    packetReady = false;

    String sv   = tokenVal(f, n, "SV=");
    String ttff = tokenVal(f, n, "TTFF=");
    String cell = tokenVal(f, n, "CELL=");

    Serial.println("\n[PKT]  RSSI " + String(rssi) + " dBm  SNR " + String(snr) + " dB");
    Serial.println("  Raw : " + rx);
    String line = "  id=" + f[0] + " seq=" + f[1];
    if (n > 3) line += "  lat=" + f[2] + " lon=" + f[3];
    if (n > 4) line += "  vbat=" + f[4] + "V";
    line += "  SV=" + sv;
    Serial.println(line);
    // v15 GPS backup-cell health line
    if (ttff.length() || cell.length()) {
      String flag = (cell == "DEAD") ? "   <<< GPS CELL DEAD - check/replace >>>"
                  : (cell == "LOW")  ? "   (cell low - charging)"
                  : "";
      Serial.println("  GPS-CELL: TTFF=" + ttff + "s  CELL=" + cell + flag);
    }
    Serial.println(tx == RADIOLIB_ERR_NONE ? "  >> ACK: " + ack : "  [!] ACK TX failed: " + String(tx));
  } else {
    Serial.println("\n[PKT - unparseable]  Raw : " + rx);
  }

  Serial.println("---------------------------------------------------");
  sx.startReceive();
}
