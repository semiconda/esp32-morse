// esp32-morse — the transmitter: a FreeRTOS task that turns a step sequence into LED edges
// on absolute microsecond deadlines (SPEC.md section 11.1).
//
// It never touches the WebSocket (section 11.2). It reports what it is doing by pushing
// events into a single-producer/single-consumer ring that the main loop drains, and it
// reports the end of a transmission through a flag the main loop polls.

#pragma once

#include <cstdint>
#include <vector>

#include "morse.h"

namespace tx {

// One event per element or gap, as the transmitter starts it.
struct Event {
  morse::Kind kind;
  uint16_t w;
  uint16_t c;
  int8_t e;
};

// How a transmission ended, as seen by the main loop.
enum class End : uint8_t { None, Done, Stopped };

// Ring capacity: 64 events, ~3.4 s of headroom at 40 WPM (section 11.2).
constexpr uint32_t kRingSize = 64;

// The dark lead-in between accepting a send and the first element, so a transmission always
// begins with a dark-to-lit edge (section 9.1). Not part of elapsed_ms.
constexpr uint32_t kLeadInUs = 200000;

// Main task, once, from setup(): creates the task, priority 5, pinned to core 1.
void begin();

// Main task. Starts a transmission of `steps` at `wpm` (already validated). The caller has
// already handed the LED over (led::handToTransmitter). `steps` must stay alive and
// unchanged until the end is reported by poll(). Returns false if a transmission is still
// running or its end has not been collected yet.
bool start(const std::vector<morse::Step>* steps, uint16_t wpm);

// Main task. Asks the running transmission to stop; the LED goes dark within one element.
// The end is reported later, by poll(), once the task has acknowledged it. Does nothing if
// idle.
void requestStop();

// Main task. True from start() until poll() has returned the end.
bool running();

// Main task. Takes one event from the ring; false if it is empty.
bool pop(Event& out);

// Main task. If the transmission has ended, returns how, and — for Done — the board's own
// measured time from the start of the first element to the end of the last (section 7.4).
// Returns End::None while still running or when idle. Call it BEFORE draining the ring with
// pop(): every event pushed before the end is then guaranteed to be visible to the drain.
End poll(uint32_t& elapsedMs);

// Events dropped because the ring was full, in the current or most recent transmission.
// Expected to be zero; anything else is a defect (section 11.2).
uint32_t dropped();

}  // namespace tx
