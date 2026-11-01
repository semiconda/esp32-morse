// esp32-morse — the transmitter task. See transmitter.h.
//
// Element boundaries are absolute deadlines accumulated from the start of the transmission:
// t += stepUs(...), never "now + delay". An edge that lands late — because the async-TCP
// task at priority 10 preempted us, say — is late on its own and the next one is still
// scheduled from where it should have been. Error cannot accumulate, which is what the
// +-0.2% of SPEC.md section 7.1 rests on.

#include "transmitter.h"

#include <Arduino.h>
#include <esp_timer.h>

#include <atomic>

#include "led.h"

namespace tx {

namespace {

// SPEC.md section 11.1: waits hand the core back for the bulk of an interval and spin only
// for the last ~1.5 ms. That figure is in ticks of 1 ms; on a platform where the tick rate
// changes this is a broken assumption, so it fails the build rather than the timing.
static_assert(configTICK_RATE_HZ == 1000, "the transmitter's waits assume a 1 ms FreeRTOS tick");
constexpr int64_t kSpinUs = 1500;

constexpr UBaseType_t kPriority = 5;   // above loop() at 1; below async-TCP at 10
constexpr BaseType_t kCore = 1;        // WiFi and lwIP live on core 0
constexpr uint32_t kStackBytes = 4096;

TaskHandle_t task = nullptr;

// The job. Written by the main task before jobPending is released; read by the transmitter
// after acquiring it. Not touched by the main task again until poll() reports the end.
const std::vector<morse::Step>* jobSteps = nullptr;
uint16_t jobWpm = 0;
std::atomic<bool> jobPending{false};

std::atomic<bool> busy{false};
std::atomic<bool> abortRequested{false};

// The end, published by the transmitter with release after everything else it wrote.
std::atomic<uint8_t> endState{static_cast<uint8_t>(End::None)};
std::atomic<uint32_t> endElapsedMs{0};

// Single producer (the transmitter), single consumer (loop()). The producer writes only
// `head`, the consumer only `tail`. When full the NEWEST event is dropped and counted:
// overwriting the oldest would mean the producer moving `tail`, which is the consumer's,
// and that is a race between priority 5 and priority 1 (section 11.2).
Event ring[kRingSize];
std::atomic<uint32_t> head{0};
std::atomic<uint32_t> tail{0};
std::atomic<uint32_t> droppedCount{0};

void push(const Event& ev) {
  const uint32_t h = head.load(std::memory_order_relaxed);
  const uint32_t t = tail.load(std::memory_order_acquire);
  if (h - t >= kRingSize) {
    droppedCount.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ring[h % kRingSize] = ev;
  head.store(h + 1, std::memory_order_release);
}

// Waits until the absolute time `deadline` on the esp_timer clock. Returns false as soon as a
// stop has been requested — a word gap at 5 WPM is 1.68 s, and Stop must not wait for it.
bool waitUntil(int64_t deadline) {
  for (;;) {
    if (abortRequested.load(std::memory_order_acquire)) return false;
    const int64_t remaining = deadline - esp_timer_get_time();
    const int64_t sleepMs = (remaining - kSpinUs) / 1000;
    if (sleepMs <= 0) break;
    // A task blocked for n ticks wakes at most n ms later, so this never sleeps past
    // (deadline - kSpinUs). requestStop() notifies the task, which ends the sleep early.
    ulTaskNotifyTake(pdTRUE, (TickType_t)sleepMs);
  }
  while (esp_timer_get_time() < deadline) {
    if (abortRequested.load(std::memory_order_relaxed)) return false;
  }
  return true;
}

void transmit(const std::vector<morse::Step>& steps, uint16_t wpm) {
  // The main task forced the LED black when it handed it over. Hold it black for the
  // lead-in, so the first element is a dark-to-lit edge even if a preview was showing a
  // moment ago (section 9.1).
  int64_t deadline = esp_timer_get_time() + kLeadInUs;
  bool completed = waitUntil(deadline);

  int64_t firstEdgeUs = 0;
  if (completed) {
    for (size_t i = 0; i < steps.size(); ++i) {
      const morse::Step& s = steps[i];
      led::mark(morse::isMark(s.kind));
      if (i == 0) firstEdgeUs = esp_timer_get_time();
      push({s.kind, s.w, s.c, s.e});
      deadline += morse::stepUs(s.kind, wpm);
      if (!waitUntil(deadline)) {
        completed = false;
        break;
      }
    }
  }

  // The end of the last element: no trailing gap (section 2.3). Both edges are timed the
  // same way — just after show() returns — so its latency cancels out of the difference.
  led::mark(false);
  const int64_t lastEdgeUs = esp_timer_get_time();

  endElapsedMs.store(completed ? (uint32_t)((lastEdgeUs - firstEdgeUs + 500) / 1000) : 0,
                     std::memory_order_relaxed);
  // Ownership goes back before the main loop is told the transmission ended (section 9.1).
  led::giveBack();
  endState.store(static_cast<uint8_t>(completed ? End::Done : End::Stopped),
                 std::memory_order_release);
}

void run(void*) {
  for (;;) {
    // Asleep between transmissions; costs nothing when idle.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // A stop sent while idle also notifies; it is not a job.
    if (!jobPending.exchange(false, std::memory_order_acquire)) continue;
    transmit(*jobSteps, jobWpm);
  }
}

}  // namespace

void begin() {
  xTaskCreatePinnedToCore(run, "tx", kStackBytes, nullptr, kPriority, &task, kCore);
}

bool start(const std::vector<morse::Step>* steps, uint16_t wpm) {
  if (busy.load()) return false;
  busy.store(true);
  jobSteps = steps;
  jobWpm = wpm;
  abortRequested.store(false, std::memory_order_relaxed);
  droppedCount.store(0, std::memory_order_relaxed);
  jobPending.store(true, std::memory_order_release);
  xTaskNotifyGive(task);
  return true;
}

void requestStop() {
  if (!busy.load()) return;
  abortRequested.store(true, std::memory_order_release);
  xTaskNotifyGive(task);
}

bool running() { return busy.load(); }

bool pop(Event& out) {
  const uint32_t t = tail.load(std::memory_order_relaxed);
  const uint32_t h = head.load(std::memory_order_acquire);
  if (t == h) return false;
  out = ring[t % kRingSize];
  tail.store(t + 1, std::memory_order_release);
  return true;
}

End poll(uint32_t& elapsedMs) {
  const auto e = static_cast<End>(endState.load(std::memory_order_acquire));
  if (e == End::None) return End::None;
  endState.store(static_cast<uint8_t>(End::None), std::memory_order_relaxed);
  elapsedMs = endElapsedMs.load(std::memory_order_relaxed);
  busy.store(false);
  return e;
}

uint32_t dropped() { return droppedCount.load(std::memory_order_relaxed); }

}  // namespace tx
