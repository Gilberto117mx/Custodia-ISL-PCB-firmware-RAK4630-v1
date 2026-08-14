/*
 * ============================================================================
 * SolarNodeRepeater.ino  —  LoRa REPEATER for the SenseCAP Solar Node P1-Pro
 * ============================================================================
 * Hardware : SenseCAP Solar Node P1-Pro  (XIAO nRF52840 Plus + Wio-SX1262)
 * Library  : RadioLib (SX1262)            FQBN: Seeeduino:nrf52:xiaonRF52840Plus
 *
 * WHAT THIS IS
 * ------------
 * A range-extending repeater that sits between the ISL tracker node (device
 * "051", the RAK4630 v7/v8 board in this repo) and a Wi-Fi gateway (programmed
 * later). It behaves like Meshtastic's Repeater role — it rebroadcasts the
 * traffic it hears so packets reach a station out of the node's direct range —
 * while ALSO doing exactly what the ISL LoRa receiver does: it ACKs the node so
 * the node's delivery guarantee is satisfied and it stops retrying.
 *
 * Per received node packet the repeater does three things, in order:
 *   1. RX + parse the node's packet  ("<id>,<seq>,lat,lon,v,ts,SV=n"  or
 *      the "CUST,<id>,<seq>,..." mesh form — both are accepted).
 *   2. ACK the node  ("ACK,<id>,<seq>")  — same handshake the ISL LoRa receiver
 *      (ISL_Board/production/v17/ISL_v17_LoRa_Receiver) performs, so the node marks it delivered.
 *   3. RELAY the verbatim payload onward to the gateway (unless it's a duplicate
 *      we already forwarded — managed-flood dedup by <id,seq>).
 *
 * ISL v15 NOTE — GPS backup-cell health (relay is UNCHANGED)
 * ---------------------------------------------------------
 * v15 collars append "…,SV=<n>,TTFF=<s>,CELL=<OK|LOW|DEAD>" to the packet. Since
 * we relay the payload VERBATIM and dedup only on <id,seq>, those fields already
 * pass through to the gateway with ZERO change to ACK/relay/dedup. We ADD one
 * log line ("[GPS-CELL] …") so a visitor watching the repeater sees which collar
 * has a failing GPS backup cell (CELL=DEAD / a large TTFF). Older nodes that
 * don't send CELL= simply skip that line. Nothing on the RF path changed.
 *
 * WHY THE 50 ms ACK DELAY (inherited lesson — do not remove)
 * ----------------------------------------------------------
 * The node finishes its TX, then needs ~3-5 ms to turn its radio TX->RX before
 * it is actually listening for the ACK. Replying too fast makes the ACK preamble
 * fly past an un-listening node and the node logs a RECEIVE TIMEOUT. We wait
 * ACK_TX_DELAY_MS (50 ms) after receiving before transmitting the ACK — well
 * past the turnaround, tiny vs the node's ACK window (8 s). See the ISL LoRa receiver.
 *
 * PHY MUST MATCH THE ISL BOARD EXACTLY (this is the whole point of "coordinates
 * with the ISL board"): 915 MHz, BW 250 kHz, SF7, CR4/5, preamble 8, sync 0x12.
 * The ISL node is production/v7 (LORA_BW=1=250 kHz, LORA_CR=0=4/5). The gateway,
 * when built, is just another receiver on this same PHY — it parses the relayed
 * payload with the identical field layout the ISL receiver uses.
 *
 * BOARD NOTES (from ../pins.md, proven on the bench in test_lora / Acknowledgment)
 *   · LoRa control pins (Arduino idx): CS=D4, DIO1=D1, RST=D2, BUSY=D3, default SPI.
 *   · MANDATORY: radio.setDio2AsRfSwitch(true) and TCXO=1.8 V via begin(), or TX
 *     power is lost / the radio misbehaves.
 *   · LEDs are register-driven (digitalWrite does NOT work here): White P0.15,
 *     Blue P0.19 (needs QSPI OFF first — Board::begin() does it).
 *   · Power-off: hold the Power button (P1.01) 5 s -> nRF System OFF.
 *
 * SERIAL / USB-CDC LOGGING BEHAVIOUR  (important for a remote, headless node)
 * --------------------------------------------------------------------------
 * `Serial` here is USB-CDC. `if (Serial)` is TRUE only while a serial monitor
 * has the port OPEN (USB DTR asserted). LOG()/LOGF() are if(Serial)-guarded, so:
 *   · No monitor  -> logs are silently dropped; the repeater runs full speed and
 *     NEVER blocks on a dead port. Plugging/unplugging USB-C does NOT reset the
 *     MCU and does NOT interrupt RX/ACK/relay — that path never touches Serial.
 *   · Monitor open -> you see live logs.
 * Because boot happens once (often long before anyone plugs in), a visitor who
 * connects later would otherwise see SILENCE — setup()'s banner already scrolled
 * past while nothing was listening. FIX: loop() watches for the monitor being
 * (re)opened and, on that edge, REPRINTS the full startup banner + live status;
 * while connected it also prints a heartbeat every HEARTBEAT_SEC. So whoever
 * plugs in gets the whole picture immediately, and the repeater is never paused.
 *
 * Standalone sketch — no companion files. Open this .ino in the Arduino IDE and
 * upload, OR from the command line (folder name must equal the .ino name):
 *   arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Plus SolarNodeRepeater
 *   arduino-cli upload  --fqbn Seeeduino:nrf52:xiaonRF52840Plus -p <PORT> SolarNodeRepeater
 * Requires: Seeeduino nRF52 core (Plus variant) + the RadioLib library.
 * ============================================================================
 */
