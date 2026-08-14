/*
 * ============================================================================
 * SolarNodeGateway.ino  —  LoRa GATEWAY for the SenseCAP Solar Node P1-Pro
 * ============================================================================
 * Hardware : SenseCAP Solar Node P1-Pro  (XIAO nRF52840 Plus + Wio-SX1262)
 *            + a XIAO ESP32S3 attached to the Grove UART for WiFi upload.
 * Library  : RadioLib (SX1262)            FQBN: Seeeduino:nrf52:xiaonRF52840Plus
 *
 * WHAT THIS IS
 * ------------
 * A twin of SolarNodeRepeater.ino, on the SAME BOARD, running the SAME PHY, but
 * TERMINAL: instead of re-transmitting the packet on air, it hands the packet
 * off to the XIAO ESP32S3 (on Grove Serial1, 115200 8N1) as a single-line JSON
 * object. The XIAO batches and POSTs to Custodia's /api/locations endpoint.
 *
 * DESIGN PRINCIPLE — "collars & repeaters stay dumb, cloud stays smart".
 * The tracker collars and the field-deployed repeaters are the hardest hardware
 * to service, so any policy that might need tuning — how to collapse duplicate
 * packets, staleness rules, RSSI weighting — is pushed to the dashboard where
 * it can be changed without a firmware push. Concrete gateway consequences:
 *   · NO dedup here. Every valid frame the radio hears (direct from collar, or
 *     a repeater's verbatim relay of that frame) is emitted on Serial1. If two
 *     repeaters both relay the same seq, the cloud sees two uploads with the
 *     same (device_id, timestamp) and collapses them in the dashboard.
 *   · Direct-heard collar packets are still ACK'd here, jittered so we don't
 *     collide with a repeater's ACK. Extra ACKs are harmless — the collar
 *     accepts the first one and stops retrying.
 *
 * Per received frame the gateway does two things, in order:
 *   1. RX + parse the same 6-field payload the repeater parses  ("<id>,<seq>,
 *      lat,lon,v,ts[,SV=n,TTFF=s,CELL=OK|LOW|DEAD]"), or the "CUST,<id>,<seq>,..."
 *      mesh form. Ignores ACK frames.
 *   2. ACK the collar (jittered), then emit JSON on Serial1 to the XIAO:
 *
 *      {"devID":51,"seq":7,"lat":24.713600,"lon":46.675301,
 *       "vbat":3.60,"ts":1751328000,
 *       "gwID":60,"rssi":-95,"snr":8
 *       [,"sv":9,"ttff":32,"cell":"OK"]}\n
 *
 * PHY MUST MATCH the ISL collar + repeater EXACTLY: 915 MHz, BW 250 kHz, SF7,
 * CR4/5, preamble 8, sync 0x12. Same 50 ms ACK turnaround guard as the
 * repeater (see SolarNodeRepeater.ino header for the WHY — do not remove).
 *
 * Grove UART wiring on the P1-Pro (from the SenseCAP block diagram — NOT the
 * standard XIAO Serial1 header pins; the Solar Node routes Grove through an
 * NMOS level-shift to two P0 GPIOs):
 *   Grove D0 (yellow, SIG1) = P0.10   <- gateway drives as TX out to XIAO
 *   Grove D1 (white,  SIG2) = P0.09   <- gateway uses as RX in from XIAO
 *
 * UART is a crossover, so:
 *   Solar Node D0 (P0.10, TX)  ->  XIAO ESP32S3 D7 (RX, GPIO 44)
 *   Solar Node D1 (P0.09, RX)  <-  XIAO ESP32S3 D6 (TX, GPIO 43)
 *   GND common (mandatory) ; 3V3 only if the XIAO is fed by the Solar Node.
 *
 * Because these pins are not on the XIAO Plus header, the Arduino core's
 * `Serial1` cannot address them - we set UARTE1 up at register level (see the
 * GroveUart namespace below). NOTE: P0.09/P0.10 are the nRF52840 NFC pins. On
 * a factory-fresh chip they default to NFC mode and won't drive as GPIO until
 * NRF_UICR->NFCPINS is written to 0xFFFFFFFE and the chip is power-cycled once.
 * The SenseCAP factory firmware already does this; the repeater firmware you
 * shipped never touches those bits, so once flashed to a Solar Node the UICR
 * stays as Seeed set it. If a "raw" chip refuses to talk, run the one-time
 * UICR disable in factoryDisableNfcPinsOnce() below.
 * ============================================================================
 */
