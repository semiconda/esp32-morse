// esp32-morse — the board makes its own network, serves the app, and talks to it over a
// WebSocket; it checks messages, and hands accepted ones to the transmitter task, which
// blinks them on the LED.
//
// Two rules inherited from the class firmware, both load-bearing:
//   * The WebSocket is touched from the main task only. The async-TCP send queue is not
//     safe to feed from another task — the transmitter is one — so the transmitter talks to
//     loop() through a ring buffer and an end flag (transmitter.h), never through the socket.
//   * loop() never calls delay().

#include <Arduino.h>
#include <WiFi.h>

#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#include "app_html.h"   // generated from app/index.html by scripts/embed_app.py
#include "led.h"
#include "morse.h"
#include "request.h"
#include "transmitter.h"

// ---------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------

// SPEC.md section 6.1. WPA2, with a passphrase that is published rather than secret: it is
// in this file, in the README and in the public repository, so it protects nothing against
// anyone who reads them. What it does do is stop a passer-by joining by accident, and keep
// the phone from nagging about an open network.
//
// It is NOT a credential and nothing in this project is protected by it. A real secret would
// belong in include/secrets.h, which is git-ignored — this one is deliberately not there,
// because hiding it would imply it mattered.
static const char* kApSsid = "esp32-morse";
static const char* kApPassword = "instrument";   // 10 chars; WPA2 needs at least 8

// The ESP32's default access-point limit, stated rather than left to be discovered when a
// fifth phone cannot join (SPEC.md section 4).
static const int kApMaxClients = 4;

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// ---------------------------------------------------------------------------------------
// Incoming messages
//
// They arrive on the async-TCP task. We queue them and handle them in loop(), so that all
// application code runs on one task. Connects and disconnects go through the same queue, so
// the list of clients below is only ever read and written by loop().
// ---------------------------------------------------------------------------------------

enum class PendingKind : uint8_t { Connect, Disconnect, Frame };

struct Pending {
  PendingKind kind;
  uint32_t client;
  String text;
};

static std::vector<Pending> pending;
static std::mutex pendingMutex;

// Bounded, so a client cannot make the board allocate without limit (SPEC.md section 6.3).
static const size_t kMaxPending = 32;
static const size_t kMaxFrameBytes = 4096;

// The connected WebSocket clients, by id. loop() only.
static std::vector<uint32_t> clients;

// The serial command path: the laptop can drive the board over USB with the same JSON
// commands the app sends, through the same handler. Its replies are printed rather than
// sent. AsyncWebSocket numbers its clients from 1, so 0 is free.
static const uint32_t kSerialClient = 0;
static String serialLine;
static bool serialOverflow = false;

// The message in flight while sending, and the last accepted one while idle, as `state`
// reports it (SPEC.md section 6.3). Replaced only when a send is accepted, which cannot
// happen while one is running, so the transmitter's pointer into `steps` stays valid for as
// long as it is used.
struct Message {
  String text;               // as received
  morse::Checked checked;    // norm, morse, units
  std::vector<morse::Step> steps;
  uint16_t wpm = 0;          // the speed it is (or was last) sent at
};
static std::unique_ptr<Message> lastMessage;

// For the serial log only: how many events the transmitter produced for this run.
static uint32_t eventsThisRun = 0;

static uint32_t lastTickMs = 0;
static const uint32_t kTickMs = 50;

static uint32_t lastHeartbeatMs = 0;
static const uint32_t kHeartbeatMs = 10000;

// ---------------------------------------------------------------------------------------
// Outgoing
//
// SPEC.md section 6.3, "Outgoing frames": `state`, `done` and replies are never dropped. If
// one cannot be queued for a client, that client is closed, and its reconnect gets it a
// fresh `state`. A slow phone never slows anything else down.
// ---------------------------------------------------------------------------------------

static AsyncWebSocketSharedBuffer toBuffer(JsonDocument& doc) {
  auto buf = std::make_shared<std::vector<uint8_t>>(measureJson(doc));
  serializeJson(doc, buf->data(), buf->size());
  return buf;
}

static void sendOrClose(uint32_t id, const AsyncWebSocketSharedBuffer& buf) {
  if (!ws.text(id, buf)) {
    Serial.printf("[ws] client %u cannot take a frame that must not be lost; closing it\n", id);
    ws.close(id);
  }
}

