#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include "pm1006.h"
#include <Adafruit_NeoPixel.h>
#include <SensirionI2CScd4x.h>
#include <Wire.h>
#include <SPI.h>
#include "Adafruit_BMP280.h"
#include "DHT.h"
#include <WiFiMulti.h>
#include "secrets.h"
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>



#define PIN_FAN 12 //PIN for PM2.5 sensor FAN
#define PIN_LED 25 //PIN for neopixel leds
#define RXD2 16    //PIN for PM1006 sensor RX (TX on PM)
#define TXD2 17    //PIN for PM1006 TX (RX on PM)
#define DHTPIN 14  // Digital pin connected to the DHT sensor

#define BRIGHTNESS_DAY 5   //brightness of LEDs for day
#define BRIGHTNESS_NIGHT 0 //brightness for LEDs for night
#define PM_LED 3           //index of PM LED on strip
#define HUM_LED 2          //index of Hum LED on strip
#define CO2_LED 1          //index of CO2 led on strip

#define BMP280_ADD (0x76) //I2C address of BMP sensor
#define DHTTYPE DHT22     // DHT 22 (AM2302)

#define DAY_START 7 * 60 + 35 //day starts at 7:35
#define DAY_END 21 * 60 + 30  //day ends at 21:30

//define object variables
WiFiMulti wifiMulti;
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;
static PM1006 pm1006(&Serial2);
Adafruit_NeoPixel rgbWS = Adafruit_NeoPixel(3, PIN_LED, NEO_GRB + NEO_KHZ800);
SensirionI2CScd4x scd4x;
InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);
Point sensor("doliny");

//define timing variables
uint64_t lastMillis_influx = 0;
uint64_t lastMillis_pm25 = 0;

uint32_t DELAY_INFLUX = 60 * 1000 * 2;
uint32_t DELAY_PM25_DAY = 60 * 1000 * 5;
uint32_t DELAY_PM25_NIGHT = 60 * 1000 * 15;

//count failures before reset
int failCount = 0;

//NTP configs
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;
const char *ntpServer = "pool.ntp.org";

//define transactional variables
bool ledActive = false;
uint16_t co2;
float dhtTemp;
float dhtHum;
float pressure;
uint16_t pm2_5;

//declare funcs
void setColorWS(byte r, byte g, byte b, int id);
void syncTime();
long getMinutesOfDay();
bool isDay();
bool measurePM25();
bool stopSCD41periodicMeasurement();
bool startSCD41periodicMeasurement();
bool measureCO2();
bool measurePressure();
bool measureDHT();
bool sendDataToInflux();
void refreshLEDs();
bool connectToWifiOrRestart();

void setupOTA();

