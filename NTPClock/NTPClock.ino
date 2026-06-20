// Smart Board — Hebrew word clock
//
// Displays the current time in Hebrew words
// on a 7.5" e-ink display (Seeed XIAO e-paper driver, model 502).
//
// Time source : NTP (internet time). The device first joins your WiFi as a
//               station; on every (re)connect it pulls UTC from NTP and the
//               Israel timezone (with DST) is applied for display.
// WiFi setup  : on first boot (or after "Reconfigure WiFi") the device opens
//               its own "HebrewClock" WiFi access point (192.168.4.1 — a
//               captive portal opens automatically). Pick your network from
//               the list, enter the password, and tap Connect. Credentials
//               are stored in NVS and reused on every boot; the device then
//               connects to your WiFi and syncs the time from NTP.
//
// Update cadence: checks every second, redraws on minute change. SNTP keeps
//                 the soft-clock in sync automatically in the background.
// No deep sleep — once connected, a small status page stays reachable.
//
// SETUP NOTE — partial refresh:
//   To enable partial-refresh updates, add this line to your library
//   setup file (e.g. User_Setups/Setup502_Seeed_XIAO_EPaper_7inch5.h):
//
//     #define USE_PARTIAL_EPAPER
//
//   Without it, epaper.updataPartial() below won't compile.
//   (Yes, "updata" — that's the actual name in the library.)
//
#include "driver.h"
#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <Fonts/Custom/NotoSerifHebrew_Bold_85.h>
#include "time_words.h"
#include "hebrew_date.h"

#ifdef EPAPER_ENABLE

EPaper epaper = EPaper();

// ── WiFi setup-portal config (only used when no creds are stored) ─────
#define AP_SSID     "HebrewClock"
#define AP_PASSWORD ""         // empty = open network

// ── Factory reset ─────────────────────────────────────────────────────
// Uncomment, flash once to wipe all stored settings (WiFi creds + date
// mode) from NVS, then comment it out again and re-flash for normal use.
// #define RESET_SETTINGS 1

// ── Timezone (Israel: UTC+2 standard, UTC+3 DST) ─────────────────────
const char* timeZone = "IST-2IDT,M3.4.4/26,M10.5.0";

// ── NTP servers ───────────────────────────────────────────────────────
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.google.com";

// ── Connection timeouts ───────────────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT_MS 20000UL   // give up joining WiFi after this
#define NTP_SYNC_TIMEOUT_MS     15000UL   // give up the first NTP wait after this

// ── Hardware / service instances ──────────────────────────────────────
WebServer   server(80);
DNSServer   dnsServer;
Preferences prefs;
static const byte DNS_PORT = 53;

// ── Date-display mode (persisted in NVS) ─────────────────────────────
// false = Gregorian date on the top row, true = Hebrew (Jewish) date.
static bool showHebrewDate   = false;
static bool pendingFullRefresh = false;   // set when the date line must be redrawn

// ── Layout ────────────────────────────────────────────────────────────
#define SCREEN_W           800
#define SCREEN_H           480
#define TEXT_SCALE         1
#define FONT_BASE_H        85
#define HEBREW_SPACE_W     20
#define LINE_GAP           24
#define FONT_ASCENT        55
#define FONT_DESCENT        7
#define TIME_BOX_X         0
#define TIME_BOX_Y         60
#define TIME_BOX_W         800
#define TIME_BOX_H         400
#define MAX_LINES          3
#define FULL_REFRESH_EVERY 15

// ── Date line (drawn above the time, in a smaller font) ───────────────
// The Hebrew font is a fixed 85px bitmap, so "smaller" is done by
// downscaling glyphs to NUM/DEN of full size (1/2 ≈ 42px here).
#define DATE_SCALE_NUM     1
#define DATE_SCALE_DEN     2
#define DATE_BASELINE_Y    48     // baseline in the 0..60 strip above the time box

// ── Time font scale (NUM/DEN of the 85px font); 16/17 ≈ 80px ──────────
#define TIME_SCALE_NUM     16
#define TIME_SCALE_DEN     17

// ── Display state ─────────────────────────────────────────────────────
static bool firstDraw     = true;
static int  partialCount  = 0;
static char prevLines[MAX_LINES][64];
static int  prevLineCount = 0;

// ── Runtime state ─────────────────────────────────────────────────────
static bool timeReady           = false;   // true once NTP has set the clock
static bool portalMode          = false;   // true while the AP setup portal runs
static int  lastDisplayedMinute = -1;
static unsigned long pendingRestartMs = 0;  // 0 = none; else millis() deadline to ESP.restart()

// ── Types used by Hebrew RTL + niqud rendering ───────────────────────
struct GlyphMetrics {
  uint8_t  width;
  uint8_t  height;
  uint8_t  xAdvance;
  int8_t   xOffset;
  int8_t   yOffset;
};

