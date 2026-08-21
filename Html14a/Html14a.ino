#include <WiFi.h>
#include <WebServer.h>

const char* ssid = "POCO M4 Pro";
const char* password = "rohan999";

WebServer server(80);

// Dummy variables (replace with real sensor data)
// int batteryPercent ;    // Example: current battery %
// bool chargingStatus ; // true = charging ON, false = charging OFF

int analogInPin  = A0;  
int sensorValue; 
float voltage;
float bat_percentage;

// HTML Page
String webpage() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Drone Charging Monitor</title>";
  html += "<style>";
  html += "body{font-family:Arial;text-align:center;background:#f4f4f4;}";
  html += ".container{margin-top:50px;}";
  html += ".battery{font-size:50px;margin:20px;color:#333;}";
  html += ".status{font-size:30px;padding:10px 20px;border-radius:10px;display:inline-block;}";
  html += ".on{background-color:#4CAF50;color:white;}";
  html += ".off{background-color:#f44336;color:white;}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h1>Drone Wireless Charging Station</h1>";
  html += "<div class='battery'>🔋 Battery: " + String(bat_percentage) + "%</div>";
  html += "<div class='status " + String(chargingStatus ? "on" : "off") + "'>";
  html += chargingStatus ? "Charging: ON" : "Charging: OFF";
  html += "</div>";
  html += "<script>";
  html += "setInterval(()=>{fetch('/data').then(r=>r.json()).then(d=>{";
  html += "document.querySelector('.battery').innerHTML='🔋 Battery: '+d.battery+'%';";
  html += "let st=document.querySelector('.status');";
  html += "if(d.charging){st.innerHTML='Charging: ON';st.className='status on';}";
  html += "else{st.innerHTML='Charging: OFF';st.className='status off';}";
  html += "});},3000);";  // refresh every 3s
  html += "</script></div></body></html>";
  return html;
}

// JSON data route
void handleData() {
  String json = "{\"battery\":" + String(bat_percentage) + ",\"charging\":" + String(chargingStatus ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  server.send(200, "text/html", webpage());
}

void setup() {
  Serial.begin(9600);
  delay(1500); 
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.println("IP Address: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("Server started");
}

void loop() {
  // Simulate changes (for testing)
  static unsigned long lastUpdate = 0;
  // if (millis() - lastUpdate > 5000) {
  //   batteryPercent = (batteryPercent + 1) % 101;  // increment % 0–100
  //   chargingStatus = batteryPercent < 100;        // ON until full
  //   lastUpdate = millis();
  // }

  sensorValue = analogRead(analogInPin);
  voltage = (((sensorValue * 3.3) / 1024) * 2 ); 
  bat_percentage = mapfloat(voltage, 2.8, 4.2, 0, 100); 
 
  if (bat_percentage >= 100)
  {
    bat_percentage = 100;
  }
  if (bat_percentage <= 0)
  {
    bat_percentage = 1;
  }
 
  Serial.print("Analog Value = ");
  Serial.print(sensorValue);
  Serial.print("\t Output Voltage = ");
  Serial.print(voltage);
  Serial.print("\t Battery Percentage = ");
  Serial.println(bat_percentage);
  delay(1000);
  server.handleClient();
}
float mapfloat(float x, float in_min, float in_max, float out_min, float out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}