#include <Arduino.h>
#if defined(NRF52840_XXAA)
  #include <Adafruit_TinyUSB.h>
#endif
#pragma push_macro("CFG_TUD_CDC")
#undef CFG_TUD_CDC
#include <RadioLib.h>
#pragma pop_macro("CFG_TUD_CDC")

#define LOG(msg)   do { if (Serial) Serial.println(msg); } while (0)
#define LOGF(msg)  do { if (Serial) Serial.print(msg);   } while (0)

// ============================================================================
//  GATEWAY CONFIGURATION
// ============================================================================
constexpr uint16_t GATEWAY_ID           = 60;      // this gateway's id (goes in the JSON as "gwID")
constexpr uint32_t XIAO_BAUD            = 115200;  // Grove UART to the XIAO ESP32S3
// Only 115200 is wired below (matches XIAO_ESP32S3_Uploader/config.h). If you
// change baud, extend GroveUart::begin() with the matching UARTE_BAUDRATE_...

// ACK timing — same TX->RX turnaround guard the repeater uses, plus a small
// jitter so gateway + repeater ACKs of the same collar packet don't collide
// on air. Base is >= node turnaround (~3-5 ms), total << ACK_TIMEOUT_SEC (8 s).
constexpr uint32_t ACK_TX_DELAY_MS      = 50;
constexpr uint32_t ACK_JITTER_MS        = 60;      // rand(0..JITTER) added

constexpr uint32_t HEARTBEAT_SEC        = 30;

// ============================================================================
//  BOARD SUPPORT — identical to SolarNodeRepeater.ino
// ============================================================================
namespace Board {
  constexpr int  GPIO_WHITE  = 15;
  constexpr int  GPIO_BLUE   = 19;
  constexpr bool LED_ON_HIGH = true;

  constexpr int      BTN_PORT       = 1;
  constexpr int      BTN_BIT        = 1;
  constexpr bool     BTN_ACTIVE_LOW = true;
  constexpr uint32_t HOLD_MS_OFF    = 5000;

  static inline NRF_GPIO_Type* port(int p) { return p == 0 ? NRF_P0 : NRF_P1; }
  inline void pinWrite(int p, int bit, bool high) {
    port(p)->DIRSET = (1UL << bit);
    if (high) port(p)->OUTSET = (1UL << bit);
    else      port(p)->OUTCLR = (1UL << bit);
  }
  inline bool pinRead(int p, int bit) { return (port(p)->IN >> bit) & 1UL; }

  inline void white(bool on) { pinWrite(0, GPIO_WHITE, on == LED_ON_HIGH); }
  inline void blue(bool on)  { pinWrite(0, GPIO_BLUE,  on == LED_ON_HIGH); }
  inline void ledsOff()      { white(false); blue(false); }

  inline bool buttonPressed() {
    bool lvl = pinRead(BTN_PORT, BTN_BIT);
    return BTN_ACTIVE_LOW ? (lvl == false) : (lvl == true);
  }

  void powerOff() {
    blue(true); delay(1000); ledsOff();
    while (buttonPressed()) { delay(10); }
    if (BTN_ACTIVE_LOW) port(BTN_PORT)->PIN_CNF[BTN_BIT] = (3UL << 2) | (3UL << 16);
    else                port(BTN_PORT)->PIN_CNF[BTN_BIT] = (1UL << 2) | (2UL << 16);
    NRF_POWER->SYSTEMOFF = 1;
    while (true) { __asm__ __volatile__("wfi"); }
  }

  void begin() {
    Serial.begin(115200);
    NRF_QSPI->ENABLE = 0;
    port(BTN_PORT)->PIN_CNF[BTN_BIT] = BTN_ACTIVE_LOW ? (3UL << 2) : (1UL << 2);
    ledsOff();
    blue(true); delay(1000); blue(false);
  }