// ── Forward declarations ──────────────────────────────────────────────
void applyTimeZone();
bool connectWiFi(const String& ssid, const String& pass);
void initNTP();
bool waitForTime(unsigned long timeoutMs);
void startConfigPortal();
void startStatusServer();
void handleRoot();
void handleScan();
void handleConnect();
void handleForget();
void handleTimeJson();
void handleDateMode();
void drawMessage(const String& title, const String& msg);
void drawTimeInWords(const struct tm& t, bool fullRefresh);
void drawCenteredLines(const String& l1, const String& l2, const String& l3, int numLines);
void splitTimePhrase(const struct tm& t, String& line1, String& line2, String& line3);
String buildDateString(const struct tm& t);
void drawDateLine(const struct tm& t);
void drawError(const String& msg);

// ──────────────────────────────────────────────────────
//  setup() — entry point on every wake
// ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);

  epaper.begin(0);
  epaper.setRotation(0);
  applyTimeZone();

  // Load persisted settings before the first draw so the top row uses the
  // chosen date mode immediately.
  prefs.begin("hebtime", false);

#ifdef RESET_SETTINGS
  // Wipe stored WiFi credentials and date mode, then continue with defaults
  // (this boot opens the setup portal). Remember to disable the flag and
  // re-flash afterwards, or every boot will clear settings again.
  prefs.clear();
  Serial.println("RESET_SETTINGS — cleared stored settings from NVS");
#endif

  showHebrewDate = (prefs.getUChar("datemode", 0) == 1);
  Serial.printf("dateMode = %s\n", showHebrewDate ? "Hebrew" : "Gregorian");

  // Try the stored WiFi credentials. If they connect, pull the time from NTP
  // and run the status server. Otherwise fall back to the AP setup portal.
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");

  if (ssid.length() > 0) {
    Serial.printf("Stored WiFi SSID=%s — connecting\n", ssid.c_str());
    drawMessage("WiFi", "Connecting to " + ssid + " ...");
    if (connectWiFi(ssid, pass)) {
      Serial.printf("WiFi connected  IP=%s\n", WiFi.localIP().toString().c_str());
      drawMessage("WiFi", "Getting time from NTP...");
      initNTP();
      if (waitForTime(NTP_SYNC_TIMEOUT_MS)) {
        timeReady = true;
        struct tm t;
        if (getLocalTime(&t)) {
          drawTimeInWords(t, true);
          firstDraw = false;
          lastDisplayedMinute = t.tm_min;
        }
      } else {
        Serial.println("NTP sync timed out — will keep retrying in loop()");
        drawMessage("NTP", "Waiting for time sync...");
      }
      startStatusServer();
      return;
    }
    Serial.println("WiFi connect failed — opening setup portal");
  } else {
    Serial.println("No stored WiFi credentials — opening setup portal");
  }

  startConfigPortal();
}

void loop() {
  // A web handler may have requested a deferred restart (save / forget WiFi).
  if (pendingRestartMs != 0 && (long)(millis() - pendingRestartMs) >= 0) {
    Serial.println("Restarting...");
    delay(100);
    ESP.restart();
  }

  if (portalMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  server.handleClient();

  // Until SNTP delivers a valid time, poll for it (getLocalTime returns false
  // while the clock is still at the epoch). SNTP keeps it synced afterwards.
  if (!timeReady) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry >= 2000UL) {
      lastTry = millis();
      struct tm t;
      if (getLocalTime(&t, 0)) {
        timeReady = true;
        Serial.println("NTP time acquired");
        drawTimeInWords(t, true);
        firstDraw = false;
        lastDisplayedMinute = t.tm_min;
      }
    }
    return;
  }

  struct tm t;
  if (!getLocalTime(&t, 0)) return;

  if (t.tm_min != lastDisplayedMinute || pendingFullRefresh) {
    drawTimeInWords(t, firstDraw || pendingFullRefresh);
    firstDraw = false;
    pendingFullRefresh = false;
    lastDisplayedMinute = t.tm_min;
  }
}

// ─────────────────────────────────────────────────────────────────────
//  Timezone
// ─────────────────────────────────────────────────────────────────────
void applyTimeZone() {
  setenv("TZ", timeZone, 1);
  tzset();
  Serial.printf("applyTimeZone: TZ=%s\n", timeZone);
}

