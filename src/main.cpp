// INMP441 I2S microphone -> ESP32-S3 -> USB serial -> Python, using Arduino framework (phase 3)

// This example records clips of raw audio and sends them to the computer over the USB serial port.
// The Python script scripts/record_clip.py asks for a clip, receives the bytes, checks that none went
// missing, and saves them as a 16 kHz, mono, 16-bit WAV file.
// The serial port carries ONLY audio (plus a small header and footer), never debug text: one stray
// printed character would shift every byte after it and turn the audio into noise. Status goes to the OLED.
// The phase 2 level meter is saved in src/archive/2026-10-05_phase2_level_meter/main.cpp.

// How one recording travels over the serial port:
//   computer -> ESP32:  'R' + how many samples it wants                                   =  5 bytes
//   ESP32 -> computer:  header: "CLIP" + sample rate + number of samples + clip number    = 16 bytes
//                       audio:  number of samples x 2 bytes (16-bit samples)
//                       footer: "DONE" + samples sent + I2S overflows + clipped samples   = 16 bytes
// every number is a 4-byte unsigned integer sent lowest byte first ("little-endian").
// "CLIP" is the start marker: Python skips any bytes until it sees it, so it always knows where a clip begins.

// ---------------------------------------------------------------------------------------------
// Libraries
// ---------------------------------------------------------------------------------------------

// core Arduino functions for the ESP32: Serial, delay(), millis(), min(), pin numbers, etc.
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

// SCK = serial clock (bit clock): the ESP32 pulses this pin once per data bit
#define I2S_SCK 4   // purple
// WS = word select: tells the mic whether the current sample slot is the left or right channel
#define I2S_WS  5   // orange
// SD = serial data: the mic sends the audio bits back to the ESP32 on this pin
#define I2S_SD  6   // yellow

// samples per second; 16 kHz captures sounds up to 8 kHz, which covers guitar notes and their overtones
#define SAMPLE_RATE 16000
// samples per read: 512 samples at 16 kHz = 32 ms of audio per block
#define BLOCK 512
// how far to shift each raw 32-bit sample right to turn it into a 16-bit sample.
// 16 would keep exactly the top 16 bits; 14 makes everything 4x (12 dB) louder, because the INMP441 is
// quiet at guitar volumes. Raise it if the clip report shows clipped samples, lower it if clips are too quiet.
#define SAMPLE_SHIFT 14
// DMA buffers: hardware copies samples into these in the background while the CPU does other work.
// 8 buffers x 256 samples = 2048 samples (128 ms) of slack before audio is lost.
// If record_clip.py reports I2S overflows (heard as gaps), raise DMA_BUF_COUNT (up to 128).
#define DMA_BUF_COUNT 8
#define DMA_BUF_LEN   256
// longest clip the ESP32 will send in one go; longer requests are cut down to this.
// 300 s = 5 minutes, long enough for one phase 4 take (the audio streams straight out, so length costs no RAM)
#define MAX_CLIP_SECONDS 300

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
// sound level that fills the whole level bar; change to rescale
#define LEVEL_FULL_SCALE 8000
// how often (in milliseconds) the screen is redrawn while waiting for a request
#define OLED_IDLE_MS 250

// ---------------------------------------------------------------------------------------------
// Global variables (visible to every function below)
// ---------------------------------------------------------------------------------------------

// buffer that i2s_read() fills with audio; each sample is a signed 32-bit integer
int32_t raw[BLOCK];
// the same block converted to 16-bit samples, ready to send
int16_t pcm[BLOCK];

// the display object: 128x64 pixels, on the Wire (I2C) bus, -1 = no reset pin wired
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
// true once the display has started; if it fails, recording still works without it
bool oledOk = false;
// millis() time of the last screen redraw, so the idle screen only updates every OLED_IDLE_MS
unsigned long lastDraw = 0;

// the I2S driver posts "events" to this queue; we watch it for overflows (audio lost because we were too slow)
QueueHandle_t i2sEvents;

// true while a clip is being sent
bool recording = false;
// counts clips since power-on; sent in the header so Python can show it
uint32_t clipNumber = 0;
// how many samples the current (or last) clip should contain
uint32_t samplesWanted = 0;
// how many samples of the current clip have been taken from the mic and handed to serial
uint32_t samplesTaken = 0;
// how many samples of the current (or last) clip have actually been written to serial
uint32_t samplesSent = 0;
// how many times the I2S buffer overflowed during the current (or last) clip
uint32_t overflows = 0;
// how many samples were too loud for 16 bits and got cut off (clipped) during the current (or last) clip
uint32_t clipped = 0;

