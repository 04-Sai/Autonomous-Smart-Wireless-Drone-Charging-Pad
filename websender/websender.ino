/*
  SkyCharge_SimpleWebServer.ino
  - Serves an embedded web UI (HTML/CSS/JS) from PROGMEM
  - Uses WiFi.h + WebServer.h only (no Async libraries)
  - Provides /telemetry (GET JSON) polled by the page every 1s
  - Provides /book?pad=...&voltage=... to start a simulated charging session
  - Provides /toggle?val=1 or 0 to set terminal online/offline
  - Simulates charging progress on the device
*/

#include <WiFi.h>
#include <WebServer.h>

#define WIFI_SSID "POCO m4 Pro"
#define WIFI_PASS "rohan999"

WebServer server(80);

// --- Simulated backend state ---
struct Telemetry {
  int battery = 32;       // percent
  float voltage = 22.4;   // volts
  bool onPad = false;
  bool isCharging = false;
  bool systemOnline = true;
  unsigned long secsCharged = 0;
  String padId = "";
} telemetry;

unsigned long lastSimTick = 0;

// --- Embedded HTML (PROGMEM) ---
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>SkyCharge - Local (ESP32)</title>
<script src="https://cdn.tailwindcss.com"></script>
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css">
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600;700&family=JetBrains+Mono:wght@400;700&display=swap" rel="stylesheet">
<style>
  body { font-family: 'Inter', sans-serif; }
  .font-mono { font-family: 'JetBrains Mono', monospace; }
  #map { height: 100%; width: 100%; }
  ::-webkit-scrollbar { width: 6px; }
  ::-webkit-scrollbar-track { background: #f1f1f1; }
  ::-webkit-scrollbar-thumb { background: #cbd5e1; border-radius: 3px; }
  .login-bg { background-color: #0f172a; }
  .admin-table th { text-transform: uppercase; font-size: 0.75rem; color: #64748b; font-weight: 700; padding: 0.75rem 1rem; text-align: left; }
  .admin-table td { padding: 1rem; border-top: 1px solid #e2e8f0; font-size: 0.875rem; color: #1e293b; }
</style>
</head>
<body class="bg-gray-100 h-screen overflow-hidden">

<!-- LOGIN -->
<section id="login-view" class="h-full w-full flex items-center justify-center login-bg absolute inset-0 z-50">
  <div class="bg-white/10 p-8 rounded-2xl w-full max-w-md">
    <div class="text-center mb-6">
      <div class="bg-blue-600 w-12 h-12 rounded-lg flex items-center justify-center mx-auto mb-4 shadow-lg"><i class="fa-solid fa-bolt text-white"></i></div>
      <h1 class="text-2xl text-white font-bold">SkyCharge (Local)</h1>
      <p class="text-sm text-blue-200">Local server only</p>
    </div>
    <form onsubmit="handleLogin(event)" class="space-y-4">
      <div><label class="text-xs text-blue-200">Email</label><input id="email" class="w-full p-3 rounded bg-slate-800 text-white" value="user@gmail.com"></div>
      <div><label class="text-xs text-blue-200">Password</label><input id="password" type="password" class="w-full p-3 rounded bg-slate-800 text-white" value="user123"></div>
      <div id="login-error" class="hidden text-red-300 text-sm">Invalid credentials</div>
      <button type="submit" class="w-full bg-blue-600 text-white p-3 rounded">Initialize Session</button>
    </form>
    <p class="text-xs text-slate-300 mt-3">Try user@gmail.com / user123 or admin@gmail.com / admin123</p>
  </div>
</section>

<!-- USER VIEW -->
<section id="user-view" class="hidden h-full flex flex-col absolute inset-0 z-40 bg-gray-100">
  <nav class="bg-white shadow p-4 flex justify-between items-center">
    <div class="flex items-center gap-3"><div class="bg-blue-600 p-2 rounded text-white"><i class="fa-solid fa-bolt"></i></div><h1 class="text-xl font-bold">SkyCharge</h1></div>
    <div><span id="unique-code" class="font-mono text-sm text-blue-600">------</span> <button onclick="logout()" class="ml-4 text-sm text-red-500">Logout</button></div>
  </nav>
  <div class="flex flex-1 overflow-hidden">
    <div class="flex-1 bg-gray-200 p-4">
      <div id="map" class="h-full flex items-center justify-center text-slate-600">Map placeholder (local)</div>
    </div>

    <aside class="w-96 bg-white border-l p-4">
      <div class="flex justify-between items-center border-b pb-2 mb-3">
        <h2 class="font-semibold">Drone Telemetry</h2><span id="conn-badge" class="text-xs bg-green-100 px-2 py-1 rounded">Connected</span>
      </div>

      <div class="space-y-4">
        <div><p class="text-xs text-gray-500">Drone Name</p><p class="font-bold">Falcon-X1</p></div>
        <div class="bg-blue-50 p-3 rounded">
          <p class="text-xs text-blue-600">On Pad</p>
          <div class="flex items-center gap-2"><span id="on-pad-badge" class="text-lg font-bold">NO</span><span id="pad-indicator" class="h-3 w-3 rounded-full bg-gray-300"></span></div>
          <p class="mt-2 text-sm" id="charging-status-text">Searching for pad...</p>
        </div>

        <div>
          <div class="flex justify-between items-end"><p class="text-sm">Battery Level</p><p id="battery-percentage" class="text-2xl font-bold">32%</p></div>
          <div class="w-full bg-gray-200 rounded-full h-2.5 overflow-hidden"><div id="battery-bar" class="h-2.5 rounded-full" style="width:32%;background:#f59e0b"></div></div>
        </div>

        <div class="grid grid-cols-2 gap-3">
          <div class="bg-gray-50 p-3 rounded text-center"><p class="text-xs text-gray-500">Voltage</p><p id="voltage-val" class="font-mono font-bold">22.4 V</p></div>
          <div class="bg-gray-50 p-3 rounded text-center"><p class="text-xs text-gray-500">Duration</p><p id="charge-duration" class="font-mono font-bold">00:00</p></div>
        </div>

        <div id="action-area" class="hidden bg-green-50 p-3 rounded text-center"><p class="text-green-800 font-bold">Charge in progress...</p></div>

        <div><button onclick="bookPad('PAD-VTI-01','48')" class="w-full bg-blue-600 text-white p-2 rounded">Book Now</button></div>
      </div>

    </aside>
  </div>
</section>

<!-- ADMIN VIEW -->
<section id="admin-view" class="hidden h-full flex flex-col absolute inset-0 z-40 bg-slate-50">
  <nav class="bg-slate-900 text-white p-4 flex justify-between items-center"><div><h1 class="font-bold">SkyCharge Admin</h1></div><div><button onclick="logout()" class="px-2 py-1 bg-slate-700 rounded">LOGOUT</button></div></nav>
  <div class="p-6 overflow-auto">
    <div class="grid grid-cols-3 gap-4">
      <div class="bg-white p-4 rounded"> <h3 class="text-xs text-gray-400 uppercase">Active Fleet</h3><p class="font-bold">Falcon-X1</p><p id="admin-battery">32%</p></div>
      <div class="bg-white p-4 rounded">
        <h3 class="text-xs">Master Override</h3>
        <p id="terminal-status" class="text-xs text-red-400">TERMINAL LOCKED</p>
        <label class="mt-2 text-sm"> <input id="terminal-toggle" type="checkbox" onchange="toggleTerminal()" /> Toggle</label>
      </div>
      <div class="bg-white p-4 rounded"><h3 class="text-xs">Energy</h3><p class="text-2xl font-bold">4,281 kWh</p></div>
    </div>
    <div class="mt-6 bg-white p-4 rounded">
      <h3 class="font-bold">Charging History Log</h3>
      <table class="w-full admin-table mt-2"><thead><tr><th>Timestamp</th><th>Drone ID</th><th>Pad</th><th>Duration</th><th>Energy</th><th>Status</th></tr></thead><tbody id="history-table-body"></tbody></table>
    </div>
  </div>
</section>

<script>
// Simple client side logic
const usersDB = [
  { email: 'admin@gmail.com', password: 'admin123', role: 'admin' },
  { email: 'user@gmail.com', password: 'user123', role: 'user' }
];

function handleLogin(e){
  e.preventDefault();
  const email = document.getElementById('email').value;
  const pass = document.getElementById('password').value;
  const u = usersDB.find(x => x.email===email && x.password===pass);
  if(!u){ document.getElementById('login-error').classList.remove('hidden'); return; }
  document.getElementById('login-view').classList.add('hidden');
  if(u.role==='admin') { document.getElementById('admin-view').classList.remove('hidden'); } else { document.getElementById('user-view').classList.remove('hidden'); generateUniqueCode(); }
  // start polling telemetry
  startTelemetryPolling();
}

function logout(){
  location.reload();
}

function generateUniqueCode(){ document.getElementById('unique-code').innerText = Math.random().toString(36).substring(2,8).toUpperCase(); }

// Polling telemetry every 1s
let telemetryTimer = null;
function startTelemetryPolling(){
  if(telemetryTimer) clearInterval(telemetryTimer);
  fetchTelemetry(); // immediate
  telemetryTimer = setInterval(fetchTelemetry, 1000);
}

function fetchTelemetry(){
  fetch('/telemetry').then(r => r.json()).then(t => {
    // battery
    if(t.battery !== undefined){
      document.getElementById('battery-percentage').innerText = t.battery + '%';
      const bar = document.getElementById('battery-bar');
      bar.style.width = t.battery + '%';
      bar.style.background = (t.battery < 50) ? '#f59e0b' : '#10b981';
      if(document.getElementById('admin-battery')) document.getElementById('admin-battery').innerText = t.battery + '%';
    }
    if(t.voltage !== undefined) document.getElementById('voltage-val').innerText = t.voltage.toFixed(1) + ' V';
    if(t.onPad !== undefined){
      document.getElementById('on-pad-badge').innerText = t.onPad ? 'YES' : 'NO';
      const ind = document.getElementById('pad-indicator');
      if(t.onPad){ ind.classList.add('bg-green-500'); ind.style.background = '#10b981'; } else { ind.classList.remove('bg-green-500'); ind.style.background = '#cbd5e1'; }
    }
    if(t.isCharging){
      document.getElementById('charging-status-text').innerText = 'Locked: ' + (t.padId||'');
      document.getElementById('action-area').classList.remove('hidden');
      const mins = Math.floor(t.secsCharged/60).toString().padStart(2,'0');
      const secs = (t.secsCharged%60).toString().padStart(2,'0');
      document.getElementById('charge-duration').innerText = `${mins}:${secs}`;
    } else {
      document.getElementById('action-area').classList.add('hidden');
      document.getElementById('charging-status-text').innerText = (t.systemOnline ? 'Ready' : 'System Offline');
    }
    // terminal
    if(t.systemOnline !== undefined){
      const term = document.getElementById('terminal-status');
      if(t.systemOnline){ term.innerText='TERMINAL ACTIVE - ROOT ACCESS'; term.className='text-xs text-green-400'; document.getElementById('terminal-toggle').checked=true; }
      else { term.innerText='TERMINAL LOCKED'; term.className='text-xs text-red-400'; document.getElementById('terminal-toggle').checked=false; }
    }
    // history
    if(Array.isArray(t.history)){
      const tbody = document.getElementById('history-table-body'); tbody.innerHTML='';
      t.history.forEach(r => {
        const tr = document.createElement('tr');
        tr.innerHTML = `<td class="font-mono">${r.time}</td><td class="font-bold">${r.drone}</td><td>${r.pad}</td><td>${r.duration}</td><td>${r.energy}</td><td><span class="px-2 py-1 text-xs rounded ${r.status==='Completed'?'bg-green-100 text-green-700':'bg-red-100 text-red-700'}">${r.status}</span></td>`;
        tbody.appendChild(tr);
      });
    }
  }).catch(err => { console.warn('telemetry error', err); document.getElementById('conn-badge').innerText='Disconnected'; });
}

// Book pad -> call server endpoint
function bookPad(pad, voltage){
  if(!confirm('Book ' + pad + ' ?')) return;
  fetch(`/book?pad=${encodeURIComponent(pad)}&voltage=${encodeURIComponent(voltage)}`).then(()=>fetchTelemetry());
}

// Toggle terminal (admin)
function toggleTerminal(){
  const val = document.getElementById('terminal-toggle').checked ? 1 : 0;
  fetch(`/toggle?val=${val}`).then(()=>fetchTelemetry());
}
</script>
</body>
</html>
)rawliteral";

// --- Helper: small history sample ---
String sampleHistory() {
  String s = "[";
  s += "{\"time\":\"2023-10-24 09:15\",\"drone\":\"DRN-8829\",\"pad\":\"VTU Main Station\",\"duration\":\"45m\",\"energy\":\"120 Wh\",\"status\":\"Completed\"},";
  s += "{\"time\":\"2023-10-24 14:30\",\"drone\":\"DRN-8829\",\"pad\":\"VTU Main Station\",\"duration\":\"12m\",\"energy\":\"35 Wh\",\"status\":\"Aborted\"}";
  s += "]";
  return s;
}

// --- Web handlers ---
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleTelemetry() {
  String out = "{";
  out += "\"battery\":" + String(telemetry.battery) + ",";
  out += "\"voltage\":" + String(telemetry.voltage,1) + ",";
  out += "\"onPad\":" + String(telemetry.onPad ? "true":"false") + ",";
  out += "\"isCharging\":" + String(telemetry.isCharging ? "true":"false") + ",";
  out += "\"secsCharged\":" + String(telemetry.secsCharged) + ",";
  out += "\"systemOnline\":" + String(telemetry.systemOnline ? "true":"false") + ",";
  out += "\"padId\":\"" + telemetry.padId + "\",";
  out += "\"history\":" + sampleHistory();
  out += "}";
  server.send(200, "application/json", out);
}

void handleBook() {
  // GET /book?pad=...&voltage=...
  if(!server.hasArg("pad")) { server.send(400, "text/plain", "missing pad"); return; }
  String pad = server.arg("pad");
  // String voltage = server.arg("voltage");
  telemetry.onPad = true;
  telemetry.isCharging = true;
  telemetry.padId = pad;
  if (telemetry.battery < 5) telemetry.battery = 5; // ensure some starting point
  telemetry.secsCharged = 0;
  server.send(200, "text/plain", "ok");
}

void handleToggle() {
  // GET /toggle?val=0 or 1
  if(!server.hasArg("val")) { server.send(400, "text/plain", "missing val"); return; }
  String v = server.arg("val");
  telemetry.systemOnline = (v == "1");
  // if turning offline, stop charging
  if(!telemetry.systemOnline) { telemetry.isCharging = false; telemetry.onPad = false; }
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to WiFi '%s' ...\n", WIFI_SSID);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(250);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("Failed to connect as STA — starting AP mode 'SkyCharge-ESP32' (password 12345678).");
    WiFi.softAP("SkyCharge-ESP32", "12345678");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // Setup routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/telemetry", HTTP_GET, handleTelemetry);
  server.on("/book", HTTP_GET, handleBook);
  server.on("/toggle", HTTP_GET, handleToggle);

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();

  // Simulate telemetry changes
  unsigned long now = millis();
  if (now - lastSimTick >= 800) { // ~0.8s step like your original
    lastSimTick = now;
    // if charging, increment battery and seconds
    if (telemetry.isCharging) {
      telemetry.secsCharged++;
      if (telemetry.battery < 100) telemetry.battery++;
      else {
        telemetry.isCharging = false;
        telemetry.onPad = false;
        telemetry.padId = "";
      }
      // small jitter on voltage
      telemetry.voltage = 22.0 + (random(-20, 20) / 10.0);
    } else {
      // passive small drift
      if (random(0, 1000) < 6 && telemetry.battery > 0) telemetry.battery--;
      telemetry.voltage = 22.0 + (random(-20, 20) / 10.0);
    }
  }

  // avoid busy loop
  delay(1);
}
