// esp32-morse — host-side tests of the Morse core and the request validation (SPEC.md
// section 7.5). Run on the laptop, no board involved:
//
//   pio test -e native -v
//
// One RUN_TEST per idea, named for what it pins, so the output reads as a list of the
// claims SPEC.md makes and whether each one holds.
//
// What these catch and what they do not (section 7.5): the self-consistency checks only
// catch a typo that collides with another character. The digit rule and the literal codes
// pin the places a typo is least visible; the rest of the table is checked by reading it
// against a published reference, which is a review step, not a test.

#include <ArduinoJson.h>
#include <unity.h>

#include <set>
#include <string>
#include <vector>

#include "morse.h"
#include "request.h"

using morse::Checked;
using morse::CheckError;
using morse::Gap;
using morse::Kind;
using morse::Step;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

// Counts and lengths. Unity's own size_t assertion is missing from older Unity releases, and
// every count here is small, so compare as uint32_t rather than depend on the version.
#define ASSERT_COUNT(expected, actual) \
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(expected), static_cast<uint32_t>(actual))

static uint32_t unitsOfText(const char* text) {
  const Checked m = morse::check(text);
  TEST_ASSERT_TRUE_MESSAGE(m.ok(), text);
  return morse::unitsOf(morse::translate(m));
}

static std::string repeatParis(int n) {
  std::string s;
  for (int i = 0; i < n; ++i) {
    if (i) s += ' ';
    s += "PARIS";
  }
  return s;
}

// The codes of a message, separated by single spaces: "SOS" -> "... --- ...".
static std::string codesOf(const char* text) {
  const Checked m = morse::check(text);
  TEST_ASSERT_TRUE_MESSAGE(m.ok(), text);
  std::string out;
  for (const std::string& code : m.morse) {
    if (code.empty()) continue;
    if (!out.empty()) out += ' ';
    out += code;
  }
  return out;
}

static std::string codeOf(char32_t c) {
  const char* code = morse::codeFor(c);
  return code ? code : "(none)";
}

// Asserts that `text` is rejected as Unsupported with exactly one bad position, `pos`.
static void assertOneBadAt(const std::string& text, uint16_t pos) {
  const Checked m = morse::check(text);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CheckError::Unsupported), static_cast<int>(m.error));
  ASSERT_COUNT(1, m.bad.size());
  TEST_ASSERT_EQUAL_UINT16(pos, m.bad[0]);
  TEST_ASSERT_EQUAL_UINT32(0, m.units);
}

// wpm read the way the firmware reads it: real JSON text through ArduinoJson.
static const char* wpmFromJson(const char* json, uint16_t& out) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  TEST_ASSERT_FALSE_MESSAGE(err, json);
  return request::wpm(doc["wpm"], out);
}

static const char* ledFromJson(const char* json, request::Led& out) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  TEST_ASSERT_FALSE_MESSAGE(err, json);
  return request::led(doc.as<JsonObjectConst>(), out);
}

static void assertWpmRejected(const char* json, const char* expectedError) {
  uint16_t out = 99;
  const char* err = wpmFromJson(json, out);
  TEST_ASSERT_NOT_NULL_MESSAGE(err, json);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(expectedError, err, json);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(99, out, "out must be left alone on failure");
}

static void assertWpmAccepted(const char* json, uint16_t expected) {
  uint16_t out = 99;
  const char* err = wpmFromJson(json, out);
  TEST_ASSERT_NULL_MESSAGE(err, err ? err : json);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(expected, out, json);
}

static const char* kWpmRange = "wpm must be an integer from 5 to 40";

// ---------------------------------------------------------------------------------------
// Timing — SPEC.md sections 2, 2.1, 2.2
// ---------------------------------------------------------------------------------------

void test_dot_at_20_wpm_is_60000_us() { TEST_ASSERT_EQUAL_UINT32(60000, morse::dotUs(20)); }

void test_dash_at_20_wpm_is_180000_us() { TEST_ASSERT_EQUAL_UINT32(180000, morse::dashUs(20)); }

void test_dot_at_5_wpm_is_240000_us_and_at_40_wpm_30000_us() {
  TEST_ASSERT_EQUAL_UINT32(240000, morse::dotUs(5));
  TEST_ASSERT_EQUAL_UINT32(30000, morse::dotUs(40));
  TEST_ASSERT_EQUAL_UINT32(120000, morse::dotUs(10));
}

// Section 2.2: the worst truncation in the range, 1200000 = 38 x 31578 + 36.
void test_dot_at_38_wpm_is_31578_us_truncated() {
  TEST_ASSERT_EQUAL_UINT32(31578, morse::dotUs(38));
}

