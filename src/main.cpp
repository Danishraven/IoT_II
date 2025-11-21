#include <Arduino.h>
//#include <OneButton.h>
#include <WiFi.h>
#include <time.h>
#include <list>
#include "FS.h"
#include "SPIFFS.h"
#include "esp_timer.h"
#include "Diode.h"
#include <array>
#include <vector>
#include "secrets.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>


constexpr const char* _ENVIROMENT_ =  ENVIROMENT;
/* You only need to format SPIFFS the first time you run a
   test or else use the SPIFFS plugin to create a partition
   https://github.com/me-no-dev/arduino-esp32fs-plugin */
#define FORMAT_SPIFFS_IF_FAILED true

// Pin definitions
#define outputs {02, 04, 18, 19}
#define inputs  {32, 35, 34, 39}

// Debounce delay in milliseconds
#define debounceDelay 50

// Deep sleep configuration
#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  36000        /* Time ESP32 will go to sleep (in seconds) */

// Attempt definitions
#define MAX_SPIFFS_ATTEMPTS 10
#define MAX_UPLOAD_ATTEMPTS 10
#define MAX_DELETE_ATTEMPTS 10

// Wifi definitions
constexpr const char* _WIFI_SSID_ =  WIFI_SSID;
constexpr const char* _WIFI_PASSWORD_ = WIFI_PASSWORD;

// MQTT definitions
constexpr const char* _MQTT_SERVER_ = MQTT_SERVER;
constexpr const int _MQTT_PORT_ = MQTT_PORT;
constexpr const char* _MQTT_ROOT_CA_ = MQTT_ROOT_CA;
constexpr const char* _MQTT_USER_ = MQTT_USER; 
constexpr const char* _MQTT_PASS_ = MQTT_PASS;
constexpr const char* _MQTT_TOPIC_ = MQTT_TOPIC;


// function declarations
void setupPin(int pin, int mode, int value);
void print_wakeup_reason();
void initWiFi();
String readFile(fs::FS &fs, const char * path);
bool writeFile(fs::FS &fs, const char * path, const char * message);
bool appendFile(fs::FS &fs, const char * path, const char * message);
bool deleteFile(fs::FS &fs, const char * path);
void wakeupExt1Handler();
void wakeupTimerHandler();
uint64_t pinsToMask(const int *pins, size_t count);
int indexOfArray(const int *arr, size_t count, int value);
void handleButtonState(size_t idx, bool pressed);
void recordButtonEvent(int idx);
void appendEventsToFile();
String getLocalTime();
String getLocalTimeString();
void PrintLocalTime();
void idleSleep();
bool mountSPIFFS();
int checkPowerLevel();

// fields

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;
const int outputPins[] = outputs;
const int inputPins[] = inputs;
constexpr size_t outputCount = sizeof(outputPins) / sizeof(outputPins[0]);
constexpr size_t inputCount  = sizeof(inputPins)  / sizeof(inputPins[0]);
String timezone = "CET-1CEST,M3.5.0,M10.5.0/3"; // Central European Time
unsigned long timeSinceLastPressMillis = 0;
const unsigned long idleMs = 120UL /*seconds*/ * 1000UL;

// instantiate diodes after diode class is defined
Diode diodes[outputCount];
std::vector<uint8_t> prevInputState; // will be resized/initialized in setup()

struct ButtonEvent { int index; String time; };
std::vector<ButtonEvent> eventLog;

void setup() {
  Serial.begin(115200);
  initWiFi();
  // prepare prevInputState so we can detect changes in loop()
  prevInputState.resize(inputCount);
  for (size_t i = 0; i < inputCount; ++i) {
    // ensure pin is input (AssignButton sets this too)
    pinMode(inputPins[i], INPUT);
    prevInputState[i] = digitalRead(inputPins[i]) ? 1 : 0;
  }
  
  for (size_t i = 0; i < outputCount; ++i) {
    setupPin(outputPins[i], OUTPUT, LOW);
    diodes[i].setPin(outputPins[i]);
  }
  
  uint64_t wakeupMask = pinsToMask(inputPins, inputCount);
  esp_sleep_enable_ext1_wakeup(wakeupMask, ESP_EXT1_WAKEUP_ANY_HIGH); // Enable wakeup on any input pin high
  print_wakeup_reason();
  timeSinceLastPressMillis = millis();
}

void loop() {
  // Poll input pins and detect state changes (press / release)
  for (size_t i = 0; i < inputCount; ++i) {
    uint8_t state = digitalRead(inputPins[i]) ? 1 : 0;
    if (state != prevInputState[i]) {
      prevInputState[i] = state;
      handleButtonState(i, state == 1);
    }
  }

  // Idle timeout => append events and deep sleep
  if (_ENVIROMENT_ == "PRODUCTION")
  {
    if ((millis() - timeSinceLastPressMillis) >= idleMs) {
      idleSleep();
    }
  } else if (_ENVIROMENT_ == "DEVELOPMENT")
  {
    if ((millis() - timeSinceLastPressMillis) >= 10UL /*seconds*/ * 1000UL) {
      idleSleep();
    }
  }
  

  

  // small sleep to avoid busy-looping; adjust to taste
  delay(20);
}