// ─────────────────────────────────────────────────────────────────────
//  WiFi (station) — join the saved network
// ─────────────────────────────────────────────────────────────────────
bool connectWiFi(const String& ssid, const String& pass) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// ─────────────────────────────────────────────────────────────────────
//  NTP — set the soft-clock to UTC, then apply the Israel TZ for display.
//  configTime(0,0,...) keeps the system clock in UTC; the TZ env var
//  (set in applyTimeZone) does the local conversion in getLocalTime().
// ─────────────────────────────────────────────────────────────────────
void initNTP() {
  configTime(0, 0, NTP_SERVER1, NTP_SERVER2);
  applyTimeZone();   // re-apply TZ — configTime may have reset it
  Serial.printf("NTP configured: %s, %s\n", NTP_SERVER1, NTP_SERVER2);
}

// Block until the clock is plausibly set (getLocalTime succeeds) or timeout.
bool waitForTime(unsigned long timeoutMs) {
  struct tm t;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (getLocalTime(&t, 0)) return true;
    delay(200);
  }
  return false;
}

// ─────────────────────────────────────────────────────────────────────
//  Web page (single self-contained HTML, no external resources)
// ─────────────────────────────────────────────────────────────────────
// Shared CSS + the date-mode radio block + the setMode() JS helper, reused by
// both the setup portal and the status page.
const char STYLE_HTML[] PROGMEM = R"rawliteral(
<style>
body{font-family:sans-serif;text-align:center;padding:40px 20px;
     background:#1a1a2e;color:#eee;margin:0}
h1{font-size:1.8em;margin-bottom:8px}
p{color:#aaa;margin-top:0}
#clock{font-size:2.8em;margin:28px 0 6px;
       font-variant-numeric:tabular-nums;letter-spacing:2px}
#ip{font-size:.95em;color:#888;margin-bottom:28px}
select,input{width:100%;max-width:320px;padding:12px;margin:8px auto;display:block;
     font-size:1em;border-radius:8px;border:1px solid #444;
     background:#16213e;color:#eee;box-sizing:border-box}
button{padding:14px 40px;font-size:1.1em;background:#e94560;
       color:#fff;border:none;border-radius:10px;cursor:pointer;margin-top:8px}
button:hover,button:active{background:#c73652}
button.alt{background:#0f3460}button.alt:hover{background:#143a6b}
#mode{margin-top:28px;font-size:1.05em;color:#ccc}
#mode .lbl{display:block;margin-bottom:8px;color:#aaa}
#mode label{margin:0 12px;cursor:pointer}
#status{margin-top:24px;font-size:1em;min-height:1.5em}
.ok{color:#a8ff78}.err{color:#ff6b6b}
</style>)rawliteral";

// ── WiFi setup portal (AP mode) ───────────────────────────────────────
const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hebrew Clock — WiFi Setup</title>
{{STYLE}}
</head><body>
<h1>Hebrew Clock</h1>
<p>Choose your WiFi network and enter the password</p>
<select id="ssid"><option>Scanning…</option></select>
<button class="alt" style="padding:8px 16px;font-size:.9em" onclick="scan()">Rescan</button>
<input id="pass" type="password" placeholder="WiFi password">
<button onclick="connect()">Connect</button>
<div id="mode">
  <span class="lbl">Top row date</span>
  <label><input type="radio" name="dm" value="0" {{G_CHECKED}} onchange="setMode(0)"> Gregorian</label>
  <label><input type="radio" name="dm" value="1" {{H_CHECKED}} onchange="setMode(1)"> Hebrew</label>
</div>
<div id="status"></div>
<script>
function scan(){
  const sel=document.getElementById('ssid');
  sel.innerHTML='<option>Scanning…</option>';
  fetch('/scan').then(r=>r.json()).then(list=>{
    sel.innerHTML='';
    if(!list.length){sel.innerHTML='<option value="">No networks found</option>';return;}
    list.forEach(n=>{
      const o=document.createElement('option');
      o.value=n.ssid;
      o.textContent=n.ssid+(n.enc?' \u{1F512}':'')+'  ('+n.rssi+'dBm)';
      sel.appendChild(o);
    });
  }).catch(()=>{sel.innerHTML='<option value="">Scan failed</option>';});
}
function connect(){
  const ssid=document.getElementById('ssid').value;
  const pass=document.getElementById('pass').value;
  const s=document.getElementById('status');
  if(!ssid){s.textContent='Pick a network first';s.className='err';return;}
  s.textContent='Saving…';s.className='';
  fetch('/connect',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)})
  .then(r=>{if(!r.ok)throw new Error(r.statusText);return r.text();})
  .then(t=>{s.textContent=t;s.className='ok';})
  .catch(e=>{s.textContent='Error: '+e;s.className='err';});
}
function setMode(m){
  const s=document.getElementById('status');
  s.textContent='Saving…';s.className='';
  fetch('/datemode',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'mode='+m})
  .then(r=>{if(!r.ok)throw new Error(r.statusText);return r.text();})
  .then(t=>{s.textContent=t;s.className='ok';})
  .catch(e=>{s.textContent='Error: '+e;s.className='err';});
}
scan();
</script></body></html>
)rawliteral";

// ── Status page (STA mode, after connecting) ──────────────────────────
const char STATUS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hebrew Clock</title>
{{STYLE}}
</head><body>
<h1>Hebrew Clock</h1>
<p>Connected to <b>{{SSID}}</b> — time from NTP</p>
<div id="clock">--:--:--</div>
<div id="ip">IP: {{IP}}</div>
<div id="mode">
  <span class="lbl">Top row date</span>
  <label><input type="radio" name="dm" value="0" {{G_CHECKED}} onchange="setMode(0)"> Gregorian</label>
  <label><input type="radio" name="dm" value="1" {{H_CHECKED}} onchange="setMode(1)"> Hebrew</label>
</div>
<button class="alt" onclick="forget()">Reconfigure WiFi</button>
<div id="status"></div>
<script>
function poll(){
  fetch('/time').then(r=>r.text())
    .then(t=>{document.getElementById('clock').textContent=t;})
    .catch(()=>{});
}
setInterval(poll,1000);poll();
function setMode(m){
  const s=document.getElementById('status');
  s.textContent='Saving…';s.className='';
  fetch('/datemode',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'mode='+m})
  .then(r=>{if(!r.ok)throw new Error(r.statusText);return r.text();})
  .then(t=>{s.textContent=t;s.className='ok';})
  .catch(e=>{s.textContent='Error: '+e;s.className='err';});
}
function forget(){
  if(!confirm('Forget WiFi and restart into setup mode?'))return;
  const s=document.getElementById('status');
  s.textContent='Clearing…';s.className='';
  fetch('/forget',{method:'POST'})
  .then(r=>{if(!r.ok)throw new Error(r.statusText);return r.text();})
  .then(t=>{s.textContent=t;s.className='ok';})
  .catch(e=>{s.textContent='Error: '+e;s.className='err';});
}
</script></body></html>
)rawliteral";

