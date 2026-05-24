#pragma once
#include <ArduinoJson.h>

// Receives "character pack" folder pushes from the Claude desktop, per the
// REFERENCE.md folder-protocol section:
//
//   char_begin → file → chunk* → file_end → ...repeat... → char_end
//
// Currently a *stub*: state machine fully parses the sequence, validates
// base64 chunks, and replies with byte-counter acks, but does NOT yet write
// files to LittleFS. That comes in P1-8b. P1-8a's job is to prove the
// round-trip works without breaking the BLE link, and to land the partition
// table + filesystem mount infrastructure.
namespace xfer {
  void begin();         // mount LittleFS (format on first boot after partition swap)
  bool isReady();       // true if LittleFS mounted successfully

  // Try to handle a "cmd": char_begin / file / chunk / file_end / char_end.
  // If handled, fills `out_ack` with the JSON response and returns true.
  // Returns false if `cmd` isn't ours (caller falls back to other handlers).
  bool handleCmd(const char *cmd, JsonDocument &d, JsonDocument &out_ack);

  // Total / used bytes — for SYSTEM diagnostics.
  unsigned long fsTotalBytes();
  unsigned long fsUsedBytes();
}
