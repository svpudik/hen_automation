#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>

const char* ssid = "<yourWifiName>extender"; // fill in here your router or wifi SSID
const char* password = "<yourWifiPassword>"; // fill in here your router or wifi password
#define RELAY 4 // relay connected to  GPIO4
#define LEDIN 2
#define LED 5 // led connected to GPIO5

int Hours;
int Minutes;
int sunsetHours;
int sunsetMinutes; 
int sunset;
int sunrise;
int test = 2;
IPAddress compIP(192, 168, 8, 98);
ESP8266WebServer server(80);

const long utcOffsetInSeconds = 7200;

char daysOfTheWeek[7][12] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

unsigned char
ntp_date, ntp_month, leap_days, leap_year_ind ;

unsigned int
epoch, ntp_year, days_since_epoch, day_of_year;

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

void setup()
{
  Serial.begin(115200); // must be same baudrate with the Serial Monitor
  Serial.println();
  Serial.println("initial timeout . . .");
  delay(1000 * 15);   
  
  pinMode(RELAY, OUTPUT);
  digitalWrite(RELAY, LOW);

  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);

  pinMode(LEDIN, OUTPUT);
  digitalWrite(LEDIN, LOW);
  do{ 
  // Connect to WiFi network
  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println(WiFi.status());
  Serial.println("WiFi connected");

  // Print the IP address
  Serial.print("Use this URL to connect: ");
  Serial.print("https://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
  if(test == 0){
    Serial.println("Reset..");
    ESP.restart();
    } 
  test = test - 1;
  
  }
  while(WiFi.localIP() != compIP);

  // Get time from web
  timeClient.begin();
  Serial.println("Time updated from NTP server");

  server.on("/", handle_OnConnect);
  server.onNotFound(handle_NotFound);

  server.begin();
  Serial.println("HTTP server started");
}

void loop()
{
  // Print current time
  timeClient.update();

  Serial.print(timeClient.getHours());
  Serial.print(":");
  Serial.println(timeClient.getMinutes());

  epoch = timeClient.getEpochTime();
  Serial.print("epoch:");
  Serial.println(epoch);

  leap_days = 0;

  days_since_epoch = epoch / 60 / 60 / 24; //number of days since epoch
  ntp_year = 1970 + (days_since_epoch / 365); // ball parking year, may not be accurate!

  leap_days;
  int i;
  for (i = 1972; i < ntp_year; i += 4) // Calculating number of leap days since epoch/1970
    if (((i % 4 == 0) && (i % 100 != 0)) || (i % 400 == 0)) leap_days++;

  day_of_year = ((days_since_epoch - leap_days) % 365) + 1;
  Serial.print("ntp_year:");
  Serial.println(ntp_year);
  Serial.print("leap_days:");
  Serial.println(leap_days);
  Serial.print("ntp_DYO:");
  Serial.println(day_of_year);
  int sunset = 1144 - 170 * cos((day_of_year + 8) / 49.4);
  Serial.print("sunset:");
  Serial.print(sunset / 60);
  Serial.print(":");
  Serial.println(sunset % 60);
  int sunrise = 392 + 117 * cos((day_of_year + 8) / 49.4);
  Serial.print("sunrise:");
  Serial.print(sunrise / 60);
  Serial.print(":");
  Serial.println(sunrise % 60);
  sunsetHours = sunset / 60;
  sunsetMinutes = sunset % 60;

  if (timeClient.getHours() == ((sunset / 60)) && timeClient.getMinutes() == (sunset % 60))
  {
    Serial.println("Time matched");
    Serial.println("RELAY=ON");
    digitalWrite(RELAY, HIGH);
    Serial.println("LED=ON");
    digitalWrite(LED, HIGH);
    delay(1000 * 15);
    Serial.println("RELAY=OFF");
    digitalWrite(RELAY, LOW);
    Serial.println("LED=OFF");
    digitalWrite(LED, LOW);
  }
  int x = 0;
  do{
  delay(1000 * 1);
  server.handleClient();
  x = x + 1;
  }
  while (x < 60);
  //-----------------------------------------------
}

void handle_OnConnect() {

  Minutes = timeClient.getMinutes();
  Hours = timeClient.getHours();
  
  server.send(200, "text/html", SendHTML(Hours, Minutes, sunsetHours, sunsetMinutes));
}

void handle_NotFound() {
  server.send(404, "text/plain", "Not found");
}
String SendHTML(float Hoursstat, float Minutesstat, float sunsetHourstat, float sunsetMinstat) {
  String ptr = "";
  ptr += (int)Hoursstat;
  ptr += ":";
  ptr += (int)Minutesstat;
  ptr += " sunset: ";
  ptr += (int)sunsetHourstat;
  ptr += ":";
  ptr += (int)sunsetMinstat;
  return ptr;
}