static void broadcastCritical(JsonDocument& doc) {
  if (clients.empty()) return;   // nobody listening: do not serialise
  AsyncWebSocketSharedBuffer buf = toBuffer(doc);
  for (uint32_t id : clients) sendOrClose(id, buf);
}

// `elem` frames refused by a phone whose send queue was full — the per-client drop policy,
// not the transmitter's ring. Reported in the [status] line so a slow phone is visible.
static uint32_t elemRefused = 0;

// `elem` is a live view of something already happening: a stale one is worthless and a
// backlog of them is worse than nothing. So it is dropped for a phone that cannot take it,
// and only for that phone. Built with snprintf rather than a JsonDocument because at 40 WPM
// there are ~19 of these a second.
static void broadcastElem(const tx::Event& ev) {
  if (clients.empty()) return;
  char json[96];
  const int n = snprintf(json, sizeof json,
                         "{\"type\":\"elem\",\"w\":%u,\"c\":%u,\"e\":%d,\"kind\":\"%s\"}", ev.w,
                         ev.c, ev.e, morse::kindName(ev.kind));
  auto buf = std::make_shared<std::vector<uint8_t>>(json, json + n);
  for (uint32_t id : clients) {
    if (!ws.text(id, buf)) ++elemRefused;
  }
}

// Every connection begins with this, so an app that has just connected — or reconnected
// after the phone slept — resynchronises without having to ask for anything.
//
// It carries the message's `morse`, not just its text: a phone that joins mid-transmission,
// or did not type the message, otherwise cannot render the third level of the live display
// (SPEC.md section 6.3).
static void fillMorse(JsonArray out, const morse::Checked& c) {
  for (const std::string& code : c.morse) out.add(code.c_str());
}

static void fillState(JsonObject out) {
  out["type"] = "state";
  out["state"] = tx::running() ? "sending" : "idle";
  const led::Setting s = led::setting();
  JsonObject l = out["led"].to<JsonObject>();
  l["r"] = s.r;
  l["g"] = s.g;
  l["b"] = s.b;
  l["brightness"] = s.brightness;
  // In `state` as well as in `done`, because a run ended with Stop produces no `done`.
  out["dropped"] = tx::dropped();
  if (lastMessage) {
    out["wpm"] = lastMessage->wpm;
    JsonObject m = out["message"].to<JsonObject>();
    m["text"] = lastMessage->text;
    m["norm"] = lastMessage->checked.norm;
    fillMorse(m["morse"].to<JsonArray>(), lastMessage->checked);
    m["units"] = lastMessage->checked.units;
  } else {
    out["wpm"] = nullptr;
    out["message"] = nullptr;
  }
  out["fw"] = FW_VERSION;
  out["clients"] = WiFi.softAPgetStationNum();
}

static void sendStateTo(uint32_t id) {
  JsonDocument doc;
  fillState(doc.to<JsonObject>());
  sendOrClose(id, toBuffer(doc));
}

static void broadcastState() {
  JsonDocument doc;
  fillState(doc.to<JsonObject>());
  broadcastCritical(doc);
}

// A rejection is a reply to the client that asked, never a broadcast: one phone typing a
// bad character must not raise an error on every other phone in the room.
// SPEC.md section 6.3.
static void reply(uint32_t clientId, JsonDocument& doc) {
  if (clientId == kSerialClient) {
    String out;
    serializeJson(doc, out);
    Serial.printf("[reply] %s\n", out.c_str());
    return;
  }
  sendOrClose(clientId, toBuffer(doc));
}

// ---------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------

static void fail(JsonDocument& res, const char* error) {
  res["ok"] = false;
  res["error"] = error;
}

// SPEC.md section 9: colour and brightness, validated field by field, kept in RAM, previewed
// for 2 s, and reported in `state` so every phone shows what the LED is actually doing.
//
// Rejected during a transmission: the LED is the instrument being measured, and its
// settings may not change in the middle of a measurement (SPEC.md section 9.1).
static void cmdLed(JsonDocument& req, JsonDocument& res) {
  if (tx::running()) return fail(res, "busy: the LED cannot change during a transmission");
  request::Led v;
  if (const char* err = request::led(req.as<JsonObjectConst>(), v)) return fail(res, err);
  led::set({v.r, v.g, v.b, v.brightness});
  res["ok"] = true;
  broadcastState();
}

