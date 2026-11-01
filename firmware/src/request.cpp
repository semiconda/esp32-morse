// esp32-morse — validating the numeric fields of a request. See request.h.

#include "request.h"

#include <cmath>
#include <cstdio>

#include "morse.h"

namespace request {

// The reason is built with the field's name in it, so it needs storage that outlives this
// call. One static buffer: every caller runs on the main loop, uses the text at once, and
// ArduinoJson 7 copies a runtime `const char*` into the reply rather than keeping the
// pointer (only string literals are stored by reference). Valid until the next failing call.
static char reason[64];

static const char* rangeError(const char* name, long lo, long hi) {
  snprintf(reason, sizeof reason, "%s must be an integer from %ld to %ld", name, lo, hi);
  return reason;
}

const char* intField(JsonVariantConst v, const char* name, long lo, long hi, long& out) {
  // Missing and null are one case: either way there is no number to use.
  if (v.isNull()) {
    snprintf(reason, sizeof reason, "%s is missing", name);
    return reason;
  }

  // A boolean is not a number, even though some libraries will happily read true as 1.
  if (v.is<bool>()) return rangeError(name, lo, hi);

  // Stored as an integer that fits a long: compare as integers, no conversion involved.
  if (v.is<long>()) {
    const long x = v.as<long>();
    if (x < lo || x > hi) return rangeError(name, lo, hi);
    out = x;
    return nullptr;
  }

  // Any other number: JSON's 20.0 arrives here as a floating-point value, and so do
  // integers too large for a long. Range-check the double BEFORE converting it, because
  // casting 1e300 to long is undefined behaviour; the comparison is written so that NaN
  // fails it. -0.0 is inside [0, hi] and integral, so it is read as 0, which is what it is.
  if (v.is<double>()) {
    const double d = v.as<double>();
    if (!(d >= static_cast<double>(lo) && d <= static_cast<double>(hi))) {
      return rangeError(name, lo, hi);
    }
    if (std::floor(d) != d) return rangeError(name, lo, hi);   // 20.5
    out = static_cast<long>(d);
    return nullptr;
  }

  // Strings (even "20"), arrays, objects.
  return rangeError(name, lo, hi);
}

const char* wpm(JsonVariantConst v, uint16_t& out) {
  long x = 0;
  if (const char* err = intField(v, "wpm", morse::kMinWpm, morse::kMaxWpm, x)) return err;
  out = static_cast<uint16_t>(x);
  return nullptr;
}

// The first bad field is reported, in the order r, g, b, brightness, and `out` is written
// only when all four are good, so a rejected request leaves no half-applied colour.
const char* led(JsonObjectConst req, Led& out) {
  long r = 0, g = 0, b = 0, brightness = 0;
  if (const char* err = intField(req["r"], "r", 0, 255, r)) return err;
  if (const char* err = intField(req["g"], "g", 0, 255, g)) return err;
  if (const char* err = intField(req["b"], "b", 0, 255, b)) return err;
  if (const char* err = intField(req["brightness"], "brightness", 0, 100, brightness)) {
    return err;
  }
  out.r = static_cast<uint8_t>(r);
  out.g = static_cast<uint8_t>(g);
  out.b = static_cast<uint8_t>(b);
  out.brightness = static_cast<uint8_t>(brightness);
  return nullptr;
}

}  // namespace request
