# CLAUDE.md

Operational notes for agents working on this firmware. Keep short.

## Project Overview

ESP32-S3 firmware for the **Waveshare ESP32-S3-RLCD-4.2** board, acting as a
Bluetooth LE *Hardware Buddy* device that pairs with the Claude desktop app's
Developer → Open Hardware Buddy… window.

Renders a Clawd pixel-art mascot + live session stats on a 4.2" 300×400
reflective monochrome LCD (Sitronix ST7305 controller).

Protocol: Nordic UART Service, newline-delimited JSON. See the sibling
`../REFERENCE.md` (in the `claude-desktop-buddy` reference repo) for the
authoritative wire spec; this firmware only consumes it.

## Hardware (Waveshare ESP32-S3-RLCD-4.2)

| Function          | Pin / detail                                |
|-------------------|---------------------------------------------|
| MCU               | ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM) |
| Display           | ST7305 RLCD 300x400 mono, SPI               |
| RLCD SCL/SDA      | GPIO 11 / 12                                |
| RLCD RS/CS/RESET  | GPIO 5 / 40 / 41 (TE on GPIO 6, unused)     |
| Touch panel       | TP_INT 7, TP_SDA 13, TP_SCL 14, TP_RESET 42 (shares I2C; unused by us) |
| Buttons (usable)  | KEY = GPIO 18, BOOT = GPIO 0                |
| Power button      | Key3 -> ETA6098 PMIC OUTH -- **not GPIO, software cannot read** |
| Battery sense     | ADC1_CH3 (GPIO 4) with 1/3 divider          |
| I2C bus           | SDA = 13, SCL = 14 (shared: SHTC3 + PCF85063 + ES8311 + QMI8658C + TP) |
| SHTC3 addr        | 0x70 (temp/humidity)                        |
| PCF85063 addr     | 0x51 (RTC); INT pin on GPIO 15 (unused)     |
| QMI8658C addr     | 0x6A or 0x6B (6-axis IMU) per datasheet. **NOT POPULATED on the test board variant** -- I2C scan only finds {0x18 ES8311, 0x40 touch, 0x51 RTC, 0x70 SHTC3}. Driver in `src/imu.{h,cpp}` is wired up and gracefully no-ops when the chip is absent; if you have a board with the IMU populated, it just works (face-down -> nap, shake -> dizzy). |
| Audio codec       | ES8311: I2S MCLK 16, SCLK 9, LRCK 45, **DSDIN 8** (ESP32->codec, playback), ASDOUT 10 (codec->ESP32, mic, unused), PA_CTRL 46. I2C addr 0x18. |
| SD card           | MOSI 21, SCK 38, MISO 39, CS 17 (R7 may be NC; unused) |
| UART0 (USB-JTAG)  | TX 43, RX 44 (Serial console)               |

## Common Commands

```bash
# Build only
pio run

# Flash (closes any local monitor first)
pio run --upload-port COM3 -t upload

# Serial monitor
pio device monitor --port COM3 --baud 115200
```

On Windows the project ships two convenience scripts:
- `flash.bat` — build + upload to COM3
- `monitor.bat` — open serial monitor

## File Layout

```
firmware-s3-rlcd/
  platformio.ini             ESP32-S3 + octal PSRAM, U8g2 + NimBLE +
                             ArduinoJson + AnimatedGIF
  partitions_lfs.csv         16 MB layout: 4 MB app + 11.8 MB LittleFS
                             (label must stay "spiffs" for Arduino LFS)
  src/
    main.cpp                 boot, loop, button routing, energy ticker
    ble_nus.{h,cpp}          NimBLE NUS peripheral + LE SC pairing/bonding
    protocol.{h,cpp}         JSON parsing, ack/permission, ASCII sanitiser,
                             5h rolling token sampler, time-sync dispatch
    state.h                  BuddyState - single shared snapshot
    sensors.{h,cpp}          Battery ADC + SHTC3 temp/humidity + charging heuristic
    rtc.{h,cpp}              PCF85063 driver (BCD, local wall-clock store)
    imu.{h,cpp}              QMI8658C 6-axis accel driver (DNP on test board)
    audio.{h,cpp}            ES8311 codec + legacy I2S TX, sin/expf tone synth
    persist.{h,cpp}          NVS persistence (tokens, names, sound, counters)
    xfer.{h,cpp}             Folder-push state machine writing to LittleFS
    pack.{h,cpp}             Runtime GIF decode -> PSRAM sprite overrides
    demo.{h,cpp}             Fake-heartbeat rotator (SYSTEM long-press)
    menu.{h,cpp}             Settings menu (USAGE long-press), 6 items,
                             two-stage confirm for destructive ones
    buttons.{h,cpp}          KEY/BOOT debouncing, short/long press
    ui.{h,cpp}               U8g2 dashboard: MAIN, USAGE, SYSTEM, CLOCK +
                             approval overlay, history overlay, menu,
                             confirm screen, passkey display
    st7305_u8g2.{h,cpp}      ST7305 <-> U8g2 backend (lifted from Waveshare)
    sprites.h                Auto-generated 128x128 1bpp built-in frames (16 sets)
  tools/
    gif_to_sprites.py        Re-generate sprites.h from clawd-on-desk gifs
  flash.bat, monitor.bat     Windows convenience scripts
```