// ─────────────────────────────────────────────────────────────────────
//  Web handlers
// ─────────────────────────────────────────────────────────────────────
void handleRoot() {
  String page = portalMode ? FPSTR(PORTAL_HTML) : FPSTR(STATUS_HTML);
  page.replace("{{STYLE}}", FPSTR(STYLE_HTML));
  page.replace("{{G_CHECKED}}", showHebrewDate ? "" : "checked");
  page.replace("{{H_CHECKED}}", showHebrewDate ? "checked" : "");
  if (!portalMode) {
    page.replace("{{SSID}}", WiFi.SSID());
    page.replace("{{IP}}",   WiFi.localIP().toString());
  }
  server.send(200, "text/html", page);
}

// Return the visible WiFi networks as JSON for the portal's dropdown.
void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;          // skip hidden / blank
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    if (json.length() > 1) json += ",";
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"enc\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

// Save the chosen WiFi credentials and restart to connect as a station.
void handleConnect() {
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
    server.send(400, "text/plain", "Missing SSID");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  server.send(200, "text/plain",
              "Saved. Restarting to connect to \"" + ssid + "\"...");
  Serial.printf("Saved WiFi creds for SSID=%s — restarting\n", ssid.c_str());
  pendingRestartMs = millis() + 1500;          // let the response flush first
}

// Clear stored credentials and restart back into the setup portal.
void handleForget() {
  prefs.remove("ssid");
  prefs.remove("pass");
  server.send(200, "text/plain", "WiFi cleared. Restarting into setup mode...");
  Serial.println("WiFi credentials cleared — restarting");
  pendingRestartMs = millis() + 1500;
}

// Current device-local time as "HH:MM:SS" (or placeholder until synced).
void handleTimeJson() {
  struct tm t;
  char buf[16];
  if (getLocalTime(&t, 0))
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  else
    strncpy(buf, "--:--:--", sizeof(buf));
  server.send(200, "text/plain", buf);
}

// Select Gregorian (mode=0) or Hebrew (mode=1) date for the top row.
void handleDateMode() {
  if (!server.hasArg("mode")) {
    server.send(400, "text/plain", "Missing mode");
    return;
  }
  bool hebrew = (server.arg("mode").toInt() == 1);
  if (hebrew != showHebrewDate) {
    showHebrewDate = hebrew;
    prefs.putUChar("datemode", hebrew ? 1 : 0);
    pendingFullRefresh = true;     // redraw the top row on the next loop
  }
  server.send(200, "text/plain",
              hebrew ? "Showing Hebrew date" : "Showing Gregorian date");
  Serial.printf("dateMode set to %s\n", hebrew ? "Hebrew" : "Gregorian");
}

