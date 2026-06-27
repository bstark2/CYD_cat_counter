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

#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>

#include "secrets.h"
// A file that contains the WiFi/MQTT credentials. It should be in the same directory
// as this file and contain the following lines:
// #define WIFI_SSID "your_wifi_ssid"
// #define WIFI_PASSWORD "your_wifi_password"
// #define MQTT_SERVER "your_broker_host"
// #define MQTT_PORT 8883
// #define MQTT_USER "your_user"
// #define MQTT_PASS "your_pass"

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
// Treat tracking config
// ----------------------------

// Max number of treat timestamps we keep in the rolling buffer.
// 24h worth of treats at a sane real-world ceiling (e.g. dispensed
// no faster than once every few seconds) - generous but bounded so a
// stuck MQTT message can't grow this without limit.
#define MAX_TREAT_LOG 500

// Every entry is stored as epoch seconds. Until NTP sync succeeds, we
// fake the epoch using millis()/1000 (so it just counts up from 0 at
// boot). This still lets the ring buffer / window math work correctly
// in relative terms - it's just not "real" time yet. See
// correctBufferForTimeSync() for what happens once sync lands.
unsigned long treatTimestamps[MAX_TREAT_LOG];
int treatLogHead = 0;   // next write index (ring buffer)
int treatLogCount = 0;  // how many valid entries are currently stored

const unsigned long ONE_HOUR_S = 3600UL;
const unsigned long ONE_DAY_S  = 86400UL;

// Celebration banner state
bool showingCelebration = false;
unsigned long celebrationStartedAt = 0;
const unsigned long CELEBRATION_DURATION_MS = 2500;

// Track when we last redrew the stats, so we're not redrawing every loop
unsigned long lastStatsRedraw = 0;
const unsigned long STATS_REDRAW_INTERVAL_MS = 1000;

// ----------------------------
// Screen navigation
// ----------------------------

// Two screens: the main stats view, and a 24h hourly bar graph.
// Tapping anywhere toggles between them.
enum Screen { SCREEN_STATS, SCREEN_GRAPH };
Screen currentScreen = SCREEN_STATS;

// The bar graph doesn't need to redraw every second like the clock does -
// it only meaningfully changes when a treat is logged or an hour
// boundary passes. Redrawing it slowly avoids needless flicker/CPU.
unsigned long lastGraphRedraw = 0;
const unsigned long GRAPH_REDRAW_INTERVAL_MS = 30000;

// Simple touch debounce: ignore repeated touch events that are part of
// the same physical tap, so one tap doesn't bounce back and forth
// between screens multiple times.
unsigned long lastTouchHandledAt = 0;
const unsigned long TOUCH_DEBOUNCE_MS = 400;

// ----------------------------
// Time sync (NTP) state
// ----------------------------

// We try to get real wall-clock time from an NTP server so "last hour"
// and "last 24 hours" mean actual elapsed time, not "time since boot."
// If sync hasn't succeeded yet, we fall back to a millis()-based fake
// epoch so the app still works - just measuring from boot instead of
// from 1970. Once sync succeeds, every existing buffer entry gets
// shifted by the difference, and all future entries use the real clock.

bool timeIsSynced = false;
unsigned long lastTimeSyncAttempt = 0;
const unsigned long TIME_SYNC_RETRY_INTERVAL_MS = 5000;    // retry every 5s until first success
const unsigned long TIME_RESYNC_INTERVAL_MS = 4UL * 3600000UL; // re-sync every 4 hours after that
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
// Los Angeles (Pacific Time). PST8PDT means standard offset is 8 hours
// behind UTC, with "PDT" daylight saving observed using the US DST
// rules (the trailing M3.2.0/M11.1.0 is implied as the default in
// most libc/newlib implementations bundled with ESP32 toolchains, but
// we spell it out explicitly here so it can't silently fall back to
// "no DST" on a toolchain that doesn't assume US rules).
// DST starts 2nd Sunday in March, ends 1st Sunday in November.
const char* TIMEZONE_STRING = "PST8PDT,M3.2.0,M11.1.0";

// Returns the current "best guess" epoch second, real or fake.
unsigned long currentEpoch() {
  if (timeIsSynced) {
    time_t now;
    time(&now);
    return (unsigned long)now;
  }
  return millis() / 1000UL;
}

