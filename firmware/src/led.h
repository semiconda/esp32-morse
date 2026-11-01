// esp32-morse — the WS2812 on GPIO 48: the transmitter, and the thing that gets filmed.
//
// Hardware: GPIO 48 drives the data line of the WS2812 already fitted to the dev board — a
// few milliamps on the pin. The LED's own current comes from the 5 V rail, at most ~60 mA
// at full white. Nothing else is connected (SPEC.md section 10).
//
// The LED has exactly one owner at a time (SPEC.md section 9.1). A show() is a timed
// bit-stream, and two tasks driving it would corrupt it.
//   * Idle: the main loop owns it — the 2-second preview and its timeout.
//   * Sending: the transmitter task owns it, exclusively.
// The functions below say which side may call them.

#pragma once

#include <cstdint>

namespace led {

struct Setting {
  uint8_t r, g, b;
  uint8_t brightness;   // percent, 0..100
};

// The filming preset (SPEC.md section 9), and what the board boots with: one green die,
// which phone sensors see best, at 20% so it does not bloom on a 240 fps recording.
constexpr Setting kFilmingPreset = {0, 255, 0, 20};

// How long a change on the panel lights the LED before it returns to black (section 9.1).
constexpr uint32_t kPreviewMs = 2000;

// Main task, once, before anything else. Leaves the LED black.
void begin();

// Main task. The current panel setting, as reported in `state`.
Setting setting();

// Main task, idle only. Stores the setting and lights the LED with it for kPreviewMs.
void set(const Setting& s);

// Main task, every loop(). Ends the preview when it is due.
void tick(uint32_t nowMs);

// Main task, on accepting a send. In this order: cancel any preview, force black, mark the
// transmitter as owner. The colour mark() lights is fixed here, already scaled by
// brightness, because `led` is rejected for as long as a transmission runs.
void handToTransmitter();

// Transmitter task only, while it owns the LED: lit with the panel colour, or black.
void mark(bool on);

// Transmitter task, at the end of a transmission, before the main loop is told: forces
// black and gives ownership back to the main loop.
void giveBack();

}  // namespace led