  void service() {
    static bool armed = false; static uint32_t t0 = 0;
    if (buttonPressed()) {
      if (!armed) { armed = true; t0 = millis(); }
      else if (millis() - t0 >= HOLD_MS_OFF) powerOff();
    } else { armed = false; }
  }
}

// ============================================================================
//  LEDs — action vocabulary
//    WHITE = packet / data activity      BLUE = radio TX (ACK) / UART emit
// ============================================================================
namespace Led {
  inline void blipWhite(int n, int ms = 60) {
    for (int i = 0; i < n; i++) { Board::white(true); delay(ms); Board::white(false); delay(ms); }
  }
  inline void blipBlue(int n, int ms = 60) {
    for (int i = 0; i < n; i++) { Board::blue(true); delay(ms); Board::blue(false); delay(ms); }
  }
  void rxPacket()    { blipWhite(2, 80); }
  void ackSent()     { blipBlue(1, 80);  }
  void jsonEmitted() { blipBlue(2, 80);  }
  void fatal()       { while (true) { Board::blue(true); delay(80); Board::blue(false); delay(80); Board::service(); } }

  void idleTick() {
    static uint32_t t = 0; static bool on = false;
    uint32_t now = millis();
    if (!on && now - t >= 2000) { Board::blue(true);  on = true;  t = now; }
    if (on  && now - t >= 20)   { Board::blue(false); on = false; }
  }
}

// ============================================================================
//  LoRa radio — SX1262   (PHY MATCHES the ISL collar + repeater)
// ============================================================================
SX1262 radio = new Module(D4, D1, D2, D3);

namespace Lora {
  constexpr float    FREQ_MHZ  = 915.0;
  constexpr float    BW_KHZ    = 250.0;
  constexpr uint8_t  SF        = 7;
  constexpr uint8_t  CR        = 5;
  constexpr uint8_t  SYNC_WORD = 0x12;
  constexpr uint16_t PREAMBLE  = 8;
  constexpr float    TCXO_V    = 1.8;
  constexpr int8_t   POWER_DBM = 22;   // ACK power; collar accepts first ACK regardless

  volatile bool rxFlag = false;
  void onRx() { rxFlag = true; }
  void armReceive() { rxFlag = false; radio.startReceive(); }

  void begin() {
    int st = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNC_WORD, POWER_DBM, PREAMBLE, TCXO_V);
    if (st != RADIOLIB_ERR_NONE) { LOGF("[LoRa] begin() failed: "); LOG(st); Led::fatal(); }
    radio.setDio2AsRfSwitch(true);
    radio.setPacketReceivedAction(onRx);
    LOG("[LoRa] ready @ 915 MHz SF7 BW250 CR4/5 sync=0x12  (matches ISL collar + repeater)");
  }
}

// ============================================================================
//  PARSE — same tolerance as the repeater
//    "<id>,<seq>,lat,lon,v,ts[,SV=n,TTFF=s,CELL=OK|LOW|DEAD]"   (bare)
//    "CUST,<id>,<seq>,lat,lon,v,ts,..."                          (mesh)
//  Returns false for ACK/garbage.
// ============================================================================
struct CollarPkt {
  uint16_t id;
  uint32_t seq;
  String   lat;
  String   lon;
  String   vbat;
  String   ts;
  // Optional v15+ diagnostic tokens; empty string if the collar didn't send them.
  String   sv;
  String   ttff;
  String   cell;
};

// Pull the value of a "KEY=" token out of the message (up to next comma).
String tokenVal(const String& msg, const char* key) {
  String k = String(key);
  int at = msg.indexOf(k);
  if (at < 0) return "";
  int start = at + k.length();
  int end = msg.indexOf(',', start);
  if (end < 0) end = msg.length();
  return msg.substring(start, end);
}

