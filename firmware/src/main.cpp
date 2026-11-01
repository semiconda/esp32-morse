// esp32-morse — T1: the board boots, makes its own network, serves the app, and talks to
// it over a WebSocket. There is no Morse in this file yet and no pin is driven; the LED
// arrives in T3 and the transmitter in T4.
//
// The point of doing it in this order is that the plumbing is the part that fails in ways
// that are hard to attribute. Once the phone can open the app and the indicator goes green,
// everything after it is logic.
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

#include <mutex>
#include <vector>

#include "app_html.h"   // generated from app/index.html by scripts/embed_app.py

// ---------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------

// Open, by design: there is no secret in this project, the repository is public, and a
// WPA2 passphrase would have to live in it. Anyone in range can join and blink the LED.
// SPEC.md section 6.1.
static const char* kApSsid = "esp32-morse";

// The ESP32's default access-point limit, stated rather than left to be discovered when a
// fifth phone cannot join (SPEC.md section 4).
static const int kApMaxClients = 4;

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// ---------------------------------------------------------------------------------------
// Incoming messages
//
// They arrive on the async-TCP task. We queue them and handle them in loop(), so that all
// application code runs on one task. T4 adds a transmitter task that talks to this one
// through a ring buffer rather than through locks.
// ---------------------------------------------------------------------------------------

struct Pending {
  uint32_t client;
  String text;
};

static std::vector<Pending> pending;
static std::mutex pendingMutex;

// Bounded, so a client cannot make the board allocate without limit (SPEC.md section 6.3).
static const size_t kMaxPending = 32;
static const size_t kMaxFrameBytes = 4096;

static uint32_t lastTickMs = 0;
static const uint32_t kTickMs = 50;

// ---------------------------------------------------------------------------------------
// Outgoing
// ---------------------------------------------------------------------------------------

// Every connection begins with this, so an app that has just connected — or reconnected
// after the phone slept — resynchronises without having to ask for anything.
//
// In T1 there is nothing to be in: no speed, no message, no LED. The shape is here so the
// app can be written against it now and the fields filled in as they start existing.
static void fillState(JsonObject out) {
  out["type"] = "state";
  out["state"] = "idle";
  out["fw"] = FW_VERSION;
  out["clients"] = WiFi.softAPgetStationNum();
}

static void sendStateTo(AsyncWebSocketClient* client) {
  if (!client) return;
  JsonDocument doc;
  fillState(doc.to<JsonObject>());
  String out;
  serializeJson(doc, out);
  client->text(out);
}

static void broadcastState() {
  if (ws.count() == 0) return;   // nobody listening: do not serialise
  JsonDocument doc;
  fillState(doc.to<JsonObject>());
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

// A rejection is a reply to the client that asked, never a broadcast: one phone typing a
// bad character must not raise an error on every other phone in the room.
// SPEC.md section 6.3.
static void reply(uint32_t clientId, JsonDocument& doc) {
  AsyncWebSocketClient* c = ws.client(clientId);
  if (!c) return;
  String out;
  serializeJson(doc, out);
  c->text(out);
}

// ---------------------------------------------------------------------------------------
// Request handling — one request, exactly one reply, matched by the id the client chose.
// ---------------------------------------------------------------------------------------

static void handleRequest(uint32_t clientId, const String& text) {
  JsonDocument req;
  DeserializationError err = deserializeJson(req, text);
  if (err) {
    Serial.printf("[ws] bad JSON from %u: %s\n", clientId, err.c_str());
    return;   // malformed frames are dropped without a reply
  }

  const char* cmd = req["cmd"] | "";

  JsonDocument res;
  if (req["id"].is<const char*>() || req["id"].is<int>()) res["id"] = req["id"];

  // T1 knows no commands yet. Answering "unknown command" rather than silence is what makes
  // the round trip visible from the phone's console, and it is the same path that send,
  // stop, led and resend will take in T4.
  res["ok"] = false;
  res["error"] = String("unknown command: ") + cmd;

  reply(clientId, res);
}

// ---------------------------------------------------------------------------------------
// WebSocket events
// ---------------------------------------------------------------------------------------

static void onWsEvent(AsyncWebSocket* /*s*/, AsyncWebSocketClient* client, AwsEventType type,
                      void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[ws] client %u connected (%u total)\n", client->id(), ws.count());
    sendStateTo(client);
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[ws] client disconnected (%u left)\n", ws.count());
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
    std::lock_guard<std::mutex> lock(pendingMutex);
    if (pending.size() < kMaxPending) {
      pending.push_back({client->id(), String((const char*)data, len)});
    }
  }
}

// ---------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------

static void startWiFi() {
  WiFi.mode(WIFI_AP);
  // One argument for the passphrase would make it WPA2; NULL is an open network.
  WiFi.softAP(kApSsid, /*passphrase*/ nullptr, /*channel*/ 1, /*hidden*/ 0, kApMaxClients);
  Serial.printf("[wifi] open AP \"%s\"  http://%s/\n", kApSsid,
                WiFi.softAPIP().toString().c_str());
  Serial.println("[wifi] the network is open: anyone in range can join");
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
  Serial.begin(115200);
  delay(300);   // give USB-CDC a moment, or the first lines are lost
  Serial.printf("\n[boot] esp32-morse %s\n", FW_VERSION);

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
  for (const Pending& p : batch) handleRequest(p.client, p.text);

  const uint32_t now = millis();
  if (now - lastTickMs >= kTickMs) {
    lastTickMs = now;
    // Mandatory with ESPAsyncWebServer: without it, dead clients accumulate and eventually
    // exhaust AsyncTCP.
    ws.cleanupClients();
  }
}
