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

/* You only need to format SPIFFS the first time you run a
   test or else use the SPIFFS plugin to create a partition
   https://github.com/me-no-dev/arduino-esp32fs-plugin */
#define FORMAT_SPIFFS_IF_FAILED true

// Pin definitions
#define outputs {2, 4, 18, 19}
#define inputs {32, 35, 34, 39}

// Debounce delay in milliseconds
#define debounceDelay 50

// Deep sleep configuration
#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  36000        /* Time ESP32 will go to sleep (in seconds) */


// function declarations
void SetupPin(int pin, int mode, int value);
void TogglePins(const int *pins, size_t count);
void TogglePin(int pin);
void Print_wakeup_reason();
void InitWiFi();
void ListDir(fs::FS &fs, const char * dirname, uint8_t levels);
void ReadFile(fs::FS &fs, const char * path);
void WriteFile(fs::FS &fs, const char * path, const char * message);
void AppendFile(fs::FS &fs, const char * path, const char * message);
void RenameFile(fs::FS &fs, const char * path1, const char * path2);
void DeleteFile(fs::FS &fs, const char * path);
void WakeupExt1Handler();
void WakeupTimerHandler();
bool IsMounted();
uint64_t PinsToMask(const int *pins, size_t count);
int IndexOfArray(const int *arr, size_t count, int value);
void handleButtonState(size_t idx, bool pressed);
void recordButtonEvent(int idx);
void appendEventsToFile();
String getLocalTime();
String getLocalTimeString();
void PrintLocalTime();

// fields
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;
bool mountSuccess = false;
const int outputPins[] = outputs;
const int inputPins[] = inputs;
constexpr size_t outputCount = sizeof(outputPins) / sizeof(outputPins[0]);
constexpr size_t inputCount  = sizeof(inputPins)  / sizeof(inputPins[0]);
String timezone = "CET-1CEST,M3.5.0,M10.5.0/3"; // Central European Time
unsigned long timeSinceLastPressMillis = 0;
const unsigned long idleMs = 10UL /*seconds*/ * 1000UL;

// instantiate diodes after diode class is defined
Diode diodes[outputCount];
std::vector<uint8_t> prevInputState; // will be resized/initialized in setup()

struct ButtonEvent { int index; String time; };
std::vector<ButtonEvent> eventLog;

void setup() {
  Serial.begin(115200);
  InitWiFi();
  // prepare prevInputState so we can detect changes in loop()
  prevInputState.resize(inputCount);
  for (size_t i = 0; i < inputCount; ++i) {
    // ensure pin is input (AssignButton sets this too)
    pinMode(inputPins[i], INPUT);
    prevInputState[i] = digitalRead(inputPins[i]) ? 1 : 0;
  }
  
  for (size_t i = 0; i < outputCount; ++i) {
    SetupPin(outputPins[i], OUTPUT, LOW);
    diodes[i].setPin(outputPins[i]);
  }
  
  uint64_t wakeupMask = PinsToMask(inputPins, inputCount);
  esp_sleep_enable_ext1_wakeup(wakeupMask, ESP_EXT1_WAKEUP_ANY_HIGH); // Enable wakeup on any input pin high
  Print_wakeup_reason();
  //esp_deep_sleep(TIME_TO_SLEEP * uS_TO_S_FACTOR); // Sleep for defined time
  //esp_deep_sleep_start();
  timeSinceLastPressMillis = millis();

  Serial.println("Setup complete.");
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
  if ((millis() - timeSinceLastPressMillis) >= idleMs) {
    Serial.println("Idle timeout reached (2 min). Saving events and entering deep sleep...");
    // append collected events to SPIFFS
    if(!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED)){
    mountSuccess = false;
    Serial.println("SPIFFS Mount Failed");
    } else {
      mountSuccess = true;
    }
    ReadFile(SPIFFS, "/events.json");
    appendEventsToFile();
    delay(50); // let serial flush
    // deep sleep until external wake (EXT1 wake mask already set in setup)
    esp_deep_sleep_start();
    // execution won't continue here until wake
  }

  // small sleep to avoid busy-looping; adjust to taste
  delay(20);
}