#include <Arduino.h>
#if defined(NRF52840_XXAA)
  #include <Adafruit_TinyUSB.h>   // links Serial over USB CDC (without it the link step fails)
#endif
// RadioLib prints a harmless #warning when Serial is USB CDC (always true here).
// It concerns RadioLib's OWN debug output (unused); our LOG() is if(Serial)-guarded.
#pragma push_macro("CFG_TUD_CDC")
#undef CFG_TUD_CDC
#include <RadioLib.h>
#pragma pop_macro("CFG_TUD_CDC")

#define LOG(msg)   do { if (Serial) Serial.println(msg); } while (0)
#define LOGF(msg)  do { if (Serial) Serial.print(msg);   } while (0)

// ============================================================================
//  REPEATER CONFIGURATION
// ============================================================================
constexpr uint16_t REPEATER_ID      = 91;    // this repeater's id (for logs / future gateway diag)

// ACK timing — the proven TX->RX turnaround guard (see header). Must be
// > node turnaround (~3-5 ms) and << the node's ACK window (ACK_TIMEOUT_SEC=8 s).
constexpr uint32_t ACK_TX_DELAY_MS  = 50;
// Small gap between sending the ACK and relaying, so the two frames don't run
// together and the gateway has settled into RX. Kept short — pure airtime.
constexpr uint32_t RELAY_GAP_MS     = 20;

// Managed-flood dedup: remember the last N <id,seq> we RELAYED so a node's
// re-send (it only re-sends when it missed our ACK) is re-ACK'd but NOT
// relayed a second time. Also breaks a would-be repeater<->repeater loop.
constexpr uint8_t  DEDUP_SIZE       = 16;

// While a serial monitor is connected, print a live status/heartbeat line this
// often, so a visitor sees the node is alive even when no packets are arriving.
// Logs are still dropped (no cost) while no monitor is attached.
constexpr uint32_t HEARTBEAT_SEC    = 30;

// ============================================================================
//  BOARD SUPPORT — Solar Node P1-Pro   (register-driven; from Vanilla/test_lora)
// ============================================================================
namespace Board {
  constexpr int  GPIO_WHITE  = 15;     // P0.15 White  (packet / data activity)
  constexpr int  GPIO_BLUE   = 19;     // P0.19 Blue   (LoRa radio activity; needs QSPI off)
  constexpr bool LED_ON_HIGH = true;   // level that turns the LED ON (flip if inverted)

