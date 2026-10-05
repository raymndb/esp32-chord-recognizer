// ARCHIVED 2026-10-05: phase 2 (mic level meter on serial + OLED), kept unchanged for reference.
// Build it with the "phase2_level_meter" environment in platformio.ini.

// INMP441 I2S microphone to ESP32 using Arduino framework

// This example reads audio from an INMP441 I2S microphone and prints the RMS and peak values to PlatformIO's serial monitor.
// The serial output is a simple 'bar' graph (hashtags represent volume) of the RMS (root mean square) and peak values.
// The same values are drawn on an SSD1306 128x64 OLED: numbers on top, a level bar, and a scrolling history of the RMS below.

// ---------------------------------------------------------------------------------------------
// Libraries
// ---------------------------------------------------------------------------------------------

// core Arduino functions for the ESP32: Serial, delay(), min(), pin numbers, etc.
#include <Arduino.h>
// Espressif's I2S driver, used to clock audio data in from the microphone
#include <driver/i2s.h>
// Arduino I2C library ("Wire"), the 2-wire bus the OLED talks over
#include <Wire.h>
// Adafruit's graphics library: text, lines, rectangles, etc. (works with many displays)
#include <Adafruit_GFX.h>
// Adafruit's driver for the SSD1306 chip inside the OLED; it uses Adafruit_GFX for drawing
#include <Adafruit_SSD1306.h>

// ---------------------------------------------------------------------------------------------
// Microphone pins and audio settings
// ---------------------------------------------------------------------------------------------

// in this example, the INMP441 is connected to the ESP32 as follows:
// purple  -> I2S_SCK (esp32, GPIO4)
// orange  -> I2S_WS  (esp32, GPIO5)
// yellow  -> I2S_SD  (esp32, GPIO6)

// #define makes a named constant: the compiler swaps the name for the number everywhere it appears
// SCK = serial clock (bit clock): the ESP32 pulses this pin once per data bit
#define I2S_SCK 4   // purple
// WS = word select: tells the mic whether the current sample slot is the left or right channel
#define I2S_WS  5   // orange
// SD = serial data: the mic sends the audio bits back to the ESP32 on this pin
#define I2S_SD  6   // yellow

// samples per second; 16 kHz captures sounds up to 8 kHz, which covers guitar notes and their
// overtones while keeping the amount of data small (the INMP441 can go up to about 48 kHz)
#define SAMPLE_RATE 16000
// samples per read: 512 samples at 16 kHz = 32 ms of audio per block
#define BLOCK 512
// how far to shift each raw sample right (divide by 2^14 = 16384) to get a manageable number;
// raise it if loud strums peak near 32767, lower it if normal playing peaks only in the hundreds
#define SAMPLE_SHIFT 14

// ---------------------------------------------------------------------------------------------
// OLED pins and screen layout
// ---------------------------------------------------------------------------------------------

// the OLED is connected to the ESP32 as follows:
// green -> SDA (esp32, GPIO8)
// blue  -> SCL (esp32, GPIO9)

// SDA = I2C data line
#define OLED_SDA 8      // green
// SCL = I2C clock line
#define OLED_SCL 9      // blue
// the display's I2C address (found by the scan in oled_test.cpp)
#define OLED_ADDR 0x3C
// screen width in pixels
#define SCREEN_W 128
// screen height in pixels
#define SCREEN_H 64
// height in pixels of the scrolling history graph at the bottom of the screen
#define GRAPH_H 42
// RMS value that fills the whole bar/graph (same as 60 hashtags on serial); change to rescale
#define LEVEL_FULL_SCALE 1200

// ---------------------------------------------------------------------------------------------
// Global variables (visible to every function below)
// ---------------------------------------------------------------------------------------------

// buffer that i2s_read() fills with audio; each sample is a signed 32-bit integer
int32_t raw[BLOCK];

// the display object: 128x64 pixels, on the Wire (I2C) bus, -1 = no reset pin wired
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
// true once the display has started; if it fails, the mic still works over serial alone
bool oledOk = false;
// last 128 RMS values, already scaled to graph pixel heights, newest at the right (one per column)
uint8_t history[SCREEN_W];