// ---------------------------------------------------------------------------------------------
// putU32: store a 32-bit number into 4 bytes, lowest byte first (little-endian), for headers/footers
//   buf   - where the 4 bytes go
//   value - the number to store
// ---------------------------------------------------------------------------------------------
void putU32(uint8_t *buf, uint32_t value) {
  // lowest 8 bits
  buf[0] = value & 0xFF;
  // next 8 bits (shift down by 8, then keep the lowest 8)
  buf[1] = (value >> 8) & 0xFF;
  // next 8 bits
  buf[2] = (value >> 16) & 0xFF;
  // highest 8 bits
  buf[3] = (value >> 24) & 0xFF;
}

// ---------------------------------------------------------------------------------------------
// countOverflows: empty the I2S event queue and return how many "receive overflow" events were in it.
// An overflow means all the DMA buffers filled up before we read them, so some audio was thrown away.
// ---------------------------------------------------------------------------------------------
uint32_t countOverflows() {
  // number of overflow events found
  uint32_t count = 0;
  // one event taken from the queue
  i2s_event_t event;
  // keep taking events until the queue is empty (0 = don't wait if there are none)
  while (xQueueReceive(i2sEvents, &event, 0) == pdTRUE) {
    // only overflows matter; other events (like "a buffer is ready") are ignored
    if (event.type == I2S_EVENT_RX_Q_OVF) count++;
  }
  // hand back the total
  return count;
}

// ---------------------------------------------------------------------------------------------
// drawStatus: show the recorder's state and the current/last clip's numbers on the OLED
//   level - current sound level, drawn as a bar while waiting (ignored while recording)
// ---------------------------------------------------------------------------------------------
void drawStatus(int level) {
  // wipe the screen buffer in the ESP32's memory (the screen itself doesn't change until display())
  display.clearDisplay();
  // move the text cursor to the top-left corner (x = 0, y = 0)
  display.setCursor(0, 0);
  // first line: what the recorder is doing
  display.println(recording ? "RECORDING..." : "ready for record_clip");
  // only show clip numbers once there has been a clip
  if (clipNumber > 0) {
    // clip number and length in seconds; (unsigned long) makes the number match %lu exactly
    display.printf("clip #%lu  %.1f s\n", (unsigned long)clipNumber, samplesWanted / (float)SAMPLE_RATE);
    // samples sent so far out of the samples wanted (only final once the clip is done)
    display.printf("sent %lu/%lu\n", (unsigned long)samplesSent, (unsigned long)samplesWanted);
    // I2S overflows: should be 0, otherwise audio has gaps
    display.printf("overflows %lu\n", (unsigned long)overflows);
    // clipped samples: should be 0 or close to it, otherwise the audio is distorted
    display.printf("clipped %lu\n", (unsigned long)clipped);
  }
  // while waiting, draw a live level bar so you can check the mic works before recording
  if (!recording) {
    // text cursor to the left edge, 5 lines down
    display.setCursor(0, 42);
    // the level as a number
    display.printf("level %d", level);
    // bar width in pixels: scale level so LEVEL_FULL_SCALE = full width, and never go past the edge
    int w = min(level * SCREEN_W / LEVEL_FULL_SCALE, SCREEN_W);
    // outline of the bar: x = 0, y = 52, full width, 12 px tall (to the bottom of the screen)
    display.drawRect(0, 52, SCREEN_W, 12, SSD1306_WHITE);
    // filled part of the bar, w pixels wide, inside the outline
    display.fillRect(0, 52, w, 12, SSD1306_WHITE);
  }
  // send the finished buffer to the screen over I2C (about 25 ms)
  display.display();
}

