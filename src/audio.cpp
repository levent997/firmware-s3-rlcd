#include "audio.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "driver/i2s.h"   // legacy I2S API (IDF 4.x — what arduino-esp32 ships)

namespace {
// CE pin tied low on the Waveshare board => I2C address 0x18.
constexpr uint8_t ES8311_ADDR = 0x18;

// Hardware pin map. The Waveshare board datasheet lists these from the
// CODEC's perspective: DSDIN = data going INTO the codec (playback path,
// ESP32 OUTPUT) = GPIO 8, ASDOUT = data going OUT of the codec (mic path,
// ESP32 INPUT) = GPIO 10. The first cut of this module had DOUT pointed
// at GPIO 10 (the mic line) and produced silence -- corrected to GPIO 8.
constexpr int PIN_PA_EN = 46;
constexpr int PIN_MCLK  = 16;
constexpr int PIN_BCLK  = 9;    // I2S SCLK
constexpr int PIN_WS    = 45;   // I2S LRCK
constexpr int PIN_DOUT  = 8;    // I2S DSDIN — ESP32 -> codec (playback)
// constexpr int PIN_DIN = 10; // I2S ASDOUT — codec -> ESP32 (mic, unused)

constexpr int SAMPLE_RATE = 16000;

bool ready = false;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// Minimum-viable ES8311 init for 16 kHz / 16-bit playback in slave-mode
// (ESP32 supplies MCLK and BCLK). Distilled from the Espressif ESP-ADF
// es8311.c driver. If the codec doesn't beep on first flash, this is the
// table most likely to need tweaking -- different boards bias the
// charge-pump / clock divisors slightly differently.
bool initES8311() {
  Wire.beginTransmission(ES8311_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("[audio] ES8311 not detected at 0x18");
    return false;
  }
  Serial.println("[audio] ES8311 detected");

  // (reg, value)
  static const uint8_t SEQ[][2] = {
    {0x00, 0x1F},   // Reset registers
    {0x45, 0x00},   // -- (datasheet: clk power)
    {0x01, 0x30},   // ClkMgr: MCLK from MCLK pin (bit 5), CLKMUL from MCLK
    {0x02, 0x00},   // Pre-divider 1, multiplier 1
    {0x03, 0x10},   // ADC OSR
    {0x16, 0x24},   // ADC dig vol-update
    {0x04, 0x10},   // DAC OSR
    {0x05, 0x00},   // Sample fs ratio
    {0x06, 0x03},   // BCLK divider (slave mode -- ESP32 provides BCLK)
    {0x07, 0x00},   // --
    {0x08, 0xFF},   // --
    {0x09, 0x0C},   // SDP-In: I2S, 16-bit
    {0x0A, 0x0C},   // SDP-Out: I2S, 16-bit
    {0x0B, 0x00},   // --
    {0x0C, 0x00},   // --
    {0x10, 0x1F},   // System power: DAC analog/digital on
    {0x11, 0x7F},   // System power: VMID, refbuf on
    {0x00, 0x80},   // CSM (clock state machine) start
    {0x01, 0x3F},   // CLKMgr: BCLK/MCLK enable
    {0x12, 0x00},   // HP charge-pump off (line out only on this board)
    {0x13, 0x10},   // DAC power on
    {0x14, 0x1A},   // MIC gain (unused; default)
    {0x37, 0x08},   // DAC mute ramp control
    {0x32, 0xBF},   // DAC volume = 0xBF/0xFF dB attenuation (~ -32 dB)
    {0x31, 0x00},   // DAC unmute, normal mode
    {0x44, 0x08},   // ADC->DAC bypass off
  };
  for (auto &p : SEQ) {
    if (!writeReg(p[0], p[1])) {
      Serial.printf("[audio] I2C write reg 0x%02X failed\n", p[0]);
      return false;
    }
    delayMicroseconds(50);
  }
  delay(10);
  return true;
}

bool initI2S() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = PIN_MCLK;
  pins.bck_io_num   = PIN_BCLK;
  pins.ws_io_num    = PIN_WS;
  pins.data_out_num = PIN_DOUT;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("[audio] i2s_driver_install failed");
    return false;
  }
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("[audio] i2s_set_pin failed");
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }
  if (i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT,
                  I2S_CHANNEL_MONO) != ESP_OK) {
    Serial.println("[audio] i2s_set_clk failed");
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }
  return true;
}

// Stream a sine tone with exponential decay. Blocking — call from a
// non-time-critical context (not from inside a tight render loop).
void playTone(int freq_hz, int duration_ms, float amplitude = 0.35f) {
  if (!ready) return;

  digitalWrite(PIN_PA_EN, HIGH);
  // Brief warm-up so the amp settles before audio hits it.
  delayMicroseconds(500);

  constexpr int CHUNK = 256;
  static int16_t buf[CHUNK];
  int total_samples = (SAMPLE_RATE * duration_ms) / 1000;
  int written = 0;
  size_t bytes_done;

  while (written < total_samples) {
    int n = CHUNK;
    if (written + n > total_samples) n = total_samples - written;
    for (int i = 0; i < n; i++) {
      int s = written + i;
      float t_sec = (float)s / SAMPLE_RATE;
      float t_norm = (float)s / total_samples;
      // Attack-decay envelope: ramp up over first 8%, decay after 25%.
      float env;
      if (t_norm < 0.08f)       env = t_norm / 0.08f;
      else if (t_norm < 0.25f)  env = 1.0f;
      else                      env = expf(-4.0f * (t_norm - 0.25f));
      float v = sinf(2.0f * (float)M_PI * freq_hz * t_sec) * amplitude * env;
      buf[i] = (int16_t)(v * 32767.0f);
    }
    i2s_write(I2S_PORT, buf, n * sizeof(int16_t), &bytes_done,
              100 / portTICK_PERIOD_MS);
    written += n;
  }

  // Drain with silence so the amp doesn't click on shutoff.
  memset(buf, 0, sizeof(buf));
  i2s_write(I2S_PORT, buf, sizeof(buf), &bytes_done, 100 / portTICK_PERIOD_MS);
  delay(15);
  digitalWrite(PIN_PA_EN, LOW);
}
} // namespace

void audio::begin() {
  pinMode(PIN_PA_EN, OUTPUT);
  digitalWrite(PIN_PA_EN, LOW);
  if (!initES8311()) return;
  if (!initI2S())    return;
  ready = true;
  Serial.println("[audio] ready");
  chirp();   // boot confirmation pip
}

bool audio::isReady() { return ready; }

void audio::ding() { playTone(880, 150); }    // A5, short and bright
void audio::buzz() { playTone(220, 250); }    // A3, longer and low
void audio::chirp() { playTone(1320, 80, 0.25f); }  // E6, tiny boot pip