void setup()
{
  //start serials and wire
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  Wire.begin();

  // Setup wifi
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("vindriktning");
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Connecting to wifi");
  failCount = 0;
  connectToWifiOrRestart();
  failCount = 0;
  Serial.print("WIFI connected, IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  //sync time for Influx
  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");
  //sync time for me - should not be needed, but acting funny without it
  syncTime();

  rgbWS.begin(); // begin with leds and black them out!

  setColorWS(0, 0, 0, 1);
  setColorWS(0, 0, 0, 2);
  setColorWS(0, 0, 0, 3);

  //configure sensor instance - this is not cleared out between runs
  sensor.addTag("device", DEVICE);
  sensor.addTag("pokoj", ROOM);

  //validate connection and restart if failed
  if (client.validateConnection())
  {
    Serial.print("Connected to InfluxDB: ");
    Serial.println(client.getServerUrl());
  }
  else
  {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(client.getLastErrorMessage());
    ESP.restart();
  }

  //setup FAN output
  pinMode(PIN_FAN, OUTPUT); // Fan

  //start DHT22
  dht.begin();

  //init SCD41 CO2 meter

  scd4x.begin(Wire);

  stopSCD41periodicMeasurement();
  startSCD41periodicMeasurement();

  //init BMP280
  unsigned status;
  status = bmp.begin(BMP280_ADD);
  if (!status)
  {
    Serial.println("Could not find a valid BME280 sensor, check wiring, address, sensor ID!");
    Serial.print("SensorID was: 0x");
    Serial.println(bmp.sensorID(), 16);
    Serial.print("        ID of 0xFF probably means a bad address, a BMP 180 or BMP 085\n");
    Serial.print("   ID of 0x56-0x58 represents a BMP 280,\n");
    Serial.print("        ID of 0x60 represents a BME 280.\n");
    Serial.print("        ID of 0x61 represents a BME 680.\n");
  }
  else
  {
    Serial.print("BMP SensorID is: 0x");
    Serial.println(bmp.sensorID(), 16);
  }
  Serial.println("Waiting for first measurement... (5 sec)");
  setupOTA();
  delay(5000);
  failCount = 0;
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    failCount = 0;
    connectToWifiOrRestart();
    failCount = 0;
  }

  //refresh PM2.5 if needed
  uint32_t CURRENT_PM25_DELAY = isDay() ? DELAY_PM25_DAY : DELAY_PM25_NIGHT;
  if (lastMillis_pm25 + CURRENT_PM25_DELAY <= millis() || lastMillis_pm25 == 0)
  {
    lastMillis_pm25 = millis();
    stopSCD41periodicMeasurement();
    measurePM25();
    startSCD41periodicMeasurement();
    //wait for 6s so CO2 can refresh
    delay(6000);
  }

  if (!measureCO2())
  {
    //CO2 measurement failed, wait for 1s and restart the loop()
    delay(1000);
    return;
  }
  measurePressure();
  measureDHT();

  if (lastMillis_influx + DELAY_INFLUX <= millis() || lastMillis_influx == 0)
  {

    if(co2 != 0){
      if(sendDataToInflux()){
        lastMillis_influx = millis();
      }
      
    }
    
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    syncTime();
  }
  refreshLEDs();
  ArduinoOTA.handle();
  delay(10000);
}

void refreshLEDs()
{

  if (co2 < 1000)
  {
    setColorWS(0, 255, 0, CO2_LED);
  }

  if ((co2 >= 1000) && (co2 < 1200))
  {
    setColorWS(128, 255, 0, CO2_LED);
  }

  if ((co2 >= 1200) && (co2 < 1500))
  {
    setColorWS(255, 255, 0, CO2_LED);
  }

  if ((co2 >= 1500) && (co2 < 2000))
  {
    setColorWS(255, 128, 0, CO2_LED);
  }

  if (co2 >= 2000)
  {
    setColorWS(255, 0, 0, CO2_LED);
  }

  if (dhtHum < 45.0)
  {
    setColorWS(0, 0, 255, HUM_LED);
  }

  if ((dhtHum >= 45.0) && (dhtHum < 55.0))
  {
    setColorWS(0, 255, 0, HUM_LED);
  }

  if (dhtHum >= 55.0 && dhtHum < 60.0)
  {
    setColorWS(128, 255, 0, HUM_LED);
  }
  if (dhtHum >= 60 && dhtHum < 65.0)
  {
    setColorWS(255, 128, 0, HUM_LED);
  }
  if (dhtHum >= 65.0)
  {
    setColorWS(255, 0, 0, HUM_LED);
  }

  // PM LED
  if (pm2_5 < 30)
  {
    setColorWS(0, 255, 0, PM_LED);
  }

  if ((pm2_5 >= 30) && (pm2_5 < 40))
  {
    setColorWS(128, 255, 0, PM_LED);
  }

  if ((pm2_5 >= 40) && (pm2_5 < 80))
  {
    setColorWS(255, 255, 0, PM_LED);
  }

  if ((pm2_5 >= 80) && (pm2_5 < 90))
  {
    setColorWS(255, 128, 0, PM_LED);
  }

  if (pm2_5 >= 90)
  {
    setColorWS(255, 0, 0, PM_LED);
  }
}

void setColorWS(byte r, byte g, byte b, int id)
{
  uint32_t rgb;
  rgb = rgbWS.Color(r, g, b);
  rgbWS.setPixelColor(id - 1, rgb);
  rgbWS.setBrightness(isDay() ? BRIGHTNESS_DAY : BRIGHTNESS_NIGHT);
  rgbWS.show();
}

