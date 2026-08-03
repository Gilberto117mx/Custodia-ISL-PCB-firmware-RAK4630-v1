/*
 * ============================================================================
 * ISL v15 Drone Receiver  ·  REFERENCE receiver for the v15 collar
 * (identical to v13/v14 - v15 only changes the collar's GPS/cell handling, not the
 *  BLE wire format; you can reuse the v13/v14 receiver unchanged.)
 * (identical to the v13 receiver - v14 only changes the collar's GPS timing, not the
 *  BLE wire format; you can reuse the v13 receiver unchanged.)
 * ============================================================================
 * NOTE: the real drone will be a different board with its own code. This is a
 * reference/bench receiver so you can verify the COLLAR emits correctly.
 *
 * Transport & behaviour are IDENTICAL to v12 (proven: new-data-per-pass, 0 bad).
 * v13's only addition is a per-record absolute TIMESTAMP on the wire ("T <ts>"),
 * which this receiver now parses and prints (`seq=… ts=… OK …`). Across passes you
 * see all-NEW seqs; the only `dup` is a pass's own 2nd BLE_BLAST_REPEATS blast.
 *
 * Transport: RUI3 `api.ble.uart`, fire-and-forget, NUS advertised by NAME; every
 * collar line fits in ONE ~20-byte NUS notification so a dropped packet loses a whole
 * line instead of corrupting the next one; the batch is sent BLE_BLAST_REPEATS times.
 *
 * We match on COMPLETE LOCAL NAME "Custodia-Tracker" (api.ble.uart advertises the
 * name, not the NUS UUID), discover the standard NUS, subscribe to TXD, then parse
 * the compact stream, verify a 16-bit checksum per record, and dedup by seq:
 *     "I <dev> <nrecs>"          collar id + how many records (announce)
 *     "R <seq> <n> <ck16>"       start of a record (seq, sample-count, 16-bit sum)
 *     "T <ts>"                   v13: this record's unix timestamp (its own line)
 *     "<x>,<y>,<z>"              a sample
 *     "E <dev> <nrecs>"          end of a blast pass
 *
 * We also accept Just-Works pairing (IO caps = None) so that, if the RUI3 collar's
 * central-initiated security request would otherwise "fail", the link can encrypt
 * and stabilise instead. On this RUI3 build notifications flow even unencrypted, so
 * this is best-effort robustness, not a hard requirement.
 *
 * Board: Adafruit / WisBlock BSP (bluefruit.h). Flash this to your receiver
 * nRF52840, not the collar.
 * ============================================================================
 */

#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

#define TARGET_NAME  "Custodia-Tracker"

BLEClientUart clientUart;

#define SEEN_MAX 512
static uint32_t seen[SEEN_MAX]; static int seenN = 0;
static bool isSeen(uint32_t s){ for(int i=0;i<seenN;i++) if(seen[i]==s) return true; return false; }
static void markSeen(uint32_t s){ if(seenN<SEEN_MAX && !isSeen(s)) seen[seenN++]=s; }

static bool     curActive=false, curSkip=false;
static uint32_t curDev=0, curSeq=0, curTs=0;
static uint16_t curN=0, curGot=0, curSum=0, curCalc=0;
static uint32_t totalRecs=0, totalDup=0, totalBad=0;

static void handleLine(char *ln)
{
  if (ln[0]=='I' && ln[1]==' ') {                        // "I <dev> <nrecs>"
    unsigned dev=0, nn=0; sscanf(ln, "I %u %u", &dev, &nn);
    curDev=dev;
    Serial.printf("\n== Collar id=%03u announcing %u records ==\n", dev, nn);

  } else if (ln[0]=='R' && ln[1]==' ') {                 // "R <seq> <n> <ck16>"
    unsigned long seq=0; unsigned nn=0, ck=0;
    sscanf(ln, "R %lu %u %u", &seq, &nn, &ck);
    // Sanity-guard the sample count: a corrupt header (e.g. from a lost fragment on
    // an older build) must not wedge accumulation. Valid records are <= 200 samples.
    if (nn==0 || nn>200) { curActive=false; return; }
    curSeq=seq; curN=nn; curSum=(uint16_t)ck; curTs=0; curGot=0; curCalc=0; curActive=true;
    curSkip = isSeen(seq);
    if (curSkip) { totalDup++; Serial.printf("[REC] seq=%lu (dup, skipping)\n", seq); }
    else         Serial.printf("[REC] seq=%lu n=%u ck=%u ...\n", seq, nn, ck);

  } else if (ln[0]=='T' && ln[1]==' ') {                 // "T <ts>"  (v13 per-record unix timestamp)
    unsigned long ts=0; sscanf(ln, "T %lu", &ts); curTs=ts;

  } else if (ln[0]=='E' && ln[1]==' ') {                 // "E <dev> <nrecs>"
    unsigned dev=0, nrecs=0; sscanf(ln, "E %u %u", &dev, &nrecs);
    Serial.printf("== END id=%03u: %u records | session new=%lu dup=%lu bad=%lu ==\n\n",
                  dev, nrecs, (unsigned long)totalRecs, (unsigned long)totalDup, (unsigned long)totalBad);
    curActive=false;

  } else if (curActive && !curSkip) {                    // "x,y,z"
    int x,y,z;
    if (sscanf(ln, "%d,%d,%d", &x,&y,&z)==3) {
      curCalc += (uint16_t)(int16_t)x + (uint16_t)(int16_t)y + (uint16_t)(int16_t)z;
      curGot++;
      if (curGot==curN) {
        if (curCalc==curSum) { markSeen(curSeq); totalRecs++;
          Serial.printf("      seq=%lu ts=%lu OK (%u samples, checksum match)\n",
                        (unsigned long)curSeq, (unsigned long)curTs, curN); }
        else { totalBad++;
          Serial.printf("      seq=%lu BAD checksum (got %u want %u)\n",
                        (unsigned long)curSeq, curCalc, curSum); }
        curActive=false;
      }
    }
  }
}

