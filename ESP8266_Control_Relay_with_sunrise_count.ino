#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>

#define CH_PD 44 //sinal de controle de CH_PD
#define RST 46 //sinal de controle de RST
#define GPIO0 48 //sinal de controle de GPIO0
uint8_t R3On[] = {0xA0, 0x01, 0x01, 0xA2};
uint8_t R3Off[] = {0xA0, 0x01, 0x00, 0xA1};
uint8_t R4On[] = {0xA0, 0x02, 0x01, 0xA3};
uint8_t R4Off[] = {0xA0, 0x02, 0x00, 0xA2};
uint8_t R1On[] = {0xA0, 0x03, 0x01, 0xA4};
uint8_t R1Off[] = {0xA0, 0x03, 0x00, 0xA3};
uint8_t R2On[] = {0xA0, 0x04, 0x01, 0xA5};
uint8_t R2Off[] = {0xA0, 0x04, 0x00, 0xA4};

const char* ssid = "<wifiSSID>"; // fill in here your router or wifi SSID
const char* password = "<wifiPassword>"; // fill in here your router or wifi password

int Hours;
int Minutes;
int sunriseMin;
int sunriseHour;
int sunsetMin;
int sunsetHour;
int sunset;
int sunrise;

ESP8266WebServer server(80);

const long utcOffsetInSeconds = 7200;
unsigned char
leap_days ;
unsigned int
epoch, ntp_year, days_since_epoch, day_of_year;

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

void setup()
{
  pinMode(CH_PD,OUTPUT);
  pinMode(RST,OUTPUT);
  pinMode(GPIO0,OUTPUT);
  digitalWrite(CH_PD,HIGH);
  digitalWrite(RST,HIGH);
  digitalWrite(GPIO0,HIGH);
  Serial.begin(115200);
  
  Serial.println("initial timeout . . .");
  delay(1000 * 8);

  // Connect to WiFi network
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
  Serial.println("WiFi connected");

  // Print the IP address
  Serial.print("Use this URL to connect: ");
  Serial.print("https://");
  Serial.print(WiFi.localIP());
  Serial.println("/");


  // Get time from web
  timeClient.begin();
  
  server.on("/", handle_OnConnect);
  server.onNotFound(handle_NotFound);
  server.begin();
  server.handleClient();
  WiFiClient client;
  HTTPClient http;
}

void loop()
{ 
  // Print current time
  timeClient.update();
  epoch = timeClient.getEpochTime();
  leap_days = 0;
  days_since_epoch = epoch / 60 / 60 / 24; //number of days since epoch
  ntp_year = 1970 + (days_since_epoch / 365); // ball parking year, may not be accurate!
  leap_days;
  int i;
  for (i = 1972; i < ntp_year; i += 4) // Calculating number of leap days since epoch/1970
    if (((i % 4 == 0) && (i % 100 != 0)) || (i % 400 == 0)) leap_days++;
  day_of_year = ((days_since_epoch - leap_days) % 365) + 1;
  int sunset = 1144 - 170 * cos((day_of_year + 8) / 49);
  int sunrise = 392 + 117 * cos((day_of_year + 8) / 49);
  sunriseHour = sunrise / 60;
  sunriseMin = sunrise % 60;
  sunsetHour = sunset / 60;
  sunsetMin = sunset % 60;
  
  if (timeClient.getHours() == (sunrise / 60) && timeClient.getMinutes() == (sunrise % 60))
  {
    Serial.write(R1On, 4);
    delay(50);
    Serial.write(R2On, 4);
    delay(1000 * 15);
    Serial.write(R1Off, 4);
    delay(50);
    Serial.write(R2Off, 4);
  }
    if (timeClient.getHours() == (sunset / 60) && timeClient.getMinutes() == (sunset % 60))
  {
    Serial.write(R3On, 4);
    delay(50);
    Serial.write(R4On, 4);
    delay(1000 * 21);
    Serial.write(R3Off, 4);
    delay(50);
    Serial.write(R4Off, 4);
  }
  int x = 0;
  do {
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

  server.send(200, "text/html", SendHTML(Hours, Minutes, sunriseHour, sunriseMin, sunsetHour, sunsetMin));
}

void handle_NotFound() {
  server.send(404, "text/plain", "Not found");
}
String SendHTML(float Hoursstat, float Minutesstat, float sunriseHourstat, float sunriseMinstat, float sunsetHourstat, float sunsetMinstat) {
  String ptr = "Current Time: ";
  ptr += (int)Hoursstat;
  ptr += ":";
  ptr += (int)Minutesstat;
  ptr += " sunrise: ";
  ptr += (int)sunriseHourstat;
  ptr += ":";
  ptr += (int)sunriseMinstat;
  ptr += " sunset: ";
  ptr += (int)sunsetHourstat;
  ptr += ":";
  ptr += (int)sunsetMinstat;
  return ptr;
}