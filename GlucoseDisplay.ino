/*
  Glucose Display: ESP32 + 5641AS 4-digit 7-segment display
  Shows the latest FreeStyle Libre 3 reading from LibreLinkUp.

  Display:
    " 112-"  value in mg/dL (milligrams per deciliter) + trend in the last digit
    "----"   no reading yet, or the newest reading is over 10 minutes old
    "Er 2"   problem (only shown when there's no fresh reading):
               Er 1  Wi-Fi not connected
               Er 2  LibreLinkUp login rejected (check email/password)
               Er 3  Terms need accepting: open the LibreLinkUp app and log in
               Er 4  Nobody is sharing with this account yet
               Er 5  Server or network error
               Er 6  Too many requests; waiting before retrying

  Trend symbols in the last digit:
    upper loop = rising fast, top bar = rising, middle bar = steady,
    bottom bar = falling, lower loop = falling fast

  Needs: ESP32 board package 3.3.12 or newer, and the ArduinoJson library (v7).
  This is a convenience display only; keep relying on the Libre 3 app's alarms.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "secrets.h"

#ifndef ESP_ARDUINO_VERSION_VAL
#error "Update the esp32 board package (Tools > Board > Boards Manager > esp32 by Espressif) to 3.3.12 or newer."
#elif ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 12)
#error "Update the esp32 board package (Tools > Board > Boards Manager > esp32 by Espressif) to 3.3.12 or newer."
#endif

// ---------------- Settings ----------------
const uint8_t SEG_PINS[7] = {13, 14, 27, 26, 25, 33, 32};  // segments A,B,C,D,E,F,G
const uint8_t DIG_PINS[4] = {23, 22, 21, 19};              // digit 1 (left) .. digit 4 (right)

const uint32_t POLL_INTERVAL_MS = 60UL * 1000;   // how often to ask for a new reading
const uint32_t STALE_AFTER_S    = 10UL * 60;     // show "----" if reading older than this
const char*    DEFAULT_API_BASE = "https://api-us.libreview.io";
const char*    DEFAULT_API_VER  = "4.16.0";      // raised automatically if Abbott asks for newer

// ---------------- Pure display/parsing logic ----------------
// LOGIC BEGIN
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Segment bits: bit0=A, bit1=B, bit2=C, bit3=D, bit4=E, bit5=F, bit6=G
static const uint8_t DIGIT_GLYPHS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
static const uint8_t GLYPH_DASH = 0x40, GLYPH_E = 0x79, GLYPH_r = 0x50, GLYPH_H = 0x76, GLYPH_I = 0x06;

// LibreLinkUp TrendArrow: 1 falling fast, 2 falling, 3 steady, 4 rising, 5 rising fast
static uint8_t trendGlyph(int trend) {
  switch (trend) {
    case 1: return 0x1C;  // C+D+E: lower loop
    case 2: return 0x08;  // D: bottom bar
    case 3: return 0x40;  // G: middle bar
    case 4: return 0x01;  // A: top bar
    case 5: return 0x23;  // A+B+F: upper loop
    default: return 0x00;
  }
}

static void encodeDashes(uint8_t out[4]) {
  for (int i = 0; i < 4; i++) out[i] = GLYPH_DASH;
}

static void encodeError(int code, uint8_t out[4]) {
  out[0] = GLYPH_E;
  out[1] = GLYPH_r;
  out[2] = 0;
  out[3] = DIGIT_GLYPHS[code % 10];
}

static void encodeReading(int mgdl, int trend, uint8_t out[4]) {
  out[3] = trendGlyph(trend);
  if (mgdl <= 0) { out[0] = out[1] = out[2] = GLYPH_DASH; return; }
  if (mgdl > 999) { out[0] = 0; out[1] = GLYPH_H; out[2] = GLYPH_I; return; }
  out[0] = mgdl >= 100 ? DIGIT_GLYPHS[(mgdl / 100) % 10] : 0;
  out[1] = mgdl >= 10 ? DIGIT_GLYPHS[(mgdl / 10) % 10] : 0;
  out[2] = DIGIT_GLYPHS[mgdl % 10];
}

// Days since 1970-01-01 for a calendar date (proleptic Gregorian).
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Parses LibreLinkUp's "FactoryTimestamp" (UTC), e.g. "11/3/2026 9:12:34 PM".
// Returns true and sets *epoch on success.
static bool parseLibreTimestamp(const char* s, int64_t* epoch) {
  int mo, d, y, h, mi, se;
  char ampm[3] = {0};
  int n = sscanf(s, "%d/%d/%d %d:%d:%d %2s", &mo, &d, &y, &h, &mi, &se, ampm);
  if (n < 6) return false;
  if (n == 7) {
    bool pm = (ampm[0] == 'P' || ampm[0] == 'p');
    bool am = (ampm[0] == 'A' || ampm[0] == 'a');
    if (!pm && !am) return false;
    if (h < 1 || h > 12) return false;
    if (am && h == 12) h = 0;
    if (pm && h != 12) h += 12;
  }
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || y < 2000 || h > 23 || mi > 59 || se > 60) return false;
  *epoch = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
  return true;
}
// LOGIC END

// ---------------- Display refresh (runs on a 2 ms timer) ----------------
volatile uint8_t displayBuf[4] = {0, 0, 0, 0};

static void refreshDisplay(void*) {
  static uint8_t d = 0;
  digitalWrite(DIG_PINS[d], HIGH);                 // current digit off (common cathode)
  d = (d + 1) & 3;
  uint8_t bits = displayBuf[d];
  for (int s = 0; s < 7; s++) digitalWrite(SEG_PINS[s], (bits >> s) & 1);
  digitalWrite(DIG_PINS[d], LOW);                  // next digit on
}

static void showBuffer(const uint8_t b[4]) {
  for (int i = 0; i < 4; i++) displayBuf[i] = b[i];
}

// ---------------- LibreLinkUp client ----------------
WiFiClientSecure tls;

String apiBase = DEFAULT_API_BASE;
String apiVersion = DEFAULT_API_VER;
String authToken;
String accountId;           // SHA-256 of the user ID, required by the API since Oct 2025
int64_t tokenExpires = 0;   // Unix time; 0 = unknown

struct Reading {
  bool valid = false;
  int mgdl = 0;
  int trend = 0;
  int64_t epoch = 0;        // sensor time (UTC), 0 if unparsed
  String rawTimestamp;
  uint32_t changedAtMs = 0; // millis() when the timestamp last changed
} reading;

int lastError = 0;
uint32_t lastPollMs = 0;
bool firstPoll = true;
uint32_t loginBlockStartMs = 0;
uint32_t loginBlockMs = 0;      // 0 = not blocked

static String sha256Hex(const String& in) {
  uint8_t out[32];
  mbedtls_sha256((const unsigned char*)in.c_str(), in.length(), out, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", out[i]);
  hex[64] = 0;
  return String(hex);
}

static bool clockSynced() { return time(nullptr) > 1700000000; }

static void addCommonHeaders(HTTPClient& http) {
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("cache-control", "no-cache");
  http.addHeader("product", "llu.android");
  http.addHeader("version", apiVersion);
}

// If the server says our app version is too old, adopt the version it asks for.
static bool checkMinimumVersion(JsonDocument& doc) {
  const char* minVer = doc["data"]["minimumVersion"];
  if (minVer && apiVersion != minVer) {
    Serial.printf("Server requires app version %s, switching.\n", minVer);
    apiVersion = minVer;
    return true;
  }
  return false;
}

static void blockLogins(uint32_t ms) { loginBlockStartMs = millis(); loginBlockMs = ms; }

static bool login() {
  if (loginBlockMs && millis() - loginBlockStartMs < loginBlockMs) return false;
  loginBlockMs = 0;

  for (int attempt = 0; attempt < 3; attempt++) {
    JsonDocument body;
    body["email"] = LLU_EMAIL;
    body["password"] = LLU_PASSWORD;
    String bodyStr;
    serializeJson(body, bodyStr);

    HTTPClient http;
    http.begin(tls, apiBase + "/llu/auth/login");
    http.setTimeout(15000);
    addCommonHeaders(http);
    int code = http.POST(bodyStr);
    String payload;
    if (code > 0) payload = http.getString();
    http.end();
    Serial.printf("Login HTTP %d\n", code);

    if (code == 429) { lastError = 6; blockLogins(15UL * 60 * 1000); return false; }
    if (code <= 0 || payload.length() == 0) { lastError = 5; blockLogins(60UL * 1000); return false; }

    JsonDocument filter;
    filter["status"] = true;
    filter["data"]["redirect"] = true;
    filter["data"]["region"] = true;
    filter["data"]["minimumVersion"] = true;
    filter["data"]["step"]["type"] = true;
    filter["data"]["user"]["id"] = true;
    filter["data"]["authTicket"]["token"] = true;
    filter["data"]["authTicket"]["expires"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
      lastError = 5; blockLogins(60UL * 1000); return false;
    }

    if (checkMinimumVersion(doc)) continue;  // retry with the newer version

    if (doc["data"]["redirect"] | false) {      // account lives in another region
      const char* region = doc["data"]["region"];
      if (!region) { lastError = 5; return false; }
      apiBase = String("https://api-") + region + ".libreview.io";
      Serial.printf("Region redirect to %s\n", apiBase.c_str());
      continue;
    }

    int status = doc["status"] | -1;
    if (status == 2) {                          // wrong email or password
      lastError = 2;
      blockLogins(30UL * 60 * 1000);            // back off hard to avoid locking the account
      return false;
    }
    if (status == 4) {                          // terms of use / privacy policy to accept
      lastError = 3;
      blockLogins(10UL * 60 * 1000);
      return false;
    }

    const char* token = doc["data"]["authTicket"]["token"];
    const char* userId = doc["data"]["user"]["id"];
    if (status != 0 || !token || !userId) {
      Serial.println(payload.substring(0, 300));
      lastError = 5; blockLogins(5UL * 60 * 1000); return false;
    }

    authToken = token;
    accountId = sha256Hex(userId);
    tokenExpires = (int64_t)(doc["data"]["authTicket"]["expires"] | 0L);
    Serial.println("Logged in.");
    return true;
  }
  lastError = 5;
  return false;
}

static bool tokenUsable() {
  if (authToken.length() == 0) return false;
  if (tokenExpires > 0 && clockSynced() && (int64_t)time(nullptr) > tokenExpires - 300) return false;
  return true;
}

// Returns true if a reading was received.
static bool fetchReading() {
  HTTPClient http;
  http.begin(tls, apiBase + "/llu/connections");
  http.setTimeout(15000);
  addCommonHeaders(http);
  http.addHeader("Authorization", "Bearer " + authToken);
  http.addHeader("Account-Id", accountId);
  int code = http.GET();
  String payload;
  if (code > 0) payload = http.getString();
  http.end();
  Serial.printf("Connections HTTP %d\n", code);

  if (code == 401) { authToken = ""; return false; }   // token rejected: log in again
  if (code == 429) { lastError = 6; return false; }
  if (code <= 0 || payload.length() == 0) { lastError = 5; return false; }

  if (payload.indexOf("minimumVersion") >= 0) {   // app version too old
    JsonDocument v;
    if (!deserializeJson(v, payload)) checkMinimumVersion(v);
    lastError = 5;
    return false;                                 // next poll uses the new version
  }

  JsonDocument filter;
  filter["status"] = true;
  filter["data"][0]["glucoseMeasurement"]["ValueInMgPerDl"] = true;
  filter["data"][0]["glucoseMeasurement"]["TrendArrow"] = true;
  filter["data"][0]["glucoseMeasurement"]["FactoryTimestamp"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) { lastError = 5; return false; }

  JsonArray conns = doc["data"].as<JsonArray>();
  if (conns.isNull()) {
    if ((doc["status"] | 0) != 0) authToken = "";  // e.g. terms changed; log in again
    lastError = 5;
    return false;
  }
  if (conns.size() == 0) { lastError = 4; return false; }

  JsonObject gm = conns[0]["glucoseMeasurement"];
  int mgdl = gm["ValueInMgPerDl"] | 0;
  if (mgdl <= 0) { lastError = 5; return false; }

  String ts = gm["FactoryTimestamp"] | "";
  if (!reading.valid || ts != reading.rawTimestamp) reading.changedAtMs = millis();
  reading.rawTimestamp = ts;
  reading.mgdl = mgdl;
  reading.trend = gm["TrendArrow"] | 0;
  int64_t e = 0;
  reading.epoch = parseLibreTimestamp(ts.c_str(), &e) ? e : 0;
  reading.valid = true;
  lastError = 0;
  Serial.printf("Glucose %d mg/dL, trend %d, at %s UTC\n", reading.mgdl, reading.trend, ts.c_str());
  if (clockSynced() && reading.epoch > 0) {
    Serial.printf("Reading is %lld seconds old\n", (long long)((int64_t)time(nullptr) - reading.epoch));
  }
  return true;
}

static void poll() {
  if (!tokenUsable() && !login()) return;
  if (!fetchReading() && authToken.length() == 0) {
    if (login()) fetchReading();   // one immediate retry after a fresh login
  }
}

static bool readingIsFresh() {
  if (!reading.valid) return false;
  if (clockSynced() && reading.epoch > 0) {
    return (int64_t)time(nullptr) - reading.epoch <= (int64_t)STALE_AFTER_S;
  }
  // No clock yet: fall back to "the timestamp has changed recently"
  return millis() - reading.changedAtMs <= STALE_AFTER_S * 1000UL;
}

static void updateDisplay() {
  uint8_t b[4];
  if (readingIsFresh())  encodeReading(reading.mgdl, reading.trend, b);
  else if (lastError)    encodeError(lastError, b);
  else                   encodeDashes(b);
  showBuffer(b);
}

// ---------------- Arduino entry points ----------------
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 7; i++) { pinMode(SEG_PINS[i], OUTPUT); digitalWrite(SEG_PINS[i], LOW); }
  for (int i = 0; i < 4; i++) { pinMode(DIG_PINS[i], OUTPUT); digitalWrite(DIG_PINS[i], HIGH); }

  esp_timer_create_args_t args = {};
  args.callback = &refreshDisplay;
  args.name = "display";
  esp_timer_handle_t timer;
  esp_timer_create(&args, &timer);
  esp_timer_start_periodic(timer, 2000);  // 2 ms per digit = 125 Hz full refresh

  uint8_t test[4] = {0x7F, 0x7F, 0x7F, 0x7F};  // "8888" wiring check
  showBuffer(test);
  delay(1500);
  uint8_t dashes[4];
  encodeDashes(dashes);
  showBuffer(dashes);

  tls.useBuiltinCACertBundle();  // verify Abbott's HTTPS certificate

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // UTC clock for staleness checks
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    lastError = 1;
    updateDisplay();
    delay(500);
    return;
  }
  if (lastError == 1) lastError = 0;

  if (firstPoll || millis() - lastPollMs >= POLL_INTERVAL_MS) {
    firstPoll = false;
    lastPollMs = millis();
    poll();
  }
  updateDisplay();
  delay(200);
}