void test_gap_us_is_1_3_7_dots() {
  for (uint16_t wpm = morse::kMinWpm; wpm <= morse::kMaxWpm; ++wpm) {
    const uint32_t dot = morse::dotUs(wpm);
    TEST_ASSERT_EQUAL_UINT32(1 * dot, morse::gapUs(Gap::Element, wpm));
    TEST_ASSERT_EQUAL_UINT32(3 * dot, morse::gapUs(Gap::Character, wpm));
    TEST_ASSERT_EQUAL_UINT32(7 * dot, morse::gapUs(Gap::Word, wpm));
    TEST_ASSERT_EQUAL_UINT32(3 * dot, morse::dashUs(wpm));
  }
  TEST_ASSERT_EQUAL_UINT32(60000, morse::gapUs(Gap::Element, 20));
  TEST_ASSERT_EQUAL_UINT32(180000, morse::gapUs(Gap::Character, 20));
  TEST_ASSERT_EQUAL_UINT32(420000, morse::gapUs(Gap::Word, 20));
}

void test_step_us_goes_through_the_timing_functions() {
  TEST_ASSERT_EQUAL_UINT32(morse::dotUs(13), morse::stepUs(Kind::Dot, 13));
  TEST_ASSERT_EQUAL_UINT32(morse::dashUs(13), morse::stepUs(Kind::Dash, 13));
  TEST_ASSERT_EQUAL_UINT32(morse::gapUs(Gap::Element, 13), morse::stepUs(Kind::GapElement, 13));
  TEST_ASSERT_EQUAL_UINT32(morse::gapUs(Gap::Character, 13),
                           morse::stepUs(Kind::GapCharacter, 13));
  TEST_ASSERT_EQUAL_UINT32(morse::gapUs(Gap::Word, 13), morse::stepUs(Kind::GapWord, 13));
}

void test_kind_names_match_the_protocol() {
  TEST_ASSERT_EQUAL_STRING("dot", morse::kindName(Kind::Dot));
  TEST_ASSERT_EQUAL_STRING("dash", morse::kindName(Kind::Dash));
  TEST_ASSERT_EQUAL_STRING("gap_el", morse::kindName(Kind::GapElement));
  TEST_ASSERT_EQUAL_STRING("gap_char", morse::kindName(Kind::GapCharacter));
  TEST_ASSERT_EQUAL_STRING("gap_word", morse::kindName(Kind::GapWord));
  TEST_ASSERT_TRUE(morse::isMark(Kind::Dot));
  TEST_ASSERT_TRUE(morse::isMark(Kind::Dash));
  TEST_ASSERT_FALSE(morse::isMark(Kind::GapElement));
  TEST_ASSERT_FALSE(morse::isMark(Kind::GapCharacter));
  TEST_ASSERT_FALSE(morse::isMark(Kind::GapWord));
}

// ---------------------------------------------------------------------------------------
// PARIS — SPEC.md sections 2, 2.3, 7.1
// ---------------------------------------------------------------------------------------

// PARIS on its own, with no trailing gap: 31 units of elements and element gaps, plus four
// character gaps.
void test_paris_alone_is_43_units() { TEST_ASSERT_EQUAL_UINT32(43, unitsOfText("PARIS")); }

// PARIS as the reference word: 50 units including its trailing word gap. Measured as what
// one more PARIS adds to a message, which is exactly the word plus the gap before it.
void test_paris_is_exactly_50_units() {
  TEST_ASSERT_EQUAL_UINT32(50, unitsOfText("PARIS PARIS") - unitsOfText("PARIS"));
}

// The trailing-gap rule, already got wrong once: 10 x 50 - 7, not 500.
void test_ten_paris_is_exactly_493_units() {
  TEST_ASSERT_EQUAL_UINT32(493, unitsOfText(repeatParis(10).c_str()));
  for (int n = 1; n <= 12; ++n) {
    TEST_ASSERT_EQUAL_UINT32(50 * n - 7, unitsOfText(repeatParis(n).c_str()));
  }
}

// The headline number: 493 x 120 ms = 59.16 s.
void test_ten_paris_at_10_wpm_lasts_59160000_us() {
  const auto steps = morse::translate(morse::check(repeatParis(10)));
  const uint64_t us = morse::durationUs(steps, 10);
  TEST_ASSERT_TRUE(us <= UINT32_MAX);
  TEST_ASSERT_EQUAL_UINT32(59160000, static_cast<uint32_t>(us));
}

void test_ten_paris_at_5_wpm_lasts_118320000_us() {
  const auto steps = morse::translate(morse::check(repeatParis(10)));
  const uint64_t us = morse::durationUs(steps, 5);
  TEST_ASSERT_TRUE(us <= UINT32_MAX);
  TEST_ASSERT_EQUAL_UINT32(118320000, static_cast<uint32_t>(us));
}

// No trailing gap after a single character.
void test_single_e_is_1_unit_and_t_is_3_units() {
  TEST_ASSERT_EQUAL_UINT32(1, unitsOfText("E"));
  TEST_ASSERT_EQUAL_UINT32(3, unitsOfText("T"));
}

void test_last_step_is_a_mark_no_trailing_gap() {
  const char* messages[] = {"E", "T", "PARIS", "PARIS PARIS", "SOS", "HELLO WORLD", "A B C"};
  for (const char* text : messages) {
    const auto steps = morse::translate(morse::check(text));
    TEST_ASSERT_FALSE_MESSAGE(steps.empty(), text);
    TEST_ASSERT_TRUE_MESSAGE(morse::isMark(steps.back().kind), text);
    TEST_ASSERT_TRUE_MESSAGE(morse::isMark(steps.front().kind), text);
  }
}