// Attempts a single NTP sync. Non-blocking-ish: configTime() kicks off
// the sync, then we poll for a short bounded window (a few seconds at
// most) rather than indefinitely, so the UI never hangs on this.
//
// The TZ environment variable is set here (not just once in setup())
// because setting it is cheap and idempotent, and keeping it next to
// the sync call means there's no risk of the offset and the sync
// drifting out of sync if this function is ever refactored.
bool attemptTimeSync() {
  // Order matters here: configTime() first kicks off NTP and may set
  // its own internal TZ state, so we set ours *after* to make sure it
  // isn't silently overwritten.
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  setenv("TZ", TIMEZONE_STRING, 1);
  tzset();

  time_t now;
  // A handful of short checks, not a long blocking wait - if it hasn't
  // landed within ~1.5s, we bail and let loop() retry later.
  for (int i = 0; i < 15; i++) {
    time(&now);
    if (now > 1700000000) { // sane "this is roughly post-2023" sanity check
      struct tm debugTm;
      localtime_r(&now, &debugTm);
      char debugBuf[32];
      strftime(debugBuf, sizeof(debugBuf), "%Y-%m-%d %H:%M:%S %Z", &debugTm);
      Serial.print("Time synced. UTC epoch=");
      Serial.print((unsigned long)now);
      Serial.print(" local=");
      Serial.println(debugBuf);
      return true;
    }
    delay(100);
  }
  return false;
}

// Called exactly once, the moment sync first succeeds. Shifts every
// existing buffer entry by the gap between the fake epoch and the real
// one, so already-logged treats land at roughly the correct real time
// instead of jumping to 1970 or "just now."
void correctBufferForTimeSync() {
  time_t realNow;
  time(&realNow);
  unsigned long fakeNow = millis() / 1000UL;
  long offset = (long)realNow - (long)fakeNow; // could be large; that's fine, it's one-time

  for (int i = 0; i < treatLogCount; i++) {
    treatTimestamps[i] = (unsigned long)((long)treatTimestamps[i] + offset);
  }
}

// ----------------------------

// ----------------------------
// Forward declarations
// ----------------------------
// The Arduino IDE auto-generates these for you, but PlatformIO compiles
// this as a normal C++ translation unit, which requires a function to
// be declared before it's used. Declaring them all up front means the
// order functions are defined in further down doesn't matter.
void setup_wifi();
void mqttCallback(char* topic, byte* message, unsigned int length);
void mqttReconnect();
void drawStatsScreen();
void updateStatsValues();
void drawGraphScreen();
void drawGraphBars();
void drawCelebration();

SPIClass mySpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

TFT_eSPI tft = TFT_eSPI();

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

// Unique per-device MQTT client ID, built from the chip's MAC address
// in setup(). Using a fixed literal ("dispens") here caused two
// physical devices on the same broker to fight over one client ID -
// each new connect would silently evict the other (state -3,
// "connection lost", from the loser's perspective).
char mqttClientId[32];

// ----------------------------
// Treat log helpers
// ----------------------------

// Records a treat event "now" into the ring buffer.
void logTreat() {
  treatTimestamps[treatLogHead] = currentEpoch();
  treatLogHead = (treatLogHead + 1) % MAX_TREAT_LOG;
  if (treatLogCount < MAX_TREAT_LOG) {
    treatLogCount++;
  }
  // Note: if the buffer fills up (MAX_TREAT_LOG treats without a restart),
  // the oldest entries get overwritten. At that point counts may undercount
  // slightly until old entries naturally age out. Raise MAX_TREAT_LOG if
  // your feeder is busier than that.
}

// Counts how many logged treats happened within the last `windowSeconds`.
// Unsigned subtraction correctly handles any rollover case.
int countTreatsInWindow(unsigned long windowSeconds) {
  unsigned long now = currentEpoch();
  int count = 0;
  for (int i = 0; i < treatLogCount; i++) {
    int idx = (treatLogHead - 1 - i + MAX_TREAT_LOG) % MAX_TREAT_LOG;
    unsigned long age = now - treatTimestamps[idx];
    if (age <= windowSeconds) {
      count++;
    } else {
      // Entries are stored in chronological order in this walk
      // (newest first), so once we hit one outside the window,
      // every entry after it is even older - safe to stop early.
      break;
    }
  }
  return count;
}

// Buckets the last 24 hours of treats into hourly bins for the bar
// graph. outCounts[0] = current hour (0-59 min ago), outCounts[1] =
// 1 hour ago, ... outCounts[23] = 23 hours ago. Caller provides a
// 24-element array to fill.
void bucketTreatsByHour(int outCounts[24]) {
  for (int i = 0; i < 24; i++) {
    outCounts[i] = 0;
  }

  unsigned long now = currentEpoch();
  for (int i = 0; i < treatLogCount; i++) {
    int idx = (treatLogHead - 1 - i + MAX_TREAT_LOG) % MAX_TREAT_LOG;
    unsigned long age = now - treatTimestamps[idx];
    if (age >= ONE_DAY_S) {
      // Chronological walk (newest first) - once we're past 24h,
      // everything further back is even older, so we can stop.
      break;
    }
    int bucket = age / ONE_HOUR_S; // 0..23
    outCounts[bucket]++;
  }
}