## Runtime / State Model

- `g_state` (state.h) holds the single source of truth: heartbeat snapshot
  fields, sensors, derived energy/mood, current view.
- `ui::render()` hashes the relevant subset of state + animation frame and
  skips redraw when unchanged (debounce ≤ 150 ms).
- Animation frame counter advances at 5 Hz regardless of activity.
- Token windowing: 300 one-minute samples → tokens_1h, tokens_5h.
- "Energy" ticker decays under sustained `running > 0`, recovers when idle.

## View State Machine

Four views, cycled by the buttons (KEY short = next, BOOT short = prev):

| View    | Purpose                                                        |
|---------|----------------------------------------------------------------|
| MAIN    | Sprite + mood/energy/fed/level + live KPIs + recent transcript |
| USAGE   | Plan-usage progress bars (5h current; weekly = n/a per protocol) |
| SYSTEM  | Diagnostics rows + memory bars + velocity histogram OR walking mascot |
| CLOCK   | Big logisoso50 HH:MM + blinking colon + date + tz/sync source  |

Long-press semantics are view-dependent because all four button events
have to multiplex through two physical keys:

| Context                | KEY short | BOOT short | KEY long          | BOOT long         |
|------------------------|-----------|------------|-------------------|-------------------|
| MAIN (no prompt)       | next view | prev view  | open history overlay | open history overlay |
| USAGE (no prompt)      | next view | prev view  | **open settings menu** | open settings menu |
| SYSTEM (no prompt)     | next view | prev view  | toggle demo mode (-> MAIN) | toggle demo mode |
| CLOCK (no prompt)      | next view | prev view  | next view         | prev view         |
| Active prompt (any view) | APPROVE  | DENY       | open history (escape) | open history     |
| Settings menu open     | next item | prev item  | activate / confirm | back / cancel    |
| Menu confirm screen    | (no nav)  | (no nav)   | EXECUTE destructive | cancel            |
| History overlay open   | close     | close      | close             | close             |
| Passkey display        | (locked)  | (locked)   | (locked)          | (locked)          |

The third physical button is hardware power-only and not software-readable.

## Render Priority (ui::render switch)

When multiple overlays could draw, this is the precedence chain (highest wins):

1. **Passkey screen** (`g_state.passkey_displaying`) — during BLE pairing.
2. **Settings menu** (`g_state.menu_open`) — possibly with confirm-screen sub-state.
3. **History overlay** (`g_state.history_open`) — 8-row transcript.
4. **Active prompt** (`g_state.prompt.active` on MAIN view) — full-screen approval.
5. Regular view (MAIN / USAGE / SYSTEM / CLOCK).

## Wire Protocol Coverage

Implemented:
- Inbound heartbeat snapshot (total/running/waiting/msg/entries[8]/tokens/tokens_today/prompt)
- Inbound commands: `status`, `name`, `owner`, `unpair` (acked).
- Inbound `time` (epoch + tz offset) — written to PCF85063 RTC.
- Inbound folder push (`char_begin / file / chunk / file_end / char_end`)
  via `src/xfer.{h,cpp}`. Files land on LittleFS as `/<pack_name>/<path>`;
  GIFs get auto-decoded into PSRAM sprite overrides by `src/pack.{h,cpp}`.
- Outbound `permission` response — driven by KEY (approve) / BOOT (deny)
  buttons while a prompt is active.
- BLE NUS service `6e400001-...`, LE Secure Connections + MITM bonding,
  on-screen passkey display. Advertise name `Claude-<MAC tail>`.

