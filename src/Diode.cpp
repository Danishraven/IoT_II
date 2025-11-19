#include "Diode.h"

void Diode::timer_cb(void* arg) {
  Diode* d = static_cast<Diode*>(arg);
  if (d) d->autoOffHandler();
}

void Diode::autoOffHandler() {
  if (isValid()) {
    digitalWrite(pin, LOW);
    lastState = false;
  }
  if (autoOffTimer) {
    esp_timer_stop(autoOffTimer);
    esp_timer_delete(autoOffTimer);
    autoOffTimer = nullptr;
  }
}

void Diode::cancelTimer() {
  if (autoOffTimer) {
    esp_timer_stop(autoOffTimer);
    esp_timer_delete(autoOffTimer);
    autoOffTimer = nullptr;
  }
}

Diode::~Diode() {
  cancelTimer();
}

void Diode::setPin(int p) {
  pin = p;
  if (pin >= 0) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    lastState = false;
  }
}

void Diode::high(bool autoOff, unsigned long ms) {
  if (!isValid()) return;
  digitalWrite(pin, HIGH);
  lastState = true;
  pressTime = millis();
  if (autoOff) startAutoOff(ms);
}

void Diode::low() {
  if (!isValid()) return;
  cancelTimer();
  digitalWrite(pin, LOW);
  lastState = false;
}

bool Diode::toggle() {
  if (!isValid()) return lastState;
  cancelTimer();
  int cur = digitalRead(pin);
  int next = (cur == HIGH) ? LOW : HIGH;
  digitalWrite(pin, next);
  lastState = (next == HIGH);
  if (lastState) {
    startAutoOff(1000);
    pressTime = millis();
  }
  return lastState;
}

bool Diode::readState() const {
  if (!isValid()) return false;
  return digitalRead(pin) == HIGH;
}

void Diode::startAutoOff(unsigned long ms) {
  if (!isValid()) return;
  cancelTimer();

  autoOffMs = ms;
  esp_timer_create_args_t args;
  args.callback = &Diode::timer_cb;
  args.arg = this;
  args.name = "diode-autooff";

  esp_err_t err = esp_timer_create(&args, &autoOffTimer);
  if (err == ESP_OK && autoOffTimer) {
    esp_timer_start_once(autoOffTimer, (uint64_t)ms * 1000ULL);
  } else {
    autoOffTimer = nullptr;
  }
}