#include <Arduino.h>

void setup() {
  Serial.begin(115200);
}

void loop() {
  neopixelWrite(RGB_BUILTIN, 0, 0, 64);   // dim blue
  Serial.println("on");
  delay(500);
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);    // off
  Serial.println("off");
  delay(500);
}
