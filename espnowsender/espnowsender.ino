#include <WiFi.h>
#include <esp_now.h>

// ===== Configuration =====
const int TRIG_PIN = 8;      // HC-SR04 TRIG
const int ECHO_PIN = 7;      // HC-SR04 ECHO
const int ADC_PIN = A0;      // ADC1 channel (battery sense)
const int RELAY_PIN = 5;     // Relay pin (safe GPIO)
const int LED_PIN = 42;      // optional status LED

const unsigned long WINDOW_MS = 5000UL;        // 5 second window
const unsigned long SEND_INTERVAL_MS = 1000UL; // interval for ESP-NOW
const float DETECT_THRESHOLD_CM = 5.0f;

const float BATTERY_MIN_VOLTAGE = 0.0;
const float BATTERY_MAX_VOLTAGE = 4.0;
float calibration = 0.0; // adjust as needed

uint8_t destMac[] = {0x48, 0x31, 0xB7, 0xD0, 0x9C, 0x86};

// ===== Packet (packed) =====
typedef struct __attribute__((packed)) {
    uint8_t charging;
    uint8_t percent;
    float voltage;
} sensor_packet_t;

// ===== Globals =====
unsigned long windowStartMs = 0;
bool relayStateHigh = false;
unsigned long lastSendMs = 0;
sensor_packet_t lastPacket;

// ===== Helpers =====
float readUltrasonicCm() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    unsigned long dur = pulseIn(ECHO_PIN, HIGH, 38000UL);
    if (dur == 0) return -1.0f;

    float dist = (dur * 0.0343f) / 2.0f;
    return dist;
}

float readBatteryVoltage() {
    int sensorValue = analogRead(ADC_PIN); // 0..4095
    float battvoltage = ((float)sensorValue / 4095.0f) * BATTERY_MAX_VOLTAGE + calibration;
    return battvoltage;
}

int voltageToPercent(float v) {
    float pct = (v - BATTERY_MIN_VOLTAGE) * 100.0f / (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE);
    int ipct = (int)round(pct);
    if (ipct < 15) ipct = 0;
    if (ipct > 100) ipct = 100;
    // Apply rounding bands
    if (ipct > 0 && ipct <= 25) ipct = 25;
    else if (ipct > 25 && ipct <= 50) ipct = 50;
    else if (ipct > 50 && ipct <= 75) ipct = 75;
    else if (ipct > 75 && ipct < 100) ipct = 90;
    return ipct;
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    Serial.print("Send status: ");
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAILED");
}

// ===== Setup & Loop =====
void setup() {
    Serial.begin(115200);
    delay(200);

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(RELAY_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(TRIG_PIN, LOW);
    digitalWrite(RELAY_PIN, LOW);
    digitalWrite(LED_PIN, LOW);

    analogReadResolution(12);

    // Initial battery read
    float initBatt = readBatteryVoltage();
    int initPct = voltageToPercent(initBatt);
    lastPacket.charging = 0;
    lastPacket.percent = (uint8_t)initPct;
    lastPacket.voltage = initBatt;

    // WiFi + ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        while (true) delay(1000);
    }

    // esp_now_register_send_cb(OnDataSent);

    // Add peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, destMac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add peer");
    } else {
        Serial.println("Peer added.");
    }

    windowStartMs = millis();
    relayStateHigh = false;
    digitalWrite(RELAY_PIN, relayStateHigh ? HIGH : LOW);

    lastSendMs = millis() - SEND_INTERVAL_MS;
    Serial.println("Sender ready");
}

void loop() {
    unsigned long now = millis();

    if (now - windowStartMs >= WINDOW_MS) {
        windowStartMs += WINDOW_MS;
        relayStateHigh = !relayStateHigh;
        digitalWrite(RELAY_PIN, relayStateHigh ? HIGH : LOW);
        Serial.print("Window toggled. Relay is now ");
        Serial.println(relayStateHigh ? "HIGH (monitoring)" : "LOW (charging)");
    }

    if (now - lastSendMs >= SEND_INTERVAL_MS) {
        lastSendMs = now;

        sensor_packet_t pktToSend;

        if (relayStateHigh) {
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
Serial.print("ADC value:");
Serial.print(analogRead(ADC_PIN));

            Serial.print("Measured battery: ");
            Serial.print(batteryV, 3);
            Serial.print(" V, ");
            Serial.print(batteryPct);
            Serial.println(" %");

            pktToSend.charging = deviceDetected ? 1 : 0;
            pktToSend.percent = (uint8_t)batteryPct;
            pktToSend.voltage = batteryV;
            lastPacket = pktToSend;
        } else {
            pktToSend = lastPacket;
            Serial.println("Charging window: resending last measured packet (no new measurements)");
        }

        esp_err_t res = esp_now_send(destMac, (uint8_t *)&pktToSend, sizeof(pktToSend));
        if (res == ESP_OK) {
            Serial.println("esp_now_send queued");
        } else {
            Serial.print("esp_now_send failed: ");
            Serial.println(res);
        }

        digitalWrite(LED_PIN, pktToSend.charging ? HIGH : LOW);

        Serial.printf("Sent -> charging=%u percent=%u voltage=%.3f\n",
                      pktToSend.charging, pktToSend.percent, pktToSend.voltage);
    }

    delay(10);
}