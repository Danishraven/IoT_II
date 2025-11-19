#pragma once
#include <Arduino.h>
#include "esp_timer.h"

// Diode helper class: compact API with async auto-off via esp_timer
class Diode {
private:
  int pin = -1;
  unsigned long pressTime = 0;
  bool lastState = false;
  esp_timer_handle_t autoOffTimer = nullptr;
  unsigned long autoOffMs = 0;

  static void timer_cb(void* arg);
  void autoOffHandler(); // called from timer task
  void cancelTimer();

public:
  Diode() = default;
  explicit Diode(int p) { setPin(p); }
  ~Diode();

  // assign a pin and initialize it as OUTPUT (low)
  void setPin(int p);

  int getPin() const { return pin; }
  bool isValid() const { return pin >= 0; }

  // set press time (store absolute ms or custom value)
  void setPressTime(unsigned long t) { pressTime = t; }
  unsigned long getPressTime() const { return pressTime; }

  // set pin high and optionally auto-off after ms (default 1000)
  void high(bool autoOff = true, unsigned long ms = 1000);

  // set pin low and cancel any pending auto-off
  void low();

  // toggle and return new state (cancels existing auto-off)
  bool toggle();

  // read the state at any moment (reads pin)
  bool readState() const;

  // start a one-shot timer to auto-switch off after ms milliseconds
  void startAutoOff(unsigned long ms);
};