// esp32-morse — the Morse core: the character table, normalisation, validation,
// translation to a sequence of steps, and timing.
//
// No Arduino headers, on purpose: this file and morse.cpp compile on the laptop, where
// `pio test -e native` checks them before any hardware is involved (SPEC.md section 7.5).
// A wrong character in the table is invisible on a blinking LED unless you read Morse.
//
// The firmware is the only holder of the table (SPEC.md section 5). The app has no copy;
// it asks the board, and renders what it is told.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace morse {

// SPEC.md section 2: 5 to 40 WPM inclusive.
constexpr uint16_t kMinWpm = 5;
constexpr uint16_t kMaxWpm = 40;

// SPEC.md section 3.1: 255 code points after normalisation, inclusive.
constexpr size_t kMaxChars = 255;

// ---------------------------------------------------------------------------------------
// Timing — SPEC.md section 2.1.
//
// All gap durations come from gapUs(). It is the only place the 1, 3 and 7 of the gaps
// appear, so Farnsworth timing is a change inside that one function. Nothing else may write
// `3 * dotUs(wpm)` or assume any ratio; that rule is enforced by review, not by a test.
//
// These functions require a validated speed (kMinWpm..kMaxWpm). They are never reached with
// an unvalidated one: the check happens once, when a send is accepted (section 2.4).
// ---------------------------------------------------------------------------------------

enum class Gap { Element, Character, Word };

uint32_t dotUs(uint16_t wpm);              // 1200000 / wpm, whole microseconds (section 2.2)
uint32_t dashUs(uint16_t wpm);
uint32_t gapUs(Gap kind, uint16_t wpm);

// ---------------------------------------------------------------------------------------
// Steps — what the transmitter emits, one per element or gap (SPEC.md section 11.2).
// ---------------------------------------------------------------------------------------

enum class Kind : uint8_t { Dot, Dash, GapElement, GapCharacter, GapWord };

// The protocol's name for a kind: "dot", "dash", "gap_el", "gap_char", "gap_word".
const char* kindName(Kind k);

// True for Dot and Dash: the LED is lit.
bool isMark(Kind k);

// The duration of one step, through dotUs / dashUs / gapUs and nothing else.
uint32_t stepUs(Kind k, uint16_t wpm);

struct Step {
  Kind kind;
  uint16_t w;   // word index
  uint16_t c;   // code point index into the normalised message
  int8_t e;     // element index within the character's code; -1 for GapCharacter, GapWord
};

// ---------------------------------------------------------------------------------------
// The table — SPEC.md section 3.
// ---------------------------------------------------------------------------------------

// The code for one character, as a string of '.' and '-', or nullptr if the character is
// not in the table. Only uppercase letters are in the table; lowercase is uppercased by
// normalise() before anything looks it up. Space is not in the table (it is a word gap).
const char* codeFor(char32_t c);

// Every character the table holds, in table order (54 of them). For the tests.
const std::vector<char32_t>& supportedCharacters();

// ---------------------------------------------------------------------------------------
// Checking a message — SPEC.md sections 3.1, 3.2, 3.3, 6.3.
// ---------------------------------------------------------------------------------------

enum class CheckError {
  None,
  Empty,          // nothing left after normalisation (section 4)
  TooLong,        // more than kMaxChars code points after normalisation
  Unsupported,    // one or more characters not in the table; positions in `bad`
};

// "empty message", "message too long", "unsupported characters", or "" for None.
const char* checkErrorText(CheckError e);

struct Checked {
  CheckError error = CheckError::None;
  bool ok() const { return error == CheckError::None; }

  // The normalised message (section 3.1), always valid UTF-8. An invalid UTF-8 byte in the
  // input becomes U+FFFD here and is reported as unsupported, so `norm` can always be sent
  // back in JSON.
  std::string norm;

  // The number of code points in `norm`.
  size_t length = 0;

  // Code point indices into `norm` of every unsupported character, ascending. Filled
  // whenever there are unsupported characters, even if the message is also too long, so the
  // app can highlight them either way.
  std::vector<uint16_t> bad;

  // One entry per code point of `norm`: its code, or "" for a space or an unsupported
  // character.
  std::vector<std::string> morse;

  // Total length in units, with no trailing gap (section 2.3). 0 unless ok().
  uint32_t units = 0;
};

// Normalise and validate. Never throws, never reads past the input, accepts any bytes.
//
// Normalisation, in this order (section 3.1):
//   1. leading and trailing U+0020 removed,
//   2. runs of U+0020 collapsed to one,
//   3. ASCII a-z uppercased.
// Only U+0020 is whitespace. Tab, CR, LF, VT and U+00A0 are unsupported characters.
// Precedence when several things are wrong: Empty, then TooLong, then Unsupported.
Checked check(const std::string& utf8);

// The step sequence of a message that passed check(). The last step is a mark: there is no
// trailing gap (section 2.3). Translation happens once, up front (section 3.3).
std::vector<Step> translate(const Checked& msg);

// The total duration in microseconds of a step sequence at a validated speed, as the sum
// of stepUs() over the steps.
uint64_t durationUs(const std::vector<Step>& steps, uint16_t wpm);

// The length in units of a step sequence, computed through the timing functions rather
// than through a second table of unit counts: durationUs at kMinWpm, whose dot divides
// exactly, divided by that dot.
uint32_t unitsOf(const std::vector<Step>& steps);

}  // namespace morse
