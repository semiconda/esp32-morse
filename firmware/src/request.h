// esp32-morse — validating the numeric fields of a request.
//
// Separate from main.cpp so it compiles on the laptop and the host tests can pin it against
// the real JSON library (SPEC.md section 7.5): whether `20.0` is 20 should be a decision
// written down here, not whatever ArduinoJson's integer coercion happens to do this release.
//
// Validation that exists only in the user interface is not validation (SPEC.md section
// 3.2). The app's slider cannot produce wpm = 0; that is not the point. wpm = 0 reaches
// 1200000 / wpm, and an integer division by zero on the Xtensa core is a reboot.

#pragma once

#include <ArduinoJson.h>
#include <cstdint>

namespace request {

// An integer field, from JSON. Accepts a JSON number with no fractional part — `20` and
// `20.0` are both 20 — within [lo, hi]. Rejects a missing field, null, a string (even "20"),
// a boolean, a fractional number, and anything out of range.
//
// On success returns nullptr and sets `out`. On failure returns a short reason naming the
// field, suitable for the reply's `error`, and leaves `out` alone.
const char* intField(JsonVariantConst v, const char* name, long lo, long hi, long& out);

// SPEC.md section 2.4: wpm is an integer 5..40.
const char* wpm(JsonVariantConst v, uint16_t& out);

// SPEC.md section 9.1: r, g, b are integers 0..255, brightness an integer 0..100.
struct Led {
  uint8_t r, g, b;
  uint8_t brightness;   // percent
};
const char* led(JsonObjectConst req, Led& out);

}  // namespace request
