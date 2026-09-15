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
#include <Wire.h>
#include <WiFiUdp.h>
extern "C" {
  #include "esp_wifi.h"
}

// ---------- Configuration ----------
const int WIFI_CHANNEL = 6; // Must match transmitter's AP channel
const int UDP_PORT = 4210;
const unsigned long PING_INTERVAL_MS = 100;
WiFiUDP pingUdp;
IPAddress transmitterIp;
unsigned long lastPingTime = 0;
const uint8_t LCD_ADDRESS = 0x27;
const uint8_t LCD_BACKLIGHT = 0x08;
const uint8_t LCD_ENABLE = 0x04;
const uint8_t LCD_RS = 0x01;

void lcdWriteNibble(uint8_t nibble, uint8_t mode) {
  uint8_t value = (nibble & 0xF0) | LCD_BACKLIGHT | mode;
  Wire.beginTransmission(LCD_ADDRESS);
  Wire.write(value | LCD_ENABLE);
  Wire.endTransmission();
  delayMicroseconds(1);
  Wire.beginTransmission(LCD_ADDRESS);
  Wire.write(value & ~LCD_ENABLE);
  Wire.endTransmission();
  delayMicroseconds(50);
}

void lcdSend(uint8_t value, uint8_t mode) {
  lcdWriteNibble(value & 0xF0, mode);
  lcdWriteNibble((value << 4) & 0xF0, mode);
}

void lcdCommand(uint8_t command) {
  lcdSend(command, 0);
  if (command == 0x01 || command == 0x02) {
    delay(2);
  }
}

void lcdPrint(const String &text) {
  for (size_t i = 0; i < text.length(); i++) {
    lcdSend(text[i], LCD_RS);
  }
}

void lcdSetCursor(uint8_t column, uint8_t row) {
  const uint8_t rowOffsets[] = {0x00, 0x40};
  lcdCommand(0x80 | (column + rowOffsets[row & 1]));
}

void lcdClear() {
  lcdCommand(0x01);
}

void lcdInit() {
  delay(50);
  lcdWriteNibble(0x30, 0);
  delay(5);
  lcdWriteNibble(0x30, 0);
  delayMicroseconds(150);
  lcdWriteNibble(0x30, 0);
  lcdWriteNibble(0x20, 0);
  lcdCommand(0x28);
  lcdCommand(0x08);
  lcdClear();
  lcdCommand(0x06);
  lcdCommand(0x0C);
}

// Compile-time flag: set to 0 if you want to omit raw CSI bytes later
// (e.g. once you've settled on features and want lighter Serial traffic)
#define INCLUDE_RAW_CSI_BYTES 1

bool targetMacReady = false;
uint8_t targetMac[6] = {0};

bool isTargetMac(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] != targetMac[i]) {
      return false;
    }
  }
  return true;
}

// ---------- Deferred CSI buffer ----------
#define MAX_CSI_LEN 512
volatile bool csiDataReady = false;
unsigned long bufferedTimestamp;
int bufferedRssi;
int bufferedCsiLen;
int8_t bufferedCsiBytes[MAX_CSI_LEN];

// The callback must remain fast; serializing 384 values here causes packet loss.
void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf) {
    return; // Guard against malformed/empty callback data
  }

  if (targetMacReady && !isTargetMac(info->mac)) {
    return;
  }

  if (csiDataReady) {
    return;
  }

  int csiLen = info->len;
  if (csiLen > MAX_CSI_LEN) {
    return;
  }
  bufferedTimestamp = millis();
  bufferedRssi = info->rx_ctrl.rssi;
  bufferedCsiLen = csiLen;
  memcpy(bufferedCsiBytes, info->buf, csiLen);
  csiDataReady = true;
}

void printBufferedCsi() {
  if (!csiDataReady) {
    return;
  }
  Serial.print(bufferedTimestamp);
  Serial.print(",");
  Serial.print(bufferedRssi);
  Serial.print(",");
  Serial.print(bufferedCsiLen);
#if INCLUDE_RAW_CSI_BYTES
  Serial.print(",");
  for (int i = 0; i < bufferedCsiLen; i++) {
    Serial.print(bufferedCsiBytes[i]);
    if (i < bufferedCsiLen - 1) {
      Serial.print(";");
    }
  }
#endif
  Serial.println();
  csiDataReady = false;
}

void updateLcdFromSerial() {
  static String command;
  while (Serial.available()) {
    char character = (char)Serial.read();
    if (character == '\n') {
      command.trim();
      if (command.startsWith("LCD:")) {
        String state = command.substring(4);
        lcdClear();
        lcdSetCursor(0, 0);
        lcdPrint("CSI state:");
        lcdSetCursor(0, 1);
        lcdPrint(state);
      }
      command = "";
    } else if (character != '\r') {
      command += character;
    }
  }
}

void setup() {
  Serial.begin(921600);
  delay(500);

  Wire.begin(25, 26);
  lcdInit();
  lcdSetCursor(0, 0);
  lcdPrint("CSI starting...");

  Serial.println();
  Serial.println("=== CSI Receiver Starting ===");

  // Associate with the transmitter AP so ping/echo traffic uses HT rates.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin("CSI_AP", "");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 40) {
    delay(250);
    retries++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    lcdClear();
    lcdSetCursor(0, 0);
    lcdPrint("WiFi failed");
    while (true) {
      delay(1000);
    }
  }
  transmitterIp = WiFi.gatewayIP();
  const uint8_t *connectedMac = WiFi.BSSID();
  if (connectedMac != nullptr) {
    memcpy(targetMac, connectedMac, sizeof(targetMac));
    targetMacReady = true;
  }
  pingUdp.begin(UDP_PORT);

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
  updateLcdFromSerial();
  printBufferedCsi();

  unsigned long now = millis();
  if (now - lastPingTime >= PING_INTERVAL_MS && WiFi.status() == WL_CONNECTED) {
    lastPingTime = now;
    pingUdp.beginPacket(transmitterIp, UDP_PORT);
    pingUdp.print("PING");
    pingUdp.endPacket();
  }
  delay(1);
}
