// INMP441 I2S microphone to ESP32 using Arduino framework 
#include <Arduino.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// This current example shows how to read audio from an INMP441 I2S microphone and print the RMS and peak values to platformIO's serial monitor. 
// The output is a simple 'bar' graph (using hashtags to represent volume) of the RMS(root mean sq)/peak values.
// The same values are drawn on an SSD1306 128x64 OLED: numbers on top, a level bar, and a scrolling history of the RMS below.

// in this example, the INMP441 is connected to the ESP32 as follows:
// purple  -> I2S_SCK (esp32, GPIO4)
// orange  -> I2S_WS  (esp32, GPIO5)
// yellow  -> I2S_SD  (esp32, GPIO6)

// defining the I2S pins for the INMP441 microphone
#define I2S_SCK 4   // purple
#define I2S_WS  5   // orange
#define I2S_SD  6   // yellow

// defining sample rate and block size for reading audio data
#define SAMPLE_RATE 16000 // the inmp441 can handle sample rates up to 48kHz, but the ESP32 can only handle 16kHz reliably, so using 16kHz here. 32kHz is possible, but the ESP32 will drop samples if the CPU is busy with other tasks
#define BLOCK 512            // samples per read (32 ms of audio)
#define SAMPLE_SHIFT 14      // right-shift for raw samples; raise if loud strums peak near 32767, lower if normal playing peaks only in the hundreds

// the OLED is connected to the ESP32 as follows:
// green -> SDA (esp32, GPIO8)
// blue  -> SCL (esp32, GPIO9)
#define OLED_SDA 8      // green
#define OLED_SCL 9      // blue
#define OLED_ADDR 0x3C
#define SCREEN_W 128
#define SCREEN_H 64
#define GRAPH_H 42            // height in pixels of the scrolling history at the bottom of the screen
#define LEVEL_FULL_SCALE 1200 // RMS that fills the bar/graph (same as 60 hashtags on serial); change to rescale

int32_t raw[BLOCK]; // buffer for reading audio data from the microphone

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);  // -1 = no reset pin
bool oledOk = false;        // false if the display didn't start, so the mic still works over serial alone
uint8_t history[SCREEN_W];  // last 128 RMS values scaled to graph pixels, newest at the right

// draw the RMS/peak numbers, a level bar and the scrolling RMS history on the OLED
void drawLevel(int rms, long peak, bool noData) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  if (noData) {
    display.println("no data from mic");
    display.display();
    return;
  }
  display.printf("rms %5d  pk %6ld", rms, peak);

  // level bar
  int w = min(rms * SCREEN_W / LEVEL_FULL_SCALE, SCREEN_W);
  display.drawRect(0, 10, SCREEN_W, 10, SSD1306_WHITE);
  display.fillRect(0, 10, w, 10, SSD1306_WHITE);

  // shift the history left one pixel and add the newest value on the right
  memmove(history, history + 1, SCREEN_W - 1);
  history[SCREEN_W - 1] = min(rms * GRAPH_H / LEVEL_FULL_SCALE, GRAPH_H);
  for (int x = 0; x < SCREEN_W; x++) {
    if (history[x] > 0) display.drawFastVLine(x, SCREEN_H - history[x], history[x], SSD1306_WHITE);
  }
  display.display();
}

// setup function to initialize the I2S driver and start reading audio data
// will stop if the I2S driver fails to initialize
void setup() {
  Serial.begin(115200);
  delay(1500);  // give the serial monitor time to connect

  // start the OLED; if it fails, carry on with serial output only
  Wire.begin(OLED_SDA, OLED_SCL);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("starting mic...");
    display.display();
  } else {
    Serial.println("SSD1306 init failed, continuing with serial output only");
  }

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
    Serial.println("I2S setup failed, check the wiring and pin definitions");
    while (true) delay(1000);
  }
  Serial.println("I2S started. Make sure the microphone is connected to the correct pins and speak into it. The output will be printed to the serial monitor.");
}

// loop function to read audio data from the microphone and print RMS and peak values
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
    raw[i] >>= SAMPLE_SHIFT;
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
    if (oledOk) drawLevel(0, 0, true);
    return;
  }
  if (oledOk) drawLevel(maxRms, maxPeak, false);
  int bars = min(maxRms / 20, 60);  // change 20 to rescale the bar
  Serial.printf("rms %5d  peak %6ld  ", maxRms, maxPeak);
  for (int i = 0; i < bars; i++) Serial.print('#');
  Serial.println();
  maxRms = 0;
  maxPeak = 0;
}