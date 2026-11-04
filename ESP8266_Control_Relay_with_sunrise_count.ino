#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <time.h>
#include <EEPROM.h>
#include <math.h>
#include <stdlib.h>

// Relay control pins
#define CH_PD 4   // GPIO4 (D2)
#define RST 5     // GPIO5 (D1)
#define GPIO0 0   // GPIO0 (D3)

// Relay command bytes
uint8_t R1On[]  = {0xA0, 0x03, 0x01, 0xA4};
uint8_t R1Off[] = {0xA0, 0x03, 0x00, 0xA3};
uint8_t R2On[]  = {0xA0, 0x04, 0x01, 0xA5};
uint8_t R2Off[] = {0xA0, 0x04, 0x00, 0xA4};
uint8_t R3On[]  = {0xA0, 0x01, 0x01, 0xA2};
uint8_t R3Off[] = {0xA0, 0x01, 0x00, 0xA1};
uint8_t R4On[]  = {0xA0, 0x02, 0x01, 0xA3};
uint8_t R4Off[] = {0xA0, 0x02, 0x00, 0xA2};

// WiFi credentials
const char* ssid = "YourWifiSSID";
const char* password = "YourWifiPassword";
const char* mdnsHostname = "hen-automation";

// Brno, Czech Republic defaults. Editable from the web page.
float latitude = 49.1951f;
float longitude = 16.6068f;
int sunriseOffsetMinutes = 0;
int sunsetOffsetMinutes = 0;
const int MIN_SUN_OFFSET_MINUTES = -180;
const int MAX_SUN_OFFSET_MINUTES = 180;

// Persist location and offsets in ESP8266 flash-backed EEPROM.
const uint32_t SETTINGS_MAGIC = 0x48454E31;
struct SavedSettings {
  uint32_t magic;
  float latitude;
  float longitude;
  int16_t sunriseOffsetMinutes;
  int16_t sunsetOffsetMinutes;
};

int Hours = 0, Minutes = 0, Seconds = 0;
int sunriseHour = 0, sunriseMin = 0, sunsetHour = 0, sunsetMin = 0;
int dayOfYear = 0;
bool morningActive = false, eveningActive = false;
bool manualOverride = false;
bool manualMorningState = false, manualEveningState = false;

const uint32_t MORNING_OPEN_TIMEOUT_MS = 60000UL;
const uint32_t MIN_DOOR_CLOSE_SECONDS = 1UL;
const uint32_t MAX_DOOR_CLOSE_SECONDS = 180UL;
uint32_t doorCloseSeconds = 16UL;
uint32_t morningStartMillis = 0, eveningStartMillis = 0;

ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0); // UTC; local offset applied below
long utcOffsetInSeconds = 3600;
bool isDaylightSavingTime = false;
int lastSunTimesDay = -1;

uint32_t getDoorCloseTimeMs() {
  return doorCloseSeconds * 1000UL;
}

void adjustDoorCloseSeconds(int delta) {
  long adjusted = (long)doorCloseSeconds + delta;
  if (adjusted < (long)MIN_DOOR_CLOSE_SECONDS) adjusted = MIN_DOOR_CLOSE_SECONDS;
  if (adjusted > (long)MAX_DOOR_CLOSE_SECONDS) adjusted = MAX_DOOR_CLOSE_SECONDS;
  doorCloseSeconds = (uint32_t)adjusted;
  Serial.printf("Door close timer set to %lu seconds\n", (unsigned long)doorCloseSeconds);
}

void loadSettings() {
  EEPROM.begin(512);
  SavedSettings saved;
  EEPROM.get(0, saved);
  if (saved.magic == SETTINGS_MAGIC && isfinite(saved.latitude) &&
      isfinite(saved.longitude) && saved.latitude >= -89.8f && saved.latitude <= 89.8f &&
      saved.longitude >= -180.0f && saved.longitude <= 180.0f &&
      saved.sunriseOffsetMinutes >= MIN_SUN_OFFSET_MINUTES &&
      saved.sunriseOffsetMinutes <= MAX_SUN_OFFSET_MINUTES &&
      saved.sunsetOffsetMinutes >= MIN_SUN_OFFSET_MINUTES &&
      saved.sunsetOffsetMinutes <= MAX_SUN_OFFSET_MINUTES) {
    latitude = saved.latitude;
    longitude = saved.longitude;
    sunriseOffsetMinutes = saved.sunriseOffsetMinutes;
    sunsetOffsetMinutes = saved.sunsetOffsetMinutes;
  }
}