void syncTime()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }
}
long getMinutesOfDay()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return 0;
  }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time");
    syncTime();
    return 0;
  }
  return timeinfo.tm_hour * 60 + timeinfo.tm_min;
};

bool isDay()
{
  return (getMinutesOfDay() >= DAY_START && getMinutesOfDay() <= DAY_END);
}

bool startSCD41periodicMeasurement()
{
  uint16_t error;
  char errorMessage[256];
  // Start Measurement
  error = scd4x.startPeriodicMeasurement();
  if (error)
  {
    Serial.print("SCD41 Error trying to execute startPeriodicMeasurement(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
    return false;
  }
  {

    return true;
  }
}

bool stopSCD41periodicMeasurement()
{
  uint16_t error;
  char errorMessage[256];
  error = scd4x.stopPeriodicMeasurement();
  if (error)
  {
    Serial.print("SCD41 Error trying to execute stopPeriodicMeasurement(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
    return false;
  }
  else
  {
    return true;
  }
}

//starts FAN, waits 10s, reads PM2.5 to global variable pm2_5 and truns fan OFF
bool measurePM25()
{
  digitalWrite(PIN_FAN, HIGH);
  Serial.println("Fan ON");
  delay(10000);
  bool allOK = false;
  if (pm1006.read_pm25(&pm2_5))
  {
    printf("PM2.5 = %u\n", pm2_5);
    allOK = true;
  }
  else
  {
    Serial.println("Measurement failed!");
    allOK=false;
  }

  digitalWrite(PIN_FAN, LOW);
  Serial.println("Fan OFF");
  delay(1000);
  return allOK;
}

bool measureCO2()
{
  uint16_t error;
  char errorMessage[256];
  uint16_t co2_local = 0;
  float temperature = 0;
  float humidity = 0;
  error = scd4x.readMeasurement(co2_local, temperature, humidity);
  if (error)
  {
    Serial.print("SCD41 Error trying to execute readMeasurement(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
    co2 = 0;
    return false;
  }
  else if (co2_local == 0)
  {
    co2 = 0;
    Serial.println("Invalid sample detected, skipping.");
    return false;
  }
  co2 = co2_local;
  Serial.println("SCD41 values");
  Serial.print("Co2:");
  Serial.print(co2);
  Serial.print("\t");
  Serial.print(" Temperature:");
  Serial.print(temperature);
  Serial.print("\t");
  Serial.print(" Humidity:");
  Serial.println(humidity);
  return true;
}

bool measurePressure()
{
  Serial.println("BMP280:");
  Serial.print("Tlak: ");
  pressure = bmp.readPressure() / 100.F;
  Serial.print(pressure);
  Serial.println(" hPa.");
  Serial.println();
  return true;
}

bool measureDHT()
{
  dhtTemp = dht.readTemperature();
  dhtHum = dht.readHumidity();
  Serial.println("DHT22:");
  Serial.print("Teplota: ");
  Serial.print(dhtTemp);
  Serial.println(" stupnu Celsia.");
  Serial.print("Vlhkost: ");
  Serial.print(dhtHum);
  Serial.println(" %.");
  Serial.println();
  return true;
}

bool sendDataToInflux()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }
  sensor.clearFields();
  sensor.addField("co2", co2);
  sensor.addField("teplota", dhtTemp);
  sensor.addField("vlhkost", dhtHum);
  sensor.addField("tlak", pressure);
  sensor.addField("pm2_5", pm2_5);

  Serial.print("Writing: ");
  Serial.println(sensor.toLineProtocol());
  if(client.validateConnection() == false){
    return false;
  }
  if (!client.writePoint(sensor))
  {
    Serial.print("InfluxDB write failed: ");
    Serial.println(client.getLastErrorMessage());
    return false;
  }
  else
  {
    return true;
  }
}

bool connectToWifiOrRestart()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return true;
  }
  failCount = 0;
  Serial.print("Wifi connection in progress: ");
  while (wifiMulti.run() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(100);
    failCount++;
    if (failCount >= 150)
    {
      delay(5000);
      ESP.restart();
    }
  }
  failCount = 0;
  return true;
}
void setupOTA(){

   ArduinoOTA.setHostname("vindriktning");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();
}