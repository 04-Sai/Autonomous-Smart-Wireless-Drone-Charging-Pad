/*
  Sender: Ultrasonic + Battery monitor + Relay toggling every 5s + ESP-NOW sending

  Behavior:
   - Relay pin toggles LOW/HIGH every 5 seconds.
   - When relay is HIGH -> measure sensors (ultrasonic + battery), form packet, send and store as lastPacket.
   - When relay is LOW  -> do NOT measure; resend lastPacket repeatedly at SEND_INTERVAL_MS.
   - Packet sent via ESP-NOW to a specific receiver MAC: 48:31:B7:D0:9C:86

  WARNING: Using GPIO9 can cause boot/flash issues on some ESP32 modules. If you encounter
  problems, move RELAY_PIN to a safe GPIO (e.g., 12,13,14,25,26,27).
*/

#include <WiFi.h>
#include <esp_now.h>

// ===== Configuration =====
const int TRIG_PIN = 7;      // HC-SR04 TRIG
const int ECHO_PIN = 8;     // HC-SR04 ECHO
const int ADC_PIN = A0;      // ADC1 channel (battery sense)
const int RELAY_PIN = 9;     // Relay pin as requested (caution: may be strapping pin on some modules)
const int LED_PIN = 42;       // optional status LED

const unsigned long WINDOW_MS = 5000UL;      // 5 second windows for relay toggle
const unsigned long SEND_INTERVAL_MS = 1000UL; // send interval in ms while operating
const float DETECT_THRESHOLD_CM = 5.0f;      // distance <= this considered "device detected"

// ADC / voltage divider settings (update R1,R2 to match your voltage divider)
const float ADC_REF_VOLTAGE = 3.3f;
const int ADC_MAX = 4095;   // 12-bit ADC
const float R1 = 100000.0f; // top resistor (ohms)
const float R2 = 100000.0f; // bottom resistor (ohms)
float calibrationOffset = 0.0f; // adjust if readings low/high

// Battery % mapping
const float BATTERY_MIN_VOLTAGE = 3.0f;
const float BATTERY_MAX_VOLTAGE = 4.2f;

// Destination receiver MAC (pad) - 48:31:B7:D0:9C:86
uint8_t destMac[] = {0x48, 0x31, 0xB7, 0xD0, 0x9C, 0x86};

// ===== Packet (packed) =====
typedef struct __attribute__((packed)) {
  uint8_t charging;  // 0 or 1
  uint8_t percent;   // 0..100
  float voltage;     // e.g., 12.34
} sensor_packet_t;

// ===== Globals =====
unsigned long windowStartMs = 0;
bool relayStateHigh = false; // current relay state: true => HIGH (monitoring), false => LOW (charging)
unsigned long lastSendMs = 0;
sensor_packet_t lastPacket;  // stores last-measured packet for resending during LOW windows

// ===== Helpers =====
float readUltrasonicCm() {
  // Trigger HC-SR04
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long dur = pulseIn(ECHO_PIN, HIGH, 38000UL); // timeout ~ 38ms
  if (dur == 0) return -1.0f; // out of range or no echo

  float dist = (dur * 0.0343f) / 2.0f;
  return dist;
}

float readBatteryVoltage() {
  int raw = analogRead(ADC_PIN); // 0..4095
  float v_adc = ((float)raw / (float)ADC_MAX) * ADC_REF_VOLTAGE;
  float scale = (R1 + R2) / R2;
  float battV = v_adc * scale + calibrationOffset;
  return battV;
}

int voltageToPercent(float v) {
  float pct = (v - BATTERY_MIN_VOLTAGE) * 100.0f / (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE);
  int ipct = (int)round(pct);
  if (ipct < 0) ipct = 0;
  if (ipct > 100) ipct = 100;
  return ipct;
}

// Modern send callback (safe): do not dereference info->mac (not portable)
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  (void)info;
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAILED");
}