void SetupPin(int pin, int mode, int value) {
  pinMode(pin, mode);
  digitalWrite(pin, value);
}

void Print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT1:
      WakeupExt1Handler();
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      WakeupTimerHandler();
      break;
    default: 
    Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason); 
    break;
  }
}

void WakeupExt1Handler() {
  uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
  int wakeup_pin = __builtin_ctzll(wakeup_pin_mask);
  int index = IndexOfArray(inputPins, inputCount, wakeup_pin); // -1 if not found
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

void WakeupTimerHandler() {
  TogglePins(outputPins, outputCount);
  delay(2000);
  TogglePins(outputPins, outputCount);
}

void TogglePins(const int *pins, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    TogglePin(pins[i]);
  }
}

void TogglePin(int pin) {
  int state = digitalRead(pin);
  digitalWrite(pin, !state);
}

void InitWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin("IoT_H3/4", "98806829");
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

void ListDir(fs::FS &fs, const char * dirname, uint8_t levels){
  if (!IsMounted()) return;

  File root = fs.open(dirname);
  if(!root){
    Serial.println("- failed to open directory");
    return;
  }
  if(!root.isDirectory()){
    Serial.println(" - not a directory");
    return;
  }

  File file = root.openNextFile();
  while(file){
    if(file.isDirectory()){
      if(levels){
          ListDir(fs, file.name(), levels -1);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

void ReadFile(fs::FS &fs, const char * path){
  if (!IsMounted()) return;
  File file = fs.open(path);
  if(!file || file.isDirectory()){
      Serial.println("- failed to open file for reading");
      return;
  }

  while(file.available()){
      Serial.write(file.read());
  }
  file.close();
}

void WriteFile(fs::FS &fs, const char * path, const char * message){
  if (!IsMounted()) return;

  File file = fs.open(path, FILE_WRITE);
  if(!file){
      Serial.println("- failed to open file for writing");
      return;
  }
  if(!file.print(message)){
      Serial.println("- write failed");
  }
  file.close();
}

void AppendFile(fs::FS &fs, const char * path, const char * message){
  if (!IsMounted()) return;

  File file = fs.open(path, FILE_APPEND);
  if(!file){
      Serial.println("- failed to open file for appending");
      WriteFile(fs, path, message);
      return;
  }
  if(!file.print(message)){
      Serial.println("- append failed");
  }
  file.close();
}

void RenameFile(fs::FS &fs, const char * path1, const char * path2){
  if (!IsMounted()) return;
  if (!fs.rename(path1, path2)) {
    
      Serial.println("- rename failed");
  }
}

void DeleteFile(fs::FS &fs, const char * path){
  if (!IsMounted()) return;
  if(!fs.remove(path)){
      Serial.println("- delete failed");
  }
}

bool IsMounted()
{
  if (!mountSuccess)
  {
    Serial.println("Filesystem not mounted");
  }
  return mountSuccess;
}

uint64_t PinsToMask(const int *pins, size_t count) {
  uint64_t mask = 0;
  for (size_t i = 0; i < count; ++i) {
    mask |= (1ULL << pins[i]);
  }
  return mask;
}

int IndexOfArray(const int *arr, size_t count, int value) {
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
  if (eventLog.empty()) return;
  if (!mountSuccess) {
    Serial.println("SPIFFS not mounted; cannot save events");
    return;
  }

  // Build a compact JSON array for this session: [{"index":0,"time":"2025-11-19 12:34:56"},...]\n
  String json = "[";
  for (size_t i = 0; i < eventLog.size(); ++i) {
    if (i) json += ",";
    json += "{\"index\":";
    json += String(eventLog[i].index);
    json += ",\"time\":\"";
    // escape double quotes in time just in case (time format shouldn't have them)
    String safe = eventLog[i].time;
    safe.replace("\"", "\\\"");
    json += safe;
    json += "\"}";
  }
  json += "]\n";

  // Append to file on SPIFFS (creates file if needed)
  AppendFile(SPIFFS, "/events.json", json.c_str());

  // clear in-memory log after writing
  eventLog.clear();
}