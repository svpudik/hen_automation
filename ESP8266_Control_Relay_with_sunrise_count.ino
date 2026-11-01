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

// Hard timeout values for motion control
const uint32_t MORNING_OPEN_TIMEOUT_MS = 60000UL;   // 60s opening window
const uint32_t MIN_DOOR_CLOSE_SECONDS = 1UL;
const uint32_t MAX_DOOR_CLOSE_SECONDS = 180UL;
uint32_t doorCloseSeconds = 16UL; // Adjustable close duration in seconds
uint32_t morningStartMillis = 0;
uint32_t eveningStartMillis = 0;

// Web server
ESP8266WebServer server(80);

// NTP Client setup
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0); // UTC time, DST handled manually

// Timezone offset in seconds (Europe: UTC+1 in winter, UTC+2 in summer)
long utcOffsetInSeconds = 3600;
bool isDaylightSavingTime = false;

uint32_t getDoorCloseTimeMs() {
  return doorCloseSeconds * 1000UL;
}

void adjustDoorCloseSeconds(int delta) {
  long adjusted = (long)doorCloseSeconds + delta;

  if (adjusted < (long)MIN_DOOR_CLOSE_SECONDS) adjusted = (long)MIN_DOOR_CLOSE_SECONDS;
  if (adjusted > (long)MAX_DOOR_CLOSE_SECONDS) adjusted = (long)MAX_DOOR_CLOSE_SECONDS;

  doorCloseSeconds = (uint32_t)adjusted;
  Serial.printf("Door close timer set to %lu seconds\n", (unsigned long)doorCloseSeconds);
}

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

  // Setup NTP time in UTC
  timeClient.begin();
  timeClient.setUpdateInterval(60000); // Update every 60 seconds
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

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
  MDNS.update();
  server.handleClient();

  timeClient.update();
  updateDaylightSavingTime();

  // Convert UTC time to local time using current DST offset
  time_t now = timeClient.getEpochTime();
  now += utcOffsetInSeconds;
  struct tm* timeinfo = gmtime(&now);

  Hours = timeinfo->tm_hour;
  Minutes = timeinfo->tm_min;
  Seconds = timeinfo->tm_sec;
  dayOfYear = timeinfo->tm_yday;

  updateSunTimes();
  checkScheduledEvents();

  delay(100);
}

void updateDaylightSavingTime() {
  time_t now = timeClient.getEpochTime();
  struct tm* utcInfo = gmtime(&now);
  int year = utcInfo->tm_year + 1900;
  int month = utcInfo->tm_mon + 1;
  int day = utcInfo->tm_mday;

  // Central European DST rules:
  // Starts last Sunday in March at 02:00 UTC
  // Ends last Sunday in October at 03:00 local time (01:00 UTC)
  bool dst = false;

  if (month > 3 && month < 10) {
    dst = true;
  } else if (month == 3) {
    int lastSunday = getLastSundayOfMonth(year, 3);
    if (day > lastSunday || (day == lastSunday && utcInfo->tm_hour >= 1)) {
      dst = true;
    }
  } else if (month == 10) {
    int lastSunday = getLastSundayOfMonth(year, 10);
    if (day < lastSunday || (day == lastSunday && utcInfo->tm_hour < 1)) {
      dst = true;
    }
  }

  isDaylightSavingTime = dst;
  utcOffsetInSeconds = isDaylightSavingTime ? 7200 : 3600;
}

int getLastSundayOfMonth(int year, int month) {
  struct tm timeStruct = {0};
  timeStruct.tm_year = year - 1900;
  timeStruct.tm_mon = month - 1;
  timeStruct.tm_mday = 31; // Last possible day in month

  time_t t = mktime(&timeStruct);
  struct tm* info = localtime(&t);
  int daysInMonth = info->tm_mday; // Should be last day of month

  for (int d = daysInMonth; d >= 1; d--) {
    timeStruct.tm_mday = d;
    time_t dayT = mktime(&timeStruct);
    struct tm* dayInfo = localtime(&dayT);
    if (dayInfo->tm_wday == 0) { // Sunday
      return d;
    }
  }
  return 1;
}

void updateSunTimes() {
  static int lastDayOfYear = -1;

  // Calculate day of year (1-366)
  time_t now = timeClient.getEpochTime();
  now += utcOffsetInSeconds;
  struct tm* timeinfo = gmtime(&now);
  dayOfYear = timeinfo->tm_yday;

  if (dayOfYear != lastDayOfYear) {
    lastDayOfYear = dayOfYear;

    int sunriseMinutes = (int)(380 + 121 * cos((dayOfYear - 8) / 58.09));
    int sunsetMinutes = (int)(1144 - 144 * cos((dayOfYear - 8) / 58.09));

    sunriseMinutes -= 20;
    sunsetMinutes += 30;

    sunriseHour = sunriseMinutes / 60;
    sunriseMin = sunriseMinutes % 60;
    sunsetHour = sunsetMinutes / 60;
    sunsetMin = sunsetMinutes % 60;

    Serial.printf("Day %d - DST=%s Sunrise: %02d:%02d, Sunset: %02d:%02d\n",
                  dayOfYear, isDaylightSavingTime ? "ON" : "OFF",
                  sunriseHour, sunriseMin, sunsetHour, sunsetMin);
  }
}

void startMorningRelays() {
  Serial.println(">> Sunrise event triggered!");
  Serial.write(R1On, 4);
  delay(50);
  Serial.write(R2On, 4);
  morningActive = true;
  morningStartMillis = millis();
}