  constexpr int      BTN_PORT       = 1;      // 0 = P0, 1 = P1
  constexpr int      BTN_BIT        = 1;      // P1.01 (Power button, confirmed on the bench)
  constexpr bool     BTN_ACTIVE_LOW = true;   // pressed = LOW
  constexpr uint32_t HOLD_MS_OFF    = 5000;   // hold 5 s to power off (factory behaviour)

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

  // Real power-off = nRF System OFF (~uA). Leaves the button armed as WAKE source.
  void powerOff() {
    blue(true); delay(1000); ledsOff();
    while (buttonPressed()) { delay(10); }          // wait for release (else it wakes instantly)
    if (BTN_ACTIVE_LOW) port(BTN_PORT)->PIN_CNF[BTN_BIT] = (3UL << 2) | (3UL << 16); // pull-up + SENSE Low
    else                port(BTN_PORT)->PIN_CNF[BTN_BIT] = (1UL << 2) | (2UL << 16); // pull-down + SENSE High
    NRF_POWER->SYSTEMOFF = 1;
    while (true) { __asm__ __volatile__("wfi"); }
  }

  // Call ONCE in setup(): USB debug, free P0.19 (QSPI), button config, LEDs off,
  // boot feedback (blue 1 s, like the factory firmware).
  void begin() {
    Serial.begin(115200);                            // USB CDC debug (LOG() filters with if(Serial))
    NRF_QSPI->ENABLE = 0;                            // free P0.19 (blue LED)
    port(BTN_PORT)->PIN_CNF[BTN_BIT] = BTN_ACTIVE_LOW ? (3UL << 2) : (1UL << 2); // input + pull
    ledsOff();
    blue(true); delay(1000); blue(false);            // power-on feedback
  }

  // Call often in loop(): power-off by holding the button (5 s). Non-blocking.
  void service() {
    static bool armed = false; static uint32_t t0 = 0;
    if (buttonPressed()) {
      if (!armed) { armed = true; t0 = millis(); }
      else if (millis() - t0 >= HOLD_MS_OFF) powerOff();
    } else { armed = false; }
  }
} // namespace Board

// ============================================================================
//  LEDs — action vocabulary
//    WHITE = packet / data activity      BLUE = LoRa radio TX activity
// ============================================================================
namespace Led {
  inline void blipWhite(int n, int ms = 60) {
    for (int i = 0; i < n; i++) { Board::white(true); delay(ms); Board::white(false); delay(ms); }
  }
  inline void blipBlue(int n, int ms = 60) {
    for (int i = 0; i < n; i++) { Board::blue(true); delay(ms); Board::blue(false); delay(ms); }
  }
  void rxPacket()  { blipWhite(2, 80); }   // a node packet was received
  void ackSent()   { blipBlue(1, 80);  }   // ACK transmitted to the node
  void relaySent() { blipBlue(2, 80);  }   // packet relayed onward to the gateway
  void dropDup()   { blipWhite(3, 40); }   // duplicate: re-ACK'd but not relayed again
  void fatal()     { while (true) { Board::blue(true); delay(80); Board::blue(false); delay(80); Board::service(); } }

  void idleTick() {  // brief blue heartbeat every 2 s = armed and listening
    static uint32_t t = 0; static bool on = false;
    uint32_t now = millis();
    if (!on && now - t >= 2000) { Board::blue(true);  on = true;  t = now; }
    if (on  && now - t >= 20)   { Board::blue(false); on = false; }
  }
} // namespace Led

// ============================================================================
//  LoRa radio — SX1262   (init PROVEN on this board; PHY MATCHES THE ISL BOARD)
//    CS=D4  DIO1=D1  RST=D2  BUSY=D3  on the default SPI
// ============================================================================
SX1262 radio = new Module(D4, D1, D2, D3);