// ===== Setup & Loop =====
void setup() {
  Serial.begin(115200);
  delay(200);

  // Pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(RELAY_PIN, LOW); // start with LOW (charging) as per your description
  digitalWrite(LED_PIN, LOW);

  // ADC
  analogReadResolution(12);

  // Initialize lastPacket with reasonable default by taking an initial reading
  {
    float initBatt = readBatteryVoltage();
    int initPct = voltageToPercent(initBatt);
    lastPacket.charging = 0;
    lastPacket.percent = (uint8_t)initPct;
    lastPacket.voltage = initBatt;
  }

  // WiFi + ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }

  esp_err_t rc = esp_now_register_send_cb(OnDataSent);
  if (rc != ESP_OK) {
    Serial.print("esp_now_register_send_cb failed: ");
    Serial.println(rc);
  }

  // Add peer (specific receiver MAC)
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, destMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  rc = esp_now_add_peer(&peerInfo);
  if (rc != ESP_OK) {
    Serial.print("esp_now_add_peer returned: ");
    Serial.println(rc);
  } else {
    Serial.println("Peer added.");
  }

  // Initialize window timer
  windowStartMs = millis();
  relayStateHigh = false; // start LOW
  digitalWrite(RELAY_PIN, relayStateHigh ? HIGH : LOW);

  lastSendMs = millis() - SEND_INTERVAL_MS; // so we send immediately
  Serial.println("Sender ready");
}

void loop() {
  unsigned long now = millis();

  // handle 5-second window toggling
  if (now - windowStartMs >= WINDOW_MS) {
    windowStartMs += WINDOW_MS;
    // toggle relay state
    relayStateHigh = !relayStateHigh;
    digitalWrite(RELAY_PIN, relayStateHigh ? HIGH : LOW);
    Serial.print("Window toggled. Relay is now ");
    Serial.println(relayStateHigh ? "HIGH (monitoring)" : "LOW (charging)");
  }

  // within send interval, either measure+send (if relay HIGH) or resend lastPacket (if relay LOW)
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;

    sensor_packet_t pktToSend;

    if (relayStateHigh) {
      // MEASURE sensors and send fresh values
      float dist = readUltrasonicCm();
      bool deviceDetected = false;
      if (dist >= 0.0f) {
        Serial.print("Distance: ");
        Serial.print(dist, 2);
        Serial.println(" cm");
        if (dist <= DETECT_THRESHOLD_CM) deviceDetected = true;
      } else {
        Serial.println("Distance: out of range");
      }

      float batteryV = readBatteryVoltage();
      int batteryPct = voltageToPercent(batteryV);

      Serial.print("Measured battery: ");
      Serial.print(batteryV, 3);
      Serial.print(" V, ");
      Serial.print(batteryPct);
      Serial.println(" %");

      // build packet from fresh measurements
      pktToSend.charging = deviceDetected ? 1 : 0;
      pktToSend.percent = (uint8_t)batteryPct;
      pktToSend.voltage = batteryV;

      // store as lastPacket for the subsequent LOW window
      lastPacket = pktToSend;
    } else {
      // CHARGING window: do not measure, resend previous packet
      pktToSend = lastPacket;
      Serial.println("Charging window: resending last measured packet (no new measurements)");
    }

    // send the packet via ESP-NOW to the specific receiver
    esp_err_t res = esp_now_send(destMac, (uint8_t *)&pktToSend, sizeof(pktToSend));
    if (res == ESP_OK) {
      Serial.println("esp_now_send queued");
    } else {
      Serial.print("esp_now_send failed: ");
      Serial.println(res);
    }

    // LED indicates device detection state (based on lastPacket.charging)
    digitalWrite(LED_PIN, pktToSend.charging ? HIGH : LOW);

    // debug print of sent values
    Serial.printf("Sent -> charging=%u percent=%u voltage=%.3f\n",
                  pktToSend.charging, pktToSend.percent, pktToSend.voltage);
  }

  delay(10); // small delay to keep loop responsive
}
