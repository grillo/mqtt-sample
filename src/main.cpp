#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ETH.h>
#include <time.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Adxl355.h> // forked from https://github.com/markrad/esp32-ADXL355
#include <math.h>
#include <esp_https_ota.h>
#include <SPIFFS.h>
#include "config.h"
#include "semver.h" // from https://github.com/h2non/semver.c
#include <cppQueue.h>

// MQTT connection details
const char *mqtt_server = "192.168.0.4";
static char MQTT_DEVICEID[30]; // Allocate a buffer large enough for "d:orgid:devicetype:deviceid"
#define MQTT_PORT 1883         // Secure MQTT 8883 / Insecure MQTT 1883
#define MQTT_TOPIC "iot-2/evt/status/fmt/json"
#define MQTT_TOPIC_ALARM "iot-2/cmd/earthquake/fmt/json"
#define MQTT_TOPIC_SAMPLERATE "iot-2/cmd/samplerate/fmt/json"
#define MQTT_TOPIC_FWCHECK "iot-2/cmd/firmwarecheck/fmt/json"
#define MQTT_TOPIC_SEND10SEC "iot-2/cmd/10secondhistory/fmt/json"
#define MQTT_TOPIC_SENDACCEL "iot-2/cmd/sendacceldata/fmt/json"
char deviceID[13];

void SetTimeESP32();
void SendLiveData2Cloud();
void Send10Seconds2Cloud();

// Timezone info
#define TZ_OFFSET -5 // (EST) Hours timezone offset to GMT (without daylight saving time)
#define TZ_DST 60    // Minutes timezone offset for Daylight saving

// MQTT objects
void callback(char *topic, byte *payload, unsigned int length);
//WiFiClientSecure wifiClient;
WiFiClient wifiClient;
PubSubClient mqtt(mqtt_server, MQTT_PORT, callback, wifiClient);

// ETH_CLOCK_GPIO17_OUT - 50MHz clock from internal APLL inverted output on GPIO17 - tested with LAN8720
#ifdef ETH_CLK_MODE
#undef ETH_CLK_MODE
#endif
#define ETH_CLK_MODE ETH_CLOCK_GPIO17_OUT
// Pin# of the enable signal for the external crystal oscillator (-1 to disable for internal APLL source)
#ifdef PRODUCTION_BOARD
#define ETH_POWER_PIN 2 // Ethernet on production board
#else
#define ETH_POWER_PIN -1 // Ethernet on prototype board
#endif
// Type of the Ethernet PHY (LAN8720 or TLK110)
#define ETH_TYPE ETH_PHY_LAN8720
// I²C-address of Ethernet PHY (0 or 1 for LAN8720, 31 for TLK110)
#define ETH_ADDR 0
// Pin# of the I²C clock signal for the Ethernet PHY
#define ETH_MDC_PIN 23
// Pin# of the I²C IO signal for the Ethernet PHY
#define ETH_MDIO_PIN 18

// Network variables
Preferences prefs;
String _ssid; // your network SSID (name) - loaded from NVM
String _pswd; // your network password    - loaded from NVM
int networksStored;
static bool eth_connected = false;
static bool wificonnected = false;

// --------------------------------------------------------------------------------------------
// ADXL Accelerometer
void IRAM_ATTR isr_adxl();

int32_t Adxl355SampleRate = 31; // Reporting Sample Rate [31,125]

int8_t CHIP_SELECT_PIN_ADXL = 15;
#ifdef PRODUCTION_BOARD
int8_t ADXL_INT_PIN = 35; // ADXL is on interrupt 35 on production board
#else
int8_t ADXL_INT_PIN = 2; // ADXL is on interrupt 2 on prototype board
#endif
Adxl355::RANGE_VALUES range = Adxl355::RANGE_VALUES::RANGE_2G;
Adxl355::ODR_LPF odr_lpf;
Adxl355::STATUS_VALUES adxstatus;
Adxl355 adxl355(CHIP_SELECT_PIN_ADXL);
SPIClass *spi1 = NULL;

long fifoOut[32][3];
long runningAverage[3] = {0, 0, 0};
long fifoDelta[32][3];
bool fifoFull = false;
int fifoCount = 0;
int numValsForAvg = 0;