namespace Lora {
  // --- PHY: MUST match the ISL node + gateway exactly (production/v7). ---
  constexpr float    FREQ_MHZ  = 915.0;   // US/MX 915 MHz band
  constexpr float    BW_KHZ    = 250.0;   // ISL LORA_BW=1 -> 250 kHz  (NOT the 125 kHz ping-pong demo)
  constexpr uint8_t  SF        = 7;
  constexpr uint8_t  CR        = 5;       // 4/5  (ISL LORA_CR=0)
  constexpr uint8_t  SYNC_WORD = 0x12;    // private network
  constexpr uint16_t PREAMBLE  = 8;
  constexpr float    TCXO_V    = 1.8;     // DIO3 powers the module TCXO at 1.8 V (mandatory)
  // TX power for the repeater's own transmissions (ACK to the node + relay to the
  // gateway). The ISL receiver used 14 dBm; a repeater exists to extend range, so
  // we default to the module max (22 dBm). The node doesn't care about ACK power.
  constexpr int8_t   POWER_DBM = 22;

  volatile bool rxFlag = false;
  void onRx() { rxFlag = true; }

  // Re-arm continuous receive and clear any stale flag (e.g. a TxDone edge).
  void armReceive() { rxFlag = false; radio.startReceive(); }

  void begin() {
    int st = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNC_WORD, POWER_DBM, PREAMBLE, TCXO_V);
    if (st != RADIOLIB_ERR_NONE) { LOGF("[LoRa] begin() failed: "); LOG(st); Led::fatal(); }
    radio.setDio2AsRfSwitch(true);          // mandatory on this module (or TX power is lost)
    radio.setPacketReceivedAction(onRx);
    LOG("[LoRa] ready @ 915 MHz SF7 BW250 CR4/5 sync=0x12  (matches ISL board)");
  }
} // namespace Lora

// ============================================================================
//  PARSE — pull <id,seq> out of a node packet. Accepts both forms:
//    "051,7,lat,lon,v,ts,SV=n"   (bare ISL packet)
//    "CUST,051,7,count|..."      (Custodia mesh packet)
//  Returns false for ACK frames, empty payloads, or anything without a numeric
//  id + numeric seq in the first two data fields.
// ============================================================================
struct NodeKey { uint16_t id; uint32_t seq; };

bool parseNodeKey(const String& raw, NodeKey& k) {
  String msg = raw; msg.trim();
  if (msg.length() == 0) return false;
  if (msg.startsWith("ACK,")) return false;          // never re-ACK/relay an ACK

  int base = 0;
  if (msg.startsWith("CUST,")) base = 5;             // skip the "CUST," tag -> id starts here

  int c1 = msg.indexOf(',', base);          if (c1 < 0) return false;
  int c2 = msg.indexOf(',', c1 + 1);        if (c2 < 0) return false;
  String idStr  = msg.substring(base, c1);
  String seqStr = msg.substring(c1 + 1, c2);
  if (idStr.length() == 0 || seqStr.length() == 0) return false;

  // Validate numeric (toInt() returns 0 on garbage, so check the chars).
  for (uint16_t i = 0; i < idStr.length();  i++) if (!isDigit(idStr[i]))  return false;
  for (uint16_t i = 0; i < seqStr.length(); i++) if (!isDigit(seqStr[i])) return false;

  k.id  = (uint16_t)idStr.toInt();
  k.seq = (uint32_t)seqStr.toInt();
  return true;
}

// ----------------------------------------------------------------------------
//  v15 GPS backup-cell health (display only — does NOT affect ACK/relay/dedup).
//  The ISL collar (v15) appends key=val fields to its packet:
//    "<id>,<seq>,lat,lon,v,ts,SV=<n>,TTFF=<s>,CELL=<OK|LOW|DEAD>"
//  We relay the payload VERBATIM regardless, so these need no change on the
//  relay/ACK path. tokenVal() just pulls a "KEY=" value out for the log so a
//  visitor can see, per collar, whether its GPS backup cell still holds
//  ephemeris (CELL=DEAD / a large TTFF => that collar's L76K MS621FE is flat).
// ----------------------------------------------------------------------------
String tokenVal(const String& msg, const char* key) {
  String k = String(key);
  int at = msg.indexOf(k);
  if (at < 0) return "";
  int start = at + k.length();
  int end = msg.indexOf(',', start);
  if (end < 0) end = msg.length();
  return msg.substring(start, end);
}