// Unit counts for a few messages, computed by hand from section 2.
void test_units_of_several_messages() {
  TEST_ASSERT_EQUAL_UINT32(27, unitsOfText("SOS"));   // 5 + 3 + 11 + 3 + 5
  TEST_ASSERT_EQUAL_UINT32(21, unitsOfText("A B"));   // 5 + 7 + 9
  TEST_ASSERT_EQUAL_UINT32(9, unitsOfText("E E"));    // 1 + 7 + 1
  TEST_ASSERT_EQUAL_UINT32(5, unitsOfText("EE"));     // 1 + 3 + 1
  TEST_ASSERT_EQUAL_UINT32(3, unitsOfText("I"));      // 1 + 1 + 1
  TEST_ASSERT_EQUAL_UINT32(5, unitsOfText("A"));      // 1 + 1 + 3
}

// Every duration is units x dot at every speed: nothing multiplies a dot behind the
// timing functions' back.
void test_duration_is_units_times_dot_at_every_speed() {
  const char* messages[] = {"PARIS", "SOS", "HELLO WORLD", "73 DE ESP32", "$@?"};
  for (const char* text : messages) {
    const auto steps = morse::translate(morse::check(text));
    const uint32_t units = morse::unitsOf(steps);
    for (uint16_t wpm = morse::kMinWpm; wpm <= morse::kMaxWpm; ++wpm) {
      const uint64_t expected = static_cast<uint64_t>(units) * morse::dotUs(wpm);
      TEST_ASSERT_TRUE_MESSAGE(morse::durationUs(steps, wpm) == expected, text);
    }
  }
}

// PARIS is 28 events in 50 units (section 11.2): 14 elements, 9 element gaps, 4 character
// gaps, and the word gap that joins it to the next PARIS.
void test_paris_is_28_events_with_its_word_gap() {
  const size_t one = morse::translate(morse::check("PARIS")).size();
  const size_t two = morse::translate(morse::check("PARIS PARIS")).size();
  ASSERT_COUNT(27, one);
  ASSERT_COUNT(28, two - one);
}

// ---------------------------------------------------------------------------------------
// The table — SPEC.md section 3
// ---------------------------------------------------------------------------------------

static const std::string kSpecCharacters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?'!/()&:;=+-_\"$@";

void test_table_has_exactly_54_characters() {
  ASSERT_COUNT(54, kSpecCharacters.size());
  ASSERT_COUNT(54, morse::supportedCharacters().size());
}

void test_every_character_of_the_spec_is_present() {
  for (char ch : kSpecCharacters) {
    const std::string msg = std::string("missing: ") + ch;
    TEST_ASSERT_NOT_NULL_MESSAGE(morse::codeFor(static_cast<unsigned char>(ch)), msg.c_str());
  }
  for (char32_t c : morse::supportedCharacters()) {
    TEST_ASSERT_TRUE(c < 0x80);
    TEST_ASSERT_TRUE(kSpecCharacters.find(static_cast<char>(c)) != std::string::npos);
  }
}

void test_space_lowercase_and_others_are_not_in_the_table() {
  TEST_ASSERT_NULL(morse::codeFor(U' '));
  TEST_ASSERT_NULL(morse::codeFor(U'a'));
  TEST_ASSERT_NULL(morse::codeFor(U'#'));
  TEST_ASSERT_NULL(morse::codeFor(U'%'));
  TEST_ASSERT_NULL(morse::codeFor(U'\t'));
  TEST_ASSERT_NULL(morse::codeFor(0x00A0));
  TEST_ASSERT_NULL(morse::codeFor(0x1F600));
  TEST_ASSERT_NULL(morse::codeFor(0));
}

void test_every_code_is_1_to_7_dots_and_dashes() {
  for (char32_t c : morse::supportedCharacters()) {
    const std::string code = codeOf(c);
    TEST_ASSERT_TRUE(code.size() >= 1 && code.size() <= 7);
    for (char e : code) TEST_ASSERT_TRUE(e == '.' || e == '-');
  }
}

void test_no_two_characters_share_a_code() {
  std::set<std::string> seen;
  for (char32_t c : morse::supportedCharacters()) {
    const std::string code = codeOf(c);
    TEST_ASSERT_TRUE_MESSAGE(seen.insert(code).second, code.c_str());
  }
  ASSERT_COUNT(54, seen.size());
}

// Asserted as the rule, not as ten literals, so a typo copied from the table cannot pass.
void test_digits_follow_their_rule() {
  for (int n = 1; n <= 10; ++n) {
    const char digit = static_cast<char>('0' + n % 10);
    std::string expected;
    if (n <= 5) {
      expected = std::string(n, '.') + std::string(5 - n, '-');
    } else {
      expected = std::string(n - 5, '-') + std::string(10 - n, '.');
    }
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), codeOf(static_cast<unsigned char>(digit)).c_str());
  }
}

