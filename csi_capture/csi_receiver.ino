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
extern "C" {
  #include "esp_wifi.h"
}

// ---------- Configuration ----------
const int WIFI_CHANNEL = 6; // Must match transmitter's AP channel

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

// ---------- CSI callback ----------
// Called automatically by the WiFi driver whenever a CSI-carrying packet is received
void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf) {
    return; // Guard against malformed/empty callback data
  }

  // Only log packets that came from our transmitter's MAC.
  // info->mac holds the source address of the packet this CSI was extracted from.
  if (!isTargetMac(info->mac)) {
    return;
  }

  unsigned long timestamp = millis();
  int rssi = info->rx_ctrl.rssi;
  int csiLen = info->len;

  // Build CSV row: timestamp, rssi, csi_len, [csi bytes...]
  Serial.print(timestamp);
  Serial.print(",");
  Serial.print(rssi);
  Serial.print(",");
  Serial.print(csiLen);

#if INCLUDE_RAW_CSI_BYTES
  Serial.print(",");
  for (int i = 0; i < csiLen; i++) {
    Serial.print((int8_t)info->buf[i]);
    if (i < csiLen - 1) {
      Serial.print(";"); // Separate raw CSI values with ';' inside the single CSV field
    }
  }
#endif

  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=== CSI Receiver Starting ===");

  // Put WiFi into station mode but don't connect to anything -
  // we just need the radio active and locked to the right channel.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Lock to the same channel as the transmitter's AP
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

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
  // Nothing needed here - all work happens in the wifi_csi_rx_cb callback
  delay(1000);
}