// --------------------------------------------------------------------------------------------
// Variables to hold accelerometer data
DynamicJsonDocument jsonDoc(4000);
DynamicJsonDocument jsonTraces(4000);
JsonArray traces = jsonTraces.to<JsonArray>();
static char msg[2000];

// 10 second FIFO queue for STA / LTA algorithm
typedef struct AccelXYZ
{
  double x;
  double y;
  double z;
} AccelReading;
cppQueue StaLtaQue(sizeof(AccelReading), 352, FIFO); // 11 seconds of Accelerometer data
uint32_t numSecsOfAccelReadings = 0;

// --------------------------------------------------------------------------------------------
// SmartConfig
int numScannedNetworks();
int numNetworksStored();
void readNetworkStored(int netId);
void storeNetwork(String ssid, String pswd);
bool WiFiScanAndConnect();
bool startSmartConfig();

// --------------------------------------------------------------------------------------------
// NeoPixel LEDs
#include <Adafruit_NeoPixel.h>
#define LED_PIN 16
#define LED_COUNT 3
//Adafruit_NeoPixel pixels = Adafruit_NeoPixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
//   NEO_KHZ800  800 KHz bitstream (most NeoPixel products w/WS2812 LEDs)
//   NEO_GRB     Pixels are wired for GRB bitstream (most NeoPixel products)
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
void NeoPixelStatus(int);
void NeoPixelBreathe();
bool breathedirection = true;
int breatheintensity = 1;

// Map the OpenEEW LED status colors to the Particle Photon status colors
#define LED_OFF 0
#define LED_CONNECTED 1     // Cyan breath
#define LED_FIRMWARE_OTA 2  // Magenta
#define LED_CONNECT_WIFI 3  // Green
#define LED_CONNECT_CLOUD 4 // Cyan fast
#define LED_LISTEN_WIFI 5   // Blue
#define LED_WIFI_OFF 6      // White
#define LED_SAFE_MODE 7     // Magenta breath
#define LED_FIRMWARE_DFU 8  // Yellow
#define LED_ERROR 9         // Red

// --------------------------------------------------------------------------------------------
// Buzzer Alarm
void EarthquakeAlarm();
void AlarmBuzzer();
int freq = 4000;
int channel = 0;
int resolution = 8;
int io = 5;

// --------------------------------------------------------------------------------------------
void IRAM_ATTR isr_adxl()
{
  fifoFull = true;
  //fifoCount++;
}

void StartADXL355()
{
  // odr_lpf is a global
  adxl355.start();
  delay(1000);

  if (adxl355.isDeviceRecognized())
  {
    Serial.println("Initializing sensor");
    adxl355.initializeSensor(range, odr_lpf, debug);
    Serial.println("Calibrating sensor");
    adxl355.calibrateSensor(5, debug);
    Serial.println("ADXL355 Accelerometer activated");
  }
  else
  {
    Serial.println("Unable to get accelerometer");
  }
  Serial.println("Finished accelerometer configuration");
}