// ============================================================================
//  DEDUP — small ring of recently RELAYED <id,seq> keys.
// ============================================================================
NodeKey  dedup[DEDUP_SIZE];
uint8_t  dedupCount = 0;   // valid entries (until the ring fills)
uint8_t  dedupHead  = 0;   // next write slot

bool alreadyRelayed(const NodeKey& k) {
  for (uint8_t i = 0; i < dedupCount; i++)
    if (dedup[i].id == k.id && dedup[i].seq == k.seq) return true;
  return false;
}

void rememberRelayed(const NodeKey& k) {
  dedup[dedupHead] = k;
  dedupHead = (dedupHead + 1) % DEDUP_SIZE;
  if (dedupCount < DEDUP_SIZE) dedupCount++;
}

// ============================================================================
//  APP  — state + headless-friendly logging (banner/status/heartbeat)
// ============================================================================
uint32_t rxCount = 0, ackCount = 0, relayCount = 0, dupCount = 0, badCount = 0;
uint32_t bootMs    = 0;       // millis() at end of setup() (uptime origin)
uint32_t lastPktMs = 0;       // millis() of the last received node packet (0 = none yet)
bool     serialWas = false;   // serial-monitor connection edge tracker

// Human-readable uptime "Dd HH:MM:SS".
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

// Full identity + configuration dump. Printed at boot AND whenever a monitor is
// (re)connected, so a visitor always knows exactly what they're looking at.
void printStartupBanner() {
  LOG("");
  LOG("================================================================");
  LOG("  SolarNode LoRa Repeater  -  firmware v1  (v15-aware: GPS-CELL health)");
  LOG("  Board : SenseCAP Solar Node P1-Pro (XIAO nRF52840 Plus + SX1262)");
  LOG("  Built : " __DATE__ "  " __TIME__);
  LOG("================================================================");
  LOG("[ROLE] RX node  ->  ACK node  ->  relay onward to gateway");
  LOG("       (Meshtastic-style managed flood, <id,seq> dedup)");
  LOG(String("[CFG]  repeaterID=") + REPEATER_ID +
      "  ackDelay=" + ACK_TX_DELAY_MS + "ms  relayGap=" + RELAY_GAP_MS +
      "ms  dedup=" + DEDUP_SIZE + "  heartbeat=" + HEARTBEAT_SEC + "s");
  LOG(String("[PHY]  915 MHz  BW=") + String(Lora::BW_KHZ, 0) + "kHz  SF" + Lora::SF +
      "  CR4/" + Lora::CR + "  sync=0x12  preamble=" + Lora::PREAMBLE +
      "  TXpwr=" + Lora::POWER_DBM + "dBm  TCXO=1.8V  (matches ISL board)");
  LOG("[PINS] LoRa CS=D4 DIO1=D1 RST=D2 BUSY=D3 | LED white=P0.15 blue=P0.19 | PWRbtn=P1.01");
}

// One-line live status: uptime, counters, dedup fill, last-packet age.
void printStatus() {
  LOG(String("[STAT] up ") + upStr() + "  |  rx=" + rxCount + " ack=" + ackCount +
      " relay=" + relayCount + " dup=" + dupCount + " bad=" + badCount +
      "  |  dedup " + dedupCount + "/" + DEDUP_SIZE);
  if (lastPktMs == 0) LOG("[STAT] no node packets heard yet - listening");
  else LOG(String("[STAT] last packet ") + ((millis() - lastPktMs) / 1000UL) + " s ago");
}

