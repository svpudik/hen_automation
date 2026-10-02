#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <time.h>

// Relay control pins
#define CH_PD 4   // GPIO4 (D2)
#define RST 5     // GPIO5 (D1)
#define GPIO0 0   // GPIO0 (D3)

// Relay command bytes (for serial communication with relay module)
uint8_t R1On[] = {0xA0, 0x03, 0x01, 0xA4};
uint8_t R1Off[] = {0xA0, 0x03, 0x00, 0xA3};
uint8_t R2On[] = {0xA0, 0x04, 0x01, 0xA5};
uint8_t R2Off[] = {0xA0, 0x04, 0x00, 0xA4};
uint8_t R3On[] = {0xA0, 0x01, 0x01, 0xA2};
uint8_t R3Off[] = {0xA0, 0x01, 0x00, 0xA1};
uint8_t R4On[] = {0xA0, 0x02, 0x01, 0xA3};
uint8_t R4Off[] = {0xA0, 0x02, 0x00, 0xA2};

// WiFi credentials
const char* ssid = "YourWifiSSID";
const char* password = "YourWifiPassword";

// mDNS hostname (you can access via http://hen-automation.local/)
const char* mdnsHostname = "hen-automation";

// Time variables
int Hours = 0;
int Minutes = 0;
int Seconds = 0;
int sunriseHour = 0;
int sunriseMin = 0;
int sunsetHour = 0;
int sunsetMin = 0;
int dayOfYear = 0;

// State tracking
bool morningActive = false;
bool eveningActive = false;
bool manualOverride = false;
bool manualMorningState = false;
bool manualEveningState = false;

// Web server
ESP8266WebServer server(80);

// NTP Client setup
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600); // UTC+1 (adjust offset as needed)

// Timezone offset in seconds (3600 for UTC+1, 7200 for UTC+2, etc.)
const long utcOffsetInSeconds = 3600;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\nESP8266 Hen Automation Starting...");
  
  // Initialize relay control pins
  pinMode(CH_PD, OUTPUT);
  pinMode(RST, OUTPUT);
  pinMode(GPIO0, OUTPUT);
  digitalWrite(CH_PD, HIGH);
  digitalWrite(RST, HIGH);
  digitalWrite(GPIO0, HIGH);
  
  // Connect to WiFi (DHCP - no static IP)
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    // Setup mDNS
    if (MDNS.begin(mdnsHostname)) {
      Serial.print("✓ mDNS started! Access via: http://");
      Serial.print(mdnsHostname);
      Serial.println(".local/");
      MDNS.addService("http", "tcp", 80);
    } else {
      Serial.println("✗ mDNS failed to start");
    }
  } else {
    Serial.println("\n✗ Failed to connect to WiFi");
  }
  
  // Setup NTP time
  timeClient.begin();
  timeClient.setUpdateInterval(60000); // Update every 60 seconds
  
  // Configure timezone
  configTime(utcOffsetInSeconds, 0, "pool.ntp.org", "time.nist.gov");
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/control", handleControl);
  server.onNotFound(handleNotFound);
  server.begin();
  
  Serial.println("Web server started");
  Serial.print("Open browser and go to: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/ or http://");
  Serial.print(mdnsHostname);
  Serial.println(".local/");
}

void loop() {
  // Handle mDNS updates
  MDNS.update();
  
  // Handle web server requests
  server.handleClient();
  
  // Update time
  timeClient.update();
  Hours = timeClient.getHours();
  Minutes = timeClient.getMinutes();
  Seconds = timeClient.getSeconds();
  
  // Calculate day of year and sunrise/sunset times
  updateSunTimes();
  
  // Check if it's time for sunrise/sunset events
  checkScheduledEvents();
  
  delay(100);
}

