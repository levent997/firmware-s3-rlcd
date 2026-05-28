#include "protocol.h"
#include "ble_nus.h"
#include "state.h"
#include "persist.h"
#include "rtc.h"
#include "xfer.h"
#include "audio.h"
#include "token_window.h"
#include <ArduinoJson.h>

namespace {
// Sanitise an incoming transcript/message string. UTF-8 is PRESERVED (the UI
// now renders with a GB2312 CJK font via drawUTF8, so Chinese shows instead
// of being stripped). We only normalise the ASCII control range: tabs and
// other control bytes (<0x20) and DEL become spaces, and runs of spaces are
// collapsed. Multi-byte UTF-8 (>=0x80) passes through verbatim so it stays a
// valid sequence for drawUTF8.
String sanitizeText(const char *s) {
  String out;
  if (!s) return out;
  out.reserve(strlen(s));
  bool last_space = false;
  for (const uint8_t *p = (const uint8_t *)s; *p; ++p) {
    uint8_t c = *p;
    if (c >= 0x80) {              // UTF-8 byte — keep verbatim
      out += (char)c;
      last_space = false;
      continue;
    }
    char emit = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';  // control/tab/DEL -> space
    if (emit == ' ') {
      if (last_space) continue;
      last_space = true;
    } else {
      last_space = false;
    }
    out += emit;
  }
  while (out.length() && out[out.length() - 1] == ' ') out.remove(out.length() - 1);
  while (out.length() && out[0] == ' ') out.remove(0, 1);
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
  data["sec"] = g_state.secure;
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

// Rolling token window lives in the Arduino-free tokenwin::TokenWindow (see
// src/token_window.* and test/test_token). tokens_boot stays in g_state
// (NVS-persisted) and is advanced in place; the window keeps the history.
tokenwin::TokenWindow g_tokwin;

void updateTokenWindows(uint32_t tokens_now) {
  g_tokwin.update(tokens_now, millis(), g_state.tokens_boot);
  g_state.tokens_1h = g_tokwin.tokens_1h;
  g_state.tokens_5h = g_tokwin.tokens_5h;
}

void handleHeartbeat(JsonDocument &d) {
  g_state.last_heartbeat_ms = millis();
  g_state.total = d["total"] | 0;
  g_state.running = d["running"] | 0;
  g_state.waiting = d["waiting"] | 0;
  updateTokenWindows(d["tokens"] | 0);
  String new_msg = sanitizeText(d["msg"] | "");
  // Audible buzz on transitions INTO an error message (don't spam on
  // every heartbeat that repeats the same error).
  {
    String lc = new_msg; lc.toLowerCase();
    String prev_lc = g_state.msg; prev_lc.toLowerCase();
    bool now_err  = lc.indexOf("error") >= 0 || lc.indexOf("fail") >= 0;
    bool prev_err = prev_lc.indexOf("error") >= 0 || prev_lc.indexOf("fail") >= 0;
    if (now_err && !prev_err) audio::buzz();
  }
  g_state.msg = new_msg;
  g_state.tokens = d["tokens"] | 0;
  g_state.tokens_today = d["tokens_today"] | 0;

  for (int i = 0; i < 8; i++) g_state.entries[i] = "";
  JsonArray ents = d["entries"].as<JsonArray>();
  int i = 0;
  for (JsonVariant v : ents) {
    if (i >= 8) break;
    g_state.entries[i++] = sanitizeText(v | "");
  }

  JsonVariant pr = d["prompt"];
  if (!pr.isNull()) {
    String new_id = String((const char *)(pr["id"] | ""));
    // Reset the timer only when the prompt id changes — repeated heartbeats
    // carrying the same prompt shouldn't reset response timing.
    if (!g_state.prompt.active || g_state.prompt.id != new_id) {
      g_state.prompt_started_ms = millis();
    }
    g_state.prompt.active = true;
    g_state.prompt.id = new_id;
    g_state.prompt.tool = sanitizeText(pr["tool"] | "");
    g_state.prompt.hint = sanitizeText(pr["hint"] | "");
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
      // Persist the local wall-clock into the PCF85063 so the top-bar clock
      // stays correct across BLE drops and (with VBAT) power-cycles. If the
      // chip is absent or write fails, we keep going with the in-memory copy.
      rtc::setLocalEpoch((uint32_t)g_state.time_epoch +
                         (int32_t)g_state.time_offset_sec);
    }
  }

  // commands
  const char *cmd = d["cmd"] | (const char *)nullptr;
  if (cmd) {
    if (!strcmp(cmd, "status"))      { sendStatusAck();          return; }
    if (!strcmp(cmd, "name"))        {
      g_state.name = String((const char *)(d["name"] | ""));
      persist::onPetNameChanged();
      sendAck("name", true);
      return;
    }
    if (!strcmp(cmd, "owner"))       {
      g_state.owner = String((const char *)(d["name"] | ""));
      persist::onOwnerNameChanged();
      sendAck("owner", true);
      return;
    }
    if (!strcmp(cmd, "unpair"))      {
      ble_nus::clearBonds();
      persist::wipe();
      sendAck("unpair", true);
      return;
    }
    // Folder-push protocol (REFERENCE.md §folder). Delegate the 5-cmd
    // state machine to xfer. Each cmd gets its own per-step ack.
    if (!strcmp(cmd, "char_begin") ||
        !strcmp(cmd, "file")       ||
        !strcmp(cmd, "chunk")      ||
        !strcmp(cmd, "file_end")   ||
        !strcmp(cmd, "char_end")) {
      JsonDocument ack;
      if (xfer::handleCmd(cmd, d, ack)) {
        String s;
        serializeJson(ack, s);
        ble_nus::sendLine(s);
      }
      return;
    }
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

  // Audible confirmation. Different tones so the user knows which button
  // they actually pressed without looking at the screen.
  if (approve) audio::ding();
  else         audio::buzz();

  if (approve) g_state.approvals++;
  else         g_state.denies++;

  // Record response time into the velocity ring buffer (matches official
  // src/stats.h::statsOnApproval). Capped at uint16_t max.
  if (g_state.prompt_started_ms) {
    uint32_t secs = (millis() - g_state.prompt_started_ms) / 1000U;
    if (secs > 65535U) secs = 65535U;
    g_state.velocity[g_state.velocity_idx] = (uint16_t)secs;
    g_state.velocity_idx = (g_state.velocity_idx + 1) % 8;
    if (g_state.velocity_count < 8) g_state.velocity_count++;
  }
  g_state.prompt_started_ms = 0;
  g_state.prompt.active = false;

  persist::onApprovalOrDenial();
}