// ---------------------------------------------------------------------------------------------
// startRecording: reset the clip counters and send the header (start marker + length) to Python
//   requested - how many samples Python asked for
// ---------------------------------------------------------------------------------------------
void startRecording(uint32_t requested) {
  // never send more than MAX_CLIP_SECONDS of audio; at least 1 sample so the clip isn't empty
  samplesWanted = constrain(requested, (uint32_t)1, (uint32_t)MAX_CLIP_SECONDS * SAMPLE_RATE);
  // nothing taken or sent yet
  samplesTaken = 0;
  samplesSent = 0;
  // no overflows yet
  overflows = 0;
  // no clipped samples yet
  clipped = 0;
  // this is a new clip
  clipNumber++;

  // build the 16-byte header
  uint8_t header[16];
  // bytes 0-3: the start marker "CLIP", which Python searches for
  memcpy(header, "CLIP", 4);
  // bytes 4-7: samples per second, so Python saves the WAV at the right speed
  putU32(header + 4, SAMPLE_RATE);
  // bytes 8-11: how many samples will follow (the length), so Python knows how many bytes to expect
  putU32(header + 8, samplesWanted);
  // bytes 12-15: the clip number
  putU32(header + 12, clipNumber);
  // send the header as raw bytes (not text)
  Serial.write(header, sizeof(header));

  // from now on, loop() sends audio instead of waiting
  recording = true;
  // show "RECORDING..." (the screen is not redrawn again until the clip is done, to keep the loop fast)
  if (oledOk) drawStatus(0);
}

// ---------------------------------------------------------------------------------------------
// finishRecording: send the footer (end marker + the clip's final counts) and go back to waiting
// ---------------------------------------------------------------------------------------------
void finishRecording() {
  // build the 16-byte footer
  uint8_t footer[16];
  // bytes 0-3: the end marker "DONE"; if Python doesn't find it right after the audio, bytes went missing
  memcpy(footer, "DONE", 4);
  // bytes 4-7: how many samples the ESP32 actually managed to write to serial
  putU32(footer + 4, samplesSent);
  // bytes 8-11: how many times the I2S buffer overflowed (audio lost before it could be sent)
  putU32(footer + 8, overflows);
  // bytes 12-15: how many samples were clipped (too loud for 16 bits)
  putU32(footer + 12, clipped);
  // send the footer as raw bytes
  Serial.write(footer, sizeof(footer));

  // back to waiting
  recording = false;
  // throw away anything Python sent during the clip, so it can't accidentally start a second clip
  while (Serial.available()) Serial.read();
  // show the final numbers (level 0 until the next idle redraw)
  if (oledOk) drawStatus(0);
  // count the idle redraw timer from now
  lastDraw = millis();
}

// ---------------------------------------------------------------------------------------------
// checkForRequest: look for a 5-byte request ('R' + 4-byte sample count) from Python and start a clip
// ---------------------------------------------------------------------------------------------
void checkForRequest() {
  // throw away any bytes that aren't the start of a request (e.g. leftovers from an earlier session)
  while (Serial.available() && Serial.peek() != 'R') Serial.read();
  // wait until the whole request has arrived (it usually comes in all at once)
  if (Serial.available() < 5) return;
  // take the 'R' off the front
  Serial.read();
  // the 4 bytes of the sample count
  uint8_t b[4];
  // read them
  Serial.readBytes(b, 4);
  // rebuild the number, lowest byte first (the reverse of putU32)
  uint32_t requested = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
  // send the header and switch to recording
  startRecording(requested);
}

