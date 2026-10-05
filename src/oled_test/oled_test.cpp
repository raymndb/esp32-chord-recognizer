// SSD1306 128x64 I2C OLED test for the ESP32-S3, built by the "oled_test" environment

// This example scans the I2C bus, prints any addresses it finds to the serial monitor,
// then draws "OLED OK", a border and a counting number on the display.

// ---------------------------------------------------------------------------------------------
// Libraries
// ---------------------------------------------------------------------------------------------

// core Arduino functions for the ESP32: Serial, delay(), etc.
#include <Arduino.h>
// Arduino I2C library ("Wire"), the 2-wire bus the OLED talks over
#include <Wire.h>
// Adafruit's graphics library: text, lines, rectangles, etc.
#include <Adafruit_GFX.h>
// Adafruit's driver for the SSD1306 chip inside the OLED
#include <Adafruit_SSD1306.h>

// ---------------------------------------------------------------------------------------------
// Pins and screen settings
// ---------------------------------------------------------------------------------------------

// in this example, the OLED is connected to the ESP32 as follows:
// green -> SDA (esp32, GPIO8)
// blue  -> SCL (esp32, GPIO9)

// SDA = I2C data line
#define OLED_SDA 8      // green
// SCL = I2C clock line
#define OLED_SCL 9      // blue
// the display's I2C address: most 128x64 modules use 0x3C, some use 0x3D (check the scan output)
#define OLED_ADDR 0x3C
// screen width in pixels
#define SCREEN_W 128
// screen height in pixels
#define SCREEN_H 64

// the display object: 128x64 pixels, on the Wire (I2C) bus, -1 = no reset pin wired
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ---------------------------------------------------------------------------------------------
// setup: runs once at power-on/reset. Scans the I2C bus and starts the display.
// Stops if the display fails to start.
// ---------------------------------------------------------------------------------------------
void setup() {
  // start the USB serial connection at 115200 bits per second
  Serial.begin(115200);
  // wait 1.5 s so the serial monitor has time to connect
  delay(1500);
  // start the I2C bus on the chosen data and clock pins
  Wire.begin(OLED_SDA, OLED_SCL);

  // scan the I2C bus so you can see what address the display answers on
  // announce the scan
  Serial.println("Scanning I2C...");
  // how many devices answered
  int found = 0;
  // try every valid 7-bit I2C address (1 to 126)
  for (uint8_t a = 1; a < 127; a++) {
    // start a message to address a...
    Wire.beginTransmission(a);
    // ...and end it straight away; 0 means a device acknowledged (something lives at that address)
    if (Wire.endTransmission() == 0) {
      // print the address in hex, e.g. 0x3C (%02X = 2-digit uppercase hex)
      Serial.printf("found device at 0x%02X\n", a);
      // count it
      found++;
    }
  }
  // nothing answered at all: almost always a wiring or power problem
  if (found == 0) Serial.println("no I2C devices found, check SDA/SCL wiring and power");

  // start the display: SWITCHCAPVCC = generate the screen's high voltage on-board; false means it failed
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    // report the failure
    Serial.println("SSD1306 init failed, check the wiring and OLED_ADDR");
    // stop here forever (waking once a second)
    while (true) delay(1000);
  }
  // success message
  Serial.println("SSD1306 started");

  // wipe the screen buffer in the ESP32's memory
  display.clearDisplay();
  // smallest text size: 6 px wide, 8 px tall per character
  display.setTextSize(1);
  // draw text with lit pixels
  display.setTextColor(SSD1306_WHITE);
  // move the text cursor to the top-left corner
  display.setCursor(0, 0);
  // write the message
  display.println("OLED OK");
  // draw a rectangle outline: x = 0, y = 12, full width, the rest of the height
  display.drawRect(0, 12, SCREEN_W, SCREEN_H - 12, SSD1306_WHITE);
  // send the buffer to the screen over I2C; nothing shows until this is called
  display.display();
}

// ---------------------------------------------------------------------------------------------
// loop: runs over and over forever. Draws a counter so you can see the display updating.
// ---------------------------------------------------------------------------------------------
void loop() {
  // the number to show; "static" keeps its value between calls of loop()
  static int n = 0;
  // erase the old number by drawing a black (unlit) rectangle over it
  display.fillRect(4, 30, 120, 16, SSD1306_BLACK);
  // move the cursor to where the number goes
  display.setCursor(4, 30);
  // double-size text: 12 px wide, 16 px tall per character
  display.setTextSize(2);
  // write the number, then add 1 to it for next time
  display.printf("%d", n++);
  // send the buffer to the screen
  display.display();
  // wait 200 ms, so the number goes up 5 times a second
  delay(200);
}