void saveSettings() {
  SavedSettings saved = { SETTINGS_MAGIC, latitude, longitude,
                          (int16_t)sunriseOffsetMinutes, (int16_t)sunsetOffsetMinutes };
  EEPROM.put(0, saved);
  EEPROM.commit();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nESP8266 Hen Automation Starting...");

  pinMode(CH_PD, OUTPUT);
  pinMode(RST, OUTPUT);
  pinMode(GPIO0, OUTPUT);
  digitalWrite(CH_PD, HIGH);
  digitalWrite(RST, HIGH);
  digitalWrite(GPIO0, HIGH);
  loadSettings();

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
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(mdnsHostname)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS started: http://%s.local/\n", mdnsHostname);
    } else {
      Serial.println("mDNS failed to start");
    }
  } else {
    Serial.println("\nFailed to connect to WiFi");
  }

  timeClient.begin();
  timeClient.setUpdateInterval(60000);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/control", handleControl);
  server.on("/settings", HTTP_POST, handleSettings);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started");
}

void loop() {
  MDNS.update();
  server.handleClient();
  timeClient.update();
  updateDaylightSavingTime();

  time_t localNow = timeClient.getEpochTime() + utcOffsetInSeconds;
  struct tm* timeinfo = gmtime(&localNow);
  Hours = timeinfo->tm_hour;
  Minutes = timeinfo->tm_min;
  Seconds = timeinfo->tm_sec;
  dayOfYear = timeinfo->tm_yday + 1; // 1..366

  updateSunTimes();
  checkScheduledEvents();
  delay(100);
}

int getLastSundayOfMonth(int year, int month) {
  struct tm date = {0};
  date.tm_year = year - 1900;
  date.tm_mon = month - 1;
  date.tm_mday = 31;
  mktime(&date);
  int daysInMonth = date.tm_mday;
  for (int d = daysInMonth; d >= 1; --d) {
    date.tm_mday = d;
    mktime(&date);
    if (date.tm_wday == 0) return d;
  }
  return 1;
}

void updateDaylightSavingTime() {
  time_t now = timeClient.getEpochTime();
  struct tm* utcInfo = gmtime(&now);
  int year = utcInfo->tm_year + 1900;
  int month = utcInfo->tm_mon + 1;
  int day = utcInfo->tm_mday;
  bool dst = false;
  if (month > 3 && month < 10) dst = true;
  else if (month == 3) {
    int lastSunday = getLastSundayOfMonth(year, 3);
    dst = day > lastSunday || (day == lastSunday && utcInfo->tm_hour >= 1);
  } else if (month == 10) {
    int lastSunday = getLastSundayOfMonth(year, 10);
    dst = day < lastSunday || (day == lastSunday && utcInfo->tm_hour < 1);
  }
  isDaylightSavingTime = dst;
  utcOffsetInSeconds = dst ? 7200 : 3600;
}

float normalizeDegrees(float value) {
  while (value < 0) value += 360.0f;
  while (value >= 360.0f) value -= 360.0f;
  return value;
}

int normalizeMinutes(int value) {
  value %= 1440;
  if (value < 0) value += 1440;
  return value;
}

// NOAA sunrise/sunset approximation. Returns local minutes after midnight,
// including the selected site's DST offset and the user-adjustable correction.
bool calculateSunTime(bool sunrise, int day, int& localMinutes) {
  const float zenith = 90.833f;
  float longitudeHour = longitude / 15.0f;
  float approximateTime = day + ((sunrise ? 6.0f : 18.0f) - longitudeHour) / 24.0f;
  float meanAnomaly = 0.9856f * approximateTime - 3.289f;
  float trueLongitude = normalizeDegrees(meanAnomaly + 1.916f * sinf(meanAnomaly * DEG_TO_RAD) +
                                        0.020f * sinf(2.0f * meanAnomaly * DEG_TO_RAD) + 282.634f);
  float rightAscension = normalizeDegrees(atanf(0.91764f * tanf(trueLongitude * DEG_TO_RAD)) * RAD_TO_DEG);
  float longitudeQuadrant = floorf(trueLongitude / 90.0f) * 90.0f;
  float ascensionQuadrant = floorf(rightAscension / 90.0f) * 90.0f;
  rightAscension = (rightAscension + longitudeQuadrant - ascensionQuadrant) / 15.0f;

  float sinDeclination = 0.39782f * sinf(trueLongitude * DEG_TO_RAD);
  float cosDeclination = cosf(asinf(sinDeclination));
  float denominator = cosDeclination * cosf(latitude * DEG_TO_RAD);
  if (fabsf(denominator) < 0.000001f) return false;
  float cosHourAngle = (cosf(zenith * DEG_TO_RAD) - sinDeclination * sinf(latitude * DEG_TO_RAD)) / denominator;
  if (cosHourAngle > 1.0f || cosHourAngle < -1.0f) return false; // no sunrise/sunset at this latitude/date

  float hourAngle = acosf(cosHourAngle) * RAD_TO_DEG;
  if (sunrise) hourAngle = 360.0f - hourAngle;
  float localMeanTime = hourAngle / 15.0f + rightAscension - 0.06571f * approximateTime - 6.622f;
  float utcHour = localMeanTime - longitudeHour;
  while (utcHour < 0) utcHour += 24.0f;
  while (utcHour >= 24.0f) utcHour -= 24.0f;

  int correction = sunrise ? sunriseOffsetMinutes : sunsetOffsetMinutes;
  localMinutes = normalizeMinutes((int)lroundf(utcHour * 60.0f) + (int)(utcOffsetInSeconds / 60) + correction);
  return true;
}

