// SSD1306 128x64 I2C OLED test for the ESP32-S3, built by the "oled_test" environment
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// This example scans the I2C bus, prints any addresses it finds to the serial monitor,
// then draws "OLED OK", a border and a counting number on the display.

// in this example, the OLED is connected to the ESP32 as follows:
// green -> SDA (esp32, GPIO8)
// blue  -> SCL (esp32, GPIO9)

#define OLED_SDA 8      // green
#define OLED_SCL 9      // blue
#define OLED_ADDR 0x3C  // most 128x64 modules use 0x3C, some use 0x3D (check the scan output)
#define SCREEN_W 128
#define SCREEN_H 64

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);  // -1 = no reset pin

// setup function to scan the I2C bus and initialize the display
// will stop if the display fails to initialize
void setup() {
  Serial.begin(115200);
  delay(1500);  // give the serial monitor time to connect
  Wire.begin(OLED_SDA, OLED_SCL);

  // scan the I2C bus so you can see what address the display answers on
  Serial.println("Scanning I2C...");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("found device at 0x%02X\n", a);
      found++;
    }
  }
  if (found == 0) Serial.println("no I2C devices found, check SDA/SCL wiring and power");

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed, check the wiring and OLED_ADDR");
    while (true) delay(1000);
  }
  Serial.println("SSD1306 started");

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("OLED OK");
  display.drawRect(0, 12, SCREEN_W, SCREEN_H - 12, SSD1306_WHITE);
  display.display();  // nothing shows until this is called
}

// loop function to draw a counter so you can see the display updating
void loop() {
  static int n = 0;
  display.fillRect(4, 30, 120, 16, SSD1306_BLACK);  // erase the old number
  display.setCursor(4, 30);
  display.setTextSize(2);
  display.printf("%d", n++);
  display.display();
  delay(200);
}
