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
String file_path;              // user-facing path inside the pack (e.g. "manifest.json")
String file_fullpath;          // LittleFS path (e.g. "/bufo/manifest.json")
uint32_t file_size = 0;        // declared size for current file
uint32_t file_written = 0;     // decoded bytes appended to current file
bool fs_ready = false;

File current_file;

// Bound on raw bytes per chunk. BLE NUS MTU is ≤ ~250 B, the JSON wrapper
// eats ~30 B, and base64 expands 3:4, so the raw payload is bounded by
// (250 - 30) * 3 / 4 ≈ 165 B. 512 is comfortable headroom for whatever the
// negotiated MTU happens to be.
constexpr size_t MAX_CHUNK_DECODED = 512;
uint8_t chunk_buf[MAX_CHUNK_DECODED];

// Wipe a directory recursively, then remove it. No-op if the path doesn't
// exist. Returns true on full success.
bool removeRecursive(const String &path) {
  if (!LittleFS.exists(path)) return true;
  File root = LittleFS.open(path);
  if (!root) return false;
  if (!root.isDirectory()) {
    root.close();
    return LittleFS.remove(path);
  }
  // Collect child paths up front so we don't iterate an open dir while
  // mutating it.
  String children[16];
  bool is_dir[16];
  int n = 0;
  File child;
  while ((child = root.openNextFile()) && n < 16) {
    children[n] = String(child.path());
    is_dir[n] = child.isDirectory();
    n++;
    child.close();
  }
  root.close();
  bool ok = true;
  for (int i = 0; i < n; i++) {
    if (is_dir[i]) {
      if (!removeRecursive(children[i])) ok = false;
    } else {
      if (!LittleFS.remove(children[i])) ok = false;
    }
  }
  if (!LittleFS.rmdir(path)) ok = false;
  return ok;
}

// Ensure every intermediate directory of `full_path` exists. The terminal
// element is treated as the filename itself.
bool ensureParentDirs(const String &full_path) {
  int last_slash = full_path.lastIndexOf('/');
  if (last_slash <= 0) return true;
  String dir = full_path.substring(0, last_slash);
  if (LittleFS.exists(dir)) return true;
  // Recurse for parent first.
  int parent_slash = dir.lastIndexOf('/');
  if (parent_slash > 0) {
    String parent = dir.substring(0, parent_slash);
    if (!LittleFS.exists(parent)) {
      if (!ensureParentDirs(dir)) return false;
    }
  }
  return LittleFS.mkdir(dir);
}

// Reject paths that try to escape the pack dir or carry control characters.
// Returns the cleaned (still-relative) path on success, empty string on
// rejection.
String sanitizePath(const String &in) {
  if (in.length() == 0 || in.length() > 96) return "";
  if (in[0] == '/') return "";                       // must be relative
  if (in.indexOf("..") >= 0) return "";              // no parent traversal
  for (size_t i = 0; i < in.length(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x20 || c == 0x7F) return "";            // control chars
  }
  return in;
}

// Same for the char (pack) name: a single path segment, no slashes.
String sanitizeName(const String &in) {
  if (in.length() == 0 || in.length() > 32) return "";
  for (size_t i = 0; i < in.length(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x20 || c == 0x7F || c == '/') return "";
  }
  if (in == "." || in == "..") return "";
  return in;
}

// Bail out of the in-flight file: close handle, delete partial file on disk.
void abortFile() {
  if (current_file) current_file.close();
  if (file_fullpath.length() && LittleFS.exists(file_fullpath)) {
    LittleFS.remove(file_fullpath);
  }
  file_fullpath = "";
  file_path = "";
  file_size = 0;
  file_written = 0;
  state = State::InChar;        // back to "have a pack open, no file"
}

// Bail out of the whole pack: file too if open, plus the pack's directory
// so we don't leave a half-written character pack around.
void abortChar(const char *why) {
  Serial.printf("[xfer] abort pack \"%s\": %s\n", char_name.c_str(), why);
  if (current_file) current_file.close();
  if (char_name.length()) {
    removeRecursive("/" + char_name);
  }
  char_name = "";
  char_total = 0;
  char_received = 0;
  file_fullpath = "";
  file_path = "";
  file_size = 0;
  file_written = 0;
  state = State::Idle;
}
} // namespace

