/*
 * ============================================================================
 * ISL v10 Drone Receiver  ·  REFERENCE receiver for the v10 collar
 * ============================================================================
 * NOTE: the real drone will be a different board with its own code. This is a
 * reference/bench receiver so you can verify the COLLAR emits correctly.
 *
 * v10 uses the SAME proven BLE transport as v8: the collar (ISL_Production_v10)
 * exposes the Nordic UART Service via RUI3 `api.ble.uart` (fire-and-forget) and
 * advertises the device NAME only (its api.ble.uart does NOT put the NUS UUID in
 * the advertising packet). So we match on the COMPLETE LOCAL NAME
 * "Custodia-Tracker" (exactly like the v8 receiver), then discover the standard
 * NUS and subscribe to TXD. Data arrives as a plain newline-delimited byte
 * stream; we split on '\n' and parse the ID-tagged records, verify each record's
 * checksum, and dedup by seq:
 *     "ID <dev> recs=<N>"                          collar id + how many records
 *     "R <dev> <seq> <ts> <n> <sum>"               start of a record
 *     "<x>,<y>,<z>"                                 a sample
 *     "E <dev> <N>"                                 end of the blast
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
static uint32_t curDev=0, curSeq=0, curTs=0, curSum=0, curCalc=0;
static uint16_t curN=0, curGot=0;
static uint32_t totalRecs=0, totalDup=0, totalBad=0;

static void handleLine(char *ln)
{
  if (ln[0]=='I' && ln[1]=='D') {                         // "ID <dev> recs=<N>"
    unsigned dev=0, nn=0; sscanf(ln, "ID %u recs=%u", &dev, &nn);
    Serial.printf("\n== Collar id=%03u announcing %u records ==\n", dev, nn);

  } else if (ln[0]=='R' && ln[1]==' ') {                  // "R <dev> <seq> <ts> <n> <sum>"
    unsigned dev=0; unsigned long seq=0, ts=0, sum=0; unsigned nn=0;
    sscanf(ln, "R %u %lu %lu %u %lu", &dev, &seq, &ts, &nn, &sum);
    curDev=dev; curSeq=seq; curTs=ts; curN=nn; curSum=sum; curGot=0; curCalc=0; curActive=true;
    curSkip = isSeen(seq);
    if (curSkip) { totalDup++; Serial.printf("[REC] id=%03u seq=%lu (dup, skipping)\n", dev, seq); }
    else         Serial.printf("[REC] id=%03u seq=%lu ts=%lu n=%u ...\n", dev, seq, ts, nn);

  } else if (ln[0]=='E' && ln[1]==' ') {                  // "E <dev> <N>"
    unsigned dev=0, nrecs=0; sscanf(ln, "E %u %u", &dev, &nrecs);
    Serial.printf("== END id=%03u: %u records | session new=%lu dup=%lu bad=%lu ==\n\n",
                  dev, nrecs, (unsigned long)totalRecs, (unsigned long)totalDup, (unsigned long)totalBad);
    curActive=false;

  } else if (curActive && !curSkip) {                     // "x,y,z"
    int x,y,z;
    if (sscanf(ln, "%d,%d,%d", &x,&y,&z)==3) {
      curCalc += (uint16_t)(int16_t)x + (uint16_t)(int16_t)y + (uint16_t)(int16_t)z;
      curGot++;
      if (curGot==curN) {
        if (curCalc==curSum) { markSeen(curSeq); totalRecs++;
          Serial.printf("      id=%03u seq=%lu OK (%u samples, checksum match)\n",
                        (unsigned)curDev, (unsigned long)curSeq, curN); }
        else { totalBad++;
          Serial.printf("      id=%03u seq=%lu BAD checksum (got %lu want %lu)\n",
                        (unsigned)curDev, (unsigned long)curSeq, (unsigned long)curCalc, (unsigned long)curSum); }
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
  Serial.println("=== ISL v10 Drone Receiver (reference) - api.ble.uart NUS, name-match, dedup by seq ===");

  Bluefruit.begin(0, 1);
  Bluefruit.setName("Custodia-Drone");

  clientUart.begin();
  clientUart.setRxCallback(bleuart_rx_callback);

  Bluefruit.Central.setConnectCallback(connect_callback);
  Bluefruit.Central.setDisconnectCallback(disconnect_callback);

  // NOTE: no Scanner.filterUuid() -- the RUI3 collar doesn't advertise the NUS
  // UUID, so we filter by name inside scan_callback instead (same as v8).
  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.useActiveScan(true);           // needed to receive the scan-response name
  Bluefruit.Scanner.start(0);

  Serial.println("[BLE] Scanning for 'Custodia-Tracker' by name...");
}

void loop() { /* driven by callbacks */ }