// ---------------------------------------------------------------------------------------------
// setup: runs once at power-on/reset. Starts serial, the OLED and the I2S microphone driver.
// Nothing is printed to serial (that would corrupt the audio stream); errors are shown on the OLED.
// ---------------------------------------------------------------------------------------------
void setup() {
  // give serial a 32 KB send buffer (default 256 bytes) = 1 second of audio. If the computer stops reading
  // for a moment (Windows busy, OneDrive syncing), audio waits here instead of being lost: Serial.write()
  // gives up after 100 ms when the buffer is full, and those samples would be missing from the clip.
  Serial.setTxBufferSize(32768);
  // start serial; on the ESP32-S3's USB port the number is ignored and data moves at full USB speed.
  // (16-bit audio at 16 kHz is 32,000 bytes/s, far more than a real 115200 baud UART could carry)
  Serial.begin(115200);

  // start the OLED; if it fails, carry on without it
  // start the I2C bus on the chosen data and clock pins
  Wire.begin(OLED_SDA, OLED_SCL);
  // start the display: SWITCHCAPVCC = generate the screen's high voltage on-board; returns true on success
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  // if the display started...
  if (oledOk) {
    // lit pixels for text (stays set for every later drawStatus())
    display.setTextColor(SSD1306_WHITE);
    // clear the screen buffer
    display.clearDisplay();
    // top-left corner
    display.setCursor(0, 0);
    // write a start-up message
    display.println("starting mic...");
    // send it to the screen
    display.display();
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
  // number of DMA buffers...
  cfg.dma_buf_count = DMA_BUF_COUNT;
  // ...and samples in each one
  cfg.dma_buf_len = DMA_BUF_LEN;

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

  // install the driver on I2S port 0 with an 8-slot event queue (so we can spot overflows) and assign
  // the pins; either call returning something other than ESP_OK means it failed
  if (i2s_driver_install(I2S_NUM_0, &cfg, 8, &i2sEvents) != ESP_OK ||
      i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    // report the failure on the OLED (not serial!)
    if (oledOk) {
      // clear the screen buffer
      display.clearDisplay();
      // top-left corner
      display.setCursor(0, 0);
      // the error message
      display.println("I2S setup failed:\ncheck the wiring and\npin definitions");
      // send it to the screen
      display.display();
    }
    // stop here forever (waking once a second); there's no point running without the mic
    while (true) delay(1000);
  }
}

// ---------------------------------------------------------------------------------------------
// loop: runs over and over forever after setup(). Always reads one block of audio (so the DMA buffers
// never fill up with old sound), then either sends it as part of a clip or checks for a new request.
// ---------------------------------------------------------------------------------------------
void loop() {
  // i2s_read() will store how many bytes it actually delivered here
  size_t bytesRead = 0;
  // read up to one full block of audio into raw[]; portMAX_DELAY = wait as long as it takes for the data
  i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, portMAX_DELAY);
  // number of samples = bytes / 4 (each sample is 4 bytes = 32 bits)
  int n = bytesRead / 4;
  // nothing arrived: skip the rest of this loop() and try again
  if (n == 0) return;

  // convert the 32-bit samples to 16-bit ones, and find the quietest and loudest for the level bar
  // lowest sample in the block (starts at the top of the range so any sample is lower)
  int16_t lo = INT16_MAX;
  // highest sample in the block (starts at the bottom of the range so any sample is higher)
  int16_t hi = INT16_MIN;
  // go through every sample in the block
  for (int i = 0; i < n; i++) {
    // shift right by SAMPLE_SHIFT bits, dropping the lowest, noisiest bits
    int32_t s = raw[i] >> SAMPLE_SHIFT;
    // a 16-bit sample can only hold -32768..32767; cut off (clip) anything outside that range
    s = constrain(s, INT16_MIN, INT16_MAX);
    // store the 16-bit sample
    pcm[i] = s;
    // keep the lowest so far
    if (s < lo) lo = s;
    // keep the highest so far
    if (s > hi) hi = s;
  }
  // check for overflows every block, so old ones don't pile up in the queue
  uint32_t newOverflows = countOverflows();

  // not recording: look for a request and keep the screen up to date, then wait for the next block
  if (!recording) {
    // start a clip if Python asked for one
    checkForRequest();
    // redraw the idle screen every OLED_IDLE_MS (and not if a clip just started: it drew its own screen)
    if (oledOk && !recording && millis() - lastDraw >= OLED_IDLE_MS) {
      // level = half the distance from lowest to highest sample (ignores the mic's DC offset)
      drawStatus((hi - lo) / 2);
      // remember when we drew
      lastDraw = millis();
    }
    // done with this block
    return;
  }

  // recording: add this block's overflows to the clip's total
  overflows += newOverflows;
  // send the whole block, or only what's left of the clip if that's less
  int toSend = min((uint32_t)n, samplesWanted - samplesTaken);
  // count clipped samples among the ones being sent (they were cut to exactly the min or max value)
  for (int i = 0; i < toSend; i++) {
    // a sample sitting at the very edge of the range was (almost certainly) clipped
    if (pcm[i] == INT16_MAX || pcm[i] == INT16_MIN) clipped++;
  }
  // write the samples as raw bytes (2 per sample, lowest byte first); returns how many bytes got through
  size_t written = Serial.write((uint8_t *)pcm, toSend * 2);
  // count them; if the computer stops reading, fewer bytes get through and Python will notice the gap
  samplesSent += written / 2;
  // count everything we tried to send, so a stalled computer can't make the clip run forever
  samplesTaken += toSend;
  // the whole clip has gone through: send the footer and go back to waiting
  if (samplesTaken >= samplesWanted) finishRecording();
}