bool parseCollar(const String& raw, CollarPkt& p) {
  String msg = raw; msg.trim();
  if (msg.length() == 0) return false;
  if (msg.startsWith("ACK,")) return false;

  int base = 0;
  if (msg.startsWith("CUST,")) base = 5;

  // Split up to six comma fields from `base`.
  int idx[7];
  idx[0] = base;
  int found = 0;
  for (int i = base; i < (int)msg.length() && found < 6; i++) {
    if (msg[i] == ',') { idx[++found] = i + 1; }
  }
  if (found < 5) return false;   // need at least 5 commas for 6 fields
  idx[6] = msg.indexOf(',', idx[5]); if (idx[6] < 0) idx[6] = msg.length();

  String idStr  = msg.substring(idx[0], idx[1] - 1);
  String seqStr = msg.substring(idx[1], idx[2] - 1);
  p.lat  = msg.substring(idx[2], idx[3] - 1);
  p.lon  = msg.substring(idx[3], idx[4] - 1);
  p.vbat = msg.substring(idx[4], idx[5] - 1);
  p.ts   = msg.substring(idx[5], idx[6]);

  if (idStr.length() == 0 || seqStr.length() == 0) return false;
  for (uint16_t i = 0; i < idStr.length();  i++) if (!isDigit(idStr[i]))  return false;
  for (uint16_t i = 0; i < seqStr.length(); i++) if (!isDigit(seqStr[i])) return false;

  p.id  = (uint16_t)idStr.toInt();
  p.seq = (uint32_t)seqStr.toInt();
  p.sv   = tokenVal(msg, "SV=");
  p.ttff = tokenVal(msg, "TTFF=");
  p.cell = tokenVal(msg, "CELL=");
  return true;
}

// ============================================================================
//  GROVE UART — register-level UARTE1 on the P1-Pro's Grove pins
//    Grove D0 (yellow, SIG1) = P0.10  -> TX  (out to XIAO D7/RX)
//    Grove D1 (white,  SIG2) = P0.09  -> RX  (in  from XIAO D6/TX)
//  115200 8N1. Blocking write via EasyDMA. Buffer must live in RAM.
// ============================================================================
namespace GroveUart {
  constexpr uint32_t TX_PIN = NRF_GPIO_PIN_MAP(0, 10);  // Grove D0 - yellow
  constexpr uint32_t RX_PIN = NRF_GPIO_PIN_MAP(0,  9);  // Grove D1 - white

  // One-shot NFC-pin -> GPIO conversion. Needed only if this chip has never
  // had it done; effect is permanent (UICR is non-volatile). Safe to call
  // repeatedly: it no-ops if UICR is already set. Requires a power cycle
  // after the write for the setting to take effect.
  void factoryDisableNfcPinsOnce() {
    if ((NRF_UICR->NFCPINS & 1) == 0) return;   // already set to GPIO
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {}
    NRF_UICR->NFCPINS = 0xFFFFFFFEUL;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {}
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    // NB: reboot required for the setting to take hold on the pin function.
  }

  void begin(uint32_t baud) {
    NRF_UARTE1->PSEL.TXD = TX_PIN;
    NRF_UARTE1->PSEL.RXD = RX_PIN;
    NRF_UARTE1->PSEL.CTS = 0xFFFFFFFFUL;
    NRF_UARTE1->PSEL.RTS = 0xFFFFFFFFUL;
    // Only 115200 wired here; add cases if you change XIAO_BAUD.
    (void)baud;
    NRF_UARTE1->BAUDRATE = (uint32_t)UARTE_BAUDRATE_BAUDRATE_Baud115200;
    NRF_UARTE1->CONFIG = 0;                     // 8N1, no HW flow control
    NRF_UARTE1->ENABLE = UARTE_ENABLE_ENABLE_Enabled;
  }

  void write(const uint8_t* buf, size_t len) {
    if (len == 0) return;
    NRF_UARTE1->EVENTS_ENDTX = 0;
    NRF_UARTE1->TXD.PTR    = (uint32_t)buf;
    NRF_UARTE1->TXD.MAXCNT = (uint32_t)len;
    NRF_UARTE1->TASKS_STARTTX = 1;
    while (NRF_UARTE1->EVENTS_ENDTX == 0) { /* poll - short at 115200 */ }
    NRF_UARTE1->EVENTS_ENDTX = 0;
  }
} // namespace GroveUart

// ============================================================================
//  UART EMIT — one JSON line per received frame -> XIAO ESP32S3 -> cloud
//  Buffer is static RAM so EasyDMA can address it.
// ============================================================================
static char g_uartBuf[400];

