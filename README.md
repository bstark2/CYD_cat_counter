# Cat Treat Feeder Monitor (ESP32 Cheap Yellow Display)

A touchscreen dashboard for a cat treat feeder. Listens for MQTT "treat
dispensed" events, tracks how many treats have gone out in the last
hour and the last 24 hours, shows a live clock, and gives a little
on-screen celebration every time a treat fires.

Built for the **ESP32 Cheap Yellow Display (CYD)** — a low-cost ESP32
board with an integrated 2.8" ILI9341 TFT display (320x240) and an
XPT2046 resistive touchscreen.

- [ESP32 Cheap Yellow Display on AliExpress](https://www.aliexpress.com/item/1005004502250619.html)
- [Brian Lough's CYD Repository](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) — reference for hardware details and pinouts

---

## What This Does

**Stats screen** (default view)
- Treats dispensed in the last hour, and in the last 24 hours, in large digits
- Day of week, date, and a live 12-hour clock with AM/PM (e.g. `Friday, Jun 26` / `02:33:20 PM`)
- Time since the last treat (e.g. "Last treat: 12m ago")
- A small dot in the top-right corner: green once the clock has synced to a real time server, grey if it's still running on a fallback clock

**24-hour bar graph screen**
- One bar per hour for the last 24 hours, current hour on the left, oldest on the right
- Current hour's bar is highlighted; bar heights auto-scale to whatever the busiest hour was
- Tap anywhere to switch back to the stats screen

**Tap anywhere** on either screen to toggle between the two.

**On a treat event:** the screen briefly shows a "Treat Dispensed!"
celebration with some on-screen confetti, then returns to whichever
screen you were on. This build doesn't play sound or drive a motor —
it's a monitor only.

---

## How It Works (the short version)

- The device connects to WiFi, then to an MQTT broker, and subscribes
  to a `feeder/treat` topic. Any message with the payload `treat`
  counts as a dispensed treat.
- Every treat event is timestamped and stored in a rolling buffer
  (capacity: 500 events). Older entries naturally fall out of the
  "last hour" / "last 24h" windows as time passes.
- The device also syncs its clock against an NTP time server so the
  hour/day windows and the on-screen clock reflect actual elapsed
  time, not just "time since the board last rebooted." If NTP hasn't
  synced yet (e.g. right after boot), the screen shows placeholder
  dashes instead of a clock, and falls back to counting from boot
  internally until sync succeeds — at which point it quietly corrects
  any treats already logged so the counts stay accurate.
- The clock is set to Pacific Time (Los Angeles) with automatic
  daylight saving — see [Changing the Timezone](#changing-the-timezone) if you're deploying this elsewhere.

---

## Hardware Notes

This sketch only uses the touchscreen and TFT display from the CYD —
no stepper motor, no buzzer. If you're combining this with a physical
dispenser (motor + speaker), that logic lives in a separate sketch and
publishes to the same `feeder/treat` MQTT topic that this one listens
on.

---

## Prerequisites

### Software

- [VS Code](https://code.visualstudio.com/)
- [PlatformIO extension for VS Code](https://platformio.org/install/ide?install=vscode)

### Libraries

These are defined in `platformio.ini` and will be installed
automatically by PlatformIO:

| Library | Purpose |
|---|---|
| [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) | Touchscreen driver |
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | TFT display driver |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | MQTT client |

WiFi, secure (TLS) networking, and NTP time sync come from the
ESP32 Arduino core itself (`WiFi.h`, `WiFiClientSecure.h`, `time.h`) —
no extra library needed for those.

### TFT_eSPI Configuration

TFT_eSPI requires configuration to match the CYD's pinout. Copy the
`User_Setup.h` file from the main directory into your project's
`.pio/libdeps/esp32dev/TFT_eSPI/` folder.

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
#define MQTT_SERVER "your_broker_host"
#define MQTT_PORT 8883
#define MQTT_USER "your_mqtt_username"
#define MQTT_PASS "your_mqtt_password"
```

> `secrets.h` is listed in `.gitignore` and will never be committed.

### 3. Open in VS Code with PlatformIO

Open the project folder in VS Code. PlatformIO will detect the
`platformio.ini` and install dependencies automatically.

### 4. Upload

Connect your CYD via USB and click **Upload** in the PlatformIO
toolbar, or run:

```bash
pio run --target upload
```

---

## Serial Monitor

Open the serial monitor at **115200 baud** to see:

- WiFi connection status
- The device's generated MQTT client ID (unique per board, derived from its MAC address — see [Multiple Devices](#multiple-devices))
- MQTT connection state and any reconnect attempts, with [PubSubClient state codes](https://pubsubclient.knolleary.net/api#state) printed on failure
- NTP sync confirmation, including the UTC epoch and resolved local time, once the clock syncs
- Raw touch coordinates on tap

---

## MQTT Topic

| Topic | Payload | Effect |
|---|---|---|
| `feeder/treat` | `treat` | Logs a treat event, triggers the celebration screen |

Any other payload on that topic is ignored.

---

## Multiple Devices

Each board generates its own MQTT client ID at boot, built from the
last 3 octets of its MAC address (e.g. `dispens-A1B2C3`). This matters
if you run more than one device against the same broker — MQTT
brokers only allow one active connection per client ID, so two devices
sharing a hardcoded ID will silently fight over the connection and
keep kicking each other off. You shouldn't need to do anything here;
it's automatic.

---

## Changing the Timezone

The clock is currently set to Los Angeles / Pacific Time using a POSIX
TZ string, which automatically handles the switch between PST and PDT:

```cpp
const char* TIMEZONE_STRING = "PST8PDT,M3.2.0,M11.1.0";
```

To use a different timezone, replace this with the POSIX TZ string for
your region. A few common examples:

| Region | TZ String |
|---|---|
| US Eastern | `EST5EDT,M3.2.0,M11.1.0` |
| US Central | `CST6CDT,M3.2.0,M11.1.0` |
| US Mountain | `MST7MDT,M3.2.0,M11.1.0` |
| UK | `GMT0BST,M3.5.0/1,M10.5.0` |

A fuller list can be found in most POSIX timezone reference tables —
search "POSIX TZ string" plus your city or region.

---

## A Note on Touch Coordinates

The XPT2046 returns raw ADC values, not pixel coordinates. Raw values
are roughly in the range of **200–3800** on each axis. This sketch
only uses touch as a yes/no "was the screen tapped" signal to switch
screens, so it doesn't need to map coordinates to anything on-screen.
If you want to add tap-sensitive buttons later, you can map raw values
to pixel coordinates with the Arduino `map()` function:

```cpp
int screenX = map(p.x, 200, 3800, 0, 320);
int screenY = map(p.y, 200, 3800, 0, 240);
```

The exact min/max values vary between devices — you may need to
calibrate for your specific board.

---

## Known Limitations

- **No persistence across reboots.** Treat history and the synced
  clock both live in RAM. If the board loses power or resets, the
  treat counts reset to zero and the clock re-syncs from scratch (NTP
  sync is usually quick, but the rolling history is gone for good).
- **Buffer capacity.** The rolling treat-history buffer holds up to
  500 events. If a feeder logs more than that in under 24 hours, the
  oldest entries get overwritten and the day-count may briefly
  undercount until they'd have aged out anyway. Not a realistic
  concern for a treat dispenser, but worth knowing if you repurpose
  this for something higher-frequency.

---

## Project Structure

```
├── src/
│   ├── main.cpp
│   ├── secrets.h          # Your WiFi/MQTT credentials (gitignored)
├── platformio.ini
├── .gitignore
└── README.md
```

---

## License

MIT