// The text field as bytes, embedded NULs and all: as<const char*>() would stop at the
// first one and quietly validate a shorter message than the one that was sent.
static bool textField(JsonDocument& req, std::string& out) {
  if (!req["text"].is<JsonString>()) return false;
  JsonString s = req["text"].as<JsonString>();
  out.assign(s.c_str(), s.size());
  return true;
}

static void fillBad(JsonDocument& res, const morse::Checked& c) {
  JsonArray bad = res["bad"].to<JsonArray>();
  for (uint16_t i : c.bad) bad.add(i);
}

// SPEC.md section 6.3: the translation and the positions of anything unsupported, in the
// board's normalised message, which the app renders as it is. No duration: that needs a
// speed, and the app can multiply units by 1200 / wpm itself.
static void cmdCheck(JsonDocument& req, JsonDocument& res) {
  std::string text;
  if (!textField(req, text)) return fail(res, "text is missing");
  const morse::Checked c = morse::check(text);
  res["ok"] = c.ok();
  if (!c.ok()) res["error"] = morse::checkErrorText(c.error);
  res["norm"] = c.norm;
  fillBad(res, c);
  fillMorse(res["morse"].to<JsonArray>(), c);
  res["units"] = c.units;
  res["length"] = c.length;
}

// Accepting a transmission. In this order (SPEC.md section 9.1):
// cancel the preview, force black, give the LED to the transmitter — then start it.
static void startTransmission(uint16_t wpm) {
  lastMessage->wpm = wpm;
  eventsThisRun = 0;
  led::handToTransmitter();
  tx::start(&lastMessage->steps, wpm);
  const uint64_t predictedUs = morse::durationUs(lastMessage->steps, wpm);
  Serial.printf("[tx] sending %u chars at %u WPM: %u units, %u steps, predicted %llu.%03llu s\n",
                (unsigned)lastMessage->checked.length, wpm, (unsigned)lastMessage->checked.units,
                (unsigned)lastMessage->steps.size(), predictedUs / 1000000,
                (predictedUs / 1000) % 1000);
  broadcastState();
}

// SPEC.md sections 3.2, 2.4, 4: everything is revalidated from scratch — the characters, the
// length, the speed — and no earlier `check` is trusted. A send while busy is rejected, not
// queued.
static void cmdSend(JsonDocument& req, JsonDocument& res) {
  if (tx::running()) return fail(res, "busy");
  std::string text;
  if (!textField(req, text)) return fail(res, "text is missing");
  morse::Checked c = morse::check(text);
  if (!c.ok()) {
    fail(res, morse::checkErrorText(c.error));
    fillBad(res, c);
    return;
  }
  uint16_t wpm;
  if (const char* err = request::wpm(req["wpm"], wpm)) return fail(res, err);

  // Translated once, up front, before anything is transmitted (section 3.3).
  std::unique_ptr<Message> m(new Message);
  m->text = String(text.c_str());
  m->steps = morse::translate(c);
  m->checked = std::move(c);
  lastMessage = std::move(m);
  startTransmission(wpm);
  res["ok"] = true;
}

// Always succeeds. `state: idle` follows only once the transmitter has acknowledged, never on
// receipt of this — otherwise pressing Send again straight away would be rejected as busy
// (section 6.3).
static void cmdStop(JsonDocument& /*req*/, JsonDocument& res) {
  tx::requestStop();
  res["ok"] = true;
}

// ---------------------------------------------------------------------------------------
// Request handling — one request, exactly one reply, matched by the id the client chose.
// ---------------------------------------------------------------------------------------

static void handleRequest(uint32_t clientId, const String& text) {
  JsonDocument req;
  DeserializationError err = deserializeJson(req, text);
  if (err || !req.is<JsonObject>()) {
    Serial.printf("[ws] bad JSON from %u: %s\n", clientId, err ? err.c_str() : "not an object");
    return;   // malformed frames are dropped without a reply
  }

  const char* cmd = req["cmd"] | "";

  JsonDocument res;
  if (req["id"].is<const char*>() || req["id"].is<int>()) res["id"] = req["id"];

  if (strcmp(cmd, "check") == 0) {
    cmdCheck(req, res);
  } else if (strcmp(cmd, "send") == 0) {
    cmdSend(req, res);
  } else if (strcmp(cmd, "stop") == 0) {
    cmdStop(req, res);
  } else if (strcmp(cmd, "led") == 0) {
    cmdLed(req, res);
  } else {
    res["ok"] = false;
    res["error"] = String("unknown command: ") + cmd;
  }

  reply(clientId, res);
}