void stopMorningRelays() {
  Serial.write(R1Off, 4);
  delay(50);
  Serial.write(R2Off, 4);
  morningActive = false;
  Serial.println("<< Sunrise relays turned off");
}

void startEveningRelays() {
  Serial.println(">> Sunset event triggered!");
  Serial.write(R3On, 4);
  delay(50);
  Serial.write(R4On, 4);
  eveningActive = true;
  eveningStartMillis = millis();
}

void stopEveningRelays() {
  Serial.write(R3Off, 4);
  delay(50);
  Serial.write(R4Off, 4);
  eveningActive = false;
  Serial.println("<< Sunset relays turned off");
}

void checkScheduledEvents() {
  // Morning event: start at sunrise time; hard timeout 60 seconds max.
  if (!manualOverride && !morningActive && Hours == sunriseHour && Minutes == sunriseMin) {
    startMorningRelays();
  }

  if (morningActive && (millis() - morningStartMillis >= MORNING_OPEN_TIMEOUT_MS)) {
    stopMorningRelays();
  }

  // Evening event: start at sunset time; hard timeout depends on configured door close time.
  if (!manualOverride && !eveningActive && Hours == sunsetHour && Minutes == sunsetMin) {
    startEveningRelays();
  }

  if (eveningActive && (millis() - eveningStartMillis >= getDoorCloseTimeMs())) {
    stopEveningRelays();
  }
}

void handleControl() {
  if (server.hasArg("action")) {
    String action = server.arg("action");

    if (action == "morningOn") {
      manualOverride = true;
      manualMorningState = true;
      startMorningRelays();
      Serial.println("Manual: Morning relays ON");
    }
    else if (action == "morningOff") {
      manualOverride = true;
      manualMorningState = false;
      stopMorningRelays();
      Serial.println("Manual: Morning relays OFF");
    }
    else if (action == "eveningOn") {
      manualOverride = true;
      manualEveningState = true;
      startEveningRelays();
      Serial.println("Manual: Evening relays ON");
    }
    else if (action == "eveningOff") {
      manualOverride = true;
      manualEveningState = false;
      stopEveningRelays();
      Serial.println("Manual: Evening relays OFF");
    }
    else if (action == "adjustCloseTime") {
      if (server.hasArg("delta")) {
        int delta = server.arg("delta").toInt();
        adjustDoorCloseSeconds(delta);
      }
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
  json += "\"doorCloseSeconds\":" + String(doorCloseSeconds, DEC) + ",";
  json += "\"dayOfYear\":" + String(dayOfYear, DEC) + ",";
  json += "\"isDST\":" + String(isDaylightSavingTime ? "true" : "false");
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
        .dst-indicator {
            text-align: center;
            font-size: 12px;
            color: #666;
            margin-bottom: 15px;
            font-weight: 600;
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
        .timer-control {
            display: grid;
            grid-template-columns: 52px 1fr 52px;
            gap: 10px;
            align-items: center;
        }
        .timer-btn {
            padding: 12px 15px;
            border: none;
            border-radius: 8px;
            font-weight: 700;
            font-size: 24px;
            cursor: pointer;
            transition: all 0.3s ease;
            background: #17a2b8;
            color: white;
        }
        .timer-btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(23, 162, 184, 0.3);
        }
        .timer-value {
            text-align: center;
            font-size: 30px;
            font-weight: bold;
            color: #333;
            background: white;
            border-radius: 8px;
            padding: 10px;
            font-family: 'Courier New', monospace;
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
            <div class="dst-indicator" id="dstIndicator">Checking time mode...</div>
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
            <div class="status-item">
                <span class="status-label">Evening Close</span>
                <span class="status-value auto" id="doorCloseDisplay">16s</span>
            </div>
        </div>

        <div class="control-section">
            <div class="control-group">
                <h3>🌅 Morning Relays</h3>
                <div class="button-group">
                    <button class="btn-on" onclick="sendControl('morningOn')">START</button>
                    <button class="btn-off" onclick="sendControl('morningOff')">STOP</button>
                </div>
            </div>

            <div class="control-group">
                <h3>🌇 Evening Relays</h3>
                <div class="button-group">
                    <button class="btn-on" onclick="sendControl('eveningOn')">START</button>
                    <button class="btn-off" onclick="sendControl('eveningOff')">STOP</button>
                </div>
            </div>

            <div class="control-group">
                <h3>🚪 Evening Close Timer</h3>
                <div class="timer-control">
                    <button class="timer-btn" onclick="adjustCloseTime(-1)" aria-label="Decrease by 1 second">−</button>
                    <div class="timer-value" id="closeTimerValue">16s</div>
                    <button class="timer-btn" onclick="adjustCloseTime(1)" aria-label="Increase by 1 second">+</button>
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
                    document.getElementById('closeTimerValue').textContent = data.doorCloseSeconds + 's';
                    document.getElementById('doorCloseDisplay').textContent = data.doorCloseSeconds + 's';

                    const dstIndicator = document.getElementById('dstIndicator');
                    if (data.isDST) {
                        dstIndicator.textContent = '☀️ LETNÍ ČAS (UTC+2)';
                        dstIndicator.style.color = '#ff9800';
                    } else {
                        dstIndicator.textContent = '❄️ ZIMNÍ ČAS (UTC+1)';
                        dstIndicator.style.color = '#2196f3';
                    }

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

        function adjustCloseTime(delta) {
            fetch(`/control?action=adjustCloseTime&delta=${delta}`)
                .then(response => response.json())
                .then(data => {
                    updateStatus();
                })
                .catch(err => console.error('Close timer update failed:', err));
        }

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