// Split the raw NUS byte stream on newlines and hand complete lines to handleLine.
static void feedByte(char c)
{
  static char lb[80]; static int lp=0;
  if (c=='\n' || c=='\r') { if (lp>0){ lb[lp]=0; handleLine(lb); lp=0; } }
  else if (lp < (int)sizeof(lb)-1) lb[lp++]=c;
}

void bleuart_rx_callback(BLEClientUart& uart)
{
  while (uart.available()) feedByte((char)uart.read());
}

// Match by NAME (RUI3 api.ble.uart advertises the name, not the service UUID).
void scan_callback(ble_gap_evt_adv_report_t* report) {
  uint8_t buf[32];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, buf, sizeof(buf) - 1);
  if (len == 0) {
    len = Bluefruit.Scanner.parseReportByType(
        report, BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME, buf, sizeof(buf) - 1);
  }

  bool match = false;
  if (len > 0) { buf[len] = 0; match = (strcmp((char*)buf, TARGET_NAME) == 0); }

  if (match) {
    Serial.println("\n[BLE] Found Custodia-Tracker (by name)! Connecting...");
    Bluefruit.Central.connect(report);
  } else {
    Bluefruit.Scanner.resume();
  }
}

void connect_callback(uint16_t conn_handle) {
  Serial.print("[BLE] Connected! Discovering NUS... ");
  if (clientUart.discover(conn_handle)) {
    Serial.println("ok.");
    if (clientUart.enableTXD()) Serial.println("[BLE] subscribed - receiving blast:\n");
    else                        Serial.println("[BLE] enableTXD FAILED");
  } else {
    Serial.println("NUS not found, disconnecting.");
    Bluefruit.disconnect(conn_handle);
  }
}

void pairing_complete_callback(uint16_t conn_handle, uint8_t auth_status) {
  (void)conn_handle;
  if (auth_status == BLE_GAP_SEC_STATUS_SUCCESS) Serial.println("[BLE] pairing success (encrypted link)");
  else Serial.printf("[BLE] pairing failed (0x%02X) - continuing unencrypted\n", auth_status);
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle; (void)reason;
  curActive=false;
  Serial.println("\n[BLE] Disconnected - resuming scan...");
  Bluefruit.Scanner.start(0);
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  Serial.println("=== ISL v15 Drone Receiver (reference) - api.ble.uart NUS, name-match, NEW data + per-record ts ===");

  Bluefruit.begin(0, 1);
  Bluefruit.setName("Custodia-Drone");

  // Accept Just-Works pairing (No input / No output -> no passkey, no MITM) so the
  // collar's central-initiated security request can complete and encrypt the link.
  Bluefruit.Security.setIOCaps(false, false, false);
  Bluefruit.Security.setMITM(false);
  Bluefruit.Security.setPairCompleteCallback(pairing_complete_callback);

  clientUart.begin();
  clientUart.setRxCallback(bleuart_rx_callback);

  Bluefruit.Central.setConnectCallback(connect_callback);
  Bluefruit.Central.setDisconnectCallback(disconnect_callback);

  // NOTE: no Scanner.filterUuid() -- the RUI3 collar doesn't advertise the NUS
  // UUID, so we filter by name inside scan_callback instead.
  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.useActiveScan(true);           // needed to receive the scan-response name
  Bluefruit.Scanner.start(0);

  Serial.println("[BLE] Scanning for 'Custodia-Tracker' by name...");
}

void loop() { /* driven by callbacks */ }