// ----------------------------
// WiFi
// ----------------------------

void setup_wifi() {
  int x = 320 / 2;
  int fontSize = 2;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Connecting to:", x, 80, fontSize);
  tft.drawCentreString(WIFI_SSID, x, 100, fontSize);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int dotCount = 0;
  int dotX = 120;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    tft.fillCircle(dotX + (dotCount * 20), 130, 3, TFT_WHITE);
    dotCount = (dotCount + 1) % 8;
    if (dotCount == 0) {
      tft.fillRect(dotX, 120, 160, 20, TFT_BLACK);
    }
  }

  tft.fillScreen(TFT_BLACK);
  tft.drawCentreString("Connected!", x, 80, fontSize);
  tft.drawCentreString(WiFi.localIP().toString().c_str(), x, 100, fontSize);
  delay(1500);
}

// ----------------------------
// MQTT
// ----------------------------

void mqttCallback(char* topic, byte* message, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)message[i];
  }

  if (String(topic) == "feeder/treat" && msg == "treat") {
    logTreat();
    showingCelebration = true;
    celebrationStartedAt = millis();
  }
}

void mqttReconnect() {
  int x = 320 / 2;
  tft.fillRect(0, 200, 320, 20, TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawCentreString("Connecting to MQTT...", x, 200, 1);

  Serial.print("MQTT state before reconnect attempt: ");
  Serial.println(mqttClient.state());
  // PubSubClient state() codes:
  //  -4 MQTT_CONNECTION_TIMEOUT     -3 MQTT_CONNECTION_LOST
  //  -2 MQTT_CONNECT_FAILED         -1 MQTT_DISCONNECTED
  //   0 MQTT_CONNECTED               1 MQTT_CONNECT_BAD_PROTOCOL
  //   2 MQTT_CONNECT_BAD_CLIENT_ID   3 MQTT_CONNECT_UNAVAILABLE
  //   4 MQTT_CONNECT_BAD_CREDENTIALS 5 MQTT_CONNECT_UNAUTHORIZED

  if (mqttClient.connect(mqttClientId, MQTT_USER, MQTT_PASS)) {
    mqttClient.subscribe("feeder/treat");
    tft.fillRect(0, 200, 320, 20, TFT_BLACK);
    Serial.println("MQTT reconnected OK");
  } else {
    Serial.print("MQTT reconnect failed, state: ");
    Serial.println(mqttClient.state());
  }
  // Note: this is a single attempt, not a blocking retry loop, so the
  // touchscreen stays responsive. loop() will call this again next pass
  // if it's still not connected, with a short delay to avoid hammering
  // the broker.
}

// ----------------------------
// Display - stats screen
// ----------------------------

void drawStatsScreen() {
  int x = 320 / 2;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Treat Tracker", x, 8, 4);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawCentreString("Last Hour", x - 80, 60, 2);
  tft.drawCentreString("Last 24 Hours", x + 80, 60, 2);

  // Hint that the screen is tappable for the graph view
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawCentreString("Tap for 24h graph", x, 218, 1);

  lastStatsRedraw = millis();
}

void updateStatsValues() {
  int x = 320 / 2;
  int hourCount = countTreatsInWindow(ONE_HOUR_S);
  int dayCount = countTreatsInWindow(ONE_DAY_S);

  // Big numbers - cleared and redrawn each pass
  tft.fillRect(x - 150, 78, 140, 60, TFT_BLACK);
  tft.fillRect(x + 10, 78, 140, 60, TFT_BLACK);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawCentreString(String(hourCount), x - 80, 82, 7);
  tft.drawCentreString(String(dayCount), x + 80, 82, 7);

  // Live clock + date, only meaningful once NTP sync has succeeded -
  // shows placeholders otherwise rather than a misleading fake time.
  tft.fillRect(0, 130, 320, 50, TFT_BLACK);

  String dateText;
  String clockText;
  if (timeIsSynced) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // Day of week + date, e.g. "Friday, Jun 26"
    char dateBuf[24];
    strftime(dateBuf, sizeof(dateBuf), "%A, %b %d", &timeinfo);
    dateText = String(dateBuf);

    // 12-hour clock with AM/PM, e.g. "02:33:20 PM"
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = (timeinfo.tm_hour < 12) ? "AM" : "PM";
    char clockBuf[16];
    snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d:%02d %s", hour12, timeinfo.tm_min, timeinfo.tm_sec, ampm);
    clockText = String(clockBuf);
  } else {
    dateText = "---, --- --";
    clockText = "--:--:-- --";
  }

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawCentreString(dateText, x, 132, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawCentreString(clockText, x, 152, 4);

  // Last treat info
  tft.fillRect(0, 184, 320, 20, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (treatLogCount > 0) {
    int lastIdx = (treatLogHead - 1 + MAX_TREAT_LOG) % MAX_TREAT_LOG;
    unsigned long secondsAgo = currentEpoch() - treatTimestamps[lastIdx];
    String agoText;
    if (secondsAgo < 60) {
      agoText = "Last treat: " + String(secondsAgo) + "s ago";
    } else if (secondsAgo < 3600) {
      agoText = "Last treat: " + String(secondsAgo / 60) + "m ago";
    } else {
      agoText = "Last treat: " + String(secondsAgo / 3600) + "h ago";
    }
    tft.drawCentreString(agoText, x, 187, 2);
  } else {
    tft.drawCentreString("No treats logged yet", x, 187, 2);
  }

  // Tiny, unobtrusive clock-sync indicator. Doesn't interrupt anything -
  // just a quiet dot so you can tell at a glance whether times are
  // "real" yet if you ever need to debug.
  tft.fillRect(300, 5, 15, 15, TFT_BLACK);
  tft.fillCircle(308, 12, 3, timeIsSynced ? TFT_GREEN : TFT_DARKGREY);

  // This is what the 1-second gate in loop() checks against. Without
  // updating it here, that gate only "resets" when drawStatsScreen()
  // runs, which meant after the first second elapsed, this function
  // got called on every single pass through loop() instead of once
  // per second - that was the redraw-too-fast bug.
  lastStatsRedraw = millis();
}

// ----------------------------
// Display - 24h bar graph screen
// ----------------------------

void drawGraphScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Last 24 Hours", 320 / 2, 8, 4);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawCentreString("Tap for stats", 320 / 2, 224, 1);

  drawGraphBars();
  lastGraphRedraw = millis();
}

// Draws (or redraws) just the bars and axis labels - separated from
// drawGraphScreen() so a data refresh doesn't need to redraw the
// titles/hint text each time, same pattern as the stats screen.
void drawGraphBars() {
  int counts[24];
  bucketTreatsByHour(counts);

  int maxCount = 1; // avoid divide-by-zero; 1 keeps bars readable if all-zero
  for (int i = 0; i < 24; i++) {
    if (counts[i] > maxCount) maxCount = counts[i];
  }

  // Chart area
  const int chartTop = 30;
  const int chartBottom = 195;
  const int chartHeight = chartBottom - chartTop;
  const int chartLeft = 10;
  const int chartWidth = 300;
  const int barGap = 2;
  const int barWidth = (chartWidth / 24) - barGap;

  // Clear chart area only (not the title/hint text above and below it)
  tft.fillRect(0, chartTop - 2, 320, chartHeight + 4, TFT_BLACK);

  for (int i = 0; i < 24; i++) {
    // i=0 is the current hour - drawn on the far left, with older
    // hours progressing to the right. "now" anchors the left edge.
    int barX = chartLeft + i * (barWidth + barGap);
    int barHeight = (counts[i] * chartHeight) / maxCount;
    if (counts[i] > 0 && barHeight < 2) barHeight = 2; // keep tiny counts visible

    uint16_t barColor = (i == 0) ? TFT_GREEN : TFT_CYAN; // highlight current hour
    if (barHeight > 0) {
      tft.fillRect(barX, chartBottom - barHeight, barWidth, barHeight, barColor);
    }
  }

  // A few axis labels along the bottom: "now", "-12h", "-24h"
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("now", chartLeft, chartBottom + 4, 1);
  tft.drawCentreString("-12h", 320 / 2, chartBottom + 4, 1);
  tft.drawRightString("-24h", chartLeft + chartWidth, chartBottom + 4, 1);
}

void drawCelebration() {
  int x = 320 / 2;
  int y = 120;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString("Treat Dispensed!", x, y - 20, 4);

  // Simple "confetti" - small filled circles scattered around.
  // Deterministic-ish but varied enough to feel festive without
  // needing a true RNG seed dependency.
  uint16_t confettiColors[] = {TFT_YELLOW, TFT_GREEN, TFT_CYAN, TFT_MAGENTA, TFT_ORANGE};
  for (int i = 0; i < 24; i++) {
    int cx = (i * 37 + 13) % 320;
    int cy = (i * 53 + 7) % 240;
    tft.fillCircle(cx, cy, 3, confettiColors[i % 5]);
  }

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Good kitty!", x, y + 20, 2);
}

// ----------------------------
// Setup
// ----------------------------

void setup() {
  Serial.begin(115200);

  mySpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(mySpi);
  ts.setRotation(1);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  setup_wifi();

  // Build a unique MQTT client ID from this device's MAC address, e.g.
  // "dispens-A1B2C3". Needs WiFi initialized first (setup_wifi() just
  // returned, so this is safe). Using a fixed ID let two physical
  // devices fight over the same broker session - whichever reconnected
  // most recently would silently evict the other.
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String idStr = "dispens-" + mac.substring(6); // last 3 octets, plenty unique
  idStr.toCharArray(mqttClientId, sizeof(mqttClientId));
  Serial.print("MQTT client ID: ");
  Serial.println(mqttClientId);

  secureClient.setInsecure(); // Only for dev / self-signed certs
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  // Try once to get real time. If it fails, we proceed anyway using the
  // millis()-based fallback clock - loop() will keep retrying in the
  // background and silently upgrade everything once it succeeds.
  lastTimeSyncAttempt = millis();
  if (attemptTimeSync()) {
    timeIsSynced = true;
    // Nothing logged yet at this point in setup(), so no correction needed.
  }

  drawStatsScreen();
  updateStatsValues();
}

// ----------------------------
// Loop
// ----------------------------

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
    if (currentScreen == SCREEN_STATS) {
      drawStatsScreen();
      updateStatsValues();
    } else {
      drawGraphScreen();
    }
  }

  // Time sync: retry every minute until first success, then quietly
  // re-sync every few hours afterward to correct for drift. Either way
  // this never blocks the UI for more than the ~1.5s bounded check
  // inside attemptTimeSync().
  unsigned long syncInterval = timeIsSynced ? TIME_RESYNC_INTERVAL_MS : TIME_SYNC_RETRY_INTERVAL_MS;
  if (millis() - lastTimeSyncAttempt > syncInterval) {
    lastTimeSyncAttempt = millis();
    bool wasSynced = timeIsSynced;
    if (attemptTimeSync()) {
      if (!wasSynced) {
        // First successful sync - shift any treats logged so far from
        // fake-epoch to real-epoch, then flip the flag.
        correctBufferForTimeSync();
        timeIsSynced = true;
      }
      // If we were already synced, this was just a routine resync -
      // nothing to correct, system time itself is already updated.
    }
  }

  if (!mqttClient.connected()) {
    static unsigned long lastAttempt = 0;
    if (millis() - lastAttempt > 5000) {
      lastAttempt = millis();
      mqttReconnect();
    }
  }
  mqttClient.loop();

  // Touch input - tap anywhere toggles between the stats screen and
  // the 24h bar graph. Debounced so one physical tap (which the touch
  // controller can report as several rapid events) doesn't bounce
  // back and forth between screens.
  if (ts.tirqTouched() && ts.touched()) {
    if (millis() - lastTouchHandledAt > TOUCH_DEBOUNCE_MS) {
      lastTouchHandledAt = millis();
      TS_Point p = ts.getPoint();
      Serial.print("Touch x=");
      Serial.print(p.x);
      Serial.print(" y=");
      Serial.println(p.y);

      if (currentScreen == SCREEN_STATS) {
        currentScreen = SCREEN_GRAPH;
        drawGraphScreen();
      } else {
        currentScreen = SCREEN_STATS;
        drawStatsScreen();
        updateStatsValues();
      }
    }
  }

  // Celebration banner takes over the screen temporarily, then we
  // return to whichever screen was active before it fired.
  if (showingCelebration) {
    drawCelebration();
    while (millis() - celebrationStartedAt < CELEBRATION_DURATION_MS) {
      mqttClient.loop(); // keep MQTT alive during the celebration pause
      delay(10);
    }
    showingCelebration = false;
    if (currentScreen == SCREEN_STATS) {
      drawStatsScreen();
      updateStatsValues();
    } else {
      drawGraphScreen();
    }
  } else if (currentScreen == SCREEN_STATS) {
    if (millis() - lastStatsRedraw > STATS_REDRAW_INTERVAL_MS) {
      updateStatsValues();
    }
  } else { // SCREEN_GRAPH
    if (millis() - lastGraphRedraw > GRAPH_REDRAW_INTERVAL_MS) {
      drawGraphBars();
      lastGraphRedraw = millis();
    }
  }
}