// Independently known codes. The four-element letters are where a transposition is both
// easy to make and invisible.
void test_literal_codes_e_t_sos_q_y_f_l() {
  TEST_ASSERT_EQUAL_STRING(".", codeOf(U'E').c_str());
  TEST_ASSERT_EQUAL_STRING("-", codeOf(U'T').c_str());
  TEST_ASSERT_EQUAL_STRING("... --- ...", codesOf("SOS").c_str());
  TEST_ASSERT_EQUAL_STRING("--.-", codeOf(U'Q').c_str());
  TEST_ASSERT_EQUAL_STRING("-.--", codeOf(U'Y').c_str());
  TEST_ASSERT_EQUAL_STRING("..-.", codeOf(U'F').c_str());
  TEST_ASSERT_EQUAL_STRING(".-..", codeOf(U'L').c_str());
}

void test_every_supported_character_passes_check() {
  const Checked m = morse::check(kSpecCharacters);
  TEST_ASSERT_TRUE(m.ok());
  ASSERT_COUNT(54, m.length);
  TEST_ASSERT_EQUAL_STRING(kSpecCharacters.c_str(), m.norm.c_str());
}

// ---------------------------------------------------------------------------------------
// Normalisation — SPEC.md section 3.1
// ---------------------------------------------------------------------------------------

void test_leading_and_trailing_spaces_are_trimmed() {
  const Checked m = morse::check("   SOS  ");
  TEST_ASSERT_TRUE(m.ok());
  TEST_ASSERT_EQUAL_STRING("SOS", m.norm.c_str());
  ASSERT_COUNT(3, m.length);
}

void test_runs_of_spaces_collapse_to_one() {
  const Checked m = morse::check("A    B  C");
  TEST_ASSERT_TRUE(m.ok());
  TEST_ASSERT_EQUAL_STRING("A B C", m.norm.c_str());
  TEST_ASSERT_EQUAL_UINT32(unitsOfText("A B C"), m.units);
}

void test_lowercase_is_accepted_and_uppercased() {
  const Checked m = morse::check("  hello,   World! ");
  TEST_ASSERT_TRUE(m.ok());
  TEST_ASSERT_EQUAL_STRING("HELLO, WORLD!", m.norm.c_str());
}

// Only ASCII letters are uppercased: e-acute stays as it is, and is unsupported.
void test_only_ascii_letters_are_uppercased() {
  const Checked m = morse::check("caf\xC3\xA9");
  TEST_ASSERT_EQUAL_STRING("CAF\xC3\xA9", m.norm.c_str());
  assertOneBadAt("caf\xC3\xA9", 3);
}

// Each of these is an unsupported character, reported at its position: in the middle, at
// the start and at the end, so it is clear it was neither trimmed nor collapsed.
static void assertNotWhitespace(const std::string& x, const char* what) {
  assertOneBadAt("AB" + x + "C", 2);
  assertOneBadAt(x + "AB", 0);
  assertOneBadAt("AB " + x, 3);
  assertOneBadAt("  " + x + "  AB", 0);
  const Checked m = morse::check("  " + x + "  AB");
  TEST_ASSERT_EQUAL_STRING_MESSAGE((x + " AB").c_str(), m.norm.c_str(), what);
}

void test_tab_is_unsupported_not_whitespace() { assertNotWhitespace("\t", "tab"); }
void test_cr_is_unsupported_not_whitespace() { assertNotWhitespace("\r", "CR"); }
void test_lf_is_unsupported_not_whitespace() { assertNotWhitespace("\n", "LF"); }
void test_vt_is_unsupported_not_whitespace() { assertNotWhitespace("\v", "VT"); }
void test_nbsp_u00a0_is_unsupported_not_whitespace() {
  assertNotWhitespace("\xC2\xA0", "U+00A0");
}

void test_multi_line_paste_fails_visibly() {
  const Checked m = morse::check("HELLO\r\nWORLD");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CheckError::Unsupported), static_cast<int>(m.error));
  ASSERT_COUNT(2, m.bad.size());
  TEST_ASSERT_EQUAL_UINT16(5, m.bad[0]);
  TEST_ASSERT_EQUAL_UINT16(6, m.bad[1]);
}

// ---------------------------------------------------------------------------------------
// Rejection — SPEC.md sections 3.1, 3.2, 4
// ---------------------------------------------------------------------------------------

void test_empty_message_rejected() {
  const Checked m = morse::check("");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CheckError::Empty), static_cast<int>(m.error));
  TEST_ASSERT_FALSE(m.ok());
  TEST_ASSERT_EQUAL_UINT32(0, m.units);
  TEST_ASSERT_TRUE(morse::translate(m).empty());
}

void test_all_spaces_message_rejected_as_empty() {
  const Checked m = morse::check("       ");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CheckError::Empty), static_cast<int>(m.error));
  TEST_ASSERT_EQUAL_STRING("", m.norm.c_str());
  ASSERT_COUNT(0, m.length);
}

void test_255_characters_accepted() {
  const Checked m = morse::check(std::string(255, 'A'));
  TEST_ASSERT_TRUE(m.ok());
  ASSERT_COUNT(255, m.length);
  TEST_ASSERT_EQUAL_UINT32(255 * 5 + 254 * 3, m.units);
}