// ---------------------------------------------------------------------------------------
// WebSocket events — on the async-TCP task. Nothing here sends; it all goes to loop().
// ---------------------------------------------------------------------------------------

static void post(PendingKind kind, uint32_t client, String text = String()) {
  std::lock_guard<std::mutex> lock(pendingMutex);
  // Connects and disconnects are never dropped: losing one would leave the client list
  // wrong. Only frames count against the bound.
  if (kind == PendingKind::Frame && pending.size() >= kMaxPending) return;
  pending.push_back({kind, client, std::move(text)});
}

// A frame larger than one TCP segment (lwIP's MSS here is 1436 bytes) reaches us in pieces,
// one WS_EVT_DATA per segment, each carrying the frame's total length and the offset of the
// piece. They are joined here, up to the 4096-byte limit. Treating every piece as a frame of
// its own would silently drop anything over ~1.4 KB: a pasted paragraph would get no reply
// at all rather than "message too long".
//
// All WebSocket events arrive on the one async-TCP task, so these buffers need no lock.
struct Partial {
  uint32_t client;
  String text;
};
static std::vector<Partial> partials;

static void forgetPartial(uint32_t client) {
  partials.erase(std::remove_if(partials.begin(), partials.end(),
                                [client](const Partial& p) { return p.client == client; }),
                 partials.end());
}

static void onWsEvent(AsyncWebSocket* /*s*/, AsyncWebSocketClient* client, AwsEventType type,
                      void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    post(PendingKind::Connect, client->id());
  } else if (type == WS_EVT_DISCONNECT) {
    forgetPartial(client->id());
    post(PendingKind::Disconnect, client->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    const uint32_t id = client->id();
    // Only text frames that are a whole message. A message split by the sender into several
    // WebSocket frames (final = false, then continuations) is not something the app sends.
    if (!info->final || info->opcode != WS_TEXT) return;
    if (info->len > kMaxFrameBytes) {
      if (info->index == 0) {
        Serial.printf("[ws] oversized frame from %u (%llu bytes), dropped\n", id,
                      (unsigned long long)info->len);
      }
      return;
    }
    if (info->index == 0 && len == info->len) {   // the common case: one piece
      post(PendingKind::Frame, id, String((const char*)data, len));
      return;
    }
    if (info->index == 0) {
      forgetPartial(id);
      partials.push_back({id, String()});
      partials.back().text.reserve(info->len);
    }
    auto it = std::find_if(partials.begin(), partials.end(),
                           [id](const Partial& p) { return p.client == id; });
    if (it == partials.end() || it->text.length() != info->index) {
      forgetPartial(id);   // a piece out of order: nothing sensible to join it to
      return;
    }
    it->text.concat((const char*)data, len);
    if (it->text.length() == info->len) {
      post(PendingKind::Frame, id, std::move(it->text));
      partials.erase(it);
    }
  }
}

// ---------------------------------------------------------------------------------------
// The serial command path — one JSON object per line, the same handler as the WebSocket.
// ---------------------------------------------------------------------------------------

static void pollSerial() {
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch < 0) break;
    if (ch == '\n' || ch == '\r') {
      if (!serialOverflow && serialLine.length() > 0) handleRequest(kSerialClient, serialLine);
      serialLine = "";
      serialOverflow = false;
    } else if (serialLine.length() < kMaxFrameBytes) {
      serialLine += (char)ch;
    } else {
      serialOverflow = true;   // oversized, like a WebSocket frame: dropped whole
    }
  }
}

// ---------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------

static void startWiFi() {
  WiFi.mode(WIFI_AP);
  // A passphrase shorter than 8 characters silently falls back to an open network on some
  // cores, which would be a quiet way to lose the setting. "instrument" is 10.
  WiFi.softAP(kApSsid, kApPassword, /*channel*/ 1, /*hidden*/ 0, kApMaxClients);
  Serial.printf("[wifi] AP \"%s\" password \"%s\"  http://%s/\n", kApSsid, kApPassword,
                WiFi.softAPIP().toString().c_str());
}

static void startServer() {
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res = req->beginResponse_P(200, "text/html", APP_HTML);
    // Without this a reflash is masked by the copy cached on the phone, and the symptom is
    // "my change did nothing".
    res->addHeader("Cache-Control", "no-cache");
    req->send(res);
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "not found");
  });

  server.begin();
  Serial.println("[http] server started");
}