void setupPin(int pin, int mode, int value) {
  pinMode(pin, mode);
  digitalWrite(pin, value);
}

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT1:
      wakeupExt1Handler();
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      wakeupTimerHandler();
      break;
    default: 
    break;
  }
}

void wakeupExt1Handler() {
  uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
  int wakeup_pin = __builtin_ctzll(wakeup_pin_mask);
  int index = indexOfArray(inputPins, inputCount, wakeup_pin); // -1 if not found
  if (index == -1) {
    Serial.printf("Wakeup pin %d not in configured list\n", wakeup_pin);
    return;
  }

  // record the wake press
  recordButtonEvent(index);

  // turn diode on and let it auto-off after 500 ms
  diodes[index].high(true, 500);

  // refresh idle timer so device doesn't immediately go back to sleep
  timeSinceLastPressMillis = millis();
}

void wakeupTimerHandler() {
  // Mount SPIFFS and read event log
  if (!mountSPIFFS())
  {
    return;
  }
  
  String data = readFile(SPIFFS, "/events.json");

  // Converting data into JSON array and removing trailing commas
  data.trim();
  if (data.length() > 0 && data.charAt(data.length() - 1) == ',') {
    data = data.substring(0, data.length() - 1);
  }
  data = '[' + data + ']';
  

  int powerLevel = checkPowerLevel();

  // build final JSON object with power level and data array
  String JSON_object = "{\"power_level\":" + String(powerLevel) + ",\"events\":" + data + "}";

  Serial.println("Event log data:");
  Serial.println(JSON_object);

  // MQTT upload (TLS) using username/password
  bool success = false;

  if (JSON_object.length() > 2) { // probably not just []
    // Ensure WiFi connected
    if (WiFi.status() != WL_CONNECTED) {
      initWiFi();
    }
    WiFiClientSecure espClient;
    PubSubClient mqttClient(espClient);
    bool MQTTIsSetup = false;
    // Configure MQTT over TLS
    Serial.println("setting client to insecure");
    espClient.setInsecure();
    // espClient.setCACert(MQTT_CA_CERT);
    mqttClient.setServer(_MQTT_SERVER_, _MQTT_PORT_);

    while (!mqttClient.connected())
    {
      Serial.print("Connecting to MQTT over TLS...");

      String clientId = "ESP32-" + String(random(0xffff), HEX);

      if (mqttClient.connect(clientId.c_str(), _MQTT_USER_, _MQTT_PASS_))
      {
        Serial.println("connected");
      }
      else
      {
        Serial.print("failed, rc=");
        Serial.print(mqttClient.state());
        Serial.println(" - retrying in 5 seconds");
        delay(5000);
      }
    }
    mqttClient.setBufferSize(65535);
    success = mqttClient.publish(_MQTT_TOPIC_, JSON_object.c_str());
  }

  if (success)
  {
    // remove the events file
    int attempts = 0;
    int maxAttempts = MAX_DELETE_ATTEMPTS;
    while (attempts < maxAttempts && !deleteFile(SPIFFS, "/events.json")) {
      Serial.println("Retrying delete /events.json");
      attempts++;
      delay(200);
    }
    Serial.println("Event log cleared.");
  } else {
    Serial.println("Upload failed; keeping events on SPIFFS");
  }

  esp_deep_sleep_start();
}

void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(_WIFI_SSID_, _WIFI_PASSWORD_);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(1000);
  }
  Serial.println(WiFi.localIP());

  // Init and get the time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  getLocalTime();
}

String getLocalTime(){
  // SetTimezone();
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return String("");
  }
  String timeString = asctime(&timeinfo);
  Serial.println(timeString);
  return timeString;
}

// Return formatted local time as "YYYY-MM-DD HH:MM:SS" or empty String if unavailable
String getLocalTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return String(); // empty -> indicates failure
  }
  char buf[32];
  if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo) == 0) {
    return String();
  }
  return String(buf);
}

// Print local time to Serial (preserves existing call-sites)
void PrintLocalTime() {
  String s = getLocalTimeString();
  if (s.length()) {
    Serial.println(s);
  } else {
    Serial.println("Failed to obtain time");
  }
}

String readFile(fs::FS &fs, const char * path){
  File file = fs.open(path);
  if(!file || file.isDirectory()){
      Serial.println("- failed to open file for reading");
      return String();
  }
  Serial.println("Reading file");
  String content;
    while(file.available()){
      int c = file.read();
      if (c < 0) break;
      content += static_cast<char>(c);
    }
  file.close();
  return content;
}