// ---------------------------------------------------------------------------------------------
// drawLevel: draw the RMS/peak numbers, a level bar and the scrolling RMS history on the OLED
//   rms    - loudness of the last ~128 ms
//   peak   - biggest single sample in the last ~128 ms
//   noData - true if the mic sent only zeros
// ---------------------------------------------------------------------------------------------
void drawLevel(int rms, long peak, bool noData) {
  // wipe the screen buffer in the ESP32's memory (the screen itself doesn't change until display())
  display.clearDisplay();
  // smallest text size: each character is 6 px wide and 8 px tall
  display.setTextSize(1);
  // draw text with lit pixels (this display only has on/off pixels)
  display.setTextColor(SSD1306_WHITE);
  // move the text cursor to the top-left corner (x = 0, y = 0)
  display.setCursor(0, 0);
  // if the mic isn't sending anything...
  if (noData) {
    // ...write a warning on the screen...
    display.println("no data from mic");
    // ...send the buffer to the screen over I2C so it actually appears...
    display.display();
    // ...and leave the function early, skipping the bar and graph
    return;
  }
  // write the numbers: %5d = whole number padded to 5 characters, %6ld = long padded to 6
  display.printf("rms %5d  pk %6ld", rms, peak);

  // level bar
  // bar width in pixels: scale rms so LEVEL_FULL_SCALE = full width, and never go past the edge
  int w = min(rms * SCREEN_W / LEVEL_FULL_SCALE, SCREEN_W);
  // outline of the bar: x = 0, y = 10, full width, 10 px tall
  display.drawRect(0, 10, SCREEN_W, 10, SSD1306_WHITE);
  // filled part of the bar, w pixels wide, inside the outline
  display.fillRect(0, 10, w, 10, SSD1306_WHITE);

  // shift the history left one pixel and add the newest value on the right
  // copy entries 1..127 into 0..126, which drops the oldest value and frees up the last slot
  memmove(history, history + 1, SCREEN_W - 1);
  // newest value in the last slot, scaled so LEVEL_FULL_SCALE = GRAPH_H pixels tall, capped at GRAPH_H
  history[SCREEN_W - 1] = min(rms * GRAPH_H / LEVEL_FULL_SCALE, GRAPH_H);
  // go through each of the 128 columns of the screen
  for (int x = 0; x < SCREEN_W; x++) {
    // draw a vertical line up from the bottom edge, as tall as that column's value (skip zeros)
    if (history[x] > 0) display.drawFastVLine(x, SCREEN_H - history[x], history[x], SSD1306_WHITE);
  }
  // send the finished buffer to the screen over I2C (about 25 ms)
  display.display();
}

// ---------------------------------------------------------------------------------------------
// setup: runs once at power-on/reset. Starts serial, the OLED and the I2S microphone driver.
// Stops if the I2S driver fails to start (the OLED is optional).
// ---------------------------------------------------------------------------------------------
void setup() {
  // start the USB serial connection at 115200 bits per second (matches monitor_speed in platformio.ini)
  Serial.begin(115200);
  // wait 1.5 s so the serial monitor has time to connect before the first messages
  delay(1500);

  // start the OLED; if it fails, carry on with serial output only
  // start the I2C bus on the chosen data and clock pins
  Wire.begin(OLED_SDA, OLED_SCL);
  // start the display: SWITCHCAPVCC = generate the screen's high voltage on-board; returns true on success
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  // if the display started...
  if (oledOk) {
    // clear the screen buffer
    display.clearDisplay();
    // lit pixels for text
    display.setTextColor(SSD1306_WHITE);
    // top-left corner
    display.setCursor(0, 0);
    // write a start-up message
    display.println("starting mic...");
    // send it to the screen
    display.display();
  // ...otherwise
  } else {
    // report the problem but keep going (the mic doesn't need the display)
    Serial.println("SSD1306 init failed, continuing with serial output only");
  }

  // I2S settings; "= {}" sets every field to zero first so nothing is left as random memory
  i2s_config_t cfg = {};
  // MASTER = the ESP32 generates the clocks, RX = it receives data (the mic only sends)
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  // samples per second
  cfg.sample_rate = SAMPLE_RATE;
  // each sample arrives in a 32-bit slot (the INMP441 puts its 24-bit sample at the top of it)
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  // only read the left channel (the mic's L/R pin tied to GND = left)
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  // standard Philips I2S timing, which the INMP441 uses
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  // DMA = hardware copies samples into memory in the background, even while the CPU does other work;
  // 8 buffers...
  cfg.dma_buf_count = 8;
  // ...of 256 samples each = 2048 samples (128 ms) of slack before audio is lost
  cfg.dma_buf_len = 256;

  // which GPIO pins I2S uses; again "= {}" zeroes everything first
  i2s_pin_config_t pins = {};
  // no master clock (MCLK) needed for the INMP441
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  // bit clock pin
  pins.bck_io_num = I2S_SCK;
  // word select (left/right) pin
  pins.ws_io_num = I2S_WS;
  // no data output: the ESP32 isn't sending audio anywhere
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  // data input pin from the mic
  pins.data_in_num = I2S_SD;

  // install the driver on I2S port 0 (0 and NULL = no event queue) and assign the pins;
  // either call returning something other than ESP_OK means it failed
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK ||
      i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    // report the failure
    Serial.println("I2S setup failed, check the wiring and pin definitions");
    // stop here forever (waking once a second); there's no point running without the mic
    while (true) delay(1000);
  }
  // success message
  Serial.println("I2S started. Make sure the microphone is connected to the correct pins and speak into it. The output will be printed to the serial monitor.");
}

