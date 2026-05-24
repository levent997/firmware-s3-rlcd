#include "xfer.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <mbedtls/base64.h>

namespace {
enum class State { Idle, InChar, InFile };

State state = State::Idle;
String char_name;
uint32_t char_total = 0;       // declared total bytes for the pack
uint32_t char_received = 0;    // decoded bytes seen so far this pack
String file_path;
uint32_t file_size = 0;        // declared size for current file
uint32_t file_written = 0;     // decoded bytes seen so far this file
bool fs_ready = false;

void resetCharState() {
  char_name = "";
  char_total = 0;
  char_received = 0;
  file_path = "";
  file_size = 0;
  file_written = 0;
  state = State::Idle;
}
} // namespace

void xfer::begin() {
  // First arg = true => format on mount failure. After the partition swap
  // the area is uninitialised, so the very first boot will format. Later
  // boots find a valid LittleFS and skip the format.
  if (LittleFS.begin(true)) {
    fs_ready = true;
    Serial.printf("[xfer] LittleFS mounted: total=%lu used=%lu\n",
                  (unsigned long)LittleFS.totalBytes(),
                  (unsigned long)LittleFS.usedBytes());
  } else {
    Serial.println("[xfer] LittleFS mount failed (even with auto-format)");
  }
}

bool xfer::isReady() { return fs_ready; }

unsigned long xfer::fsTotalBytes() {
  return fs_ready ? (unsigned long)LittleFS.totalBytes() : 0;
}
unsigned long xfer::fsUsedBytes() {
  return fs_ready ? (unsigned long)LittleFS.usedBytes() : 0;
}

bool xfer::handleCmd(const char *cmd, JsonDocument &d, JsonDocument &out_ack) {
  // char_begin — open a new pack
  if (!strcmp(cmd, "char_begin")) {
    out_ack["ack"] = "char_begin";
    if (!fs_ready) {
      out_ack["ok"] = false;
      out_ack["error"] = "filesystem unavailable";
      out_ack["n"] = 0;
      return true;
    }
    resetCharState();
    char_name = String((const char *)(d["name"] | "unknown"));
    char_total = d["total"] | 0;
    state = State::InChar;
    Serial.printf("[xfer] char_begin name=\"%s\" total=%lu bytes\n",
                  char_name.c_str(), (unsigned long)char_total);
    out_ack["ok"] = true;
    out_ack["n"] = 0;
    return true;
  }

  // file — open a single file within the pack
  if (!strcmp(cmd, "file")) {
    out_ack["ack"] = "file";
    if (state != State::InChar) {
      out_ack["ok"] = false;
      out_ack["error"] = "no active char";
      out_ack["n"] = 0;
      return true;
    }
    file_path = String((const char *)(d["path"] | ""));
    file_size = d["size"] | 0;
    file_written = 0;
    if (file_path.length() == 0) {
      out_ack["ok"] = false;
      out_ack["error"] = "empty path";
      out_ack["n"] = 0;
      return true;
    }
    state = State::InFile;
    Serial.printf("[xfer] file path=\"%s\" size=%lu (stub: not writing)\n",
                  file_path.c_str(), (unsigned long)file_size);
    out_ack["ok"] = true;
    out_ack["n"] = 0;
    return true;
  }

  // chunk — base64 payload appended to the current file
  if (!strcmp(cmd, "chunk")) {
    out_ack["ack"] = "chunk";
    if (state != State::InFile) {
      out_ack["ok"] = false;
      out_ack["error"] = "no active file";
      out_ack["n"] = 0;
      return true;
    }
    const char *b64 = d["d"] | "";
    size_t b64_len = strlen(b64);
    // We only need the decoded length right now (P1-8a is stub-only).
    // Asking mbedtls with dst=NULL returns BUFFER_TOO_SMALL and writes the
    // required length into &decoded_len.
    size_t decoded_len = 0;
    int r = mbedtls_base64_decode(nullptr, 0, &decoded_len,
                                  (const unsigned char *)b64, b64_len);
    if (r != 0 && r != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
      Serial.printf("[xfer] chunk base64 invalid (mbedtls=%d)\n", r);
      out_ack["ok"] = false;
      out_ack["error"] = "invalid base64";
      out_ack["n"] = file_written;
      return true;
    }
    file_written += decoded_len;
    char_received += decoded_len;
    out_ack["ok"] = true;
    out_ack["n"] = file_written;
    return true;
  }

  // file_end — close current file (size of decoded bytes)
  if (!strcmp(cmd, "file_end")) {
    out_ack["ack"] = "file_end";
    if (state != State::InFile) {
      out_ack["ok"] = false;
      out_ack["error"] = "no active file";
      out_ack["n"] = 0;
      return true;
    }
    Serial.printf("[xfer] file_end path=\"%s\" wrote=%lu (declared %lu)\n",
                  file_path.c_str(),
                  (unsigned long)file_written,
                  (unsigned long)file_size);
    state = State::InChar;
    out_ack["ok"] = true;
    out_ack["n"] = file_written;
    return true;
  }

  // char_end — close the pack
  if (!strcmp(cmd, "char_end")) {
    out_ack["ack"] = "char_end";
    if (state != State::InChar) {
      out_ack["ok"] = false;
      out_ack["error"] = "no active char";
      out_ack["n"] = 0;
      return true;
    }
    Serial.printf("[xfer] char_end name=\"%s\" received=%lu (declared %lu)\n",
                  char_name.c_str(),
                  (unsigned long)char_received,
                  (unsigned long)char_total);
    resetCharState();
    out_ack["ok"] = true;
    out_ack["n"] = 0;
    return true;
  }

  return false;
}