// ─────────────────────────────────────────────────────────────────────
//  WiFi setup portal — AP mode + captive portal + HTTP server
// ─────────────────────────────────────────────────────────────────────
void startConfigPortal() {
  portalMode = true;
  // AP_STA so the network scan (handleScan) can see nearby APs while the
  // device serves its own AP.
  WiFi.mode(WIFI_AP_STA);
  const char* pw = (strlen(AP_PASSWORD) > 0) ? AP_PASSWORD : nullptr;
  WiFi.softAP(AP_SSID, pw);
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("Setup AP started  SSID=%s  IP=%s\n",
                AP_SSID, apIP.toString().c_str());

  // Captive portal: resolve every hostname to us so phones auto-open the page.
  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/scan",     HTTP_GET,  handleScan);
  server.on("/connect",  HTTP_POST, handleConnect);
  server.on("/datemode", HTTP_POST, handleDateMode);
  // Any other URL (incl. OS connectivity-check probes) → redirect to the page,
  // which triggers the "sign in to network" captive-portal prompt.
  server.onNotFound([]() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
  Serial.println("Setup HTTP server started");

  drawMessage("WiFi Setup",
              "Join WiFi \"" + String(AP_SSID) + "\" to configure");
}

// ─────────────────────────────────────────────────────────────────────
//  Status server — runs once connected as a station
// ─────────────────────────────────────────────────────────────────────
void startStatusServer() {
  portalMode = false;
  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/time",     HTTP_GET,  handleTimeJson);
  server.on("/datemode", HTTP_POST, handleDateMode);
  server.on("/forget",   HTTP_POST, handleForget);
  server.onNotFound([]() { handleRoot(); });
  server.begin();
  Serial.printf("Status HTTP server started  http://%s/\n",
                WiFi.localIP().toString().c_str());
}

// ──────────────────────────────────────────────────────
//  Hebrew RTL + niqud support.
//  GFXfont can't position combining marks (niqud) correctly
//  because it lacks OpenType GPOS support. We draw glyphs
//  individually and center each niqud mark on its base letter.
// ──────────────────────────────────────────────────────
bool isNiqudCP(uint16_t cp) {
  return (cp >= 0x05B0 && cp <= 0x05BD) || cp == 0x05BF ||
         cp == 0x05C1 || cp == 0x05C2 || cp == 0x05C7;
}

bool isNikudByte(uint8_t b0, uint8_t b1) {
  uint16_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
  return isNiqudCP(cp);
}

uint16_t decodeUTF8(const String& s, int& pos) {
  uint8_t b0 = (uint8_t)s[pos];
  if ((b0 & 0xE0) == 0xC0 && pos + 1 < (int)s.length()) {
    uint8_t b1 = (uint8_t)s[pos + 1];
    pos += 2;
    return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
  }
  pos++;
  return b0;
}

// Draw a glyph at scale sn/sd (sn==sd → 1:1). When shrinking, each
// destination pixel samples its source footprint and goes black when at
// least half the covered source pixels are set (majority sampling keeps
// thin serif strokes legible).
void drawGlyphBitmap(const GFXfont* font, uint16_t cp, int x, int y, int sn, int sd) {
  if (cp < pgm_read_word(&font->first) || cp > pgm_read_word(&font->last)) return;
  const GFXglyph* glyph = &((const GFXglyph*)pgm_read_ptr(&font->glyph))[cp - pgm_read_word(&font->first)];
  const uint8_t* bitmap = (const uint8_t*)pgm_read_ptr(&font->bitmap);

  uint32_t bo = pgm_read_dword(&glyph->bitmapOffset);
  int w = pgm_read_byte(&glyph->width);
  int h = pgm_read_byte(&glyph->height);

  if (sn == sd) {                                   // fast path: 1:1
    int bit = 0;
    for (int row = 0; row < h; row++)
      for (int col = 0; col < w; col++) {
        if (pgm_read_byte(&bitmap[bo + bit / 8]) & (0x80 >> (bit & 7)))
          epaper.drawPixel(x + col, y + row, TFT_BLACK);
        bit++;
      }
    return;
  }

  int dW = (w * sn) / sd;
  int dH = (h * sn) / sd;
  for (int dy = 0; dy < dH; dy++) {
    int sy0 = (dy * sd) / sn;
    int sy1 = ((dy + 1) * sd) / sn;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    if (sy1 > h) sy1 = h;
    for (int dx = 0; dx < dW; dx++) {
      int sx0 = (dx * sd) / sn;
      int sx1 = ((dx + 1) * sd) / sn;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      if (sx1 > w) sx1 = w;
      int black = 0, total = 0;
      for (int sy = sy0; sy < sy1; sy++)
        for (int sx = sx0; sx < sx1; sx++) {
          int bit = sy * w + sx;
          if (pgm_read_byte(&bitmap[bo + bit / 8]) & (0x80 >> (bit & 7))) black++;
          total++;
        }
      if (total > 0 && black * 2 >= total)
        epaper.drawPixel(x + dx, y + dy, TFT_BLACK);
    }
  }
}

GlyphMetrics getGlyphMetrics(const GFXfont* font, uint16_t cp) {
  GlyphMetrics m = {0, 0, 0, 0, 0};
  if (cp < pgm_read_word(&font->first) || cp > pgm_read_word(&font->last)) return m;
  const GFXglyph* glyph = &((const GFXglyph*)pgm_read_ptr(&font->glyph))[cp - pgm_read_word(&font->first)];
  m.width    = pgm_read_byte(&glyph->width);
  m.height   = pgm_read_byte(&glyph->height);
  m.xAdvance = pgm_read_byte(&glyph->xAdvance);
  m.xOffset  = (int8_t)pgm_read_byte(&glyph->xOffset);
  m.yOffset  = (int8_t)pgm_read_byte(&glyph->yOffset);
  return m;
}

int drawHebrewWord(const GFXfont* font, const String& word, int x, int y, int sn, int sd) {
  int cursor = x;
  int lastBaseX = x;
  int lastBaseXOff = 0;
  int lastBaseW = 0;

  int i = 0;
  while (i < (int)word.length()) {
    uint16_t cp = decodeUTF8(word, i);
    GlyphMetrics m = getGlyphMetrics(font, cp);
    int gW   = ((int)m.width    * sn) / sd;
    int gXo  = ((int)m.xOffset  * sn) / sd;
    int gYo  = ((int)m.yOffset  * sn) / sd;
    int gAdv = ((int)m.xAdvance * sn) / sd;

    if (isNiqudCP(cp)) {
      int markX;
      if (cp == 0x05C1)        // SHIN DOT — right side of letter
        markX = lastBaseX + lastBaseXOff + lastBaseW - gW;
      else if (cp == 0x05C2 || cp == 0x05B9)  // SIN DOT / HOLAM — left side
        markX = lastBaseX + lastBaseXOff;
      else                     // all other niqud — centered
        markX = lastBaseX + lastBaseXOff + lastBaseW / 2 - gW / 2;
      int markY = y + gYo;
      drawGlyphBitmap(font, cp, markX, markY, sn, sd);
    } else {
      drawGlyphBitmap(font, cp, cursor + gXo, y + gYo, sn, sd);
      lastBaseX = cursor;
      lastBaseXOff = gXo;
      lastBaseW = gW;
      cursor += gAdv;
    }
  }
  return cursor - x;
}

int measureHebrewWord(const GFXfont* font, const String& word, int sn, int sd) {
  int width = 0;
  int i = 0;
  while (i < (int)word.length()) {
    uint16_t cp = decodeUTF8(word, i);
    if (!isNiqudCP(cp))
      width += ((int)getGlyphMetrics(font, cp).xAdvance * sn) / sd;
  }
  return width;
}

String reverseHebrew(const String& word) {
  String clusters[32];
  int count = 0;
  int i = 0;
  while (i < (int)word.length() && count < 32) {
    uint8_t c = (uint8_t)word[i];
    if ((c & 0xE0) == 0xC0 && i + 1 < (int)word.length()) {
      clusters[count] = word.substring(i, i + 2);
      i += 2;
      while (i + 1 < (int)word.length() &&
             isNikudByte((uint8_t)word[i], (uint8_t)word[i + 1])) {
        clusters[count] += word.substring(i, i + 2);
        i += 2;
      }
      count++;
    } else if (c >= '0' && c <= '9') {
      // Numbers stay left-to-right inside RTL text — keep the whole digit
      // run as one cluster so "14" doesn't get flipped to "41".
      int ds = i;
      while (i < (int)word.length() &&
             (uint8_t)word[i] >= '0' && (uint8_t)word[i] <= '9') i++;
      clusters[count++] = word.substring(ds, i);
    } else {
      clusters[count++] = word.substring(i, i + 1);
      i++;
    }
  }
  String result;
  for (int j = count - 1; j >= 0; j--)
    result += clusters[j];
  return result;
}

// ──────────────────────────────────────────────────────
//  Draw a single line of Hebrew text centred at cx, top at y.
//  Each word is reversed for LTR rendering, and words are
//  placed right-to-left across the line.
// ──────────────────────────────────────────────────────
int drawHebrewLine(const String& text, int cx, int y, int sn, int sd) {
  if (text.length() == 0) return 0;

  const GFXfont* font = &NotoSerifHebrew_Bold_85;
  int spaceW = (HEBREW_SPACE_W * sn) / sd;

  String words[10];
  int wordCount = 0;
  int start = 0;
  for (int i = 0; i <= (int)text.length(); i++) {
    if (i == (int)text.length() || text[i] == ' ') {
      if (i > start && wordCount < 10)
        words[wordCount++] = text.substring(start, i);
      start = i + 1;
    }
  }

  String reversed[10];
  for (int i = 0; i < wordCount; i++)
    reversed[i] = reverseHebrew(words[i]);

  int totalW = 0;
  for (int i = 0; i < wordCount; i++) {
    totalW += measureHebrewWord(font, reversed[i], sn, sd);
    if (i < wordCount - 1) totalW += spaceW;
  }

  int curX = cx - totalW / 2;

  for (int i = wordCount - 1; i >= 0; i--) {
    int wordW = drawHebrewWord(font, reversed[i], curX, y, sn, sd);
    curX += wordW;
    if (i > 0) curX += spaceW;
  }

  return (FONT_BASE_H * sn) / sd;
}

// ──────────────────────────────────────────────────────
//  Draw lines vertically centred inside the TIME_BOX.
// ──────────────────────────────────────────────────────
void drawCenteredLines(const String& l1, const String& l2, const String& l3, int numLines) {
  const int sn = TIME_SCALE_NUM, sd = TIME_SCALE_DEN;
  int cx      = SCREEN_W / 2;
  int fontH   = (FONT_BASE_H  * sn) / sd;
  int ascent  = (FONT_ASCENT  * sn) / sd;
  int descent = (FONT_DESCENT * sn) / sd;
  int lineGap = (LINE_GAP     * sn) / sd;
  int step    = fontH + lineGap;
  int visH    = ascent + (numLines - 1) * step + descent;
  int y       = TIME_BOX_Y + (TIME_BOX_H - visH) / 2 + ascent;

  drawHebrewLine(l1, cx, y, sn, sd);
  drawHebrewLine(l2, cx, y + step, sn, sd);
  if (numLines == 3)
    drawHebrewLine(l3, cx, y + 2 * step, sn, sd);
}

// ──────────────────────────────────────────────────────
//  Build the two-line phrase for the current time
// ──────────────────────────────────────────────────────
static int countWords(const String& s) {
  int n = 0;
  bool inWord = false;
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == ' ') { inWord = false; }
    else if (!inWord) { inWord = true; n++; }
  }
  return n;
}