// ---------------------------------------------------------------------------------------------
// loop: runs over and over forever after setup(). Reads one block of audio, measures how loud it
// was, and every 4th block prints/draws the loudest values seen.
// ---------------------------------------------------------------------------------------------
void loop() {
  // counts blocks; "static" means it keeps its value between calls of loop() instead of resetting
  static int counter = 0;
  // i2s_read() will store how many bytes it actually delivered here
  size_t bytesRead = 0;
  // read up to one full block of audio into raw[]; portMAX_DELAY = wait as long as it takes for the data
  i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, portMAX_DELAY);
  // number of samples = bytes / 4 (each sample is 4 bytes = 32 bits)
  int n = bytesRead / 4;
  // nothing arrived: skip the rest of this loop() and try again
  if (n == 0) return;

  // The mic sends 24-bit audio in a 32-bit slot; shift each sample down to a smaller range
  // running total of all samples, used to find the average
  long sum = 0;
  // assume every sample is zero until one isn't (1 = true, 0 = false)
  int allZero = 1;
  // go through every sample in the block
  for (int i = 0; i < n; i++) {
    // shift right by SAMPLE_SHIFT bits (same as dividing by 16384), dropping the lowest, noisiest bits
    raw[i] >>= SAMPLE_SHIFT;
    // add it to the running total
    sum += raw[i];
    // any non-zero sample means the mic is sending something
    if (raw[i] != 0) allZero = 0;
  }
  // average value of the block = the DC offset (a constant shift the mic adds that isn't sound)
  long mean = sum / n;

  // running total of the squared samples (a double, because the total gets very large)
  double sq = 0;
  // biggest distance from the average seen in this block
  long peak = 0;
  // go through every sample again
  for (int i = 0; i < n; i++) {
    // remove the offset so silence sits at 0 and sound swings above and below it
    long s = raw[i] - mean;
    // square it (makes negatives positive and weights loud samples more) and add to the total
    sq += (double)s * s;
    // labs() = absolute value of a long; keep it if it's the biggest so far
    if (labs(s) > peak) peak = labs(s);
  }
  // RMS = square root of the average of the squares: a steady measure of loudness
  int rms = (int)sqrt(sq / n);

  // Keep the loudest block since the last print so short sounds (claps) aren't skipped
  // loudest RMS since the last print ("static" so it survives between loop() calls)
  static int maxRms = 0;
  // biggest peak since the last print
  static long maxPeak = 0;
  // remember this block's RMS if it's the loudest so far
  if (rms > maxRms) maxRms = rms;
  // remember this block's peak if it's the biggest so far
  if (peak > maxPeak) maxPeak = peak;

  // add 1 to the counter; unless it's now a multiple of 4, stop here.
  // 4 blocks x 32 ms = 128 ms, so the output updates about 8 times per second
  if (++counter % 4 != 0) return;

  // if every sample in the latest block was zero, the mic isn't sending data
  if (allZero) {
    // warn on serial
    Serial.println("all zeros: no data from mic");
    // and on the OLED, if it's working
    if (oledOk) drawLevel(0, 0, true);
    // skip the normal output
    return;
  }
  // draw the loudest values on the OLED, if it's working
  if (oledOk) drawLevel(maxRms, maxPeak, false);
  // number of hashtags for the serial bar: one per 20 RMS, at most 60 (change 20 to rescale the bar)
  int bars = min(maxRms / 20, 60);
  // print the numbers, padded to fixed widths so the bars line up
  Serial.printf("rms %5d  peak %6ld  ", maxRms, maxPeak);
  // print one '#' per bar
  for (int i = 0; i < bars; i++) Serial.print('#');
  // end the line
  Serial.println();
  // reset the loudest RMS ready for the next 4 blocks
  maxRms = 0;
  // reset the biggest peak ready for the next 4 blocks
  maxPeak = 0;
}