void xfer::begin() {
  if (LittleFS.begin(true)) {
    fs_ready = true;
    Serial.printf("[xfer] LittleFS mounted: total=%lu used=%lu\n",
                  (unsigned long)LittleFS.totalBytes(),
                  (unsigned long)LittleFS.usedBytes());
    // List existing top-level pack directories so we know what survived
    // reboots and partition swaps.
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
      File f;
      while ((f = root.openNextFile())) {
        Serial.printf("[xfer]   entry: %s %s (%lu B)\n",
                      f.isDirectory() ? "DIR " : "FILE",
                      f.path(),
                      (unsigned long)f.size());
        f.close();
      }
      root.close();
    }
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
  // char_begin — start a new pack. Wipes any pre-existing directory of the
  // same name so a re-push fully replaces the previous version.
  if (!strcmp(cmd, "char_begin")) {
    out_ack["ack"] = "char_begin";
    out_ack["n"] = 0;
    if (!fs_ready) {
      out_ack["ok"] = false;
      out_ack["error"] = "filesystem unavailable";
      return true;
    }
    // If a previous transfer left us mid-stream (desktop crash, BLE drop),
    // clean up before accepting the new one.
    if (state != State::Idle) {
      abortChar("interrupted by new char_begin");
    }
    String name_in = String((const char *)(d["name"] | ""));
    String name = sanitizeName(name_in);
    if (name.length() == 0) {
      out_ack["ok"] = false;
      out_ack["error"] = "invalid name";
      return true;
    }
    uint32_t total = d["total"] | 0;
    unsigned long free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (total > free_bytes) {
      out_ack["ok"] = false;
      out_ack["error"] = "pack too large for available storage";
      return true;
    }
    // Replace any existing pack of this name.
    String dir = "/" + name;
    if (LittleFS.exists(dir)) {
      Serial.printf("[xfer] replacing existing pack \"%s\"\n", name.c_str());
      removeRecursive(dir);
    }
    if (!LittleFS.mkdir(dir)) {
      out_ack["ok"] = false;
      out_ack["error"] = "mkdir failed";
      return true;
    }
    char_name = name;
    char_total = total;
    char_received = 0;
    state = State::InChar;
    Serial.printf("[xfer] char_begin name=\"%s\" total=%lu free=%lu\n",
                  name.c_str(), (unsigned long)total, free_bytes);
    out_ack["ok"] = true;
    return true;
  }

  // file — open the next file in the pack.
  if (!strcmp(cmd, "file")) {
    out_ack["ack"] = "file";
    out_ack["n"] = 0;
    if (state != State::InChar) {
      out_ack["ok"] = false;
      out_ack["error"] = "no active char";
      return true;
    }
    String path_in = String((const char *)(d["path"] | ""));
    String rel = sanitizePath(path_in);
    if (rel.length() == 0) {
      out_ack["ok"] = false;
      out_ack["error"] = "invalid path";
      return true;
    }
    String full = "/" + char_name + "/" + rel;
    if (!ensureParentDirs(full)) {
      out_ack["ok"] = false;
      out_ack["error"] = "mkdir failed";
      return true;
    }
    if (current_file) current_file.close();
    current_file = LittleFS.open(full, "w");
    if (!current_file) {
      out_ack["ok"] = false;
      out_ack["error"] = "open failed";
      return true;
    }
    file_path = rel;
    file_fullpath = full;
    file_size = d["size"] | 0;
    file_written = 0;
    state = State::InFile;
    Serial.printf("[xfer] file path=\"%s\" size=%lu\n",
                  file_path.c_str(), (unsigned long)file_size);
    out_ack["ok"] = true;
    return true;
  }

  // chunk — base64 payload appended to the open file.
  if (!strcmp(cmd, "chunk")) {
    out_ack["ack"] = "chunk";
    if (state != State::InFile || !current_file) {
      out_ack["ok"] = false;
      out_ack["error"] = "no active file";
      out_ack["n"] = file_written;
      return true;
    }
    const char *b64 = d["d"] | "";
    size_t b64_len = strlen(b64);
    size_t decoded_len = 0;
    int r = mbedtls_base64_decode(chunk_buf, sizeof(chunk_buf), &decoded_len,
                                  (const unsigned char *)b64, b64_len);
    if (r != 0) {
      Serial.printf("[xfer] chunk decode failed mbedtls=%d b64_len=%u\n",
                    r, (unsigned)b64_len);
      abortFile();
      out_ack["ok"] = false;
      out_ack["error"] = (r == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
                         ? "chunk exceeds 512 B decoded"
                         : "invalid base64";
      out_ack["n"] = 0;
      return true;
    }
    size_t wrote = current_file.write(chunk_buf, decoded_len);
    if (wrote != decoded_len) {
      Serial.printf("[xfer] write short %u/%u — disk full?\n",
                    (unsigned)wrote, (unsigned)decoded_len);
      abortFile();
      out_ack["ok"] = false;
      out_ack["error"] = "write failed";
      out_ack["n"] = file_written;
      return true;
    }
    file_written += decoded_len;
    char_received += decoded_len;
    out_ack["ok"] = true;
    out_ack["n"] = file_written;
    return true;
  }

  // file_end — close the file. Optionally verify declared size matches.
  if (!strcmp(cmd, "file_end")) {
    out_ack["ack"] = "file_end";
    if (state != State::InFile) {
      out_ack["ok"] = false;
      out_ack["error"] = "no active file";
      out_ack["n"] = 0;
      return true;
    }
    if (current_file) current_file.close();
    bool size_ok = (file_size == 0) || (file_written == file_size);
    Serial.printf("[xfer] file_end path=\"%s\" wrote=%lu declared=%lu %s\n",
                  file_path.c_str(),
                  (unsigned long)file_written,
                  (unsigned long)file_size,
                  size_ok ? "ok" : "SIZE MISMATCH");
    uint32_t final_n = file_written;
    file_fullpath = "";
    file_path = "";
    file_size = 0;
    file_written = 0;
    state = State::InChar;
    out_ack["ok"] = size_ok;
    if (!size_ok) out_ack["error"] = "size mismatch";
    out_ack["n"] = final_n;
    return true;
  }

  // char_end — close the pack.
  if (!strcmp(cmd, "char_end")) {
    out_ack["ack"] = "char_end";
    out_ack["n"] = 0;
    if (state != State::InChar) {
      out_ack["ok"] = false;
      out_ack["error"] = "no active char";
      return true;
    }
    bool total_ok = (char_total == 0) || (char_received == char_total);
    Serial.printf("[xfer] char_end name=\"%s\" received=%lu declared=%lu fs_used=%lu/%lu %s\n",
                  char_name.c_str(),
                  (unsigned long)char_received,
                  (unsigned long)char_total,
                  (unsigned long)LittleFS.usedBytes(),
                  (unsigned long)LittleFS.totalBytes(),
                  total_ok ? "ok" : "SIZE MISMATCH");
    char_name = "";
    char_total = 0;
    char_received = 0;
    state = State::Idle;
    out_ack["ok"] = total_ok;
    if (!total_ok) out_ack["error"] = "size mismatch";
    return true;
  }

  return false;
}