void test_256_characters_rejected_as_too_long() {
  const Checked m = morse::check(std::string(256, 'A'));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CheckError::TooLong), static_cast<int>(m.error));
  TEST_ASSERT_EQUAL_UINT32(0, m.units);
  TEST_ASSERT_TRUE(morse::translate(m).empty());
}

// The limit applies after normalisation.
void test_255_characters_with_surrounding_spaces_accepted() {
  const Checked m = morse::check("    " + std::string(255, 'A') + "   ");
  TEST_ASSERT_TRUE(m.ok());
  ASSERT_COUNT(255, m.length);
}

void test_runs_collapsed_before_the_length_is_counted() {
  std::string s;
  for (int i = 0; i < 128; ++i) s += "A     ";   // 128 x "A " = 255 after normalisation
  const Checked m = morse::check(s);
  TEST_ASSERT_TRUE(m.ok());
  ASSERT_COUNT(255, m.length);
}

// Counted in code points, not bytes: 254 letters and one 4-byte emoji are 255 code points
// (rejected for the emoji, not for length); 255 letters and the emoji are 256.
void test_length_is_counted_in_code_points() {
  const std::string emoji = "\xF0\x9F\x98\x80";
  const Checked a = morse::check(std::string(254, 'A') + emoji);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CheckError::Unsupported), static_cast<int>(a.error));
  ASSERT_COUNT(255, a.length);
  const Checked b = morse::check(std::string(255, 'A') + emoji);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CheckError::TooLong), static_cast<int>(b.error));
}

// Precedence Empty > TooLong > Unsupported, and `bad` is filled even when too long so the
// app can highlight either way.
void test_too_long_wins_over_unsupported_and_still_reports_bad() {
  const Checked m = morse::check("#" + std::string(300, 'A') + "#");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CheckError::TooLong), static_cast<int>(m.error));
  ASSERT_COUNT(2, m.bad.size());
  TEST_ASSERT_EQUAL_UINT16(0, m.bad[0]);
  TEST_ASSERT_EQUAL_UINT16(301, m.bad[1]);
  TEST_ASSERT_EQUAL_UINT32(0, m.units);
}

void test_unsupported_rejected_with_positions_ascending() {
  const Checked m = morse::check("A#B%C");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CheckError::Unsupported), static_cast<int>(m.error));
  ASSERT_COUNT(2, m.bad.size());
  TEST_ASSERT_EQUAL_UINT16(1, m.bad[0]);
  TEST_ASSERT_EQUAL_UINT16(3, m.bad[1]);
  TEST_ASSERT_EQUAL_UINT32(0, m.units);
  TEST_ASSERT_TRUE(morse::translate(m).empty());
}

void test_check_error_texts() {
  TEST_ASSERT_EQUAL_STRING("", morse::checkErrorText(CheckError::None));
  TEST_ASSERT_EQUAL_STRING("empty message", morse::checkErrorText(CheckError::Empty));
  TEST_ASSERT_EQUAL_STRING("message too long", morse::checkErrorText(CheckError::TooLong));
  TEST_ASSERT_EQUAL_STRING("unsupported characters",
                           morse::checkErrorText(CheckError::Unsupported));
}

// ---------------------------------------------------------------------------------------
// Positions in code points — SPEC.md section 6.3
// ---------------------------------------------------------------------------------------

// One emoji is 4 bytes, 2 UTF-16 units and 1 code point. The position is 2, not a byte
// offset, and there is one of it, not two.
void test_emoji_reports_one_bad_position_at_code_point_2() {
  const std::string text = "AB\xF0\x9F\x98\x80" "CD";
  assertOneBadAt(text, 2);
  const Checked m = morse::check(text);
  ASSERT_COUNT(5, m.length);
  ASSERT_COUNT(5, m.morse.size());
  TEST_ASSERT_EQUAL_STRING("", m.morse[2].c_str());
  TEST_ASSERT_EQUAL_STRING(text.c_str(), m.norm.c_str());
}

void test_invalid_utf8_byte_becomes_u_fffd_and_one_bad_position() {
  const Checked m = morse::check("A\xFF" "B");
  TEST_ASSERT_EQUAL_STRING("A\xEF\xBF\xBD" "B", m.norm.c_str());
  ASSERT_COUNT(3, m.length);
  assertOneBadAt("A\xFF" "B", 1);
}

// Each offending byte becomes its own U+FFFD: a truncated sequence, an overlong encoding,
// an encoded surrogate and a code point above U+10FFFF.
void test_malformed_utf8_sequences_become_one_u_fffd_per_byte() {
  const Checked truncated = morse::check("A\xE2\x82");
  ASSERT_COUNT(3, truncated.length);
  ASSERT_COUNT(2, truncated.bad.size());

  const Checked overlong = morse::check("\xC0\xAF" "A");
  ASSERT_COUNT(3, overlong.length);
  ASSERT_COUNT(2, overlong.bad.size());

  const Checked overlong3 = morse::check("\xE0\x80\xAF" "A");
  ASSERT_COUNT(4, overlong3.length);

  const Checked surrogate = morse::check("\xED\xA0\x80" "A");
  ASSERT_COUNT(4, surrogate.length);
  ASSERT_COUNT(3, surrogate.bad.size());

  const Checked tooHigh = morse::check("\xF4\x90\x80\x80" "A");
  ASSERT_COUNT(5, tooHigh.length);

  // A well-formed character resumes decoding normally after the bad bytes.
  const Checked resumes = morse::check("\xFF\xC3\xA9" "A");
  ASSERT_COUNT(3, resumes.length);
  TEST_ASSERT_EQUAL_STRING("\xEF\xBF\xBD\xC3\xA9" "A", resumes.norm.c_str());
}

