#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

BLEClientUart clientUart;

void scan_callback(ble_gap_evt_adv_report_t* report) {
  if (Bluefruit.Scanner.checkReportForService(report, clientUart)) {
    Serial.println("\n[BLE] Found Custodia Tracker! Connecting...");
    Bluefruit.Central.connect(report);
  } else {
    Bluefruit.Scanner.resume();
  }
}

void connect_callback(uint16_t conn_handle) {
  Serial.print("[BLE] Connected! Discovering BLE UART service... ");

  if (clientUart.discover(conn_handle)) {
    Serial.println("Discovered.");

    // Enable notifications on peripheral's TX characteristic
    if (clientUart.enableTXD()) {
      Serial.println("[BLE] Subscribed to incoming data stream.");
    } else {
      Serial.println("[BLE] Failed to subscribe to TXD!");
    }
  } else {
    Serial.println("Failed to discover BLE UART service.");
    Bluefruit.disconnect(conn_handle);
  }
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;
  (void)reason;
  Serial.println("\n[BLE] Disconnected — resuming scan...");
  Bluefruit.Scanner.start(0);
}

void bleuart_rx_callback(BLEClientUart& uart) {
  while (uart.available()) {
    char c = (char)uart.read();
    Serial.print(c);
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  Serial.println("=== Custodia Receiver — BLE Central ===");

  // Initialize Bluefruit with 0 Peripherals and 1 Central connection
  Bluefruit.begin(0, 1);
  Bluefruit.setName("Custodia-Receiver");

  // Initialize Client UART service
  clientUart.begin();
  clientUart.setRxCallback(bleuart_rx_callback);

  // Set callbacks
  Bluefruit.Central.setConnectCallback(connect_callback);
  Bluefruit.Central.setDisconnectCallback(disconnect_callback);

  // Set scanner callbacks and options
  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  Bluefruit.Scanner.setInterval(160, 80);           // In units of 0.625 ms
  Bluefruit.Scanner.filterUuid(clientUart.uuid);     // Filter specifically for Nordic UART Service
  Bluefruit.Scanner.useActiveScan(true);
  Bluefruit.Scanner.start(0);                        // Scan indefinitely

  Serial.println("[BLE] Scanning for emitter...");
}

void loop() {
  // Driven entirely by BLE callbacks
}
