/*
 * csi_receiver.ino
 * Role: ESP32 #2 - Promiscuous mode CSI receiver
 * Purpose: Capture WiFi Channel State Information (CSI) from incoming
 *          packets (sent by csi_transmitter.ino) and log each frame as a
 *          CSV row over Serial for the laptop to capture.
 *
 * CSV row format:
 *   timestamp_ms,rssi,csi_len,csi_bytes
 *
 * Where csi_bytes is a semicolon-separated list of raw signed 8-bit CSI
 * values (so it survives being inside one CSV field without extra parsing
 * complexity on the Python side).
 *
 * Pattern follows the ESP32-CSI-Toolkit approach (Steven Hernandez, MoWiNG Lab).
 */

#include <WiFi.h>
#include <WiFiUdp.h>
extern "C" {
  #include "esp_wifi.h"
}

// ---------- Configuration ----------
const int WIFI_CHANNEL = 6; // Must match transmitter's AP channel

// ---------- Ping Configuration ----------
// Periodically sending a small unicast packet to the transmitter and
// receiving its echo back keeps a steady stream of HT-rate unicast frames
// flowing, which reliably triggers CSI extraction - unlike broadcast or
// beacon frames alone, which are sent at legacy rates without LTF fields.
WiFiUDP pingUdp;
const int PING_PORT = 4210;
const unsigned long PING_INTERVAL_MS = 100;
unsigned long lastPingTime = 0;
IPAddress transmitterIp; // Resolved after connecting (the AP's gateway IP)

// Compile-time flag: set to 0 if you want to omit raw CSI bytes later
// (e.g. once you've settled on features and want lighter Serial traffic)
#define INCLUDE_RAW_CSI_BYTES 1

// Transmitter's AP MAC address - only CSI from this source will be logged.
// This filters out ambient/neighboring WiFi traffic on the same channel.
const uint8_t TARGET_MAC[6] = {0x30, 0x76, 0xF5, 0x94, 0xC8, 0xD9};

bool isTargetMac(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] != TARGET_MAC[i]) {
      return false;
    }
  }
  return true;
}

// ---------- Deferred logging buffer ----------
// The CSI callback runs inside the WiFi driver's own task context. Doing
// slow work there (like printing hundreds of values over Serial) can block
// that task long enough to starve loop() of CPU time - which then can't
// send the next ping, so the whole echo cycle silently stalls.
// Fix: the callback only copies data into this buffer and sets a flag;
// all Serial printing happens in loop(), where it's safe to take time.
#define MAX_CSI_LEN 512
volatile bool csiDataReady = false;
unsigned long bufTimestamp;
int bufRssi;
int bufCsiLen;
int8_t bufCsiBytes[MAX_CSI_LEN];

// ---------- CSI callback ----------
// Called automatically by the WiFi driver whenever a CSI-carrying packet is received.
// Keep this FAST - no Serial prints here.
void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf) {
    return; // Guard against malformed/empty callback data
  }

  // Only capture packets that came from our transmitter's MAC.
  if (!isTargetMac(info->mac)) {
    return;
  }

  if (csiDataReady) {
    return; // loop() hasn't consumed the previous row yet - drop this one rather than block
  }

  int csiLen = info->len;
  if (csiLen > MAX_CSI_LEN) {
    csiLen = MAX_CSI_LEN; // Safety clamp, shouldn't normally happen
  }

  bufTimestamp = millis();
  bufRssi = info->rx_ctrl.rssi;
  bufCsiLen = csiLen;
  memcpy(bufCsiBytes, info->buf, csiLen);

  csiDataReady = true; // Signal loop() that a row is ready to print
}

// Prints one CSV row from the buffer. Called from loop(), safe to take time here.
void printBufferedCsiRow() {
  Serial.print(bufTimestamp);
  Serial.print(",");
  Serial.print(bufRssi);
  Serial.print(",");
  Serial.print(bufCsiLen);

#if INCLUDE_RAW_CSI_BYTES
  Serial.print(",");
  for (int i = 0; i < bufCsiLen; i++) {
    Serial.print(bufCsiBytes[i]);
    if (i < bufCsiLen - 1) {
      Serial.print(";");
    }
  }
#endif

  Serial.println();
}

void setup() {
  Serial.begin(921600); // Higher baud reduces time spent transmitting each CSI row,
                         // preventing dropped/delayed frames during bursts.
  delay(500);

  Serial.println();
  Serial.println("=== CSI Receiver Starting ===");

  // Connect to the transmitter's AP as a station. Associated STA<->AP traffic
  // (beacons, ACKs, data) happens at HT rates, which reliably carries the
  // HT-LTF fields needed for CSI extraction - unlike passive promiscuous
  // sniffing of broadcast frames from an AP with no associated clients.
  WiFi.mode(WIFI_STA);
  WiFi.begin("CSI_AP", ""); // Must match transmitter's AP_SSID / AP_PASSWORD

  Serial.print("Connecting to CSI_AP");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 40) {
    delay(250);
    Serial.print(".");
    retries++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: Failed to connect to CSI_AP. Check SSID/power/range. Halting.");
    while (true) { delay(1000); }
  }

  Serial.print("Connected. Receiver IP: ");
  Serial.println(WiFi.localIP());

  transmitterIp = WiFi.gatewayIP(); // The AP's IP, e.g. 192.168.4.1
  Serial.print("Transmitter (AP) IP: ");
  Serial.println(transmitterIp);
  pingUdp.begin(PING_PORT);

  // Configure CSI collection
  wifi_csi_config_t csi_config = {
    .lltf_en = true,
    .htltf_en = true,
    .stbc_htltf2_en = true,
    .ltf_merge_en = true,
    .channel_filter_en = false,
    .manu_scale = false,
    .shift = false
  };

  esp_wifi_set_csi_config(&csi_config);
  esp_wifi_set_csi_rx_cb(&wifi_csi_rx_cb, NULL);
  esp_wifi_set_csi(true);

  Serial.print("Listening for CSI on channel ");
  Serial.println(WIFI_CHANNEL);
  Serial.println("=== Setup complete. Logging CSV rows... ===");
  Serial.println("timestamp_ms,rssi,csi_len,csi_bytes");
}

void loop() {
  // Print any CSI row the callback captured, as fast as possible but safely
  // outside the WiFi driver's task context.
  if (csiDataReady) {
    printBufferedCsiRow();
    csiDataReady = false; // Free the buffer for the next callback
  }

  // Periodically ping the transmitter. Its echo response is what generates
  // continuous unicast HT-rate traffic for the CSI callback to trigger on.
  unsigned long now = millis();
  if (now - lastPingTime >= PING_INTERVAL_MS) {
    lastPingTime = now;
    pingUdp.beginPacket(transmitterIp, PING_PORT);
    pingUdp.print("PING");
    pingUdp.endPacket();
  }
}