void emitJson(const CollarPkt& p, int16_t rssi, int8_t snr) {
  int len = snprintf(g_uartBuf, sizeof(g_uartBuf),
      "{\"devID\":%u,\"seq\":%lu,\"lat\":%s,\"lon\":%s,"
      "\"vbat\":%s,\"ts\":%s,"
      "\"gwID\":%u,\"rssi\":%d,\"snr\":%d",
      (unsigned)p.id, (unsigned long)p.seq,
      p.lat.c_str(), p.lon.c_str(),
      p.vbat.c_str(), p.ts.c_str(),
      (unsigned)GATEWAY_ID, (int)rssi, (int)snr);
  if (len < 0 || len >= (int)sizeof(g_uartBuf)) return;

  if (p.sv.length()) {
    int n = snprintf(g_uartBuf + len, sizeof(g_uartBuf) - len, ",\"sv\":%s", p.sv.c_str());
    if (n > 0) len += n;
  }
  if (p.ttff.length()) {
    int n = snprintf(g_uartBuf + len, sizeof(g_uartBuf) - len, ",\"ttff\":%s", p.ttff.c_str());
    if (n > 0) len += n;
  }
  if (p.cell.length()) {
    int n = snprintf(g_uartBuf + len, sizeof(g_uartBuf) - len, ",\"cell\":\"%s\"", p.cell.c_str());
    if (n > 0) len += n;
  }
  int n = snprintf(g_uartBuf + len, sizeof(g_uartBuf) - len, "}\n");
  if (n > 0) len += n;

  if (len > 0 && len < (int)sizeof(g_uartBuf)) {
    GroveUart::write((const uint8_t*)g_uartBuf, (size_t)len);
  }
}

// ============================================================================
//  APP  — counters, headless-friendly logging
// ============================================================================
uint32_t rxCount = 0, ackCount = 0, emitCount = 0, badCount = 0;
uint32_t bootMs    = 0;
uint32_t lastPktMs = 0;
bool     serialWas = false;

String upStr() {
  uint32_t s = (millis() - bootMs) / 1000UL;
  uint32_t d = s / 86400UL; s %= 86400UL;
  uint32_t h = s / 3600UL;  s %= 3600UL;
  uint32_t m = s / 60UL;    s %= 60UL;
  char b[32];
  snprintf(b, sizeof(b), "%lud %02lu:%02lu:%02lu",
           (unsigned long)d, (unsigned long)h, (unsigned long)m, (unsigned long)s);
  return String(b);
}

void printStartupBanner() {
  LOG("");
  LOG("================================================================");
  LOG("  SolarNode LoRa Gateway  -  firmware v1  (feeds XIAO ESP32S3 uploader)");
  LOG("  Board : SenseCAP Solar Node P1-Pro (XIAO nRF52840 Plus + SX1262)");
  LOG("  Built : " __DATE__ "  " __TIME__);
  LOG("================================================================");
  LOG("[ROLE] RX collar/repeater  ->  ACK collar (jittered)  ->  JSON on Grove UART");
  LOG("       (no dedup here - cloud/dashboard handles duplicates)");
  LOG(String("[CFG]  gwID=") + GATEWAY_ID +
      "  ackDelay=" + ACK_TX_DELAY_MS + "ms  ackJitter=0.." + ACK_JITTER_MS + "ms" +
      "  xiaoBaud=" + XIAO_BAUD + "  heartbeat=" + HEARTBEAT_SEC + "s");
  LOG(String("[PHY]  915 MHz  BW=") + String(Lora::BW_KHZ, 0) + "kHz  SF" + Lora::SF +
      "  CR4/" + Lora::CR + "  sync=0x12  preamble=" + Lora::PREAMBLE +
      "  TXpwr=" + Lora::POWER_DBM + "dBm  TCXO=1.8V  (matches ISL collar + repeater)");
  LOG("[PINS] LoRa CS=D4 DIO1=D1 RST=D2 BUSY=D3 | LED white=P0.15 blue=P0.19 | PWRbtn=P1.01");
  LOG("[UART] Grove -> XIAO ESP32S3  (TX=Grove D0/P0.10/yellow, RX=Grove D1/P0.09/white)");
  LOG("       XIAO D7(RX) <- Solar Node D0 ; XIAO D6(TX) -> Solar Node D1 ; GND common");
}

