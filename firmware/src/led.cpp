// esp32-morse — the WS2812 on GPIO 48. See led.h for the ownership rule.

#include "led.h"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include <atomic>

namespace led {

namespace {

// GPIO 48: the WS2812 on the dev board itself. Data line only. Not one of the pins an N16R8
// reserves (26-37 flash and octal PSRAM, 19/20 USB, 0/3/45/46 strapping, 43/44 UART0).
constexpr int kPin = 48;

Adafruit_NeoPixel pixel(1, kPin, NEO_GRB + NEO_KHZ800);

enum class Owner : uint8_t { Main, Transmitter };
std::atomic<Owner> owner{Owner::Main};

Setting current = kFilmingPreset;

bool previewing = false;
uint32_t previewStartMs = 0;

// The colour mark(true) lights, fixed at handToTransmitter().
uint8_t onR = 0, onG = 0, onB = 0;

// Brightness is applied here, linearly, rather than with setBrightness(): that one rescales
// the stored pixel in place and loses precision every time it is called.
uint8_t scale(uint8_t c, uint8_t percent) {
  return (uint8_t)(((uint32_t)c * percent + 50) / 100);
}

void show(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

}  // namespace

void begin() {
  pixel.begin();
  show(0, 0, 0);
}

Setting setting() { return current; }

void set(const Setting& s) {
  current = s;
  if (owner.load() != Owner::Main) return;   // never while the transmitter owns it
  show(scale(s.r, s.brightness), scale(s.g, s.brightness), scale(s.b, s.brightness));
  previewing = true;
  previewStartMs = millis();
}

void tick(uint32_t nowMs) {
  if (!previewing || owner.load() != Owner::Main) return;
  if (nowMs - previewStartMs >= kPreviewMs) {
    previewing = false;
    show(0, 0, 0);   // idle is black (SPEC.md section 9.1)
  }
}

void handToTransmitter() {
  previewing = false;
  show(0, 0, 0);
  onR = scale(current.r, current.brightness);
  onG = scale(current.g, current.brightness);
  onB = scale(current.b, current.brightness);
  owner.store(Owner::Transmitter);
}

void mark(bool on) {
  if (on) show(onR, onG, onB);
  else show(0, 0, 0);
}

void giveBack() {
  show(0, 0, 0);
  owner.store(Owner::Main);
}

}  // namespace led