void test_morse_has_one_entry_per_code_point_and_empty_for_space() {
  const Checked m = morse::check("SOS  e#");
  TEST_ASSERT_EQUAL_STRING("SOS E#", m.norm.c_str());
  ASSERT_COUNT(6, m.morse.size());
  TEST_ASSERT_EQUAL_STRING("...", m.morse[0].c_str());
  TEST_ASSERT_EQUAL_STRING("---", m.morse[1].c_str());
  TEST_ASSERT_EQUAL_STRING("...", m.morse[2].c_str());
  TEST_ASSERT_EQUAL_STRING("", m.morse[3].c_str());
  TEST_ASSERT_EQUAL_STRING(".", m.morse[4].c_str());
  TEST_ASSERT_EQUAL_STRING("", m.morse[5].c_str());   // unsupported
}

// ---------------------------------------------------------------------------------------
// The step sequence — SPEC.md section 6.3, *What elem indices mean during a gap*
// ---------------------------------------------------------------------------------------

static void assertStep(const Step& s, Kind kind, int w, int c, int e) {
  TEST_ASSERT_EQUAL_STRING(morse::kindName(kind), morse::kindName(s.kind));
  TEST_ASSERT_EQUAL_INT(w, s.w);
  TEST_ASSERT_EQUAL_INT(c, s.c);
  TEST_ASSERT_EQUAL_INT(e, s.e);
}

// "EE T": the character and word gaps belong to the character that has just finished,
// with e = -1; T is in word 1 at code point 3.
void test_step_sequence_of_ee_t() {
  const auto steps = morse::translate(morse::check("EE T"));
  ASSERT_COUNT(5, steps.size());
  assertStep(steps[0], Kind::Dot, 0, 0, 0);
  assertStep(steps[1], Kind::GapCharacter, 0, 0, -1);
  assertStep(steps[2], Kind::Dot, 0, 1, 0);
  assertStep(steps[3], Kind::GapWord, 0, 1, -1);
  assertStep(steps[4], Kind::Dash, 1, 3, 0);
}

// Inside a character, a gap carries the index of the element that has just finished.
void test_step_sequence_of_a_is_dot_gap_dash() {
  const auto steps = morse::translate(morse::check("A"));
  ASSERT_COUNT(3, steps.size());
  assertStep(steps[0], Kind::Dot, 0, 0, 0);
  assertStep(steps[1], Kind::GapElement, 0, 0, 0);
  assertStep(steps[2], Kind::Dash, 0, 0, 1);
}

// A 7-element code, the longest: e runs 0..6 and the gaps 0..5.
void test_step_sequence_of_dollar_has_seven_elements() {
  const auto steps = morse::translate(morse::check("$"));
  ASSERT_COUNT(13, steps.size());
  for (size_t i = 0; i < steps.size(); ++i) {
    if (i % 2 == 0) {
      TEST_ASSERT_TRUE(morse::isMark(steps[i].kind));
      TEST_ASSERT_EQUAL_INT(static_cast<int>(i / 2), steps[i].e);
    } else {
      TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::GapElement), static_cast<int>(steps[i].kind));
      TEST_ASSERT_EQUAL_INT(static_cast<int>(i / 2), steps[i].e);
    }
  }
}

void test_word_index_counts_words_after_normalisation() {
  const auto steps = morse::translate(morse::check("  E   E  E "));
  TEST_ASSERT_EQUAL_INT(0, steps.front().w);
  TEST_ASSERT_EQUAL_INT(2, steps.back().w);
  TEST_ASSERT_EQUAL_INT(4, steps.back().c);   // "E E E"
}

// ---------------------------------------------------------------------------------------
// Speed validation through real JSON — SPEC.md sections 2.4, 7.5
// ---------------------------------------------------------------------------------------

