/* SkyChargePad.ino
   ESP32 charging pad: Web server + ESP-NOW receiver
   - Serves the SkyCharge web UI at '/'
   - Receives ESP-NOW packets from drone: { uint8_t charging; uint8_t percent; float voltage; }
   - Updates `state` used by /state (so UI reflects latest received values)
   - Controls relay (RELAY_PIN) HIGH when charging==1, LOW otherwise
   - Mirrors LED (LED_PIN) while charging
*/

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>

// ---------------------- Config --------------------------------
const char* WIFI_SSID = "POCO M4 Pro";
const char* WIFI_PASSWORD = "rohan999";
String hostName = "skycharge-esp32";
// Hardware pins
const int RELAY_PIN = 5;   // relay to enable charging (HIGH => charging)
const int LED_PIN   = 42;  // indicator LED mirror
// ----------------------------------------------------------------

WebServer server(80);

// Device state stored in-memory (used by web UI)
struct DeviceState {
  String mac;
  int percent;
  float voltage;
  String status;
  String duration;
  double lat;
  double lng;
  unsigned long timestamp;
  String padName;
  bool slotsAvailable;
} state;

// Packet layout - MUST match sender's struct exactly
typedef struct __attribute__((packed)) {
  uint8_t charging;   // 0 = false, 1 = true
  uint8_t percent;    // 0..100
  float voltage;      // 4-byte float
} sensor_packet_t;