void setup() {
  // With USB plugged in but no program reading the port, a write can wait for the host. The
  // default is 100 ms per write, which loop() cannot afford; 0 is not an option either, as
  // the core counts its retries down from this value. A line that does not fit is lost,
  // which is what a log with nobody reading it should do.
  Serial.setTxTimeoutMs(10);
  // The receive queue holds 256 bytes unless told otherwise, and a command line arrives in
  // one USB burst: a 256-character message is ~280 bytes and overflowed it, losing bytes
  // and corrupting the next line. Sized for the longest line the handler accepts. Must be
  // set before begin().
  Serial.setRxBufferSize(kMaxFrameBytes + 64);
  Serial.begin(115200);
  delay(300);   // give USB-CDC a moment, or the first lines are lost
  Serial.printf("\n[boot] esp32-morse %s\n", FW_VERSION);

  led::begin();
  tx::begin();
  startWiFi();
  startServer();
}

// What the transmitter has done since the last loop(). The end is read BEFORE the ring is
// drained: the transmitter pushes its last event and then publishes the end, so reading in
// this order guarantees every event of the run is drained before the end is acted on.
static void pollTransmitter() {
  uint32_t elapsedMs = 0;
  const tx::End end = tx::poll(elapsedMs);

  tx::Event ev;
  while (tx::pop(ev)) {
    ++eventsThisRun;
    broadcastElem(ev);
  }

  if (end == tx::End::Done) {
    Serial.printf("[done] elapsed_ms=%u dropped=%u events=%u/%u\n", elapsedMs,
                  tx::dropped(), eventsThisRun, (unsigned)lastMessage->steps.size());
    // `done` only ever describes a transmission that ran to completion: its elapsed_ms is
    // compared against a prediction, and a truncated one next to a prediction is a trap.
    JsonDocument doc;
    doc["type"] = "done";
    doc["elapsed_ms"] = elapsedMs;
    doc["dropped"] = tx::dropped();
    broadcastCritical(doc);
    broadcastState();
  } else if (end == tx::End::Stopped) {
    Serial.printf("[tx] stopped after %u of %u events\n", eventsThisRun,
                  (unsigned)lastMessage->steps.size());
    broadcastState();
  }
}

void loop() {
  // Drain the queue under the lock for microseconds only, then handle outside it.
  std::vector<Pending> batch;
  {
    std::lock_guard<std::mutex> lock(pendingMutex);
    batch.swap(pending);
  }
  for (const Pending& p : batch) {
    switch (p.kind) {
      case PendingKind::Connect:
        clients.push_back(p.client);
        Serial.printf("[ws] client %u connected (%u total)\n", p.client, (unsigned)clients.size());
        sendStateTo(p.client);
        break;
      case PendingKind::Disconnect:
        clients.erase(std::remove(clients.begin(), clients.end(), p.client), clients.end());
        Serial.printf("[ws] client %u disconnected (%u left)\n", p.client,
                      (unsigned)clients.size());
        break;
      case PendingKind::Frame:
        handleRequest(p.client, p.text);
        break;
    }
  }

  pollSerial();
  pollTransmitter();

  const uint32_t now = millis();
  led::tick(now);

  if (now - lastTickMs >= kTickMs) {
    lastTickMs = now;
    // Mandatory with ESPAsyncWebServer: without it, dead clients accumulate and eventually
    // exhaust AsyncTCP.
    ws.cleanupClients();
  }

  // A heartbeat, because a board that prints only at boot cannot be diagnosed: attach a
  // monitor a second too late and it looks identical to a board that is dead. Ten seconds
  // is often enough to answer "is the access point actually up?" without being noise.
  if (now - lastHeartbeatMs >= kHeartbeatMs) {
    lastHeartbeatMs = now;
    Serial.printf("[status] up %lus  AP \"%s\" on %s  stations=%u  ws=%u  tx=%s  "
                  "elem_refused=%u  heap=%u\n",
                  (unsigned long)(now / 1000), kApSsid,
                  WiFi.softAPIP().toString().c_str(),
                  WiFi.softAPgetStationNum(), (unsigned)clients.size(),
                  tx::running() ? "sending" : "idle", elemRefused,
                  (unsigned)ESP.getFreeHeap());
  }
}