// Called every loop: detect a monitor being (re)opened -> greet with the full
// banner + status; and while connected, emit a heartbeat every HEARTBEAT_SEC.
// All output is if(Serial)-guarded inside LOG(), so this is free when headless.
void serviceSerial() {
  bool now = (bool)Serial;
  if (now && !serialWas) {                 // a human just opened the serial monitor
    printStartupBanner();
    LOG("[USB] monitor connected - repeater was never paused; resuming live logs");
    printStatus();
    LOG("[LoRa] listening for node packets...");
  }
  serialWas = now;

  static uint32_t lastHb = 0;
  if (now && (millis() - lastHb) >= HEARTBEAT_SEC * 1000UL) {
    lastHb = millis();
    printStatus();
  }
}

void setup() {
  Board::begin();               // USB-CDC up, LEDs, power button, boot LED feedback
  bootMs = millis();

  printStartupBanner();         // seen only if a monitor is already attached at boot
  Lora::begin();                // prints "[LoRa] ready..." (or flashes fatal on failure)
  printStatus();

  Lora::armReceive();
  LOG("[LoRa] listening for node packets...");

  serialWas = (bool)Serial;     // so loop() doesn't immediately re-print the banner
}

void loop() {
  Board::service();                       // power-off by button (5 s) — do not remove
  serviceSerial();                        // monitor (re)connect banner + heartbeat (headless-safe)

  if (!Lora::rxFlag) { Led::idleTick(); return; }
  Lora::rxFlag = false;

  // --- 1) receive ---
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

  NodeKey k;
  if (!parseNodeKey(msg, k)) {
    badCount++;
    LOG("[RX] not a node data packet (ACK/garbage) — ignoring, not ACK'd");
    Lora::armReceive();
    return;
  }
  LOG(String("[RX] parsed id=") + k.id + " seq=" + k.seq);

  // --- 1b) v15 GPS backup-cell health (log only; relay is still verbatim) ---
  // Pull SV / TTFF / CELL if the collar sent them (v15+). Empty => older node.
  String cell = tokenVal(msg, "CELL=");
  if (cell.length()) {
    String sv   = tokenVal(msg, "SV=");
    String ttff = tokenVal(msg, "TTFF=");
    String flag = (cell == "DEAD") ? "   <<< GPS CELL DEAD - check/replace this collar >>>"
                : (cell == "LOW")  ? "   (cell low - collar is charging it)"
                : "";
    LOG(String("[GPS-CELL] id=") + k.id + "  SV=" + sv +
        "  TTFF=" + ttff + "s  CELL=" + cell + flag);
  }

  // --- 2) ACK the node (same handshake as the ISL receiver) ---
  // Wait for the node's TX->RX turnaround FIRST, then reply, so the ACK doesn't
  // fly past an un-listening node (the proven intermittent-ACK fix).
  String ack = String("ACK,") + k.id + "," + k.seq;
  delay(ACK_TX_DELAY_MS);
  Board::blue(true);
  st = radio.transmit(ack);
  Board::blue(false);
  if (st == RADIOLIB_ERR_NONE) {
    ackCount++;
    LOG(String("[ACK] sent \"") + ack + "\"  (" + ackCount + " total)");
    Led::ackSent();
  } else {
    LOGF("[ACK] TX error: "); LOG(st);   // relay is still attempted below
  }

  // --- 3) relay onward to the gateway (managed-flood dedup) ---
  if (alreadyRelayed(k)) {
    dupCount++;
    LOG(String("[RELAY] duplicate id=") + k.id + " seq=" + k.seq +
        " — re-ACK'd, NOT relayed again (" + dupCount + " dups)");
    Led::dropDup();
  } else {
    delay(RELAY_GAP_MS);                 // let the ACK finish + the gateway settle into RX
    Board::blue(true);
    st = radio.transmit(msg);            // relay the VERBATIM node payload
    Board::blue(false);
    if (st == RADIOLIB_ERR_NONE) {
      rememberRelayed(k);
      relayCount++;
      LOG(String("[RELAY] forwarded id=") + k.id + " seq=" + k.seq +
          " onward (" + relayCount + " total)");
      Led::relaySent();
    } else {
      LOGF("[RELAY] TX error: "); LOG(st);   // not remembered -> a later copy can retry the relay
    }
  }

  Lora::armReceive();                     // back to listening for the next node packet
}
