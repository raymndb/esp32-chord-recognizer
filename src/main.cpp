#include <Arduino.h>
#include <driver/i2s.h>

// Pins: must match your wiring
#define I2S_SCK 4   // purple
#define I2S_WS  5   // orange
#define I2S_SD  6   // yellow

#define SAMPLE_RATE 16000
#define BLOCK 512            // samples per read (32 ms of audio)

int32_t raw[BLOCK];

void setup() {
  Serial.begin(115200);
  delay(1500);  // give the serial monitor time to connect

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;   // L/R tied to GND = left
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;   // no master clock needed for the INMP441
  pins.bck_io_num = I2S_SCK;
  pins.ws_io_num = I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = I2S_SD;

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK ||
      i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    Serial.println("I2S setup failed");
    while (true) delay(1000);
  }
  Serial.println("I2S started. Make some noise.");
}

void loop() {
  static int counter = 0;
  size_t bytesRead = 0;
  i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, portMAX_DELAY);
  int n = bytesRead / 4;
  if (n == 0) return;

  // The mic sends 24-bit audio in a 32-bit slot; shift down to a 16-bit range
  long sum = 0;
  int allZero = 1;
  for (int i = 0; i < n; i++) {
    raw[i] >>= 14;
    sum += raw[i];
    if (raw[i] != 0) allZero = 0;
  }
  long mean = sum / n;  // DC offset

  double sq = 0;
  long peak = 0;
  for (int i = 0; i < n; i++) {
    long s = raw[i] - mean;  // remove the offset
    sq += (double)s * s;
    if (labs(s) > peak) peak = labs(s);
  }
  int rms = (int)sqrt(sq / n);

  // Keep the loudest block since the last print so short sounds (claps) aren't skipped
  static int maxRms = 0;
  static long maxPeak = 0;
  if (rms > maxRms) maxRms = rms;
  if (peak > maxPeak) maxPeak = peak;

  if (++counter % 4 != 0) return;  // print about 8 times per second

  if (allZero) {
    Serial.println("all zeros: no data from mic");
    return;
  }
  int bars = min(maxRms / 20, 60);  // change 20 to rescale the bar
  Serial.printf("rms %5d  peak %6ld  ", maxRms, maxPeak);
  for (int i = 0; i < bars; i++) Serial.print('#');
  Serial.println();
  maxRms = 0;
  maxPeak = 0;
}