bool writeFile(fs::FS &fs, const char * path, const String message){
  File file = fs.open(path, FILE_WRITE);
  if(!file){
      Serial.println("- failed to open file for writing");
      return false;
  }
  if(!file.print(message)){
      Serial.println("- write failed");
  }
  file.close();
  return true;
}

bool appendFile(fs::FS &fs, const char * path, const String message){
  if (message == "")
  {
    return true;
  }
  
  File file = fs.open(path, FILE_APPEND);
  if(!file){
      Serial.println("- failed to open file for appending");
      while (!writeFile(fs, path, message))
      
      return false;
  }
  if(!file.print(message)){
      Serial.println("- append failed");
  }
  file.close();
  return true;
}

bool deleteFile(fs::FS &fs, const char * path){
  if(!fs.remove(path)){
      Serial.println("- delete failed");
      return false;
  }
  return true;
}

uint64_t pinsToMask(const int *pins, size_t count) {
  uint64_t mask = 0;
  for (size_t i = 0; i < count; ++i) {
    mask |= (1ULL << pins[i]);
  }
  return mask;
}

int indexOfArray(const int *arr, size_t count, int value) {
  for (size_t i = 0; i < count; ++i) {
    if (arr[i] == value) return static_cast<int>(i);
  }
  return -1;
}

void handleButtonState(size_t idx, bool pressed) {
  if (idx >= inputCount) return;
  if (pressed) {
    // record the press and hold diode ON (no auto-off while pressed)
    recordButtonEvent(static_cast<int>(idx));
    timeSinceLastPressMillis = millis();
    diodes[idx].high(false); // keep ON while pressed
  } else {
    // released -> turn diode off immediately
    diodes[idx].low();
  }
}

void recordButtonEvent(int idx) {
  // Prefer wall-clock time string; fallback to ms-since-boot if not available
  String tstr = getLocalTimeString();
  if (tstr.length() == 0) {
    tstr = "Error getting time";
  }

  // avoid accidental duplicate events if the same index/time was just recorded
  if (!eventLog.empty()) {
    const ButtonEvent &last = eventLog.back();
    if (last.index == idx && last.time == tstr) {
      return;
    }
  }
  eventLog.push_back({ idx, tstr });
}

void appendEventsToFile() {
  if (!mountSPIFFS())
  {
    timeSinceLastPressMillis = millis();
    return;
  }
  
  // Build a compact JSON array for this session: [{"index":0,"time":"2025-11-19 12:34:56"},...]\n
  String json = "";
  for (size_t i = 0; i < eventLog.size(); ++i) {
    json += "{\"index\":";
    json += String(eventLog[i].index);
    json += ",\"time\":\"";
    // escape double quotes in time just in case (time format shouldn't have them)
    String safe = eventLog[i].time;
    safe.replace("\"", "\\\"");
    json += safe;
    json += "\"},";
  }

  // Append to file on SPIFFS (creates file if needed)
  while (!appendFile(SPIFFS, "/events.json", json))
  
  // clear in-memory log after writing
  eventLog.clear();
}

void idleSleep() {
  // append collected events to SPIFFS
  appendEventsToFile();
  delay(50); // let serial flush
  // calculate how long to sleep until 02:00 next day
  struct tm timeinfo;
  uint64_t sleepDurationUs;
  if (_ENVIROMENT_ == "PRODUCTION")
  {
    if (getLocalTime(&timeinfo)) {
      timeinfo.tm_hour = 2;
      timeinfo.tm_min = 0;
      timeinfo.tm_sec = 0;
      time_t target = mktime(&timeinfo);
      time_t now = time(NULL);
      if (target <= now) {
        // already past 2 AM today -> set to next day
        target += 24 * 3600;
      }
      time_t delta = target - now;
      sleepDurationUs = static_cast<uint64_t>(delta) * uS_TO_S_FACTOR;
    } else {
      Serial.println("Failed to get time; sleeping until awakened externally.");
      esp_deep_sleep_start();
    }
  } else if (_ENVIROMENT_ == "DEVELOPMENT")
  {
    sleepDurationUs = 10 * uS_TO_S_FACTOR; // TEMP: sleep 10 seconds for testing
  }
  esp_deep_sleep(sleepDurationUs);
}

bool mountSPIFFS() {
  int attempts = 0;
  int maxAttempts = MAX_SPIFFS_ATTEMPTS;
  while (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED) && attempts < maxAttempts) {
    Serial.println("Failed to mount SPIFFS. Retrying...");
    attempts++;
    delay(500);
  }
  if (attempts == maxAttempts) {
    Serial.println("Failed to mount SPIFFS after multiple attempts.");
    return false;
  }
  return true;
}

int checkPowerLevel() {
  // Placeholder for power level checking logic
  return 0;
}