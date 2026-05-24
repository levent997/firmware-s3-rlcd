#include "protocol.h"
#include "ble_nus.h"
#include "state.h"
#include "persist.h"
#include "rtc.h"
#include "xfer.h"
#include <ArduinoJson.h>

namespace {
// Sanitise a string for Latin-1 fonts. Replaces UTF-8 multi-byte sequences
// with a single space (not '?', so the line stays readable) and collapses
// runs of whitespace. Keeps the line even if some characters were lost so
// the user still sees tool calls / timestamps embedded in mixed content.
//
// Mirrors the M5StickC reference firmware's behaviour, which never drops
// entries — it just renders whatever bytes it gets. We replace non-ASCII
// bytes with spaces (rather than rendering raw and getting mojibake)
// because our font is Latin-1 only.
String asciiOnly(const char *s) {
  String out;
  if (!s) return out;
  out.reserve(strlen(s));
  bool last_space = false;
  for (const uint8_t *p = (const uint8_t *)s; *p; ++p) {
    char emit = 0;
    if (*p >= 0x20 && *p < 0x7F) {
      emit = (char)*p;
    } else if (*p == '\t') {
      emit = ' ';
    } else if (*p >= 0x80) {
      // UTF-8 lead byte: collapse the whole sequence to a single space
      if ((*p & 0xC0) == 0xC0) emit = ' ';
      // continuation bytes (0x80..0xBF) are silently dropped
    }
    if (emit == 0) continue;
    if (emit == ' ') {
      if (last_space) continue;
      last_space = true;
    } else {
      last_space = false;
    }
    out += emit;
  }
  // Trim trailing whitespace.
  while (out.length() && out[out.length() - 1] == ' ') out.remove(out.length() - 1);
  // Trim leading whitespace.
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

// Rolling token window: keep 5 hours of samples at 1-minute granularity.
// Records cumulative tokens_today (or boot fallback) per sample.
constexpr int TOK_HISTORY = 300;       // 300 minutes = 5h
struct TokSample { uint32_t t_ms; uint32_t tokens_cum; };
TokSample tok_hist[TOK_HISTORY];
int tok_count = 0;
int tok_head = 0;
// UINT32_MAX as sentinel: distinguishes "never seen a heartbeat" from
// "last heartbeat was 0". Without this, the very first heartbeat after
// firmware boot would add the desktop's full cumulative (e.g. 184502 tok)
// to tokens_boot, falsely inflating the local level counter.
uint32_t last_tokens_seen = UINT32_MAX;
uint32_t last_sample_ms = 0;

void updateTokenWindows(uint32_t tokens_now) {
  uint32_t now = millis();

  if (last_tokens_seen == UINT32_MAX) {
    // First heartbeat after firmware boot — anchor without backfill.
    last_tokens_seen = tokens_now;
  } else if (tokens_now < last_tokens_seen) {
    // Desktop restarted — re-anchor without counting the pre-restart
    // value as new delta (was a bug: the old code added the post-restart
    // cumulative to tokens_boot on every desktop restart).
    last_tokens_seen = tokens_now;
  } else {
    uint32_t delta = tokens_now - last_tokens_seen;
    if (delta > 0) g_state.tokens_boot += delta;
    last_tokens_seen = tokens_now;
  }

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

  for (int i = 0; i < 8; i++) g_state.entries[i] = "";
  JsonArray ents = d["entries"].as<JsonArray>();
  int i = 0;
  for (JsonVariant v : ents) {
    if (i >= 8) break;
    g_state.entries[i++] = asciiOnly(v | "");
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