// Handle subscribed MQTT topics - Alerts and Sample Rate changes
void callback(char *topic, byte *payload, unsigned int length)
{
  StaticJsonDocument<100> jsonMQTTReceiveDoc;
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] : ");

  payload[length] = 0; // ensure valid content is zero terminated so can treat as c-string
  Serial.println((char *)payload);
  DeserializationError err = deserializeJson(jsonMQTTReceiveDoc, (char *)payload);
  if (err)
  {
    Serial.print(F("deserializeJson() failed with code : "));
    Serial.println(err.c_str());
  }
  else
  {
    JsonObject cmdData = jsonMQTTReceiveDoc.as<JsonObject>();
    if (strcmp(topic, MQTT_TOPIC_ALARM) == 0)
    {
      // Sound the Buzzer & Blink the LED
      EarthquakeAlarm();
    }
    else if (strcmp(topic, MQTT_TOPIC_SEND10SEC) == 0)
    {
      // Send 10 seconds of accelerometer history
      Serial.println("Send 10 seconds of accelerometer history to the cloud");
      Send10Seconds2Cloud();
    }
    else if (strcmp(topic, MQTT_TOPIC_SENDACCEL) == 0)
    {
      // Start sending live accelometer data to the cloud. The payload asks for n seconds of data
      numSecsOfAccelReadings = cmdData["LiveDataDuration"].as<uint32_t>();
      Serial.print("Send live accelometer data to the cloud (secs):");
      Serial.println(numSecsOfAccelReadings);
    }
    else if (strcmp(topic, MQTT_TOPIC_SAMPLERATE) == 0)
    {
      // Set the ADXL355 Sample Rate
      int32_t NewSampleRate = 0;
      bool SampleRateChanged = false;

      NewSampleRate = cmdData["SampleRate"].as<int32_t>(); // this form allows you specify the type of the data you want from the JSON object
      if (NewSampleRate == 31)
      {
        // Requested sample rate of 31 is valid
        Adxl355SampleRate = 31;
        SampleRateChanged = true;
        odr_lpf = Adxl355::ODR_LPF::ODR_31_25_AND_7_813;
      }
      else if (NewSampleRate == 125)
      {
        // Requested sample rate of 125 is valid
        Adxl355SampleRate = 125;
        SampleRateChanged = true;
        odr_lpf = Adxl355::ODR_LPF::ODR_125_AND_31_25;
      }
      else if (NewSampleRate == 0)
      {
        // Turn off the sensor ADXL
        Adxl355SampleRate = 0;
        SampleRateChanged = false; // false so the code below doesn't restart it
        Serial.println("Stopping the ADXL355");
        adxl355.stop();
        StaLtaQue.flush(); // flush the Queue
        strip.clear();     // Off
        strip.show();
      }
      else
      {
        // invalid - leave the Sample Rate unchanged
      }

      Serial.print("ADXL355 Sample Rate has been changed:");
      Serial.println(Adxl355SampleRate);
      //SampleRateChanged = false;
      Serial.println(SampleRateChanged);
      if (SampleRateChanged)
      {
        Serial.println("Changing the ADXL355 Sample Rate");
        adxl355.stop();
        delay(1000);
        Serial.println("Restarting");
        StartADXL355();
        breatheintensity = 1;
        breathedirection = true;
      }
      jsonMQTTReceiveDoc.clear();
    }
    else
    {
      Serial.println("Unknown command received");
    }
  }
}

void Connect2MQTTbroker()
{
  while (!mqtt.connected())
  {
    Serial.print("Attempting MQTT connection...");
    NeoPixelStatus(LED_CONNECT_CLOUD); // blink cyan
    // Attempt to connect / re-connect to IBM Watson IoT Platform
    // These params are globals assigned in setup()
    //if (mqtt.connect(mqtt_server, 1883))
    if (mqtt.connect(MQTT_DEVICEID))
    { // No Token Authentication
      Serial.println("MQTT Connected");
      mqtt.subscribe(MQTT_TOPIC_ALARM);
      mqtt.subscribe(MQTT_TOPIC_SAMPLERATE);
      mqtt.subscribe(MQTT_TOPIC_FWCHECK);
      mqtt.subscribe(MQTT_TOPIC_SEND10SEC);
      mqtt.subscribe(MQTT_TOPIC_SENDACCEL);
      mqtt.setBufferSize(2000);
      mqtt.loop();
    }
    else
    {
      Serial.println("MQTT Failed to connect!");
      delay(5000);
    }
  }
}