void updateSunTimes() {
  static int lastDayOfYear = -1;
  
  // Calculate day of year (1-366)
  time_t now = timeClient.getEpochTime();
  struct tm* timeinfo = localtime(&now);
  dayOfYear = timeinfo->tm_yday;
  
  // Only recalculate once per day
  if (dayOfYear != lastDayOfYear) {
    lastDayOfYear = dayOfYear;
    
    // Simplified sunrise/sunset calculation (adjust for your latitude/longitude)
    // Formula: sunrise/sunset varies between roughly 380-500 min and 1050-1150 min throughout year
    int sunriseMinutes = (int)(380 + 121 * cos((dayOfYear - 8) / 58.09));
    int sunsetMinutes = (int)(1144 - 144 * cos((dayOfYear - 8) / 58.09));
    
    // Adjust for your location (optional fine-tuning)
    sunriseMinutes -= 20;  // Adjust as needed
    sunsetMinutes += 30;   // Adjust as needed
    
    sunriseHour = sunriseMinutes / 60;
    sunriseMin = sunriseMinutes % 60;
    sunsetHour = sunsetMinutes / 60;
    sunsetMin = sunsetMinutes % 60;
    
    Serial.printf("Day %d - Sunrise: %02d:%02d, Sunset: %02d:%02d\n", 
                  dayOfYear, sunriseHour, sunriseMin, sunsetHour, sunsetMin);
  }
}

void checkScheduledEvents() {
  // Sunrise event (morning relays)
  if (!manualOverride && !morningActive && 
      Hours == sunriseHour && Minutes == sunriseMin) {
    Serial.println(">> Sunrise event triggered!");
    Serial.write(R1On, 4);
    delay(50);
    Serial.write(R2On, 4);
    morningActive = true;
    delay(1000); // Debounce
  }
  
  // Turn off sunrise after 1 minute
  if (morningActive && Minutes >= (sunriseMin + 1)) {
    if (Hours == sunriseHour || Hours == (sunriseHour + 1)) {
      Serial.write(R1Off, 4);
      delay(50);
      Serial.write(R2Off, 4);
      morningActive = false;
      Serial.println("<< Sunrise relays turned off");
    }
  }
  
  // Sunset event (evening relays)
  if (!manualOverride && !eveningActive && 
      Hours == sunsetHour && Minutes == sunsetMin) {
    Serial.println(">> Sunset event triggered!");
    Serial.write(R3On, 4);
    delay(50);
    Serial.write(R4On, 4);
    eveningActive = true;
    delay(1000); // Debounce
  }
  
  // Turn off sunset after 15 minutes
  if (eveningActive && Minutes >= (sunsetMin + 15)) {
    if (Hours == sunsetHour || Hours == (sunsetHour + 1)) {
      Serial.write(R3Off, 4);
      delay(50);
      Serial.write(R4Off, 4);
      eveningActive = false;
      Serial.println("<< Sunset relays turned off");
    }
  }
}