void printStatus() {
  LOG(String("[STAT] up ") + upStr() + "  |  rx=" + rxCount + " ack=" + ackCount +
      " emit=" + emitCount + " bad=" + badCount);
  if (lastPktMs == 0) LOG("[STAT] no frames heard yet - listening");
  else LOG(String("[STAT] last frame ") + ((millis() - lastPktMs) / 1000UL) + " s ago");
}

void serviceSerial() {
  bool now = (bool)Serial;
  if (now && !serialWas) {
    printStartupBanner();
    LOG("[USB] monitor connected - gateway was never paused; resuming live logs");
    printStatus();
    LOG("[LoRa] listening for collar/repeater frames...");
  }
  serialWas = now;

  static uint32_t lastHb = 0;
  if (now && (millis() - lastHb) >= HEARTBEAT_SEC * 1000UL) {
    lastHb = millis();
    printStatus();
  }
}

void setup() {
  Board::begin();
  bootMs = millis();

  // Grove UART to the XIAO ESP32S3 (custom pins P0.10=TX/D0, P0.09=RX/D1).
  // Ensure NFC pins are configured as GPIO (permanent, one-time; needs reboot).
  GroveUart::factoryDisableNfcPinsOnce();
  GroveUart::begin(XIAO_BAUD);

  // Diverge ACK jitter per gateway so we don't collide with a co-hearing repeater.
  randomSeed((uint32_t)GATEWAY_ID * 2654435761UL + micros());

  printStartupBanner();
  Lora::begin();
  printStatus();

  Lora::armReceive();
  LOG("[LoRa] listening for collar/repeater frames...");
  serialWas = (bool)Serial;
}

void loop() {
  Board::service();
  serviceSerial();

  if (!Lora::rxFlag) { Led::idleTick(); return; }
  Lora::rxFlag = false;

  String msg;
  int st = radio.readData(msg);
  if (st != RADIOLIB_ERR_NONE) {
    LOGF("[RX] readData error: "); LOG(st);
    Lora::armReceive();
    return;
  }
  float rssi = radio.getRSSI();
  float snr  = radio.getSNR();
  rxCount++;
  lastPktMs = millis();
  Led::rxPacket();
  LOG(String("[RX] #") + rxCount + "  RSSI=" + String(rssi, 1) + " dBm  SNR=" +
      String(snr, 1) + " dB  len=" + msg.length() + "  \"" + msg + "\"");

  CollarPkt p;
  if (!parseCollar(msg, p)) {
    badCount++;
    LOG("[RX] not a collar frame (ACK/garbage) - ignoring, not ACK'd");
    Lora::armReceive();
    return;
  }
  LOG(String("[RX] parsed id=") + p.id + " seq=" + p.seq);
  if (p.cell.length()) {
    String flag = (p.cell == "DEAD") ? "   <<< GPS CELL DEAD >>>"
                : (p.cell == "LOW")  ? "   (cell low)"
                : "";
    LOG(String("[GPS-CELL] id=") + p.id + "  SV=" + p.sv +
        "  TTFF=" + p.ttff + "s  CELL=" + p.cell + flag);
  }

  // --- ACK the collar (jittered so we diverge from any repeater's ACK) ---
  uint32_t backoff = ACK_TX_DELAY_MS + (uint32_t)random((long)(ACK_JITTER_MS + 1));
  String ack = String("ACK,") + p.id + "," + p.seq;
  delay(backoff);
  Board::blue(true);
  st = radio.transmit(ack);
  Board::blue(false);
  if (st == RADIOLIB_ERR_NONE) {
    ackCount++;
    LOG(String("[ACK] sent \"") + ack + "\"  (backoff " + backoff + " ms, " + ackCount + " total)");
    Led::ackSent();
  } else {
    LOGF("[ACK] TX error: "); LOG(st);   // emit still runs below
  }

  // --- Emit JSON on Grove UART to the XIAO (no dedup - cloud handles it) ---
  emitJson(p, (int16_t)rssi, (int8_t)snr);
  emitCount++;
  LOG(String("[EMIT] id=") + p.id + " seq=" + p.seq + " -> Grove UART (" + emitCount + " total)");
  Led::jsonEmitted();

  Lora::armReceive();
}