void Send10Seconds2Cloud()
{
  // DynamicJsonDocument is stored on the heap
  // Allocate a ArduinoJson buffer large enough to 10 seconds of Accelerometer trace data
  DynamicJsonDocument historydoc(16384);
  JsonObject payload = historydoc.to<JsonObject>();
  JsonObject status = payload.createNestedObject("d");
  JsonArray alltraces = status.createNestedArray("traces");

  // Load the key/value pairs into the serialized ArduinoJSON format
  status["device_id"] = deviceID;

  // Generate an array of json objects that contain x,y,z arrays of 32 floats.
  // [{"x":[],"y":[],"z":[]},{"x":[],"y":[],"z":[]}]
  JsonObject acceleration = alltraces.createNestedObject();

  AccelReading AccelRecord;
  //char reading[75];
  for (uint16_t idx = 0; idx < StaLtaQue.getCount(); idx++)
  {
    if (StaLtaQue.peekIdx(&AccelRecord, idx))
    {
      //sprintf( reading, "[ x=%3.3f , y=%3.3f , z=%3.3f ]", AccelRecord.x, AccelRecord.y, AccelRecord.z);
      //Serial.println(reading);

      acceleration["x"].add(AccelRecord.x);
      acceleration["y"].add(AccelRecord.y);
      acceleration["z"].add(AccelRecord.z);
    }
  }

  // Serialize the History Json object into a string to be transmitted
  //serializeJson(historydoc,Serial);  // print to console
  static char historymsg[16384];
  ;
  serializeJson(historydoc, historymsg, 16383);

  int jsonSize = measureJson(historydoc);
  Serial.print("Sending 10 seconds of accelerometer readings in a MQTT packet of size: ");
  Serial.println(jsonSize);
  mqtt.setBufferSize((jsonSize + 50)); // increase the MQTT buffer size

  // Publish the message to MQTT Broker
  if (!mqtt.publish(MQTT_TOPIC, historymsg))
  {
    Serial.println("MQTT Publish failed");
  }
  else
  {
    NeoPixelStatus(LED_CONNECTED); // Success - blink cyan
  }

  historydoc.clear();
}

void SendLiveData2Cloud()
{
  // variables to hold accelerometer data
  // DynamicJsonDocument is stored on the heap
  JsonObject payload = jsonDoc.to<JsonObject>();
  JsonObject status = payload.createNestedObject("d");

  // Load the key/value pairs into the serialized ArduinoJSON format
  status["device_id"] = deviceID;
  status["traces"] = traces;

  // Serialize the entire string to be transmitted
  serializeJson(jsonDoc, msg, 2000);
  Serial.println(msg);

  // Publish the message to MQTT Broker
  if (!mqtt.publish(MQTT_TOPIC, msg))
  {
    Serial.println("MQTT Publish failed");
  }
  else
  {
    NeoPixelStatus(LED_CONNECTED); // Success - blink cyan
  }

  jsonDoc.clear();
}

void NetworkEvent(WiFiEvent_t event)
{
  switch (event)
  {
  case SYSTEM_EVENT_WIFI_READY: // 0
    Serial.println("ESP32 WiFi interface ready");
    break;
  case SYSTEM_EVENT_STA_START: // 2
    Serial.println("ESP32 WiFi started");
    break;
  case SYSTEM_EVENT_SCAN_DONE:
    Serial.println("Completed scan for access points");
    break;
  case SYSTEM_EVENT_STA_CONNECTED: // 4
    Serial.println("ESP32 WiFi connected to AP");
    WiFi.setHostname("openeew-sensor-wifi");
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    Serial.println("Disconnected from WiFi access point");
    wificonnected = false;
    break;
  case SYSTEM_EVENT_STA_GOT_IP: // 7
    Serial.println("ESP32 station got IP from connected AP");
    Serial.print("Obtained IP address: ");
    Serial.println(WiFi.localIP());
    if (eth_connected)
    {
      Serial.println("Ethernet is already connected");
    }
    break;
  case SYSTEM_EVENT_ETH_START:
    Serial.println("ETH Started");
    //set eth / wifi hostname here
    ETH.setHostname("openeew-sensor-eth");
    break;
  case SYSTEM_EVENT_ETH_CONNECTED:
    Serial.println("ETH Connected");
    Serial.print("ETH MAC: ");
    Serial.println(ETH.macAddress());
    break;
  case SYSTEM_EVENT_ETH_GOT_IP:
    Serial.print("ETH MAC: ");
    Serial.print(ETH.macAddress());
    Serial.print(", IPv4: ");
    Serial.print(ETH.localIP());
    if (ETH.fullDuplex())
    {
      Serial.print(", FULL_DUPLEX");
    }
    Serial.print(", ");
    Serial.print(ETH.linkSpeed());
    Serial.println("Mbps");
    eth_connected = true;

    // Switch the MQTT connection to Ethernet from WiFi (or initially)
    // Preference the Ethernet wired interence if its available
    // Disconnect the MQTT session
    if (mqtt.connected())
    {
      mqtt.disconnect();
      // No need to call mqtt.setClient(ETH); because ETH is a ETHClient which is not the same class as WiFi client
      // Connect2MQTTbroker(); // The MQTT reconnect will be handled by the main loop()
    }
    break;
  case SYSTEM_EVENT_ETH_DISCONNECTED:
    Serial.println("ETH Disconnected");
    eth_connected = false;
    // Disconnect the MQTT client
    if (mqtt.connected())
    {
      mqtt.disconnect();
    }
    break;
  case SYSTEM_EVENT_ETH_STOP:
    Serial.println("ETH Stopped");
    eth_connected = false;
    break;
  case SYSTEM_EVENT_STA_STOP:
    Serial.println("WiFi Stopped");
    NeoPixelStatus(LED_WIFI_OFF); // White
    break;
  case SYSTEM_EVENT_AP_STOP:
    Serial.println("ESP32 soft-AP stop");
    break;
  case SYSTEM_EVENT_AP_STACONNECTED:
    Serial.println("a station connected to ESP32 soft-AP");
    break;
  default:
    Serial.print("Unhandled Network Interface event : ");
    Serial.println(event);
    break;
  }
}

