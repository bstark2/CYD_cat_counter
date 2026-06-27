 CYD (ESP32 Cheap Yellow Display) Starter Template
 
A beginner-friendly starter template for the ESP32 Cheap Yellow Display (CYD). This project demonstrates how to initialize the display, connect to WiFi, and handle touchscreen input — a solid base to build your own projects on.
 
---
 
## Hardware
 
This project is built for the **ESP32 Cheap Yellow Display (CYD)** — a low-cost ESP32 development board with an integrated 2.8" ILI9341 TFT display (320x240) and XPT2046 resistive touchscreen.
 
- [ESP32 Cheap Yellow Display on AliExpress](https://www.aliexpress.com/item/1005004502250619.html)
- [Brian Lough's CYD Repository](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) — excellent reference for hardware details and pinouts
---
 
## What This Template Does
 
- Initializes the TFT display and touchscreen
- Connects to WiFi and shows connection progress on screen
- Displays the assigned IP address on successful connection
- Reads touch input and prints the raw X, Y, and pressure values to both the display and serial monitor
---
 
## Prerequisites
 
### Software
 
- [VS Code](https://code.visualstudio.com/)
- [PlatformIO extension for VS Code](https://platformio.org/install/ide?install=vscode)
### Libraries
 
These are defined in `platformio.ini` and will be installed automatically by PlatformIO:
 
| Library | Purpose |
|---|---|
| [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) | Touchscreen driver |
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | TFT display driver |
| [ArduinoJson](https://arduinojson.org/) | JSON parsing (for future use) |
 
### TFT_eSPI Configuration
 
TFT_eSPI requires configuration to match the CYD's pinout. Copy the User_Setup.h file from the main directory into your projects .pio/libdeps/esp32dev/TFT_eSPI/ folder
 
---
 
## Setup
 
### 1. Clone the repository
 
```bash
git clone https://github.com/YOUR_USERNAME/YOUR_REPO_NAME.git
cd YOUR_REPO_NAME
```
 
### 2. Create your secrets file
 
Copy the template and fill in your credentials:
 
```bash
cp src/secrets_template.h src/secrets.h
```
 
Edit `src/secrets.h`:
 
```cpp
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_password"
```
 
> `secrets.h` is listed in `.gitignore` and will never be committed.
 
### 3. Open in VS Code with PlatformIO
 
Open the project folder in VS Code. PlatformIO will detect the `platformio.ini` and install dependencies automatically.
 
### 4. Upload
 
Connect your CYD via USB and click **Upload** in the PlatformIO toolbar, or run:
 
```bash
pio run --target upload
```
 
---
 
## Serial Monitor
 
Open the serial monitor at **115200 baud** to see touch coordinates and WiFi connection status logged in real time.
 
---
 
## A Note on Touch Coordinates
 
The XPT2046 returns raw ADC values, not pixel coordinates. Raw values are roughly in the range of **200–3800** on each axis. If you need to map touch input to screen coordinates, use the Arduino `map()` function:
 
```cpp
int screenX = map(p.x, 200, 3800, 0, 320);
int screenY = map(p.y, 200, 3800, 0, 240);
```
 
The exact min/max values vary between devices — you may need to calibrate for your specific board.
 
---
 
## Project Structure
 
```
├── src/
│   ├── main.cpp
│   ├── secrets.h          # Your WiFi credentials (gitignored)
├── platformio.ini
├── .gitignore
└── README.md
```
 
---
 
## License
 
MIT
