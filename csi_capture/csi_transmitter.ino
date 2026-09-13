/*
 * csi_transmitter.ino
 * Role: ESP32 #1 - Access Point + UDP broadcaster
 * Purpose: Generate consistent WiFi traffic so ESP32 #2 (receiver) can
 *          extract CSI (Channel State Information) from incoming packets.
 *
 * Behavior:
 *  - Starts a WiFi AP with SSID "CSI_AP" on channel 6
 *  - Broadcasts a small UDP packet every 100ms to keep CSI frames flowing
 *  - Blinks the onboard LED as a heartbeat so you can visually confirm
 *    the transmitter is alive and sending packets
 */

#include <WiFi.h>
#include <WiFiUdp.h>

// ---------- AP Configuration ----------
const char* AP_SSID     = "CSI_AP";
const char* AP_PASSWORD = "";        // Open network. Set a password here if you want WPA2.
const int   WIFI_CHANNEL = 6;

// ---------- UDP Configuration ----------
WiFiUDP udp;
const int UDP_PORT = 4210;
const IPAddress BROADCAST_IP(192, 168, 4, 255); // Default ESP32 AP subnet broadcast address
const unsigned long SEND_INTERVAL_MS = 100;

// ---------- LED Heartbeat ----------
const int LED_PIN = 2; // Onboard LED on most ESP32 WROOM-32 dev boards
const unsigned long HEARTBEAT_INTERVAL_MS = 500; // Blink every 500ms (slower than packet rate, just a "still alive" signal)

// ---------- Timing state ----------
unsigned long lastSendTime = 0;
unsigned long lastHeartbeatTime = 0;
bool ledState = false;
uint32_t packetCounter = 0;

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println();
  Serial.println("=== CSI Transmitter Starting ===");

  // Start AP with fixed channel (important: receiver must listen on the same channel)
  WiFi.mode(WIFI_AP);
  bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, WIFI_CHANNEL);

  if (apStarted) {
    Serial.print("AP started. SSID: ");
    Serial.print(AP_SSID);
    Serial.print(" | Channel: ");
    Serial.println(WIFI_CHANNEL);
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("ERROR: Failed to start AP. Halting.");
    while (true) { delay(1000); }
  }

  udp.begin(UDP_PORT);
  Serial.print("UDP broadcasting on port ");
  Serial.println(UDP_PORT);
  Serial.println("=== Setup complete. Broadcasting... ===");
}

void loop() {
  unsigned long now = millis();

  // ---- Send UDP packet every SEND_INTERVAL_MS ----
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    sendUdpPacket();
  }

  // ---- Heartbeat LED blink every HEARTBEAT_INTERVAL_MS ----
  if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatTime = now;
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }
}

void sendUdpPacket() {
  packetCounter++;

  // Small fixed payload; content doesn't matter much for CSI purposes,
  // it just needs to be a real packet the receiver can capture.
  String payload = "CSI_PKT," + String(packetCounter) + "," + String(millis());

  udp.beginPacket(BROADCAST_IP, UDP_PORT);
  udp.print(payload);
  udp.endPacket();

  // Optional lightweight debug (comment out if it clutters Serial during data collection)
  // Serial.println(payload);
}