// MQTT SSL requires a relatively accurate time between broker and client
void SetTimeESP32()
{
  // Set time from NTP servers
  configTime(TZ_OFFSET * 3600, TZ_DST * 60, "pool.ntp.org", "0.pool.ntp.org");
  Serial.println("\nWaiting for time");
  while (time(nullptr) <= 100000)
  {
    NeoPixelStatus(LED_FIRMWARE_DFU); // blink yellow
    Serial.print(".");
    delay(100);
  }
  unsigned timeout = 5000;
  unsigned start = millis();
  while (millis() - start < timeout)
  {
    time_t now = time(nullptr);
    if (now > (2019 - 1970) * 365 * 24 * 3600)
    {
      break;
    }
    delay(100);
  }
  delay(1000); // Wait for time to fully sync
  Serial.println("Time sync'd");
  time_t now = time(nullptr);
  Serial.println(ctime(&now));
}

//------------SETUP-----------------//

void setup()
{
  // Start serial console
  Serial.begin(115200);
  Serial.setTimeout(2000);
  while (!Serial)
  {
  }
  Serial.println();
  Serial.println("OpenEEW Sensor Application");

  strip.setBrightness(130); // Dim the LED to 50% - 0 off, 255 full bright

  // Start WiFi connection
  WiFi.onEvent(NetworkEvent);
  WiFi.mode(WIFI_STA);

  wificonnected = WiFiScanAndConnect();
  if (!wificonnected)
  {
    while (!startSmartConfig())
    {
      // loop in SmartConfig until the user provides
      // the correct WiFi SSID and password
    }
  }
  Serial.println("WiFi Connected");

  byte mac[6]; // the MAC address of your Wifi shield
  WiFi.macAddress(mac);

  // Output this ESP32 Unique WiFi MAC Address
  Serial.print("WiFi MAC: ");
  Serial.println(WiFi.macAddress());

  // Start the ETH interface
  ETH.begin(ETH_ADDR, ETH_POWER_PIN, ETH_MDC_PIN, ETH_MDIO_PIN, ETH_TYPE, ETH_CLK_MODE);
  Serial.print("ETH  MAC: ");
  Serial.println(ETH.macAddress());

  // Use the reverse octet Mac Address as the MQTT deviceID
  //sprintf(deviceID,"%02X%02X%02X%02X%02X%02X",mac[5],mac[4],mac[3],mac[2],mac[1],mac[0]);
  sprintf(deviceID, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println(deviceID);

  // Set the time on the ESP32
  SetTimeESP32();

  //sprintf(MQTT_DEVICEID,"d:%s:%s:%02X%02X%02X%02X%02X%02X",MQTT_ORGID,MQTT_DEVICETYPE,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  //Serial.println(MQTT_DEVICEID);

  //char mqttparams[100]; // Allocate a buffer large enough for this string ~95 chars
  //sprintf(mqttparams, "MQTT_USER:%s  MQTT_TOKEN:%s  MQTT_DEVICEID:%s", MQTT_USER, MQTT_TOKEN, MQTT_DEVICEID);
  //Serial.println(mqttparams);

  // Connect to MQTT - IBM Watson IoT Platform
  Serial.print("connect to mqtt");
  Connect2MQTTbroker();

#if OPENEEW_SAMPLE_RATE_125
  odr_lpf = Adxl355::ODR_LPF::ODR_125_AND_31_25;
#endif

#if OPENEEW_SAMPLE_RATE_31_25
  odr_lpf = Adxl355::ODR_LPF::ODR_31_25_AND_7_813;
#endif

  pinMode(ADXL_INT_PIN, INPUT);
  pinMode(CHIP_SELECT_PIN_ADXL, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(ADXL_INT_PIN), isr_adxl, FALLING);

  spi1 = new SPIClass(HSPI);
  adxl355.initSPI(*spi1);
  StartADXL355();

  ledcSetup(channel, freq, resolution);
  ledcAttachPin(io, channel);
  pinMode(io, OUTPUT);
  digitalWrite(io, LOW); // turn off buzzer
}

//------------LOOP-----------------//
void loop()
{
  mqtt.loop();
  // Confirm Connection to MQTT
  Connect2MQTTbroker();

  //====================== ADXL Accelerometer =====================
  if (fifoFull)
  {
    fifoFull = false;
    adxstatus = adxl355.getStatus();

    if (adxstatus & Adxl355::STATUS_VALUES::FIFO_FULL)
    {
      int numEntriesFifo = adxl355.readFifoEntries((long *)fifoOut);
      if (numEntriesFifo != -1)
      {
        long sumFifo[3];
        sumFifo[0] = 0;
        sumFifo[1] = 0;
        sumFifo[2] = 0;

        for (int i = 0; i < 32; i++)
        {
          for (int j = 0; j < 3; j++)
          {
            fifoDelta[i][j] = fifoOut[i][j] - runningAverage[j];
            sumFifo[j] += fifoOut[i][j];
          }
        }

        for (int j = 0; j < 3; j++)
        {
          runningAverage[j] = (numValsForAvg * runningAverage[j] + sumFifo[j]) / (numValsForAvg + 32);
        }

        numValsForAvg = min(numValsForAvg + 32, 2000);

        // Generate an array of json objects that contain x,y,z arrays of 32 floats.
        // [{"x":[],"y":[],"z":[]},{"x":[],"y":[],"z":[]}]
        JsonObject acceleration = traces.createNestedObject();

        // [{"x":[9.479,0],"y":[0.128,-1.113],"z":[-0.185,123.321]},{"x":[9.479,0],"y":[0.128,-1.113],"z":[-0.185,123.321]}]
        double gal;
        double x, y, z;
        for (int i = 0; i < numEntriesFifo; i++)
        {
          AccelReading AccelRecord;
          gal = adxl355.valueToGals(fifoDelta[i][0]);
          x = round(gal * 1000) / 1000;
          acceleration["x"].add(x);
          AccelRecord.x = x;

          gal = adxl355.valueToGals(fifoDelta[i][1]);
          y = round(gal * 1000) / 1000;
          acceleration["y"].add(y);
          AccelRecord.y = y;

          gal = adxl355.valueToGals(fifoDelta[i][2]);
          z = round(gal * 1000) / 1000;
          acceleration["z"].add(z);
          AccelRecord.z = z;

          StaLtaQue.push(&AccelRecord);
        }

        // Do some STA / LTA math here...
        // ...
        // Whoa - STA/LTA algorithm detected some anomalous shaking
        bool bPossibleEarthQuake = false;
        if (bPossibleEarthQuake)
        {
          // Start sending 5 minutes of live accelerometer data
          numSecsOfAccelReadings = 300;
          // Send the previous 10 seconds of history to the cloud
          Send10Seconds2Cloud();
        }
        char mathmsg[65];
        sprintf(mathmsg, "%d accelerometer readings on the StaLta Queue", StaLtaQue.getCount());
        Serial.println(mathmsg);
        // When the math is done, drop 32 records off the queue
        if (StaLtaQue.isFull())
        {
          for (int i = 0; i < 32; i++)
            StaLtaQue.drop();
        }

        if (numSecsOfAccelReadings > 0)
        {
          SendLiveData2Cloud();
          numSecsOfAccelReadings--;
        }
        // Clear & Reset JsonArrays
        jsonTraces.clear();
        traces = jsonTraces.to<JsonArray>();

        //Switch the direction of the LEDs
        breathedirection = breathedirection ? false : true;
      }
    }
  }
  if (adxstatus)
    NeoPixelBreathe();

  delay(10);
}

//MORE STUFF

//================================= WiFi Handling ================================
//Scan networks in range and return how many are they.
int numScannedNetworks()
{
  int n = WiFi.scanNetworks();
  Serial.println("WiFi Network scan done");
  if (n == 0)
  {
    Serial.println("No networks found");
  }
  else
  {
    Serial.print(n);
    Serial.println(" network(s) found");
    //#if LOG_L2
    for (int i = 0; i < n; ++i)
    {
      // Print SSID and RSSI for each network found
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.print(")");
      Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " " : "*");
      delay(10);
    }
    //#endif
  }
  return n;
}

