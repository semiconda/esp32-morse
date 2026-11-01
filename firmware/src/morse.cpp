// esp32-morse — the Morse core. See morse.h for the contract; this file is the only place
// the character table and the unit counts live.

#include "morse.h"

namespace morse {

// ---------------------------------------------------------------------------------------
// Timing — SPEC.md section 2.1.
// ---------------------------------------------------------------------------------------

// Whole microseconds, integer division (section 2.2). The worst truncation across 5-40 WPM
// is 0.95 us per unit, at 38 WPM. No floating point anywhere in the timing.
uint32_t dotUs(uint16_t wpm) { return 1200000u / wpm; }

// The only place the dash's 3 appears.
uint32_t dashUs(uint16_t wpm) { return 3u * dotUs(wpm); }

// The only place the gaps' 1, 3 and 7 appear. Farnsworth timing is a change in here and
// nowhere else.
uint32_t gapUs(Gap kind, uint16_t wpm) {
  switch (kind) {
    case Gap::Element:   return 1u * dotUs(wpm);
    case Gap::Character: return 3u * dotUs(wpm);
    case Gap::Word:      return 7u * dotUs(wpm);
  }
  return 0;
}

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Dot:          return "dot";
    case Kind::Dash:         return "dash";
    case Kind::GapElement:   return "gap_el";
    case Kind::GapCharacter: return "gap_char";
    case Kind::GapWord:      return "gap_word";
  }
  return "";
}

bool isMark(Kind k) { return k == Kind::Dot || k == Kind::Dash; }

// Dispatches only. No ratio is written here, so the rule of section 2.1 holds by
// construction: every duration goes through dotUs, dashUs or gapUs.
uint32_t stepUs(Kind k, uint16_t wpm) {
  switch (k) {
    case Kind::Dot:          return dotUs(wpm);
    case Kind::Dash:         return dashUs(wpm);
    case Kind::GapElement:   return gapUs(Gap::Element, wpm);
    case Kind::GapCharacter: return gapUs(Gap::Character, wpm);
    case Kind::GapWord:      return gapUs(Gap::Word, wpm);
  }
  return 0;
}

// ---------------------------------------------------------------------------------------
// The table — SPEC.md section 3, in the same order, so it can be read line by line against
// the specification and against a published reference (ITU-R M.1677-1 for letters, digits
// and most punctuation; the remainder are the long-standing amateur conventions).
//
// The self-consistency tests catch only a typo that collides with another character. A code
// mistyped into an unused slot passes all of them, which is why this table is also read
// aloud against a reference before it is committed (PLAN.md, T2).
// ---------------------------------------------------------------------------------------

struct Entry {
  char ch;
  const char* code;
};

static const Entry kTable[] = {
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},    {'E', "."},
    {'F', "..-."},   {'G', "--."},    {'H', "...."},   {'I', ".."},     {'J', ".---"},
    {'K', "-.-"},    {'L', ".-.."},   {'M', "--"},     {'N', "-."},     {'O', "---"},
    {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},    {'S', "..."},    {'T', "-"},
    {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},   {'Y', "-.--"},
    {'Z', "--.."},

    {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},  {'4', "....-"},
    {'5', "....."},  {'6', "-...."},  {'7', "--..."},  {'8', "---.."},  {'9', "----."},

    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."}, {'!', "-.-.--"},
    {'/', "-..-."},  {'(', "-.--."},  {')', "-.--.-"}, {'&', ".-..."},  {':', "---..."},
    {';', "-.-.-."}, {'=', "-...-"},  {'+', ".-.-."},  {'-', "-....-"}, {'_', "..--.-"},
    {'"', ".-..-."}, {'$', "...-..-"}, {'@', ".--.-."},
};

const char* codeFor(char32_t c) {
  for (const Entry& e : kTable) {
    if (static_cast<char32_t>(static_cast<unsigned char>(e.ch)) == c) return e.code;
  }
  return nullptr;
}

const std::vector<char32_t>& supportedCharacters() {
  static const std::vector<char32_t> all = [] {
    std::vector<char32_t> v;
    for (const Entry& e : kTable) v.push_back(static_cast<unsigned char>(e.ch));
    return v;
  }();
  return all;
}

// ---------------------------------------------------------------------------------------
// UTF-8 — positions are code point indices (SPEC.md section 6.3, *Positions*), so the input
// has to be decoded, not indexed by byte.
// ---------------------------------------------------------------------------------------

static constexpr char32_t kReplacement = 0xFFFD;

// Decodes well-formed UTF-8 (the Unicode definition: no overlongs, no surrogates, nothing
// above U+10FFFF). Every byte that does not begin a well-formed sequence becomes one U+FFFD,
// and decoding resumes at the next byte. That keeps `norm` valid UTF-8 for the JSON reply,
// and a stray byte is still reported as unsupported rather than dropped (section 3.2).
static std::vector<char32_t> decode(const std::string& s) {
  std::vector<char32_t> out;
  out.reserve(s.size());
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
      out.push_back(b0);
      i += 1;
      continue;
    }

    size_t len = 0;
    char32_t cp = 0;
    unsigned char lo = 0x80, hi = 0xBF;   // allowed range of the second byte
    if (b0 >= 0xC2 && b0 <= 0xDF) {
      len = 2; cp = b0 & 0x1F;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
      len = 3; cp = b0 & 0x0F;
      if (b0 == 0xE0) lo = 0xA0;          // overlong
      if (b0 == 0xED) hi = 0x9F;          // surrogates
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
      len = 4; cp = b0 & 0x07;
      if (b0 == 0xF0) lo = 0x90;          // overlong
      if (b0 == 0xF4) hi = 0x8F;          // above U+10FFFF
    }
    // C0, C1 (always overlong), F5-FF and stray continuation bytes leave len at 0.

    bool good = len != 0 && i + len <= n;
    for (size_t k = 1; good && k < len; ++k) {
      const unsigned char b = static_cast<unsigned char>(s[i + k]);
      const unsigned char kLo = (k == 1) ? lo : 0x80;
      const unsigned char kHi = (k == 1) ? hi : 0xBF;
      if (b < kLo || b > kHi) good = false;
      else cp = (cp << 6) | (b & 0x3F);
    }

    if (good) {
      out.push_back(cp);
      i += len;
    } else {
      out.push_back(kReplacement);
      i += 1;
    }
  }
  return out;
}

