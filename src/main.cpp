// ----------------------------
// Standard Libraries
// ----------------------------

#include <SPI.h>

// ----------------------------
// Additional Libraries - each one of these will need to be installed.
// ----------------------------

#include <XPT2046_Touchscreen.h>
// A library for interfacing with the touch screen
//
// Can be installed from the library manager (Search for "XPT2046")
//https://github.com/PaulStoffregen/XPT2046_Touchscreen

#include <TFT_eSPI.h>
// A library for interfacing with LCD displays
//
// Can be installed from the library manager (Search for "TFT_eSPI")
//https://github.com/Bodmer/TFT_eSPI


#include <WiFi.h>
// A library for connecting to WiFi networks

#include "secrets.h" 
// A file that contains the WiFi credentials. It should be in the same directory as this file and contain the following lines:
// #define WIFI_SSID "your_wifi_ssid"
// #define WIFI_PASSWORD "your_wifi_password"





// ----------------------------
// Touch Screen pins
// ----------------------------

// The CYD touch uses some non default
// SPI pins

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// ----------------------------

SPIClass mySpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

TFT_eSPI tft = TFT_eSPI();

void setup() {
  Serial.begin(115200);

  // Start the SPI for the touch screen and init the TS library
  mySpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(mySpi);
  ts.setRotation(1);

  // Start the tft display and set it to black
  tft.init();
  tft.setRotation(1); //This is the display in landscape

  // Clear the screen before writing to it
  tft.fillScreen(TFT_BLACK);

  int x = 320 / 2;
  int fontSize = 2;

  // Show SSID being connected to
  tft.drawCentreString("Connecting to:", x, 80, fontSize);
  tft.drawCentreString(WIFI_SSID, x, 100, fontSize);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int dotCount = 0;
  int dotX = 120; // starting x for dots
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    tft.fillCircle(dotX + (dotCount * 20), 130, 3, TFT_WHITE);
    dotCount = (dotCount + 1) % 8; // reset after 8 dots
    if (dotCount == 0) {
      // clear the dot row when resetting
      tft.fillRect(dotX, 120, 160, 20, TFT_BLACK);
    }
  }

  tft.fillScreen(TFT_BLACK);
  tft.drawCentreString("Connected!", x, 80, fontSize);
  tft.drawCentreString(WiFi.localIP().toString().c_str(), x, 100, fontSize);
  delay(2000);

  tft.fillScreen(TFT_BLACK);
  tft.drawCentreString("Touch Screen to Start", x, 100, fontSize);
}

void printTouchToSerial(TS_Point p) {
  Serial.print("Pressure = ");
  Serial.print(p.z);
  Serial.print(", x = ");
  Serial.print(p.x);
  Serial.print(", y = ");
  Serial.print(p.y);
  Serial.println();
}

void printTouchToDisplay(TS_Point p) {

  // Clear screen first
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  int x = 320 / 2; // center of display
  int y = 100;
  int fontSize = 2;

  String temp = "Pressure = " + String(p.z);
  tft.drawCentreString(temp, x, y, fontSize);

  y += 16;
  temp = "X = " + String(p.x);
  tft.drawCentreString(temp, x, y, fontSize);

  y += 16;
  temp = "Y = " + String(p.y);
  tft.drawCentreString(temp, x, y, fontSize);
}

void loop() {
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point p = ts.getPoint();
    printTouchToSerial(p);
    printTouchToDisplay(p);
    delay(100);

  }
}