//Return how many networks are stored in the NVM
int numNetworksStored()
{
  prefs.begin("networks", true);
  networksStored = prefs.getInt("num_nets");
  Serial.print("Stored networks : ");
  Serial.println(networksStored);
  prefs.end();

  return networksStored;
}

//Each network as an id so reading the network stored with said ID.
void readNetworkStored(int netId)
{
  Serial.println("Reading stored networks from NVM");

  prefs.begin("networks", true);
  String idx;
  idx = "SSID" + (String)netId;
  _ssid = prefs.getString(idx.c_str(), "");
  idx = "key" + (String)netId;
  _pswd = prefs.getString(idx.c_str(), "");
  prefs.end();

  Serial.print("Found network ");
  Serial.print(_ssid);
  Serial.print(" , ");
  DEBUG_L2(_pswd); // off by default
  Serial.println("xxxxxx");
}

//Save a pair of SSID and PSWD to NVM
void storeNetwork(String ssid, String pswd)
{
  Serial.print("Writing network to NVM: ");
  Serial.print(ssid);
  Serial.print(",");
  Serial.println(pswd);

  prefs.begin("networks", false);
  int aux_num_nets = prefs.getInt("num_nets");
  Serial.print("Stored networks in NVM: ");
  Serial.println(aux_num_nets);
  aux_num_nets++;
  String idx;
  idx = "SSID" + (String)aux_num_nets;
  prefs.putString(idx.c_str(), ssid);
  idx = "key" + (String)aux_num_nets;
  prefs.putString(idx.c_str(), pswd);
  prefs.putInt("num_nets", aux_num_nets);
  prefs.end();
  Serial.print("Device has ");
  Serial.print(aux_num_nets);
  Serial.println(" networks stored in NVM");
}

