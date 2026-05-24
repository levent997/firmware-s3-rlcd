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
| Display           | ST7305 RLCD 300×400 mono, SPI               |
| RLCD SCK/MOSI     | GPIO 11 / 12                                |
| RLCD DC/CS/RST    | GPIO 5 / 40 / 41 (TE on GPIO 6, unused)     |
| Buttons (usable)  | KEY = GPIO 18, BOOT = GPIO 0                |
| Power button      | Key3 → ETA6098 PMIC OUTH — **not GPIO, software cannot read** |
| Battery sense     | ADC1_CH3 (GPIO 4) with 1/3 divider          |
| I²C (SHTC3 + PCF85063 + ES8311) | SDA = 13, SCL = 14            |
| SHTC3 addr        | 0x70                                        |
| Audio codec       | ES8311 — I2S MCLK 16, BCLK 9, WS 45, DIN 10, DOUT 8, PA_EN 46 (unused) |
| SD card           | MOSI 21, SCK 38, MISO 39, CS 17 (R7 may be NC) |

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
  platformio.ini             ESP32-S3 + octal PSRAM, U8g2 + NimBLE + ArduinoJson
  src/
    main.cpp                 boot, loop, button → view switch, energy ticker
    ble_nus.{h,cpp}          NimBLE Nordic UART peripheral, line-buffered TX/RX
    protocol.{h,cpp}         JSON parsing, ack/permission responses,
                             ASCII sanitiser, 5h rolling token sampler
    state.h                  BuddyState — single shared snapshot
    sensors.{h,cpp}          Battery ADC + SHTC3 temp/humidity
    buttons.{h,cpp}          KEY/BOOT debouncing, short/long press
    ui.{h,cpp}               U8g2 dashboard: MAIN, USAGE, SYSTEM views
    st7305_u8g2.{h,cpp}      ST7305 ↔ U8g2 backend (lifted from Waveshare)
    sprites.h                Auto-generated 128×128 1bpp Clawd animation frames
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

Three views, cycled by the buttons:

| View    | Purpose                                                        |
|---------|----------------------------------------------------------------|
| MAIN    | Sprite + mood/energy + live KPIs + recent transcript           |
| USAGE   | Plan-usage progress bars (5h, today, weekly all, weekly Sonnet) |
| SYSTEM  | Hardware diagnostics + memory bars + walking mascot strip      |

Buttons:
- `KEY` short = next view (`MAIN → USAGE → SYSTEM → MAIN`)
- `BOOT` short = previous view (reverse cycle)
- Long press: reserved (currently unused)

The third physical button is hardware power-only and not software-readable.

## Wire Protocol Coverage

Implemented:
- Inbound heartbeat snapshot (total/running/waiting/msg/entries/tokens/tokens_today/prompt)
- Inbound commands: `status`, `name`, `owner`, `unpair` (acked); `char_begin` (rejected)
- Inbound `time` (epoch + tz offset)
- Outbound `permission` response (legacy path — unused now that buttons no
  longer drive approve/deny; left in `protocol::sendPermission` for future re-enable)
- BLE NUS service `6e400001-...`, advertise name `Claude-<MAC tail>`

**Not implemented** (intentional):
- `turn` event ingestion (4 KB cap, lower priority than heartbeats)
- Folder push (`char_begin → file → chunk → file_end → char_end`). We reject
  `char_begin` so the desktop times out gracefully.
- LE Secure Connections bonding. Link is unencrypted; OK for personal desks,
  not OK for any environment where transcript snippets matter.

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

Currently embedded sprites:
`idle / building / typing / thinking / happy / notification / error / sleeping`.

State → sprite mapping lives in `ui.cpp::moodToSprite`.

## Constraints / Gotchas

- U8g2 fonts used are **Latin-1 only**. Incoming non-ASCII bytes are replaced
  with `?`; any entry that loses content is dropped (see `protocol::asciiOnly`).
  Switching to a CJK-capable font (e.g. `u8g2_font_unifont_t_chinese*`) would
  cost ~150 KB flash; skip unless asked.
- ESP32-S3 USB-Serial/JTAG → `pio device monitor` opens the same COM as the
  firmware's `Serial`. Upload requires monitor closed (port lock).
- `BOARD_HAS_PSRAM` + `board_build.arduino.memory_type = qio_opi` are **required**
  in `platformio.ini` to enable the 8 MB octal PSRAM on the N16R8 variant.
  The base `esp32-s3-devkitc-1` board JSON does not enable PSRAM by itself.
- `getEfuseMac()` returns bytes in reverse order; the broadcast name uses the
  *low* two bytes which correspond to the *first* two MAC bytes (the user's
  device shows `Claude-E658`, not `Claude-7050`, even though USB MAC ends
  `:70:50`).
- ST7305 frame buffer is full-page (≈15 KB) — pulled from PROGMEM into a RAM
  scratch buffer per sprite draw call. Don't keep multiple decoded frames hot.
- BLE TX sends are MTU-fragmented in `ble_nus::sendLine`. Desktop reassembles
  on `\n`; **never** call `sendLine` from a tight loop without a delay or
  the radio queue can fill.

## High-Risk Areas

- **Disconnect mystery**: occasional BLE drops a few seconds after pairing.
  Current theory: missing LE Secure Connections bonding causes Windows to
  drop the link, or our notify is not being subscribed to in time. The
  `[ble] tx subscribe state=` log line is the diagnostic — capture it next
  time before changing anything.
- **`sprites.h` is generated.** Don't hand-edit. Re-run the converter and
  commit the result.

## Testing

There is no host-side test rig. Manual verification flow:
1. `pio run` — must compile clean.
2. `flash.bat` — must upload with `Hash of data verified.`
3. `monitor.bat` — `Advertising as Claude-XXXX` should appear within ~1 s of
   reset, then `[hb]` lines every 5 s, then `[ble] connected` after pairing.
4. Walk through all 3 views with KEY/BOOT.
5. Watch the showcase carousel on MAIN while idle to confirm each sprite
   renders without artifacts.

## Out of Scope

This firmware does NOT cover the desktop side of the protocol. The Claude
desktop app implements the heartbeat/folder-push/permission/time-sync
counterparts; the `../REFERENCE.md` in the parent reference repo is the
spec. Do not propose changes that require new fields from the desktop —
flag them to the user instead.