**Not implemented** (intentional):
- `turn` event ingestion (4 KB cap, lower priority than heartbeats).

## Sprite Pipeline

`tools/gif_to_sprites.py` reads the gifs in `../clawd-on-desk/assets/gif/`,
takes 8 evenly-spaced frames per animation, crops to the union bbox across
frames, thresholds **non-white, non-black, non-transparent** pixels as ink
(this keeps Clawd's body filled while leaving the black eyes/mouth as
"holes"), fits each into a 128×128 canvas, and emits a flat
`SPRITE_<NAME>[frames][SPRITE_BYTES]` PROGMEM array in `src/sprites.h`.

To regenerate (e.g. after editing the gif set or target size):

```bash
python tools/gif_to_sprites.py
```

Currently embedded sprites (16):
`idle / idle_reading / bubble / building / typing / thinking / sweeping /
juggling / carrying / headphones / happy / notification / double_jump /
annoyed / error / sleeping`.

State -> sprite mapping lives in `ui.cpp::moodToSprite`.

**Runtime overrides**: `src/pack.{h,cpp}` listens for folder pushes
(`/<pack_name>/<sprite>.gif` on LittleFS) and decodes each GIF into
PSRAM-allocated 1bpp frames that win over `SPRITES[]` at draw time. Most
recent pushed pack is auto-loaded on boot. Same filename -> SpriteId map as
`tools/gif_to_sprites.py`.

## Constraints / Gotchas

- LittleFS partition label MUST stay `spiffs` in `partitions_lfs.csv`.
  Arduino-ESP32's `LittleFS::begin()` defaults to that label and won't mount
  any other name without an explicit override. Found out the hard way --
  reflashing with a `littlefs`-named partition shows `partition "spiffs"
  could not be found` and the mount fails silently.
- `partitions_lfs.csv` MUST be ASCII (no em-dash, smart quotes, CJK). The
  PlatformIO partition parser reads the file with the system codec (GBK on
  Windows-CN) and chokes on UTF-8 multibyte sequences during link.
- **Audio I2S `DSDIN` (ESP32 → codec for playback) is GPIO 8, NOT GPIO 10.**
  GPIO 10 is `ASDOUT` (codec → ESP32, mic input). Writing PCM to GPIO 10
  produces silence. The Waveshare datasheet labels them from the codec's
  perspective; CLAUDE.md naming was ambiguous in an earlier revision and
  the audio module shipped silent for a flash before this got noticed.
- **Audio is muted when `g_state.sound_on = false`.** Boot chirp respects
  this too because `persist::load()` runs before `audio::begin()` in setup().
- **persist::load() must run before audio::begin()** so the loaded sound
  setting takes effect on the boot chirp. Don't reorder them.
- U8g2 fonts used are **Latin-1 only**. Incoming non-ASCII bytes are replaced
  with space (not '?') so the line stays readable; see `protocol::asciiOnly`.
  Switching to a CJK-capable font (e.g. `u8g2_font_unifont_t_chinese*`) would
  cost ~150 KB flash; skip unless asked.
- **Never hardcode x for strings adjacent to variable-width fonts**
  (helvB12/14/18, logisoso24/50). Always `getStrWidth(first) + gap`. Hit
  this overlap bug three separate times before learning.
- ESP32-S3 USB-Serial/JTAG → `pio device monitor` opens the same COM as the
  firmware's `Serial`. Upload requires monitor closed (port lock).
- `BOARD_HAS_PSRAM` + `board_build.arduino.memory_type = qio_opi` are **required**
  in `platformio.ini` to enable the 8 MB octal PSRAM on the N16R8 variant.
  The base `esp32-s3-devkitc-1` board JSON does not enable PSRAM by itself.
  Same goes for `board_build.flash_size = 16MB` + `board_upload.flash_size = 16MB`
  -- the base JSON pretends it's an 8 MB chip.
- `getEfuseMac()` returns bytes in reverse order; the broadcast name uses the
  *low* two bytes which correspond to the *first* two MAC bytes (the user's
  device shows `Claude-E658`, not `Claude-7050`, even though USB MAC ends
  `:70:50`).
- ST7305 frame buffer is full-page (≈15 KB) — pulled from PROGMEM into a RAM
  scratch buffer per sprite draw call. Don't keep multiple decoded frames hot.
