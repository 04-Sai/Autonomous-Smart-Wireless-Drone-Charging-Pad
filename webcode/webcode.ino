/* SkyCharge.ino (updated)
   - Marker at given coords (15.822076881830819, 74.48906593739606)
   - Clicking marker shows pad name + "Available slots: Yes/No"
   - New state fields: padName (String), slotsAvailable (bool)
   - /update accepts padName and slots (yes/no or 1/0/true/false)
*/

#include <WiFi.h>
#include <WebServer.h>

// ---------------------- Config --------------------------------
const char* WIFI_SSID = "POCO M4 Pro";
const char* WIFI_PASSWORD = "rohan999";
String hostName = "skycharge-esp32";
// ----------------------------------------------------------------

WebServer server(80);

// Device state stored in-memory
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

// HTML page served (PROGMEM). Replace YOUR_GOOGLE_MAPS_API_KEY.
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>SkyCharge</title>
  <style>
    :root{--nav-h:56px}
    html,body{height:100%;margin:0;font-family:Inter,system-ui,Segoe UI,Roboto,'Helvetica Neue',Arial}
    .navbar{height:var(--nav-h);display:flex;align-items:center;justify-content:space-between;padding:0 16px;background:#0f172a;color:#fff;box-shadow:0 2px 6px rgba(0,0,0,0.15);}
    .brand{font-weight:700;font-size:18px}
    .mac{opacity:0.9;font-family:monospace}
    .container{height:calc(100% - var(--nav-h));display:flex;min-height:0}
    /* Map area */
    #map{flex:1;min-width:0;height:100%}
    /* Sidebar */
    .sidebar{width:320px;padding:16px;background:#f8fafc;border-left:1px solid #e6eef6;box-sizing:border-box;overflow:auto}
    .card{background:#fff;padding:12px;margin-bottom:12px;border-radius:10px;box-shadow:0 1px 2px rgba(16,24,40,0.04)}
    .label{font-size:12px;color:#6b7280;margin-bottom:6px}
    .value{font-size:20px;font-weight:600}
    .status-ok{color:green}
    .status-charging{color:orange}
    .status-error{color:red}

    /* Progress bar layout under percentage */
    .progress-wrap { margin-top:10px }
    .progress { height:14px; background:#e6eef6; border-radius:999px; overflow:hidden; }
    .progress > .bar { height:100%; width:0%; border-radius:999px; transition: width 400ms ease; background: linear-gradient(90deg,#22c1c3,#fdbb2d); }
    .progress-label { font-size:12px; color:#475569; margin-top:6px; text-align:right; }

    /* small responsive fix so map shows on narrow screens */
    @media (max-width:720px) {
      .container { flex-direction:column; }
      .sidebar { width:100%; height:320px; order:2; }
      #map { order:1; height:60vh; }
    }
  </style>
</head>
<body>
  <div class="navbar">
    <div class="brand">SkyCharge</div>
    <div class="mac" id="mac-display">MAC: --:--:--:--:--:--</div>
  </div>

  <div class="container">
    <div id="map">Loading map... (ensure Google Maps API key is set and device has internet)</div>

    <div class="sidebar">
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
        <div class="value" id="duration">--</div>
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
        <div class="value" id="lastupdate">--</div>
      </div>
    </div>
  </div>

  <!-- Replace YOUR_GOOGLE_MAPS_API_KEY with your real key -->
  <script async defer src="https://maps.googleapis.com/maps/api/js?key=YOUR_GOOGLE_MAPS_API_KEY&callback=initMap"></script>

  <script>
    let map, marker;
    const padCoords = { lat: 15.822076881830819, lng: 74.48906593739606 };

    function initMap() {
      // create map and marker
      map = new google.maps.Map(document.getElementById('map'), {
        center: padCoords,
        zoom: 16,
        gestureHandling: 'greedy'
      });
      marker = new google.maps.Marker({
        position: padCoords,
        map,
        title: 'Charging Pad'
      });

      // show info window on click
      marker.addListener('click', async () => {
        try {
          const res = await fetch('/state');
          const data = await res.json();
          const padName = data.padName || 'Charging Pad';
          const slots = (data.slotsAvailable === true || data.slotsAvailable === 'true') ? 'Yes' : 'No';
          const content = `<div style="font-family:Inter,Arial,sans-serif"><b>${padName}</b><br>Available slots: ${slots}</div>`;
          const infowindow = new google.maps.InfoWindow({ content });
          infowindow.open(map, marker);
        } catch (e) {
          new google.maps.InfoWindow({ content: '<div>Error fetching pad info</div>' }).open(map, marker);
        }
      });

      // initial UI refresh
      pollState();
      setInterval(pollState, 2000);
    }

    // Poll state and update UI (including progress bar)
    async function pollState() {
      try {
        const res = await fetch('/state');
        if (!res.ok) return;
        const data = await res.json();
        if (!data) return;

        if (data.mac) document.getElementById('mac-display').innerText = 'MAC: ' + data.mac;

        // Percentage & bar
        const pctEl = document.getElementById('percent');
        const barEl = document.getElementById('percent-bar');
        const progressLabel = document.getElementById('progress-label');

        let pct = (typeof data.percent !== 'undefined' && data.percent !== null) ? Number(data.percent) : NaN;
        if (!isNaN(pct)) {
          if (pct < 0) pct = 0;
          if (pct > 100) pct = 100;
          pctEl.innerText = pct + '%';
          barEl.style.width = pct + '%';
          // update ARIA value for accessibility
          const progress = barEl.parentElement;
          if (progress) progress.setAttribute('aria-valuenow', String(pct));
          progressLabel.innerText = pct + '%';
        } else {
          pctEl.innerText = '--%';
          barEl.style.width = '0%';
          progressLabel.innerText = '--%';
        }

        if (typeof data.voltage !== 'undefined' && data.voltage !== null) document.getElementById('voltage').innerText = data.voltage + ' V';
        if (data.status) document.getElementById('status').innerText = data.status;
        if (data.duration) document.getElementById('duration').innerText = data.duration;
        if (data.padName) document.getElementById('padname').innerText = data.padName;

        let slotsText = '--';
        if (typeof data.slotsAvailable !== 'undefined' && data.slotsAvailable !== null) {
          slotsText = (data.slotsAvailable === true || data.slotsAvailable === 'true' || data.slotsAvailable === '1') ? 'Yes' : 'No';
        }
        document.getElementById('slots').innerText = slotsText;

        document.getElementById('lastupdate').innerText = new Date((data.timestamp || Date.now())).toLocaleString();

        // update marker position if device sends coordinates
        if (typeof data.lat === 'number' && typeof data.lng === 'number') {
          const pos = { lat: data.lat, lng: data.lng };
          marker.setPosition(pos);
          map.panTo(pos);
        }
      } catch (err) {
        // silently ignore; fallback remains visible text in map div
        console.error('pollState error', err);
      }
    }

    // fallback if Maps never initialized
    window.initMap || (window.initMap = function(){ document.getElementById('map').innerText = 'Map failed to load — check API key & internet.'; });
  </script>
</body>
</html>
)rawliteral";


// ---------------------- Server helpers ---------------------------
void sendIndex() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send_P(200, "text/html", index_html);
}

void handleState() {
  // Build JSON string including padName and slotsAvailable
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

// ---------------------- Setup & loop ------------------------------
void setup() {
  Serial.begin(115200);
  delay(100);

  // Set hostname (requires const char*)
  WiFi.setHostname(hostName.c_str());

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempt++;
    if (attempt > 60) {
      Serial.println("\nFailed to connect to WiFi - restarting...");
      ESP.restart();
    }
  }
  Serial.println();
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());

  // fill initial MAC and timestamp and new fields
  state.mac = WiFi.macAddress();
  state.percent = -1;
  state.voltage = 0.0;
  state.status = "Unknown";
  state.duration = "--:--:--";
  // set initial marker coordinates to the provided pad coords
  state.lat = 15.822076881830819;
  state.lng = 74.48906593739606;
  state.timestamp = millis();
  state.padName = "Charging Pad A";
  state.slotsAvailable = true;

  // HTTP routes
  server.on("/", HTTP_GET, sendIndex);
  server.on("/state", HTTP_GET, handleState);
  server.on("/update", HTTP_GET, handleUpdate);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();

  // If you want to update state from sensors on this ESP, update `state.*` here and set state.timestamp = millis();
}