// HTML served (keeps your existing UI; API key already inside)
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>SkyCharge</title>
  <style>
    :root{--nav-h:56px}
    html,body{height:100%;margin:0;font-family:Inter,system-ui,Segoe UI,Roboto,'Helvetica Neue',Arial;background:#ffffff}
    .navbar{height:var(--nav-h);display:flex;align-items:center;justify-content:space-between;padding:0 16px;background:#0f172a;color:#fff;box-shadow:0 2px 6px rgba(0,0,0,0.12);}
    .brand{font-weight:700;font-size:18px}
    .mac{opacity:0.95;font-family:monospace}
    .container{height:calc(100% - var(--nav-h));display:flex;min-height:0}
    /* Map area */
    #map{flex:1;min-width:0;height:100%;background:#e9eef6;display:flex;align-items:center;justify-content:center;color:#475569}
    /* Sidebar */
    .sidebar{width:340px;padding:18px;background:#f8fafc;border-left:1px solid #e6eef6;box-sizing:border-box;overflow:auto}
    .card{background:#fff;padding:12px;margin-bottom:12px;border-radius:10px;box-shadow:0 1px 2px rgba(16,24,40,0.04)}
    .label{font-size:12px;color:#6b7280;margin-bottom:6px}
    .value{font-size:20px;font-weight:700;color:#0f172a}
    .muted{font-size:13px;color:#475569}
    .status-ok{color:#059669}
    .status-charging{color:#f59e0b}
    .status-error{color:#ef4444}

    /* Progress bar */
    .progress-wrap { margin-top:10px }
    .progress { height:14px; background:#e6eef6; border-radius:999px; overflow:hidden; }
    .progress > .bar { height:100%; width:0%; border-radius:999px; transition: width 400ms ease; background: linear-gradient(90deg,#22c1c3,#fdbb2d); }
    .progress-label { font-size:12px; color:#475569; margin-top:6px; text-align:right; }

    /* small responsive fix */
    @media (max-width:720px) {
      .container { flex-direction:column; }
      .sidebar { width:100%; height:320px; order:2; }
      #map { order:1; height:60vh; }
    }

    /* map-error overlay */
    .map-error {
      position:absolute; left:16px; top:80px; z-index:1000;
      background:rgba(255,255,255,0.95); border-radius:8px; padding:10px 12px;
      box-shadow:0 6px 20px rgba(12,20,30,0.12); color:#b91c1c; font-weight:600;
    }
  </style>
</head>
<body>
  <div class="navbar">
    <div class="brand">SkyCharge</div>
    <div class="mac" id="mac-display">MAC: --:--:--:--:--:--</div>
  </div>

  <div class="container">
    <div id="map">Loading map... (ensure your browser/device has internet)</div>

    <div class="sidebar" id="sidebar">
      <div class="card">
        <div class="label">Charging Percentage</div>
        <div class="value" id="percent">--%</div>

        <!-- Progress bar -->
        <div class="progress-wrap" aria-hidden="false">
          <div class="progress" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0">
            <div class="bar" id="percent-bar" style="width:0%"></div>
          </div>
          <div class="progress-label" id="progress-label">--%</div>
        </div>
      </div>

      <div class="card">
        <div class="label">Voltage</div>
        <div class="value" id="voltage">-- V</div>
      </div>

      <div class="card">
        <div class="label">Charging Status</div>
        <div class="value" id="status">--</div>
      </div>

      <div class="card">
        <div class="label">Duration</div>
        <div class="value muted" id="duration">--</div>
      </div>

      <div class="card">
        <div class="label">Pad Name</div>
        <div class="value" id="padname">--</div>
      </div>

      <div class="card">
        <div class="label">Available Slots</div>
        <div class="value" id="slots">--</div>
      </div>

      <div class="card">
        <div class="label">Last update</div>
        <div class="value muted" id="lastupdate">--</div>
      </div>
    </div>
  </div>

  <!-- Diagnostic message area (hidden unless error) -->
  <div id="map-error" class="map-error" style="display:none;"></div>

  <script>
    let map, marker;
    const padCoords = { lat: 15.822076881830819, lng: 74.48906593739606 };

    function showMapError(msg) {
      const el = document.getElementById('map-error');
      el.style.display = 'block';
      el.innerText = msg;
      console.error('Map error:', msg);
    }

    function gm_authFailure() {
      showMapError('Google Maps authorization failed: Invalid API key or restrictions. Check your API key & quotas.');
    }

    window.initMap = function initMap() {
      try {
        map = new google.maps.Map(document.getElementById('map'), {
          center: padCoords,
          zoom: 16,
          gestureHandling: 'greedy',
        });

        marker = new google.maps.Marker({
          position: padCoords,
          map,
          title: 'Charging Pad'
        });

        marker.addListener('click', async () => {
          try {
            const res = await fetch('/state');
            if (!res.ok) {
              new google.maps.InfoWindow({ content: '<div>Unable to fetch pad info</div>' }).open(map, marker);
              return;
            }
            const data = await res.json();
            const padName = data.padName || 'Charging Pad';
            const slots = (data.slotsAvailable === true || data.slotsAvailable === 'true') ? 'Yes' : 'No';
            const content = `<div style="font-family:Inter,Arial,sans-serif"><b>${padName}</b><br>Available slots: ${slots}</div>`;
            new google.maps.InfoWindow({ content }).open(map, marker);
          } catch (err) {
            new google.maps.InfoWindow({ content: '<div>Error fetching pad info</div>' }).open(map, marker);
          }
        });

        pollState();
        setInterval(pollState, 2000);
      } catch (err) {
        showMapError('Failed to initialize map. ' + String(err));
      }
    };

    async function pollState() {
      try {
        const res = await fetch('/state', { cache: 'no-store' });
        if (!res.ok) {
          console.warn('/state fetch failed:', res.status);
          return;
        }
        const data = await res.json();
        if (!data) return;

        if (data.mac) document.getElementById('mac-display').innerText = 'MAC: ' + data.mac;

        const pctEl = document.getElementById('percent');
        const barEl = document.getElementById('percent-bar');
        const progressLabel = document.getElementById('progress-label');

        let pct = (typeof data.percent !== 'undefined' && data.percent !== null) ? Number(data.percent) : NaN;
        if (!isNaN(pct)) {
          pct = Math.min(100, Math.max(0, Math.round(pct)));
          pctEl.innerText = pct + '%';
          barEl.style.width = pct + '%';
          const progressParent = barEl.parentElement;
          if (progressParent) progressParent.setAttribute('aria-valuenow', String(pct));
          progressLabel.innerText = pct + '%';
        } else {
          pctEl.innerText = '--%';
          barEl.style.width = '0%';
          progressLabel.innerText = '--%';
        }

        if (typeof data.voltage !== 'undefined' && data.voltage !== null) document.getElementById('voltage').innerText = Number(data.voltage).toFixed(2) + ' V';
        if (data.status) {
          const statusEl = document.getElementById('status');
          statusEl.innerText = data.status;
          statusEl.classList.remove('status-ok','status-charging','status-error');
          const s = data.status.toLowerCase();
          if (s.includes('ok') || s.includes('idle') || s.includes('ready')) statusEl.classList.add('status-ok');
          else if (s.includes('charge')) statusEl.classList.add('status-charging');
          else statusEl.classList.add('status-error');
        }
        if (data.duration) document.getElementById('duration').innerText = data.duration;
        if (data.padName) document.getElementById('padname').innerText = data.padName;
        let slotsText = '--';
        if (typeof data.slotsAvailable !== 'undefined' && data.slotsAvailable !== null) {
          slotsText = (data.slotsAvailable === true || data.slotsAvailable === 'true' || data.slotsAvailable === '1') ? 'Yes' : 'No';
        }
        document.getElementById('slots').innerText = slotsText;

        document.getElementById('lastupdate').innerText = new Date((data.timestamp || Date.now())).toLocaleString();

        if (typeof data.lat === 'number' && typeof data.lng === 'number' && marker && map) {
          const pos = { lat: data.lat, lng: data.lng };
          marker.setPosition(pos);
          const center = map.getCenter();
          if (!center || center.toString() !== (new google.maps.LatLng(pos)).toString()) {
            map.panTo(pos);
          }
        }
      } catch (err) {
        console.error('pollState error', err);
      }
    }

    setTimeout(() => {
      if (!window.google || !window.google.maps) {
        showMapError('Google Maps script did not load. Check internet access and API key settings.');
      }
    }, 6000);
  </script>

  <!-- Load the Maps JS API asynchronously. Callback name must match window.initMap -->
  <script async defer src="https://maps.googleapis.com/maps/api/js?key=AIzaSyAas2qXL1S-vIe8sk6Eqnd1BwMDdn7ApwA&callback=initMap"></script>
</body>
</html>
)rawliteral";

// ---------------------- Server helpers ---------------------------
void sendIndex() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send_P(200, "text/html", index_html);
}

void handleState() {
  // Build JSON string including padName and slotsAvailable (state updated by ESP-NOW callback)
  String resp = "{";
  resp += "\"mac\":\"" + state.mac + "\"";
  resp += ",\"percent\":" + String(state.percent);
  resp += ",\"voltage\":" + String(state.voltage, 2);
  resp += ",\"status\":\"" + state.status + "\"";
  resp += ",\"duration\":\"" + state.duration + "\"";
  resp += ",\"lat\":" + String(state.lat, 6);
  resp += ",\"lng\":" + String(state.lng, 6);
  resp += ",\"timestamp\":" + String(state.timestamp);
  resp += ",\"padName\":\"" + state.padName + "\"";
  resp += ",\"slotsAvailable\":" + String(state.slotsAvailable ? "true" : "false");
  resp += "}";
  server.send(200, "application/json", resp);
}

void handleUpdate() {
  bool changed = false;

  if (server.hasArg("percent")) {
    int p = server.arg("percent").toInt();
    state.percent = p;
    changed = true;
  }
  if (server.hasArg("voltage")) {
    state.voltage = server.arg("voltage").toFloat();
    changed = true;
  }
  if (server.hasArg("status")) {
    state.status = server.arg("status");
    changed = true;
  }
  if (server.hasArg("duration")) {
    state.duration = server.arg("duration");
    changed = true;
  }
  if (server.hasArg("lat")) {
    state.lat = server.arg("lat").toDouble();
    changed = true;
  }
  if (server.hasArg("lng")) {
    state.lng = server.arg("lng").toDouble();
    changed = true;
  }
  if (server.hasArg("padName")) {
    state.padName = server.arg("padName");
    changed = true;
  }
  if (server.hasArg("slots")) {
    String s = server.arg("slots");
    s.toLowerCase();
    if (s == "yes" || s == "true" || s == "1") state.slotsAvailable = true;
    else state.slotsAvailable = false;
    changed = true;
  }

  if (changed) state.timestamp = millis();

  handleState();
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ---------------------- ESP-NOW receive callback -----------------
void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  // Validate size
  if (len != sizeof(sensor_packet_t)) {
    Serial.printf("espnow: unexpected len %d (expected %d)\n", len, (int)sizeof(sensor_packet_t));
    return;
  }

  sensor_packet_t pkt;
  memcpy(&pkt, data, sizeof(sensor_packet_t));

  // Optional: print sender MAC
  char macStr[18] = {0};
  if (recv_info && recv_info->src_addr) {
    const uint8_t *mac = recv_info->src_addr;
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.print("Received from: ");
    Serial.println(macStr);
  } else {
    Serial.println("Received from: unknown");
  }

  Serial.printf("Payload -> charging=%u percent=%u voltage=%.3f\n", pkt.charging, pkt.percent, pkt.voltage);

  // Update shared state (disable interrupts briefly to avoid race with server)
  noInterrupts();
  state.percent = pkt.percent;
  state.voltage = pkt.voltage;
  state.status = pkt.charging ? String("Charging") : String("Idle");
  state.timestamp = millis();
  interrupts();

  // Control relay and LED
  digitalWrite(RELAY_PIN, pkt.charging ? HIGH : LOW);
  digitalWrite(LED_PIN, pkt.charging ? HIGH : LOW);
}

// ---------------------- Setup & loop ------------------------------
void setup() {
  Serial.begin(115200);
  delay(100);

  // Setup pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  // Set hostname
  WiFi.setHostname(hostName.c_str());

  // Start WiFi in STA mode (so web UI can be accessed)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempt++;
    if (attempt > 60) {
      Serial.println("\nFailed to connect to WiFi - continuing without AP (ESP-NOW only)");
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
  }

  // initialize state defaults
  state.mac = WiFi.macAddress();
  state.percent = -1;
  state.voltage = 0.0;
  state.status = "Idle";
  state.duration = "--:--:--";
  state.lat = 15.822076881830819;
  state.lng = 74.48906593739606;
  state.timestamp = millis();
  state.padName = "Charging Pad A";
  state.slotsAvailable = true;

  // Init ESP-NOW receiver
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed on pad - continuing without ESP-NOW");
  } else {
    esp_now_register_recv_cb(onDataRecv);
    Serial.println("ESP-NOW receiver ready on pad");
  }

  // Start HTTP server routes
  server.on("/", HTTP_GET, sendIndex);
  server.on("/state", HTTP_GET, handleState);
  server.on("/update", HTTP_GET, handleUpdate);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  // optional: add safety auto-off if no packet for X seconds
  delay(10);
}