- BLE TX sends are MTU-fragmented in `ble_nus::sendLine`. Desktop reassembles
  on `\n`; **never** call `sendLine` from a tight loop without a delay or
  the radio queue can fill.
- **QMI8658C IMU is DNP** on the test board variant. I2C scan finds
  `{0x18 ES8311, 0x40 touch, 0x51 RTC, 0x70 SHTC3}` -- no IMU at any
  address. The driver gracefully no-ops and the BLE-disconnect-5min
  heuristic continues to drive nap. Swap to a populated board variant and
  the driver activates without code change.
- **Arduino-ESP32 3.x ships IDF 4.x I2S API only** (`driver/i2s.h`), NOT
  the IDF 5.x `driver/i2s_std.h`. First cut of audio.cpp included the
  latter and wouldn't compile. Stay on legacy.
- **Token counter cold-start over-count**: `updateTokenWindows()` uses
  `UINT32_MAX` as sentinel for "never seen a heartbeat" specifically so
  the first heartbeat after firmware boot doesn't add the desktop's full
  cumulative count to `tokens_boot`. Same for desktop-restart detection
  (re-anchor without backfill). Don't change this back to 0-init.
- **Rate KPI divisor**: must be `min(uptime_minutes, 60)` not hardcoded
  60. Cold-boot rate would otherwise underestimate by up to 60x.

## High-Risk Areas

- **`sprites.h` is generated.** Don't hand-edit. Re-run the converter and
  commit the result.
- **Partition table swaps preserve NVS only.** The LittleFS partition gets
  reformatted on first boot after any size/offset change. NVS at 0x9000 is
  the only thing that survives intact (kept persisted tokens, names, level
  counters across the P1-8a partition swap). Don't move it.
- **GIF decode blocks the loop for seconds.** `pack::tick()` runs the
  AnimatedGIF decoder synchronously when a new pack lands. UI freezes during
  that window. If we ever need to decode larger packs, move it to a
  FreeRTOS task pinned to the other core.

## Testing

There is no host-side test rig. Manual verification flow:
1. `pio run` — must compile clean (RAM ~12%, flash ~28% of 4 MB app slot).
2. `flash.bat` — must upload with `Hash of data verified.`
3. `monitor.bat` — within ~1 s of reset the log should show, in order:
   ```
   [persist] loaded: ...
   [sensors] SHTC3 OK
   [rtc] PCF85063 ready (...)
   [imu] I2C scan: ... [imu] QMI8658 not on bus (DNP)
   [xfer] LittleFS mounted: total=12451840 ...
   [pack] ... (built-in sprites in use)  | OR auto-loads pushed pack
   [audio] ES8311 detected / [audio] ready
   Advertising as Claude-XXXX
   ```
   Then `[hb]` lines every 5 s, then `[ble] connected` and `[ble] auth OK,
   link encrypted` after pairing.
4. Walk through all 4 views with KEY/BOOT (MAIN → USAGE → SYSTEM → CLOCK).
5. Watch the showcase carousel on MAIN while idle to confirm each sprite
   renders without artifacts.
6. Long-press MAIN -> history overlay shows; any press dismisses.
7. Long-press USAGE -> settings menu; navigate with KEY/BOOT short.
   Sound toggle should immediately mute / unmute subsequent dings.
   Reboot from menu should land you back at boot log within ~2 s.
8. Long-press SYSTEM -> demo mode kicks in (DEMO chip in top bar, scenes
   rotate every 7 s). Long-press SYSTEM again to exit.
9. If a real desktop is connected and you trigger an approval, KEY plays
   ding + sends `decision:once`, BOOT plays buzz + sends `decision:deny`.

## Heuristic Smoke Tests

After any non-trivial change, also verify:

- `cmd:unpair` from desktop wipes BLE bonds AND NVS (persisted tokens etc).
- Reboot survives: tokens_boot, level, approvals, denies, turns, names,
  sound_on, and the RTC clock should all be restored.
- Partition swap survives NVS but reformats LittleFS (intentional).
- Push a folder pack -> see `[pack] decoded /<name>/idle.gif -> slot 0`
  and the MAIN sprite changes within a few seconds.

## Out of Scope

This firmware does NOT cover the desktop side of the protocol. The Claude
desktop app implements the heartbeat/folder-push/permission/time-sync
counterparts; the `../REFERENCE.md` in the parent reference repo is the
spec. Do not propose changes that require new fields from the desktop —
flag them to the user instead.
