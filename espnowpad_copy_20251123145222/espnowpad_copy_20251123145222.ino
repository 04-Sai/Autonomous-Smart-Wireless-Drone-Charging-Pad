#include <esp_now.h>
#include <WiFi.h>

#define LED_PIN 42

// Packet format - must match sender exactly
typedef struct __attribute__((packed)) {
  uint8_t charging;   // 0 = false, 1 = true
  uint8_t percent;    // 0..100
  float voltage;      // float voltage e.g. 12.34
} sensor_packet_t;

// Global state -> you can use these in your web server code
volatile bool lastCharging = false;
volatile uint8_t lastPercent = 0;
volatile float lastVoltage = 0.0;

void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Configure WiFi in station mode (required for ESP-NOW)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); // drop any previous connections
  Serial.print("SLAVE MAC: ");
  Serial.println(WiFi.macAddress());

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }

  // Register receive callback (new-style for IDF v5)
  esp_now_register_recv_cb(onDataRecv);

  Serial.println("ESP-NOW receiver ready");
}

void loop() {
  // nothing required here for receive-only; keep loop free for web server or other tasks
  delay(500);
}

void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  // Print sender MAC
  char macStr[18];
  const uint8_t *mac = recv_info->src_addr;
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print("Received from ");
  Serial.print(macStr);
  Serial.print("  len=");
  Serial.println(len);

  // Validate packet size
  if (len != sizeof(sensor_packet_t)) {
    Serial.printf("Unexpected packet size: %d (expected %d)\n", len, (int)sizeof(sensor_packet_t));
    // Optionally print raw bytes for debugging:
    for (int i = 0; i < len; ++i) Serial.printf("%02X ", data[i]);
    Serial.println();
    return;
  }

  // Unpack into struct
  sensor_packet_t pkt;
  memcpy(&pkt, data, sizeof(sensor_packet_t));

  // Update globals (volatile because might be read elsewhere)
  lastCharging = (pkt.charging != 0);
  lastPercent  = pkt.percent;
  lastVoltage  = pkt.voltage;

  // Print values
  Serial.printf("charging=%s, percent=%u, voltage=%.2f\n",
                lastCharging ? "true" : "false",
                lastPercent,
                lastVoltage);

  // LED: on while charging, off otherwise
  digitalWrite(LED_PIN, lastCharging ? HIGH : LOW);
}
