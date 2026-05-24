#pragma once

// ES8311 codec + I2S TX feedback tones.
//
// Hardware: ES8311 mono codec at I2C 0x18 sharing the bus with SHTC3/RTC,
// I2S MCLK/BCLK/WS/DOUT on GPIO 16/9/45/10, PA_EN amplifier enable on
// GPIO 46. Speaker is single-channel.
//
// We synthesise short tones (sine + exponential decay) in firmware rather
// than embedding wave files. That's why the API is just ding() / buzz()
// instead of play(buffer, len) -- there's no need for general playback.
//
// audio::begin() is safe to call before sensors::begin() initialises Wire,
// since it probes the codec and bails out cleanly if absent. If the codec
// or I2S init fails the rest of the firmware keeps running silently.
namespace audio {
  void begin();
  bool isReady();
  void ding();           // short rising chime, ~150 ms (approve / good news)
  void buzz();           // longer low tone, ~250 ms (deny / error)
  void chirp();          // tiny boot-test pip (~80 ms) — proves audio path
}