void updateSunTimes() {
  if (dayOfYear == lastSunTimesDay) return;
  lastSunTimesDay = dayOfYear;
  int sunriseMinutes, sunsetMinutes;
  if (calculateSunTime(true, dayOfYear, sunriseMinutes)) {
    sunriseHour = sunriseMinutes / 60;
    sunriseMin = sunriseMinutes % 60;
  }
  if (calculateSunTime(false, dayOfYear, sunsetMinutes)) {
    sunsetHour = sunsetMinutes / 60;
    sunsetMin = sunsetMinutes % 60;
  }
  Serial.printf("Day %d DST=%s Sunrise %02d:%02d Sunset %02d:%02d (lat %.4f, lon %.4f)\n",
                dayOfYear, isDaylightSavingTime ? "ON" : "OFF", sunriseHour, sunriseMin,
                sunsetHour, sunsetMin, latitude, longitude);
}

void startMorningRelays() {
  Serial.println(">> Sunrise event triggered!");
  Serial.write(R1On, 4); delay(50); Serial.write(R2On, 4);
  morningActive = true;
  morningStartMillis = millis();
}
void stopMorningRelays() {
  Serial.write(R1Off, 4); delay(50); Serial.write(R2Off, 4);
  morningActive = false;
  Serial.println("<< Sunrise relays turned off");
}
void startEveningRelays() {
  Serial.println(">> Sunset event triggered!");
  Serial.write(R3On, 4); delay(50); Serial.write(R4On, 4);
  eveningActive = true;
  eveningStartMillis = millis();
}
void stopEveningRelays() {
  Serial.write(R3Off, 4); delay(50); Serial.write(R4Off, 4);
  eveningActive = false;
  Serial.println("<< Sunset relays turned off");
}

void checkScheduledEvents() {
  if (!manualOverride && !morningActive && Hours == sunriseHour && Minutes == sunriseMin) startMorningRelays();
  if (morningActive && millis() - morningStartMillis >= MORNING_OPEN_TIMEOUT_MS) stopMorningRelays();
  if (!manualOverride && !eveningActive && Hours == sunsetHour && Minutes == sunsetMin) startEveningRelays();
  if (eveningActive && millis() - eveningStartMillis >= getDoorCloseTimeMs()) stopEveningRelays();
}

void handleControl() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    if (action == "morningOn") { manualOverride = true; manualMorningState = true; startMorningRelays(); }
    else if (action == "morningOff") { manualOverride = true; manualMorningState = false; stopMorningRelays(); }
    else if (action == "eveningOn") { manualOverride = true; manualEveningState = true; startEveningRelays(); }
    else if (action == "eveningOff") { manualOverride = true; manualEveningState = false; stopEveningRelays(); }
    else if (action == "adjustCloseTime" && server.hasArg("delta")) adjustDoorCloseSeconds(server.arg("delta").toInt());
    else if (action == "autoMode") { manualOverride = false; Serial.println("Auto mode restored"); }
  }
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

bool parseSettingFloat(const String& text, float& value) {
  if (text.length() == 0) return false;
  char* end = NULL;
  value = (float)strtod(text.c_str(), &end);
  return end != text.c_str() && *end == '\0' && isfinite(value);
}

