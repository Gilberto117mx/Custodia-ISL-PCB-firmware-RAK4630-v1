/*
 * ============================================================================
 * ISL v9 Drone Receiver  ·  Bluefruit central, custom-open NUS offload
 * ============================================================================
 * Pairs with ISL_Production_v9 (the RUI3 tracker). The tracker exposes the Nordic
 * UART Service via api.ble.custom with RAK_SET_OPEN (NO pairing) and blasts its
 * accelerometer ring ONE-DIRECTIONALLY as fixed 20-byte frames ([len][payload]).
 * There is no GO/ACK — reliability comes from the tracker re-sending its whole
 * ring each pass and this receiver DEDUPING by record seq.
 *
 * Discovery: by NUS SERVICE UUID (the custom advertiser truncates the name to
 * "Cust", but it DOES advertise the service UUID), so we filter on it.
 *
 * Frame -> stream -> lines:
 *   each 20 B notification = [validLen][payload…]; we concatenate payloads and
 *   split on '\n'. Line grammar:
 *     "R <seq> <ts> <n> <sum>"   start of a record (n samples, checksum sum)
 *     "<x>,<y>,<z>"              a sample
 *     "E <nRecs>"                end of the blast
 *
 * Board: Adafruit / WisBlock BSP (bluefruit.h).
 * ============================================================================
 */

#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

BLEClientUart clientUart;

// ---- dedup: seqs already fully + correctly received ----
#define SEEN_MAX 512
static uint32_t seen[SEEN_MAX]; static int seenN = 0;
static bool isSeen(uint32_t s){ for(int i=0;i<seenN;i++) if(seen[i]==s) return true; return false; }
static void markSeen(uint32_t s){ if(seenN<SEEN_MAX && !isSeen(s)) seen[seenN++]=s; }

// ---- current record being reassembled ----
static bool     curActive=false, curSkip=false;
static uint32_t curSeq=0, curTs=0, curSum=0, curCalc=0;
static uint16_t curN=0, curGot=0;
static uint32_t totalRecs=0, totalDup=0, totalBad=0;

static void handleLine(char *ln)
{
  if (ln[0]=='R' && ln[1]==' ') {
    unsigned long seq=0, ts=0, sum=0; unsigned nn=0;
    sscanf(ln, "R %lu %lu %u %lu", &seq, &ts, &nn, &sum);
    curSeq=seq; curTs=ts; curN=nn; curSum=sum; curGot=0; curCalc=0; curActive=true;
    curSkip = isSeen(seq);
    if (curSkip) { totalDup++; Serial.printf("[REC] seq=%lu (dup, skipping)\n", seq); }
    else         Serial.printf("[REC] seq=%lu ts=%lu n=%u ...\n", seq, ts, nn);

  } else if (ln[0]=='E' && ln[1]==' ') {
    unsigned nrecs=0; sscanf(ln, "E %u", &nrecs);
    Serial.printf("== END of blast: %u records sent | this session: new=%lu dup=%lu bad=%lu ==\n\n",
                  nrecs, (unsigned long)totalRecs, (unsigned long)totalDup, (unsigned long)totalBad);
    curActive=false;

  } else if (curActive && !curSkip) {
    int x,y,z;
    if (sscanf(ln, "%d,%d,%d", &x,&y,&z)==3) {
      curCalc += (uint16_t)(int16_t)x + (uint16_t)(int16_t)y + (uint16_t)(int16_t)z;
      curGot++;
      if (curGot==curN) {                         // record complete -> verify
        if (curCalc==curSum) { markSeen(curSeq); totalRecs++;
          Serial.printf("      seq=%lu OK (%u samples, checksum match)\n", (unsigned long)curSeq, curN); }
        else { totalBad++;
          Serial.printf("      seq=%lu BAD checksum (got %lu want %lu) - not marked, will retry\n",
                        (unsigned long)curSeq, (unsigned long)curCalc, (unsigned long)curSum); }
        curActive=false;
      }
    }
  }
}

// ---- 20-byte frame reassembly -> line assembly ----
static void feedByte(char c)
{
  static char lb[80]; static int lp=0;
  if (c=='\n' || c=='\r') { if (lp>0){ lb[lp]=0; handleLine(lb); lp=0; } }
  else if (lp < (int)sizeof(lb)-1) lb[lp++]=c;
}
void bleuart_rx_callback(BLEClientUart& uart)
{
  static uint8_t f[20]; static int fp=0;
  while (uart.available()) {
    f[fp++] = uart.read();
    if (fp==20) { fp=0;
      uint8_t len=f[0];
      if (len>=1 && len<=19) for (int i=0;i<len;i++) feedByte((char)f[1+i]);
    }
  }
}

void scan_callback(ble_gap_evt_adv_report_t* report) {
  if (Bluefruit.Scanner.checkReportForService(report, clientUart)) {   // NUS service UUID present
    Serial.println("\n[BLE] Found tracker (NUS service)! Connecting...");
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
  Serial.println("=== ISL v9 Drone Receiver - custom-open NUS, dedup by seq ===");

  Bluefruit.begin(0, 1);                 // 0 peripheral, 1 central
  Bluefruit.setName("Custodia-Drone");

  clientUart.begin();
  clientUart.setRxCallback(bleuart_rx_callback);

  Bluefruit.Central.setConnectCallback(connect_callback);
  Bluefruit.Central.setDisconnectCallback(disconnect_callback);

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.filterUuid(clientUart.uuid);   // match the Nordic UART Service
  Bluefruit.Scanner.useActiveScan(true);
  Bluefruit.Scanner.start(0);

  Serial.println("[BLE] Scanning for the tracker by NUS service UUID...");
}

void loop() {
  // driven by callbacks
}