void splitTimePhrase(const struct tm& t, String& line1, String& line2, String& line3) {
  int hour12 = t.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  int min = t.tm_min;
  String period = String(getTimePeriod(t.tm_hour));
  line3 = "";

  if (isSubtractMinute(min)) {
    int next = (hour12 % 12) + 1;       // 12 -> 1
    line1 = String(SUBTRACT_AMOUNT[min]) + " " + String(HOURS_LAMED[next - 1]);
    line2 = period;
  } else if (min == 0) {
    line1 = String(HOURS[hour12 - 1]);
    line2 = period;
  } else {
    String minPart = String(MINUTE_PREFIX[min]);
    if (hour12 >= 11 || countWords(minPart) == 3) {
      line1 = String(HOURS[hour12 - 1]);
      line2 = minPart;
      line3 = period;
    } else {
      line1 = String(HOURS[hour12 - 1]) + " " + minPart;
      line2 = period;
    }
  }
}

// ──────────────────────────────────────────────────────
//  Date line — "<day> <hebrew-month> <year>", e.g. "14 בְּיוּנִי 2026".
//  Drawn in a smaller font above the time. Hebrew reads right-to-left,
//  so the day ends up on the right and the year on the left.
// ──────────────────────────────────────────────────────
String buildDateString(const struct tm& t) {
  int day      = t.tm_mday;
  int monthIdx = t.tm_mon + 1;          // tm_mon is 0..11
  int year     = t.tm_year + 1900;
  String month = (monthIdx >= 1 && monthIdx <= 12)
                   ? String(SUBTRACT_MONTH[monthIdx]) : String("");
  return String(day) + " " + month + " " + String(year);
}