void handleSettings() {
  float newLatitude, newLongitude;
  char* end = NULL;
  long newSunriseOffset = strtol(server.arg("sunriseOffset").c_str(), &end, 10);
  bool sunriseValid = server.hasArg("sunriseOffset") && end != server.arg("sunriseOffset").c_str() && *end == '\0';
  String sunsetArg = server.arg("sunsetOffset");
  end = NULL;
  long newSunsetOffset = strtol(sunsetArg.c_str(), &end, 10);
  bool sunsetValid = server.hasArg("sunsetOffset") && end != sunsetArg.c_str() && *end == '\0';

  if (!server.hasArg("latitude") || !parseSettingFloat(server.arg("latitude"), newLatitude) ||
      !server.hasArg("longitude") || !parseSettingFloat(server.arg("longitude"), newLongitude) ||
      !sunriseValid || !sunsetValid || newLatitude < -89.8f || newLatitude > 89.8f ||
      newLongitude < -180.0f || newLongitude > 180.0f ||
      newSunriseOffset < MIN_SUN_OFFSET_MINUTES || newSunriseOffset > MAX_SUN_OFFSET_MINUTES ||
      newSunsetOffset < MIN_SUN_OFFSET_MINUTES || newSunsetOffset > MAX_SUN_OFFSET_MINUTES) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid settings\"}");
    return;
  }

  latitude = newLatitude;
  longitude = newLongitude;
  sunriseOffsetMinutes = (int)newSunriseOffset;
  sunsetOffsetMinutes = (int)newSunsetOffset;
  saveSettings();
  lastSunTimesDay = -1;
  updateSunTimes();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleStatus() {
  String json = "{";
  json += "\"time\":\"" + String(Hours) + ":" + (Minutes < 10 ? "0" : "") + String(Minutes) + "\",";
  json += "\"sunrise\":\"" + String(sunriseHour) + ":" + (sunriseMin < 10 ? "0" : "") + String(sunriseMin) + "\",";
  json += "\"sunset\":\"" + String(sunsetHour) + ":" + (sunsetMin < 10 ? "0" : "") + String(sunsetMin) + "\",";
  json += "\"morningActive\":" + String(morningActive ? "true" : "false") + ",";
  json += "\"eveningActive\":" + String(eveningActive ? "true" : "false") + ",";
  json += "\"manualOverride\":" + String(manualOverride ? "true" : "false") + ",";
  json += "\"doorCloseSeconds\":" + String(doorCloseSeconds) + ",";
  json += "\"dayOfYear\":" + String(dayOfYear) + ",";
  json += "\"isDST\":" + String(isDaylightSavingTime ? "true" : "false") + ",";
  json += "\"latitude\":" + String(latitude, 4) + ",";
  json += "\"longitude\":" + String(longitude, 4) + ",";
  json += "\"sunriseOffsetMinutes\":" + String(sunriseOffsetMinutes) + ",";
  json += "\"sunsetOffsetMinutes\":" + String(sunsetOffsetMinutes);
  json += "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  String html = R"====(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hen Automation Control</title><style>
*{box-sizing:border-box}body{font-family:Arial,sans-serif;background:linear-gradient(135deg,#667eea,#764ba2);min-height:100vh;margin:0;padding:20px;display:flex;justify-content:center;align-items:center;color:#333}.container{background:#fff;border-radius:15px;box-shadow:0 20px 60px #0004;padding:25px;max-width:620px;width:100%}h1{text-align:center}.card{background:#f8f9fa;border-radius:10px;padding:18px;margin:16px 0;border-left:4px solid #667eea}.clock{text-align:center;font-size:42px;font-weight:bold;color:#667eea}.sun-times,.buttons{display:grid;grid-template-columns:1fr 1fr;gap:12px;text-align:center}.sun-times div{background:white;border-radius:8px;padding:10px}.sun-times strong{display:block;font-size:23px;margin-top:5px}.row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #ddd}.row:last-child{border:0}button{padding:12px;border:0;border-radius:8px;color:white;font-weight:bold;cursor:pointer;background:#17a2b8}.on{background:#28a745}.off{background:#dc3545}.wide{grid-column:1/-1}.timer{display:flex;align-items:center;justify-content:center;gap:15px}.timer strong{font-size:25px}input{width:100%;padding:10px;margin:5px 0 12px;border:1px solid #bbb;border-radius:6px}label{font-weight:bold}.hint{font-size:13px;color:#666;line-height:1.4}.message{min-height:20px;color:#b00}
</style></head><body><main class="container"><h1>🐔 Hen Automation Control</h1>
<section class="card"><div class="clock" id="currentTime">--:--</div><div style="text-align:center" id="dst">Loading…</div><div class="sun-times"><div>🌅 Sunrise<strong id="sunrise">--:--</strong></div><div>🌇 Sunset<strong id="sunset">--:--</strong></div></div></section>
<section class="card"><div class="row"><b>Morning relays</b><span id="morning">INACTIVE</span></div><div class="row"><b>Evening relays</b><span id="evening">INACTIVE</span></div><div class="row"><b>Mode</b><span id="mode">AUTO</span></div><div class="row"><b>Evening close timer</b><span id="closeDisplay">16s</span></div></section>
<section class="card"><h3>Morning relays</h3><div class="buttons"><button class="on" onclick="control('morningOn')">START</button><button class="off" onclick="control('morningOff')">STOP</button></div><h3>Evening relays</h3><div class="buttons"><button class="on" onclick="control('eveningOn')">START</button><button class="off" onclick="control('eveningOff')">STOP</button></div><h3>Evening close duration</h3><div class="timer"><button onclick="control('adjustCloseTime',-1)">−</button><strong id="closeTimer">16s</strong><button onclick="control('adjustCloseTime',1)">+</button></div><p><button class="wide" onclick="control('autoMode')">RETURN TO AUTO MODE</button></p></section>
<section class="card"><h3>Location and solar-time correction</h3><p class="hint">Brno defaults: latitude 49.1951° N, longitude 16.6068° E. Enter coordinates in decimal degrees. Positive offsets make that event later; negative offsets make it earlier. Settings are saved across restarts.</p><form id="settingsForm"><label for="latitude">Latitude (−89.8 to 89.8)</label><input id="latitude" name="latitude" type="number" step="0.0001" min="-89.8" max="89.8" required><label for="longitude">Longitude (−180 to 180)</label><input id="longitude" name="longitude" type="number" step="0.0001" min="-180" max="180" required><label for="sunriseOffset">Sunrise correction, minutes (−180 to 180)</label><input id="sunriseOffset" name="sunriseOffset" type="number" step="1" min="-180" max="180" required><label for="sunsetOffset">Sunset correction, minutes (−180 to 180)</label><input id="sunsetOffset" name="sunsetOffset" type="number" step="1" min="-180" max="180" required><button type="submit">SAVE LOCATION & OFFSETS</button></form><div class="message" id="settingsMessage"></div></section>
<p class="hint" style="text-align:center">Last update: <span id="lastUpdate">--:--:--</span></p></main>
<script>
function updateStatus(){fetch('/status').then(r=>r.json()).then(d=>{document.getElementById('currentTime').textContent=d.time;document.getElementById('sunrise').textContent=d.sunrise;document.getElementById('sunset').textContent=d.sunset;document.getElementById('closeTimer').textContent=d.doorCloseSeconds+'s';document.getElementById('closeDisplay').textContent=d.doorCloseSeconds+'s';document.getElementById('dst').textContent=d.isDST?'☀️ LETNÍ ČAS (UTC+2)':'❄️ ZIMNÍ ČAS (UTC+1)';document.getElementById('morning').textContent=d.morningActive?'ACTIVE':'INACTIVE';document.getElementById('evening').textContent=d.eveningActive?'ACTIVE':'INACTIVE';document.getElementById('mode').textContent=d.manualOverride?'MANUAL':'AUTO';if(!document.getElementById('settingsForm').dataset.loaded){document.getElementById('latitude').value=d.latitude;document.getElementById('longitude').value=d.longitude;document.getElementById('sunriseOffset').value=d.sunriseOffsetMinutes;document.getElementById('sunsetOffset').value=d.sunsetOffsetMinutes;document.getElementById('settingsForm').dataset.loaded='true'}document.getElementById('lastUpdate').textContent=new Date().toLocaleTimeString()}).catch(console.error)}
function control(action,delta){let url='/control?action='+action;if(delta!==undefined)url+='&delta='+delta;fetch(url).then(()=>updateStatus()).catch(console.error)}
document.getElementById('settingsForm').addEventListener('submit',e=>{e.preventDefault();const form=e.currentTarget;fetch('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(form))}).then(async r=>{const d=await r.json();if(!r.ok)throw new Error(d.message||'Save failed');document.getElementById('settingsMessage').style.color='#187a2f';document.getElementById('settingsMessage').textContent='Settings saved';form.dataset.loaded='';updateStatus()}).catch(err=>{document.getElementById('settingsMessage').style.color='#b00';document.getElementById('settingsMessage').textContent=err.message})});updateStatus();setInterval(updateStatus,1000);
</script></body></html>
)====";
  server.send(200, "text/html", html);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}