void test_wpm_zero_rejected() { assertWpmRejected("{\"wpm\":0}", kWpmRange); }
void test_wpm_4_rejected() { assertWpmRejected("{\"wpm\":4}", kWpmRange); }
void test_wpm_41_rejected() { assertWpmRejected("{\"wpm\":41}", kWpmRange); }
void test_wpm_negative_rejected() { assertWpmRejected("{\"wpm\":-20}", kWpmRange); }
void test_wpm_missing_rejected() { assertWpmRejected("{}", "wpm is missing"); }
void test_wpm_null_rejected() { assertWpmRejected("{\"wpm\":null}", "wpm is missing"); }
void test_wpm_string_20_rejected() { assertWpmRejected("{\"wpm\":\"20\"}", kWpmRange); }
void test_wpm_true_rejected() { assertWpmRejected("{\"wpm\":true}", kWpmRange); }
void test_wpm_20_point_5_rejected() { assertWpmRejected("{\"wpm\":20.5}", kWpmRange); }
void test_wpm_1e300_rejected() { assertWpmRejected("{\"wpm\":1e300}", kWpmRange); }
void test_wpm_huge_integer_rejected() {
  assertWpmRejected("{\"wpm\":18446744073709551616}", kWpmRange);
  assertWpmRejected("{\"wpm\":4294967316}", kWpmRange);   // 2^32 + 20
}
void test_wpm_array_and_object_rejected() {
  assertWpmRejected("{\"wpm\":[20]}", kWpmRange);
  assertWpmRejected("{\"wpm\":{\"v\":20}}", kWpmRange);
}
// -0.0 is integral and equal to zero, so it is read as 0 — and 0 is out of range.
void test_wpm_negative_zero_rejected_as_zero() { assertWpmRejected("{\"wpm\":-0.0}", kWpmRange); }

void test_wpm_20_point_0_accepted_as_20() { assertWpmAccepted("{\"wpm\":20.0}", 20); }
void test_wpm_5_and_40_accepted() {
  assertWpmAccepted("{\"wpm\":5}", 5);
  assertWpmAccepted("{\"wpm\":40}", 40);
  assertWpmAccepted("{\"wpm\":5.0}", 5);
  assertWpmAccepted("{\"wpm\":4e1}", 40);
}
void test_wpm_every_value_5_to_40_accepted() {
  for (int w = 5; w <= 40; ++w) {
    const std::string json = "{\"wpm\":" + std::to_string(w) + "}";
    assertWpmAccepted(json.c_str(), static_cast<uint16_t>(w));
  }
}

// ---------------------------------------------------------------------------------------
// LED fields through real JSON — SPEC.md section 9.1
// ---------------------------------------------------------------------------------------

static void assertLedRejected(const char* json, const char* expectedError) {
  request::Led out{1, 2, 3, 4};
  const char* err = ledFromJson(json, out);
  TEST_ASSERT_NOT_NULL_MESSAGE(err, json);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(expectedError, err, json);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, out.r, "out must be left alone on failure");
  TEST_ASSERT_EQUAL_UINT8(2, out.g);
  TEST_ASSERT_EQUAL_UINT8(3, out.b);
  TEST_ASSERT_EQUAL_UINT8(4, out.brightness);
}

void test_led_valid_accepted() {
  request::Led out{};
  const char* err = ledFromJson("{\"r\":255,\"g\":128,\"b\":0,\"brightness\":100}", out);
  TEST_ASSERT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(255, out.r);
  TEST_ASSERT_EQUAL_UINT8(128, out.g);
  TEST_ASSERT_EQUAL_UINT8(0, out.b);
  TEST_ASSERT_EQUAL_UINT8(100, out.brightness);

  err = ledFromJson("{\"r\":0,\"g\":0,\"b\":255.0,\"brightness\":-0.0}", out);
  TEST_ASSERT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(255, out.b);
  TEST_ASSERT_EQUAL_UINT8(0, out.brightness);
}

void test_led_brightness_500_rejected() {
  assertLedRejected("{\"r\":1,\"g\":1,\"b\":1,\"brightness\":500}",
                    "brightness must be an integer from 0 to 100");
}
void test_led_r_minus_1_rejected() {
  assertLedRejected("{\"r\":-1,\"g\":1,\"b\":1,\"brightness\":50}",
                    "r must be an integer from 0 to 255");
}
void test_led_b_string_rejected() {
  assertLedRejected("{\"r\":1,\"g\":1,\"b\":\"x\",\"brightness\":50}",
                    "b must be an integer from 0 to 255");
}
void test_led_missing_g_rejected() {
  assertLedRejected("{\"r\":1,\"b\":1,\"brightness\":50}", "g is missing");
}
void test_led_g_256_and_fraction_rejected() {
  assertLedRejected("{\"r\":1,\"g\":256,\"b\":1,\"brightness\":50}",
                    "g must be an integer from 0 to 255");
  assertLedRejected("{\"r\":1,\"g\":1,\"b\":1,\"brightness\":50.5}",
                    "brightness must be an integer from 0 to 100");
}
// The first bad field is reported, in the order r, g, b, brightness.
void test_led_reports_first_bad_field_in_order() {
  assertLedRejected("{\"r\":300,\"g\":1,\"b\":1,\"brightness\":500}",
                    "r must be an integer from 0 to 255");
  assertLedRejected("{\"brightness\":500,\"b\":\"x\",\"g\":1,\"r\":1}",
                    "b must be an integer from 0 to 255");
}