void handleControl() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    
    if (action == "morningOn") {
      manualOverride = true;
      manualMorningState = true;
      Serial.write(R1On, 4);
      delay(50);
      Serial.write(R2On, 4);
      Serial.println("Manual: Morning relays ON");
    } 
    else if (action == "morningOff") {
      manualOverride = true;
      manualMorningState = false;
      Serial.write(R1Off, 4);
      delay(50);
      Serial.write(R2Off, 4);
      Serial.println("Manual: Morning relays OFF");
    } 
    else if (action == "eveningOn") {
      manualOverride = true;
      manualEveningState = true;
      Serial.write(R3On, 4);
      delay(50);
      Serial.write(R4On, 4);
      Serial.println("Manual: Evening relays ON");
    } 
    else if (action == "eveningOff") {
      manualOverride = true;
      manualEveningState = false;
      Serial.write(R3Off, 4);
      delay(50);
      Serial.write(R4Off, 4);
      Serial.println("Manual: Evening relays OFF");
    }
    else if (action == "autoMode") {
      manualOverride = false;
      Serial.println("Auto mode restored");
    }
  }
  
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleStatus() {
  String json = "{";
  json += "\"time\":\"" + String(Hours, DEC) + ":" + (Minutes < 10 ? "0" : "") + String(Minutes, DEC) + "\",";
  json += "\"sunrise\":\"" + String(sunriseHour, DEC) + ":" + (sunriseMin < 10 ? "0" : "") + String(sunriseMin, DEC) + "\",";
  json += "\"sunset\":\"" + String(sunsetHour, DEC) + ":" + (sunsetMin < 10 ? "0" : "") + String(sunsetMin, DEC) + "\",";
  json += "\"morningActive\":" + String(morningActive ? "true" : "false") + ",";
  json += "\"eveningActive\":" + String(eveningActive ? "true" : "false") + ",";
  json += "\"manualOverride\":" + String(manualOverride ? "true" : "false") + ",";
  json += "\"dayOfYear\":" + String(dayOfYear, DEC);
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleRoot() {
  String html = R"====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Hen Automation Control</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 15px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            padding: 30px;
            max-width: 600px;
            width: 100%;
        }
        h1 {
            color: #333;
            margin-bottom: 30px;
            text-align: center;
            font-size: 28px;
        }
        .info-section {
            background: #f8f9fa;
            border-radius: 10px;
            padding: 20px;
            margin-bottom: 25px;
            border-left: 4px solid #667eea;
        }
        .time-display {
            font-size: 48px;
            font-weight: bold;
            color: #667eea;
            text-align: center;
            margin-bottom: 15px;
            font-family: 'Courier New', monospace;
        }
        .sun-times {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
            margin-top: 15px;
        }
        .sun-time {
            text-align: center;
            padding: 10px;
            background: white;
            border-radius: 8px;
        }
        .sun-time label {
            display: block;
            font-size: 12px;
            color: #666;
            margin-bottom: 5px;
            font-weight: 600;
            text-transform: uppercase;
        }
        .sun-time .time {
            font-size: 22px;
            font-weight: bold;
            color: #333;
            font-family: 'Courier New', monospace;
        }
        .status-section {
            background: #f8f9fa;
            border-radius: 10px;
            padding: 20px;
            margin-bottom: 25px;
            border-left: 4px solid #28a745;
        }
        .status-item {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 10px 0;
            border-bottom: 1px solid #e0e0e0;
        }
        .status-item:last-child {
            border-bottom: none;
        }
        .status-label {
            font-weight: 600;
            color: #555;
        }
        .status-value {
            padding: 6px 12px;
            border-radius: 20px;
            font-size: 13px;
            font-weight: 600;
        }
        .status-value.active {
            background: #28a745;
            color: white;
        }
        .status-value.inactive {
            background: #dc3545;
            color: white;
        }
        .status-value.auto {
            background: #17a2b8;
            color: white;
        }
        .control-section {
            background: #f8f9fa;
            border-radius: 10px;
            padding: 20px;
            border-left: 4px solid #ffc107;
        }
        .control-group {
            margin-bottom: 20px;
        }
        .control-group:last-child {
            margin-bottom: 0;
        }
        .control-group h3 {
            color: #333;
            font-size: 14px;
            margin-bottom: 10px;
            text-transform: uppercase;
            font-weight: 600;
        }
        .button-group {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 10px;
        }
        button {
            padding: 12px 15px;
            border: none;
            border-radius: 8px;
            font-weight: 600;
            font-size: 14px;
            cursor: pointer;
            transition: all 0.3s ease;
            text-transform: uppercase;
        }
        .btn-on {
            background: #28a745;
            color: white;
        }
        .btn-on:hover {
            background: #218838;
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(40, 167, 69, 0.3);
        }
        .btn-off {
            background: #dc3545;
            color: white;
        }
        .btn-off:hover {
            background: #c82333;
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(220, 53, 69, 0.3);
        }
        .btn-auto {
            background: #17a2b8;
            color: white;
            grid-column: 1 / -1;
        }
        .btn-auto:hover {
            background: #138496;
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(23, 162, 184, 0.3);
        }
        .footer {
            text-align: center;
            margin-top: 25px;
            font-size: 12px;
            color: #999;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🐔 Hen Automation Control</h1>
        
        <div class="info-section">
            <div class="time-display" id="currentTime">--:--</div>
            <div class="sun-times">
                <div class="sun-time">
                    <label>🌅 Sunrise</label>
                    <div class="time" id="sunriseTime">--:--</div>
                </div>
                <div class="sun-time">
                    <label>🌇 Sunset</label>
                    <div class="time" id="sunsetTime">--:--</div>
                </div>
            </div>
        </div>

        <div class="status-section">
            <div class="status-item">
                <span class="status-label">Morning Relays</span>
                <span class="status-value" id="morningStatus">INACTIVE</span>
            </div>
            <div class="status-item">
                <span class="status-label">Evening Relays</span>
                <span class="status-value" id="eveningStatus">INACTIVE</span>
            </div>
            <div class="status-item">
                <span class="status-label">Control Mode</span>
                <span class="status-value auto" id="modeStatus">AUTO</span>
            </div>
        </div>

        <div class="control-section">
            <div class="control-group">
                <h3>🌅 Morning Relays</h3>
                <div class="button-group">
                    <button class="btn-on" onclick="sendControl('morningOn')">ON</button>
                    <button class="btn-off" onclick="sendControl('morningOff')">OFF</button>
                </div>
            </div>

            <div class="control-group">
                <h3>🌇 Evening Relays</h3>
                <div class="button-group">
                    <button class="btn-on" onclick="sendControl('eveningOn')">ON</button>
                    <button class="btn-off" onclick="sendControl('eveningOff')">OFF</button>
                </div>
            </div>

            <div class="control-group">
                <button class="btn-auto" onclick="sendControl('autoMode')">↻ RETURN TO AUTO MODE</button>
            </div>
        </div>

        <div class="footer">
            <p>Last update: <span id="lastUpdate">--:--:--</span></p>
        </div>
    </div>

    <script>
        function updateStatus() {
            fetch('/status')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('currentTime').textContent = data.time;
                    document.getElementById('sunriseTime').textContent = data.sunrise;
                    document.getElementById('sunsetTime').textContent = data.sunset;
                    
                    const morningBadge = document.getElementById('morningStatus');
                    morningBadge.textContent = data.morningActive ? 'ACTIVE' : 'INACTIVE';
                    morningBadge.className = data.morningActive ? 'status-value active' : 'status-value inactive';
                    
                    const eveningBadge = document.getElementById('eveningStatus');
                    eveningBadge.textContent = data.eveningActive ? 'ACTIVE' : 'INACTIVE';
                    eveningBadge.className = data.eveningActive ? 'status-value active' : 'status-value inactive';
                    
                    const modeStatus = document.getElementById('modeStatus');
                    if (data.manualOverride) {
                        modeStatus.textContent = 'MANUAL';
                        modeStatus.style.background = '#ffc107';
                        modeStatus.style.color = '#333';
                    } else {
                        modeStatus.textContent = 'AUTO';
                        modeStatus.style.background = '#17a2b8';
                        modeStatus.style.color = 'white';
                    }
                    
                    const now = new Date();
                    document.getElementById('lastUpdate').textContent = 
                        now.getHours().toString().padStart(2, '0') + ':' +
                        now.getMinutes().toString().padStart(2, '0') + ':' +
                        now.getSeconds().toString().padStart(2, '0');
                })
                .catch(err => console.error('Status update failed:', err));
        }

        function sendControl(action) {
            fetch(`/control?action=${action}`)
                .then(response => response.json())
                .then(data => {
                    updateStatus();
                })
                .catch(err => console.error('Control failed:', err));
        }

        // Update status on page load and every 1 second
        updateStatus();
        setInterval(updateStatus, 1000);
    </script>
</body>
</html>
)====" ;
  
  server.send(200, "text/html", html);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}