//Joins the previous functions, gets the stored networks and compares to the available, if there is a match and connects, return true
//if no match or unable to connect, return false.
bool WiFiScanAndConnect()
{
  int num_nets = numNetworksStored();
  int num_scan = numScannedNetworks();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);

  for (int i = 1; i < (num_nets + 1); i++)
  {
    readNetworkStored(i);

    for (int j = 0; j < num_scan; j++)
    {
      if (_ssid == WiFi.SSID(j))
      {
        //Serial.print("Status from connection attempt");
        WiFi.begin(_ssid.c_str(), _pswd.c_str());
        WiFi.setSleep(false);
        unsigned long t0 = millis();

        while (WiFi.status() != WL_CONNECTED && (millis() - t0) < CONNECTION_TO)
        {
          NeoPixelStatus(LED_LISTEN_WIFI); // blink blue
          delay(1000);
        }
        if (WiFi.status() == WL_CONNECTED)
        {
          Serial.println("WiFi was successfully connected");
          return true;
        }
        else
        {
          Serial.println("There was a problem connecting to WiFi");
        }
      }
      else
      {
        Serial.println("Got no match for network");
      }
    }
  }
  Serial.println("Found no matches for saved networks");
  return false;
}

// Executes the Smart config routine and if connected, will save the network for future use
bool startSmartConfig()
{
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.beginSmartConfig();

  // Wait for SmartConfig packet from mobile
  Serial.println("Waiting for SmartConfig.");
  while (!WiFi.smartConfigDone() || eth_connected)
  {
    delay(500);
    Serial.print(".");
    NeoPixelStatus(LED_LISTEN_WIFI); // blink blue
  }

  for (int i = 0; i < 4; i++)
  {
    delay(500);
    NeoPixelStatus(LED_CONNECT_WIFI); // Success - blink green
  }

  if (eth_connected)
  {
    // Ethernet cable was connected during or before SmartConfig
    // Skip SmartConfig
    return true;
  }
  Serial.println("SmartConfig received.");

  // Wait for WiFi to connect to AP
  Serial.println("Waiting for WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < CONNECTION_TO)
  {
    delay(500);
    Serial.print(".");
    NeoPixelStatus(LED_LISTEN_WIFI); // blink blue
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    _ssid = WiFi.SSID();
    _pswd = WiFi.psk();
    Serial.print("Smart Config done, connected to: ");
    Serial.print(_ssid);
    Serial.print(" with psswd: ");
    Serial.println("xxxxxx");
    DEBUG_L2(_pswd) // off by default
    storeNetwork(_ssid, _pswd);
    NeoPixelStatus(LED_CONNECT_WIFI); // Success - blink green
    return true;
  }
  else
  {
    Serial.println("Something went wrong with SmartConfig");
    WiFi.stopSmartConfig();
    return false;
  }
}

void NeoPixelStatus(int status)
{
  // Turn leds off to cause a blink effect
  strip.clear(); // Off
  strip.show();  // This sends the updated pixel color to the hardware.
  delay(400);    // Delay for a period of time (in milliseconds).

  switch (status)
  {
  case LED_OFF:
    strip.clear(); // Off
    break;
  case LED_CONNECTED:
    strip.fill(strip.Color(0, 255, 255), 0, 3); // Cyan breath
    Serial.println("LED_CONNECTED - Cyan");
    break;
  case LED_FIRMWARE_OTA:
    strip.fill(strip.Color(255, 0, 255), 0, 3); // Magenta
    Serial.println("LED_FIRMWARE_OTA - Magenta");
    break;
  case LED_CONNECT_WIFI:
    strip.fill(strip.Color(0, 255, 0), 0, 3); // Green
    Serial.println("LED_CONNECT_WIFI - Green");
    break;
  case LED_CONNECT_CLOUD:
    strip.fill(strip.Color(0, 255, 255), 0, 3); // Cyan fast
    Serial.println("LED_CONNECT_CLOUD - Cyan");
    break;
  case LED_LISTEN_WIFI:
    strip.fill(strip.Color(0, 0, 255), 0, 3); // Blue
    Serial.println("LED_LISTEN_WIFI - Blue");
    break;
  case LED_WIFI_OFF:
    strip.fill(strip.Color(255, 255, 255), 0, 3); // White
    Serial.println("LED_WIFI_OFF - White");
    break;
  case LED_SAFE_MODE:
    strip.fill(strip.Color(255, 0, 255), 0, 3); // Magenta breath
    Serial.println("LED_SAFE_MODE - Magenta");
    break;
  case LED_FIRMWARE_DFU:
    strip.fill(strip.Color(255, 255, 0), 0, 3); // Yellow
    Serial.println("LED_FIRMWARE_DFU - Yellow");
    break;
  case LED_ERROR:
    strip.fill(strip.Color(255, 0, 0), 0, 3); // Red
    Serial.println("LED_ERROR - Red");
    break;
  default:
    strip.clear(); // Off
    break;
  }
  strip.show(); // Send the updated pixel color to the hardware
}

void NeoPixelBreathe()
{
  if (breatheintensity < 0)
    breatheintensity = 0;
  strip.setBrightness(breatheintensity); // slow breathe the LED
  // Serial.printf("Brightness is %d\n",breatheintensity);
  strip.fill(strip.Color(0, 255, 255), 0, 3);
  strip.show();

  // Increase or decrease the LED intensity
  breathedirection ? breatheintensity++ : breatheintensity--;
}

// Sound the Buzzer & Blink the LED
void EarthquakeAlarm()
{
  Serial.println("Earthquake Alarm!");
  for (int i = 0; i < 10; i++)
  {
    delay(500);
    NeoPixelStatus(LED_ERROR); // Alarm - blink red
    AlarmBuzzer();
  }
  digitalWrite(io, LOW); // turn off buzzer
}

// Generate Buzzer sounds
void AlarmBuzzer()
{
  ledcWrite(channel, 50);
  delay(100);
  ledcWrite(channel, 500);
  delay(100);
  ledcWrite(channel, 2000);
  delay(100);
  ledcWrite(channel, 4000);
  delay(100);
}