void drawDateLine(const struct tm& t) {
  String dateStr;
  if (showHebrewDate) {
    const char* heb = hebrewDateForTm(t);          // from hebrew_date.h
    if (heb) {
      dateStr = String(heb);
      // The font ends at 0x05EA, so it has no glyphs for the Hebrew
      // geresh/gershayim numeral punctuation (U+05F3 ׳ / U+05F4 ״). Swap in
      // the ASCII apostrophe/quote, which DO exist in the font (0x27 / 0x22),
      // sit at the same height, and read identically. The UTF-8 byte pairs
      // are D7 B3 (geresh) and D7 B4 (gershayim).
      dateStr.replace("\xD7\xB4", "\"");   // gershayim ״ → "
      dateStr.replace("\xD7\xB3", "'");    // geresh    ׳ → '
    } else {
      // Fall back to the Gregorian date if t is outside the generated range.
      dateStr = buildDateString(t);
    }
  } else {
    dateStr = buildDateString(t);
  }
  drawHebrewLine(dateStr, SCREEN_W / 2, DATE_BASELINE_Y,
                 DATE_SCALE_NUM, DATE_SCALE_DEN);
}

// ──────────────────────────────────────────────────────
//  Main draw routine
//  fullRefresh=true on first boot only — clears the whole
//  screen and uses full e-ink update. Subsequent calls
//  redraw only the time box and use partial refresh.
// ──────────────────────────────────────────────────────
void drawTimeInWords(const struct tm& t, bool fullRefresh) {
  String line1, line2, line3;
  splitTimePhrase(t, line1, line2, line3);

  int numLines = (line3.length() > 0) ? 3 : 2;

  static int lastDrawnYday = -1;
  bool dateChanged = (t.tm_yday != lastDrawnYday);

  // Check if anything actually changed since last draw
  if (!fullRefresh && !dateChanged) {
    bool anyChanged = (numLines != prevLineCount);
    if (!anyChanged) {
      for (int i = 0; i < numLines; i++) {
        const char* prev = prevLines[i];
        const char* cur  = (i == 0) ? line1.c_str() : (i == 1) ? line2.c_str() : line3.c_str();
        if (strcmp(cur, prev) != 0) {
          anyChanged = true;
          break;
        }
      }
    }
    if (!anyChanged) {
      Serial.println("No lines changed — skipping refresh");
      return;
    }
  }

  Serial.printf("Drawing time hour=%d min=%d fullRefresh=%d\n",
    t.tm_hour, t.tm_min, fullRefresh);

  bool doFullRefresh = fullRefresh || dateChanged || (partialCount >= FULL_REFRESH_EVERY);

  // For differential partial refresh: render old text first to build
  // the pixel-perfect old buffer the UC8179 needs for clean transitions.
  uint8_t* oldBuf = nullptr;
  if (!doFullRefresh && prevLineCount > 0) {
    epaper.fillRect(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H, TFT_WHITE);
    drawCenteredLines(String(prevLines[0]), String(prevLines[1]),
                      String(prevLines[2]), prevLineCount);
    oldBuf = epaper.capturePartialWindow(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H);
  }

  // Render new text. A full refresh clears the whole panel, so the date
  // (which lives above the time box) must be redrawn then. Partial updates
  // only touch the time box, leaving the date intact from the last full draw.
  if (doFullRefresh) {
    epaper.fillScreen(TFT_WHITE);
    drawDateLine(t);
  } else {
    epaper.fillRect(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H, TFT_WHITE);
  }

  drawCenteredLines(line1, line2, line3, numLines);

  if (doFullRefresh) {
    epaper.update();
    partialCount = 0;
  } else {
    epaper.updataPartial(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H, oldBuf);
    partialCount++;
  }
  if (oldBuf) free(oldBuf);

  // Save current state for next partial-refresh comparison
  for (int i = 0; i < MAX_LINES; i++) {
    if (i < numLines) {
      const char* src = (i == 0) ? line1.c_str() : (i == 1) ? line2.c_str() : line3.c_str();
      strncpy(prevLines[i], src, sizeof(prevLines[i]) - 1);
    } else {
      prevLines[i][0] = '\0';
    }
  }
  prevLineCount = numLines;
  lastDrawnYday = t.tm_yday;

  Serial.printf("Drew \"%s\" / \"%s\" / \"%s\"\n", line1.c_str(), line2.c_str(), line3.c_str());
}

// ──────────────────────────────────────────────────────
//  Error screen
// ──────────────────────────────────────────────────────
void drawError(const String& msg) {
  drawMessage("Error:", msg);
}

// ──────────────────────────────────────────────────────
//  Status / message screen (boot, WiFi, NTP feedback)
// ──────────────────────────────────────────────────────
void drawMessage(const String& title, const String& msg) {
  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_BLACK);
  epaper.drawCentreString(title.c_str(), SCREEN_W/2, SCREEN_H/2 - 30, 4);
  epaper.drawCentreString(msg.c_str(),   SCREEN_W/2, SCREEN_H/2 + 10, 2);
  epaper.update();
  // Force a full redraw of the clock face after any message screen.
  firstDraw = true;
  lastDisplayedMinute = -1;
}

#else
// Stubs when EPAPER_ENABLE isn't defined (mirrors the original sketch's behaviour).
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("EPAPER: NOT DEFINED");
}
void loop() {}
#endif
