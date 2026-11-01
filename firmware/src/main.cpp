// esp32-morse — the board makes its own network, serves the app, and talks to it over a
// WebSocket. T3 adds the LED and its panel; the transmitter arrives in T4.
//
// Two rules inherited from the class firmware, both load-bearing:
//   * The WebSocket is touched from the main task only. The async-TCP send queue is not
//     safe to feed from another task, and T4 adds a task.
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
#include "request.h"

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

// Every connection begins with this, so an app that has just connected — or reconnected
// after the phone slept — resynchronises without having to ask for anything.
static void fillState(JsonObject out) {
  out["type"] = "state";
  out["state"] = "idle";
  out["wpm"] = nullptr;
  const led::Setting s = led::setting();
  JsonObject l = out["led"].to<JsonObject>();
  l["r"] = s.r;
  l["g"] = s.g;
  l["b"] = s.b;
  l["brightness"] = s.brightness;
  out["dropped"] = 0;
  out["message"] = nullptr;
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
static void cmdLed(JsonDocument& req, JsonDocument& res) {
  request::Led v;
  if (const char* err = request::led(req.as<JsonObjectConst>(), v)) return fail(res, err);
  led::set({v.r, v.g, v.b, v.brightness});
  res["ok"] = true;
  broadcastState();
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

  if (strcmp(cmd, "led") == 0) {
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

static void onWsEvent(AsyncWebSocket* /*s*/, AsyncWebSocketClient* client, AwsEventType type,
                      void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    post(PendingKind::Connect, client->id());
  } else if (type == WS_EVT_DISCONNECT) {
    post(PendingKind::Disconnect, client->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    // Only complete, single-frame text messages. Without this test you get partial payloads
    // and deserializeJson fails intermittently on larger messages — the classic
    // ESPAsyncWebServer trap.
    if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)) {
      return;
    }
    if (len > kMaxFrameBytes) {
      Serial.printf("[ws] oversized frame from %u (%u bytes), dropped\n", client->id(),
                    (unsigned)len);
      return;
    }
    post(PendingKind::Frame, client->id(), String((const char*)data, len));
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
  Serial.begin(115200);
  delay(300);   // give USB-CDC a moment, or the first lines are lost
  Serial.printf("\n[boot] esp32-morse %s\n", FW_VERSION);

  led::begin();
  startWiFi();
  startServer();
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
    Serial.printf("[status] up %lus  AP \"%s\" on %s  stations=%u  ws=%u  heap=%u\n",
                  (unsigned long)(now / 1000), kApSsid,
                  WiFi.softAPIP().toString().c_str(),
                  WiFi.softAPgetStationNum(), (unsigned)clients.size(),
                  (unsigned)ESP.getFreeHeap());
  }
}