// ---------------------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();

  // Timing
  RUN_TEST(test_dot_at_20_wpm_is_60000_us);
  RUN_TEST(test_dash_at_20_wpm_is_180000_us);
  RUN_TEST(test_dot_at_5_wpm_is_240000_us_and_at_40_wpm_30000_us);
  RUN_TEST(test_dot_at_38_wpm_is_31578_us_truncated);
  RUN_TEST(test_gap_us_is_1_3_7_dots);
  RUN_TEST(test_step_us_goes_through_the_timing_functions);
  RUN_TEST(test_kind_names_match_the_protocol);

  // PARIS and unit counts
  RUN_TEST(test_paris_alone_is_43_units);
  RUN_TEST(test_paris_is_exactly_50_units);
  RUN_TEST(test_ten_paris_is_exactly_493_units);
  RUN_TEST(test_ten_paris_at_10_wpm_lasts_59160000_us);
  RUN_TEST(test_ten_paris_at_5_wpm_lasts_118320000_us);
  RUN_TEST(test_single_e_is_1_unit_and_t_is_3_units);
  RUN_TEST(test_last_step_is_a_mark_no_trailing_gap);
  RUN_TEST(test_units_of_several_messages);
  RUN_TEST(test_duration_is_units_times_dot_at_every_speed);
  RUN_TEST(test_paris_is_28_events_with_its_word_gap);

  // The table
  RUN_TEST(test_table_has_exactly_54_characters);
  RUN_TEST(test_every_character_of_the_spec_is_present);
  RUN_TEST(test_space_lowercase_and_others_are_not_in_the_table);
  RUN_TEST(test_every_code_is_1_to_7_dots_and_dashes);
  RUN_TEST(test_no_two_characters_share_a_code);
  RUN_TEST(test_digits_follow_their_rule);
  RUN_TEST(test_literal_codes_e_t_sos_q_y_f_l);
  RUN_TEST(test_every_supported_character_passes_check);

  // Normalisation
  RUN_TEST(test_leading_and_trailing_spaces_are_trimmed);
  RUN_TEST(test_runs_of_spaces_collapse_to_one);
  RUN_TEST(test_lowercase_is_accepted_and_uppercased);
  RUN_TEST(test_only_ascii_letters_are_uppercased);
  RUN_TEST(test_tab_is_unsupported_not_whitespace);
  RUN_TEST(test_cr_is_unsupported_not_whitespace);
  RUN_TEST(test_lf_is_unsupported_not_whitespace);
  RUN_TEST(test_vt_is_unsupported_not_whitespace);
  RUN_TEST(test_nbsp_u00a0_is_unsupported_not_whitespace);
  RUN_TEST(test_multi_line_paste_fails_visibly);

  // Rejection
  RUN_TEST(test_empty_message_rejected);
  RUN_TEST(test_all_spaces_message_rejected_as_empty);
  RUN_TEST(test_255_characters_accepted);
  RUN_TEST(test_256_characters_rejected_as_too_long);
  RUN_TEST(test_255_characters_with_surrounding_spaces_accepted);
  RUN_TEST(test_runs_collapsed_before_the_length_is_counted);
  RUN_TEST(test_length_is_counted_in_code_points);
  RUN_TEST(test_too_long_wins_over_unsupported_and_still_reports_bad);
  RUN_TEST(test_unsupported_rejected_with_positions_ascending);
  RUN_TEST(test_check_error_texts);

  // Positions
  RUN_TEST(test_emoji_reports_one_bad_position_at_code_point_2);
  RUN_TEST(test_invalid_utf8_byte_becomes_u_fffd_and_one_bad_position);
  RUN_TEST(test_malformed_utf8_sequences_become_one_u_fffd_per_byte);
  RUN_TEST(test_morse_has_one_entry_per_code_point_and_empty_for_space);

  // Steps
  RUN_TEST(test_step_sequence_of_ee_t);
  RUN_TEST(test_step_sequence_of_a_is_dot_gap_dash);
  RUN_TEST(test_step_sequence_of_dollar_has_seven_elements);
  RUN_TEST(test_word_index_counts_words_after_normalisation);

  // Speed
  RUN_TEST(test_wpm_zero_rejected);
  RUN_TEST(test_wpm_4_rejected);
  RUN_TEST(test_wpm_41_rejected);
  RUN_TEST(test_wpm_negative_rejected);
  RUN_TEST(test_wpm_missing_rejected);
  RUN_TEST(test_wpm_null_rejected);
  RUN_TEST(test_wpm_string_20_rejected);
  RUN_TEST(test_wpm_true_rejected);
  RUN_TEST(test_wpm_20_point_5_rejected);
  RUN_TEST(test_wpm_1e300_rejected);
  RUN_TEST(test_wpm_huge_integer_rejected);
  RUN_TEST(test_wpm_array_and_object_rejected);
  RUN_TEST(test_wpm_negative_zero_rejected_as_zero);
  RUN_TEST(test_wpm_20_point_0_accepted_as_20);
  RUN_TEST(test_wpm_5_and_40_accepted);
  RUN_TEST(test_wpm_every_value_5_to_40_accepted);

  // LED
  RUN_TEST(test_led_valid_accepted);
  RUN_TEST(test_led_brightness_500_rejected);
  RUN_TEST(test_led_r_minus_1_rejected);
  RUN_TEST(test_led_b_string_rejected);
  RUN_TEST(test_led_missing_g_rejected);
  RUN_TEST(test_led_g_256_and_fraction_rejected);
  RUN_TEST(test_led_reports_first_bad_field_in_order);

  return UNITY_END();
}
