#include "protocol.h"
#include "ble_nus.h"
#include "state.h"
#include <ArduinoJson.h>

namespace {
// Strip non-ASCII so the Latin-1 u8g2 fonts don't produce mojibake.
// If more than 40% of the result would be '?', drop the whole string —
// no point showing "?Hardware Buddy ? `Claude-E658` ?" garbage.
String asciiOnly(const char *s) {
  String out;
  if (!s) return out;
  out.reserve(strlen(s));
  int qmarks = 0;
  bool last_q = false;
  for (const uint8_t *p = (const uint8_t *)s; *p; ++p) {
    if (*p >= 0x20 && *p < 0x7F) {
      out += (char)*p;
      last_q = false;
    } else if (*p >= 0x80) {
      if ((*p & 0xC0) == 0xC0) {
        if (!last_q) { out += '?'; qmarks++; last_q = true; }
      }
    } else if (*p == '\t' || *p == ' ') {
      out += ' ';
      last_q = false;
    }
  }
  if (out.length() == 0) return out;
  // Any non-ASCII replacement => the source had information we can't render,
  // so the line is effectively garbled. Drop it entirely.
  if (qmarks > 0) return String();
  return out;
}
void sendAck(const char *cmd, bool ok) {
  JsonDocument d;
  d["ack"] = cmd;
  d["ok"] = ok;
  d["n"] = 0;
  String s;
  serializeJson(d, s);
  ble_nus::sendLine(s);
}

void sendStatusAck() {
  JsonDocument d;
  d["ack"] = "status";
  d["ok"] = true;
  JsonObject data = d["data"].to<JsonObject>();
  data["name"] = g_state.name;
  data["sec"] = false;
  JsonObject sys = data["sys"].to<JsonObject>();
  sys["up"] = (uint32_t)(millis() / 1000);
  sys["heap"] = (uint32_t)ESP.getFreeHeap();
  JsonObject st = data["stats"].to<JsonObject>();
  st["appr"] = g_state.approvals;
  st["deny"] = g_state.denies;
  String s;
  serializeJson(d, s);
  ble_nus::sendLine(s);
}

// Rolling token window: keep 5 hours of samples at 1-minute granularity.
// Records cumulative tokens_today (or boot fallback) per sample.
constexpr int TOK_HISTORY = 300;       // 300 minutes = 5h
struct TokSample { uint32_t t_ms; uint32_t tokens_cum; };
TokSample tok_hist[TOK_HISTORY];
int tok_count = 0;
int tok_head = 0;
uint32_t last_tokens_seen = 0;
uint32_t last_sample_ms = 0;

void updateTokenWindows(uint32_t tokens_now) {
  uint32_t now = millis();

  // Detect resets (desktop restarted, tokens went down)
  if (tokens_now < last_tokens_seen) {
    last_tokens_seen = 0;
  }
  uint32_t delta = tokens_now - last_tokens_seen;
  if (delta > 0) {
    g_state.tokens_boot += delta;
  }
  last_tokens_seen = tokens_now;

  // Sample at most once per minute.
  if (last_sample_ms != 0 && now - last_sample_ms < 60000) {
    // recompute windows from existing samples plus current cum
  } else {
    last_sample_ms = now;
    tok_hist[tok_head] = { now, g_state.tokens_boot };
    tok_head = (tok_head + 1) % TOK_HISTORY;
    if (tok_count < TOK_HISTORY) tok_count++;
  }

  // Compute windows: find the oldest sample within 1h and 5h windows.
  auto cumAgo = [&](uint32_t window_ms) -> uint32_t {
    uint32_t cutoff = (now > window_ms) ? (now - window_ms) : 0;
    uint32_t cum_at_cutoff = g_state.tokens_boot;  // default to "no data" => 0 diff
    for (int i = 0; i < tok_count; i++) {
      int idx = (tok_head - 1 - i + TOK_HISTORY) % TOK_HISTORY;
      if (tok_hist[idx].t_ms <= cutoff) {
        cum_at_cutoff = tok_hist[idx].tokens_cum;
        break;
      }
      cum_at_cutoff = tok_hist[idx].tokens_cum;
    }
    return g_state.tokens_boot - cum_at_cutoff;
  };
  g_state.tokens_1h = cumAgo(60UL * 60UL * 1000UL);
  g_state.tokens_5h = cumAgo(5UL * 60UL * 60UL * 1000UL);
}

void handleHeartbeat(JsonDocument &d) {
  g_state.last_heartbeat_ms = millis();
  g_state.total = d["total"] | 0;
  g_state.running = d["running"] | 0;
  g_state.waiting = d["waiting"] | 0;
  updateTokenWindows(d["tokens"] | 0);
  g_state.msg = asciiOnly(d["msg"] | "");
  g_state.tokens = d["tokens"] | 0;
  g_state.tokens_today = d["tokens_today"] | 0;

  for (int i = 0; i < 3; i++) g_state.entries[i] = "";
  JsonArray ents = d["entries"].as<JsonArray>();
  int i = 0;
  for (JsonVariant v : ents) {
    if (i >= 3) break;
    g_state.entries[i++] = asciiOnly(v | "");
  }

  JsonVariant pr = d["prompt"];
  if (!pr.isNull()) {
    g_state.prompt.active = true;
    g_state.prompt.id = String((const char *)(pr["id"] | ""));
    g_state.prompt.tool = asciiOnly(pr["tool"] | "");
    g_state.prompt.hint = asciiOnly(pr["hint"] | "");
  } else {
    g_state.prompt.active = false;
    g_state.prompt.id = "";
    g_state.prompt.tool = "";
    g_state.prompt.hint = "";
  }
}
} // namespace

void protocol::handleLine(const String &line) {
  JsonDocument d;
  DeserializationError err = deserializeJson(d, line);
  if (err) return;

  // time sync
  if (d["time"].is<JsonArray>()) {
    JsonArray t = d["time"].as<JsonArray>();
    if (t.size() >= 2) {
      g_state.time_epoch = t[0] | 0;
      g_state.time_offset_sec = t[1] | 0;
      g_state.time_sync_ms = millis();
    }
  }

  // commands
  const char *cmd = d["cmd"] | (const char *)nullptr;
  if (cmd) {
    if (!strcmp(cmd, "status"))      { sendStatusAck();          return; }
    if (!strcmp(cmd, "name"))        { g_state.name  = String((const char *)(d["name"]  | "")); sendAck("name",   true); return; }
    if (!strcmp(cmd, "owner"))       { g_state.owner = String((const char *)(d["name"]  | "")); sendAck("owner",  true); return; }
    if (!strcmp(cmd, "unpair"))      { sendAck("unpair", true);  return; }
    if (!strcmp(cmd, "char_begin"))  { sendAck("char_begin", false); return; } // not accepting pushes
    // unknown cmd: silent
    return;
  }

  // heartbeat snapshot has total/running/waiting (no cmd field)
  if (d["total"].is<int>() || d["running"].is<int>() || d["waiting"].is<int>()) {
    handleHeartbeat(d);
    return;
  }
}

void protocol::sendPermission(const String &id, bool approve) {
  JsonDocument d;
  d["cmd"] = "permission";
  d["id"] = id;
  d["decision"] = approve ? "once" : "deny";
  String s;
  serializeJson(d, s);
  ble_nus::sendLine(s);
  if (approve) g_state.approvals++;
  else g_state.denies++;
}