static void encode(char32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// ---------------------------------------------------------------------------------------
// Checking a message — SPEC.md sections 3.1, 3.2, 6.3.
// ---------------------------------------------------------------------------------------

const char* checkErrorText(CheckError e) {
  switch (e) {
    case CheckError::None:        return "";
    case CheckError::Empty:       return "empty message";
    case CheckError::TooLong:     return "message too long";
    case CheckError::Unsupported: return "unsupported characters";
  }
  return "";
}

Checked check(const std::string& utf8) {
  Checked r;
  const std::vector<char32_t> in = decode(utf8);

  // Section 3.1, in its order. Only U+0020 counts as whitespace: a tab, a newline or the
  // U+00A0 that iOS inserts on its own is kept and reported, never silently trimmed.
  size_t first = 0, last = in.size();
  while (first < last && in[first] == U' ') ++first;
  while (last > first && in[last - 1] == U' ') --last;

  std::vector<char32_t> cps;
  cps.reserve(last - first);
  for (size_t i = first; i < last; ++i) {
    char32_t c = in[i];
    if (c == U' ' && !cps.empty() && cps.back() == U' ') continue;   // collapse
    if (c >= U'a' && c <= U'z') c = c - U'a' + U'A';                 // ASCII only
    cps.push_back(c);
  }

  r.length = cps.size();
  r.morse.reserve(cps.size());
  for (size_t i = 0; i < cps.size(); ++i) {
    const char32_t c = cps[i];
    encode(c, r.norm);
    if (c == U' ') {
      r.morse.emplace_back();
      continue;
    }
    const char* code = codeFor(c);
    if (code) {
      r.morse.emplace_back(code);
    } else {
      r.morse.emplace_back();
      // Positions are uint16_t in the contract. A message that long is rejected as TooLong
      // anyway, and the board never sees one (frames are capped at 4096 bytes), so beyond
      // the last representable index there is nothing useful left to report.
      if (i <= UINT16_MAX) r.bad.push_back(static_cast<uint16_t>(i));
    }
  }

  // Precedence: Empty, then TooLong, then Unsupported. `bad` stays filled under TooLong.
  if (cps.empty()) {
    r.error = CheckError::Empty;
  } else if (cps.size() > kMaxChars) {
    r.error = CheckError::TooLong;
  } else if (!r.bad.empty()) {
    r.error = CheckError::Unsupported;
  }

  if (r.ok()) r.units = unitsOf(translate(r));
  return r;
}

// ---------------------------------------------------------------------------------------
// Translation — SPEC.md sections 3.3 and 6.3, *What elem indices mean during a gap*.
//
// A gap between characters, or between words, belongs to the character that has just
// finished, with e = -1: attributing it to the next one would move the app's highlight a
// whole word gap early, 1.68 s at 5 WPM. A gap between elements carries the index of the
// element that has just finished. Nothing follows the last element (section 2.3).
// ---------------------------------------------------------------------------------------

std::vector<Step> translate(const Checked& msg) {
  std::vector<Step> steps;
  if (!msg.ok()) return steps;

  // When ok(), every character of `norm` is in the table or is a space, and all of those are
  // ASCII, so byte i of `norm` is code point i and `morse[i]` is its code.
  bool havePrev = false;
  uint16_t prevC = 0, prevW = 0;
  uint16_t w = 0;
  bool spaceSincePrev = false;

  for (size_t i = 0; i < msg.morse.size(); ++i) {
    if (msg.norm[i] == ' ') {
      ++w;
      spaceSincePrev = true;
      continue;
    }
    const uint16_t c = static_cast<uint16_t>(i);
    if (havePrev) {
      steps.push_back({spaceSincePrev ? Kind::GapWord : Kind::GapCharacter, prevW, prevC, -1});
    }
    const std::string& code = msg.morse[i];
    for (size_t j = 0; j < code.size(); ++j) {
      if (j > 0) steps.push_back({Kind::GapElement, w, c, static_cast<int8_t>(j - 1)});
      steps.push_back({code[j] == '-' ? Kind::Dash : Kind::Dot, w, c, static_cast<int8_t>(j)});
    }
    havePrev = true;
    prevC = c;
    prevW = w;
    spaceSincePrev = false;
  }
  return steps;
}

uint64_t durationUs(const std::vector<Step>& steps, uint16_t wpm) {
  uint64_t total = 0;
  for (const Step& s : steps) total += stepUs(s.kind, wpm);
  return total;
}

// Units through the timing functions, not through a second table of 1s, 3s and 7s: at
// kMinWpm the dot is 240000 us exactly, so the division is exact.
uint32_t unitsOf(const std::vector<Step>& steps) {
  return static_cast<uint32_t>(durationUs(steps, kMinWpm) / dotUs(kMinWpm));
}

}  // namespace morse
