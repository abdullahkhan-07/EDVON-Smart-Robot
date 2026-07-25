#include <WiFi.h>
#include <WebServer.h>

// ── WIFI MODE ────────────────────────────────────────────────
#define WIFI_MODE 0        // 0 = AUTO(fallback), 1 = STA only, 2 = AP only

const char* STA_SSID = "YOUR_WIFI_NAME";
const char* STA_PASS = "YOUR_WIFI_PASSWORD";

const char* AP_SSID  = "ROBO_CAR";
const char* AP_PASS  = "robot1234";     

const uint32_t STA_TIMEOUT_MS = 12000;

// ── BUZZER ───────────────────────────────────────────────────
#define BUZZER_PIN     25
#define BUZZER_ACTIVE   1     // 1 = active buzzer, 0 = passive buzzer
#define BUZZ_CHANNEL    0     // LEDC channel (passive buzzer only)

// Tones (Hz) — only used for a PASSIVE buzzer
#define TONE_BOOT     1800
#define TONE_ALARM    2600
#define TONE_CLEAR    1500
#define TONE_CLICK    2200

// ── Ultrasonic pins ──────────────────────────────────────────
#define TRIG_PIN  5
#define ECHO_PIN  18

// ── Obstacle thresholds (hysteresis) ─────────────────────────
#define OBST_ON_CM     10
#define OBST_OFF_CM    15
#define CLOSE_CONFIRM   2
#define CLEAR_CONFIRM   4

// ── UART2 to EDVON ───────────────────────────────────────────
#define RXD2 16
#define TXD2 17

WebServer server(80);

// ── State ────────────────────────────────────────────────────
bool     manualMode       = false;
bool     obstacleDetected = false;
bool     muted            = false;
float    distance         = 999;
String   currentAction    = "STOPPED";
String   netInfo          = "";

uint32_t lastUltraCheck = 0;
uint32_t lastKeepAlive  = 0;
uint8_t  closeCount     = 0;
uint8_t  farCount       = 0;

// ── Buzzer state (non-blocking player) ───────────────────────
uint8_t  beepsLeft  = 0;
uint16_t beepFreq   = TONE_CLICK;
uint16_t beepOnMs   = 80;
uint16_t beepOffMs  = 80;
bool     beepIsOn   = false;
uint32_t beepTimer  = 0;
bool     alarmMode  = false;   // repeat forever while true

// ============================================================
//  BUZZER — low level
// ============================================================
void buzzerOn(uint16_t freq) {
  if (muted) return;
#if BUZZER_ACTIVE
  digitalWrite(BUZZER_PIN, HIGH);          // active buzzer: just power it
#else
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(BUZZER_PIN, freq);       // core 3.x API
  #else
    ledcWriteTone(BUZZ_CHANNEL, freq);     // core 2.x API
  #endif
#endif
}

void buzzerOff() {
#if BUZZER_ACTIVE
  digitalWrite(BUZZER_PIN, LOW);
#else
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(BUZZER_PIN, 0);
  #else
    ledcWriteTone(BUZZ_CHANNEL, 0);
  #endif
#endif
}

void buzzerSetup() {
#if BUZZER_ACTIVE
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#else
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(BUZZER_PIN, 2000, 8);
  #else
    ledcSetup(BUZZ_CHANNEL, 2000, 8);
    ledcAttachPin(BUZZER_PIN, BUZZ_CHANNEL);
  #endif
  buzzerOff();
#endif
}

// Queue a beep pattern. count = how many beeps (0 with alarm=true -> endless)
void beep(uint8_t count, uint16_t freq, uint16_t onMs, uint16_t offMs) {
  beepsLeft = count;
  beepFreq  = freq;
  beepOnMs  = onMs;
  beepOffMs = offMs;
  beepIsOn  = false;
  beepTimer = 0;              // fire immediately on next update
}

void stopBeeping() {
  beepsLeft = 0;
  alarmMode = false;
  beepIsOn  = false;
  buzzerOff();
}

// Call every loop — advances the beep pattern without blocking
void updateBuzzer() {
  uint32_t now = millis();

  // Keep the alarm going for as long as the obstacle is there
  if (alarmMode && beepsLeft == 0 && !beepIsOn) {
    beepsLeft = 1;            // re-arm one more beep, forever
  }

  if (beepsLeft == 0 && !beepIsOn) return;

  if (!beepIsOn) {
    if (now - beepTimer >= beepOffMs) {
      buzzerOn(beepFreq);
      beepIsOn  = true;
      beepTimer = now;
    }
  } else {
    if (now - beepTimer >= beepOnMs) {
      buzzerOff();
      beepIsOn  = false;
      beepTimer = now;
      if (beepsLeft > 0) beepsLeft--;
    }
  }
}

// ============================================================
//  ULTRASONIC
// ============================================================
float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 25000);
  if (duration == 0) return 999;
  return duration * 0.034 / 2;
}

void sendToEDVON(char cmd) { Serial2.write(cmd); }

// ============================================================
//  WEB PAGE (UTF-8)
// ============================================================
String getHTML() {
  String page = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
  <title>EDVON Robot</title>
  <style>
    :root{
      --bg:#0d1424; --panel:#141d33; --panel-2:#1b2742;
      --line:#26324f; --text:#e8edf7; --muted:#8595b5;
      --accent:#39d3c4; --accent-ink:#04211f;
      --warn:#ff5470; --warn-soft:#2a1730;
      --btn:#1b2742; --btn-active:#39d3c4;
    }
    *{box-sizing:border-box; margin:0; padding:0; -webkit-tap-highlight-color:transparent;}
    body{
      font-family:'Segoe UI',system-ui,-apple-system,Roboto,sans-serif;
      background:radial-gradient(1200px 600px at 50% -10%, #16223d 0%, var(--bg) 60%);
      color:var(--text); min-height:100vh; padding:22px 16px 34px;
      display:flex; flex-direction:column; align-items:center;
    }
    .wrap{width:100%; max-width:380px;}
    header{text-align:center; margin-bottom:18px; position:relative;}
    header h1{font-size:22px; letter-spacing:.3px; font-weight:700;}
    header .dot{color:var(--accent);}
    header p{color:var(--muted); font-size:12.5px; margin-top:3px; letter-spacing:.4px;}
    .mute{
      position:absolute; right:0; top:0; width:38px; height:38px;
      border:1px solid var(--line); border-radius:10px; background:var(--panel);
      display:flex; align-items:center; justify-content:center; cursor:pointer;
    }
    .mute svg{width:19px; height:19px; stroke:var(--accent); fill:none;
      stroke-width:2; stroke-linecap:round; stroke-linejoin:round;}
    .mute.off svg{stroke:var(--muted);}
    .alert{
      display:none; align-items:center; gap:10px;
      background:var(--warn-soft); border:1px solid var(--warn);
      color:var(--warn); border-radius:12px; padding:12px 14px;
      font-weight:600; font-size:14px; margin-bottom:14px;
    }
    .card{
      background:var(--panel); border:1px solid var(--line);
      border-radius:16px; padding:16px 18px; margin-bottom:16px;
    }
    .row{display:flex; justify-content:space-between; align-items:center; padding:9px 0;}
    .row + .row{border-top:1px solid var(--line);}
    .k{color:var(--muted); font-size:13px; letter-spacing:.3px;}
    .v{font-weight:700; font-size:14px;}
    .v.mono{font-family:'Consolas',ui-monospace,monospace; font-size:18px; letter-spacing:.5px;}
    .v.ok{color:var(--accent);} .v.bad{color:var(--warn);}
    .net{color:var(--muted); font-size:11.5px; text-align:center; margin-bottom:14px; letter-spacing:.3px;}
    .prox{margin-top:12px;}
    .prox-track{height:8px; border-radius:999px; background:var(--panel-2); overflow:hidden;}
    .prox-fill{height:100%; width:0%; border-radius:999px;
      background:var(--accent); transition:width .2s linear, background .2s;}
    .modes{display:flex; gap:10px; margin-bottom:16px;}
    .mode{
      flex:1; padding:13px; border:1px solid var(--line); border-radius:12px;
      background:var(--panel); color:var(--muted); font-size:14px; font-weight:700;
      letter-spacing:.4px; cursor:pointer; transition:.15s;
    }
    .mode.active{background:var(--accent); color:var(--accent-ink); border-color:var(--accent);}
    .mode:active{transform:scale(.97);}
    .pad{display:grid; grid-template-columns:repeat(3,1fr); gap:12px;}
    .cell{aspect-ratio:1/1;}
    .empty{visibility:hidden;}
    .btn{
      width:100%; height:100%; border:1px solid var(--line); border-radius:16px;
      background:var(--btn); color:var(--text); display:flex; align-items:center;
      justify-content:center; cursor:pointer; touch-action:none; user-select:none;
      transition:transform .08s, background .12s, border-color .12s;
    }
    .btn:active,.btn.hold{background:var(--btn-active); border-color:var(--btn-active); transform:scale(.94);}
    .btn:active svg,.btn.hold svg{stroke:var(--accent-ink);}
    .btn svg{width:34px; height:34px; stroke:var(--text); stroke-width:2.4; fill:none;
      stroke-linecap:round; stroke-linejoin:round;}
    .stop{border-color:var(--warn);}
    .stop .sq{width:18px; height:18px; border-radius:4px; background:var(--warn);}
    .stop:active,.stop.hold{background:var(--warn); border-color:var(--warn);}
    .stop:active .sq,.stop.hold .sq{background:var(--accent-ink);}
  </style>
</head>
<body>
  <div class="wrap">
    <header>
      <h1><span class="dot">&#9679;</span> EDVON Robot</h1>
      <p>WiFi Controller &nbsp;&middot;&nbsp; Line Follower</p>
      <div class="mute" id="mute" onclick="toggleMute()">
        <svg viewBox="0 0 24 24"><path d="M11 5 6 9H2v6h4l5 4V5z"/><path d="M15.5 8.5a5 5 0 0 1 0 7"/></svg>
      </div>
    </header>

    <div class="alert" id="alert">
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor"
           stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
        <path d="M12 9v4M12 17h.01M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z"/>
      </svg>
      <span>Obstacle detected &mdash; robot stopped</span>
    </div>

    <div class="card">
      <div class="row"><span class="k">Mode</span><span class="v" id="mode">&mdash;</span></div>
      <div class="row"><span class="k">Distance</span><span class="v mono" id="dist">&mdash; cm</span></div>
      <div class="row"><span class="k">Obstacle</span><span class="v ok" id="obst">Clear</span></div>
      <div class="row"><span class="k">Action</span><span class="v ok" id="action">&mdash;</span></div>
      <div class="prox">
        <div class="prox-track"><div class="prox-fill" id="prox"></div></div>
      </div>
    </div>

    <div class="net" id="net"></div>

    <div class="modes">
      <button class="mode" id="mAuto"   onclick="setMode('A')">AUTO</button>
      <button class="mode" id="mManual" onclick="setMode('M')">MANUAL</button>
    </div>

    <div class="pad">
      <div class="cell empty"></div>
      <div class="cell">
        <div class="btn" data-cmd="F"><svg viewBox="0 0 24 24"><path d="M12 19V5M5 12l7-7 7 7"/></svg></div>
      </div>
      <div class="cell empty"></div>

      <div class="cell">
        <div class="btn" data-cmd="L"><svg viewBox="0 0 24 24"><path d="M19 12H5M12 19l-7-7 7-7"/></svg></div>
      </div>
      <div class="cell">
        <div class="btn stop" onclick="tap('S')"><div class="sq"></div></div>
      </div>
      <div class="cell">
        <div class="btn" data-cmd="R"><svg viewBox="0 0 24 24"><path d="M5 12h14M12 5l7 7-7 7"/></svg></div>
      </div>

      <div class="cell empty"></div>
      <div class="cell">
        <div class="btn" data-cmd="B"><svg viewBox="0 0 24 24"><path d="M12 5v14M5 12l7 7 7-7"/></svg></div>
      </div>
      <div class="cell empty"></div>
    </div>
  </div>

  <script>
    function send(c){ fetch('/cmd?c=' + c); }
    function tap(c){ send(c); }
    function toggleMute(){ fetch('/mute'); }

    // Hold buttons: repeat while held (feeds the EDVON watchdog)
    document.querySelectorAll('.btn[data-cmd]').forEach(function(b){
      var cmd = b.getAttribute('data-cmd');
      var timer = null;
      var press = function(e){
        e.preventDefault();
        b.classList.add('hold');
        send(cmd);
        if (timer) clearInterval(timer);
        timer = setInterval(function(){ send(cmd); }, 200);
      };
      var release = function(){
        b.classList.remove('hold');
        if (timer){ clearInterval(timer); timer = null; }
        send('S');
      };
      b.addEventListener('pointerdown', press);
      b.addEventListener('pointerup', release);
      b.addEventListener('pointerleave', release);
      b.addEventListener('pointercancel', release);
    });

    function setMode(m){
      send(m);
      document.getElementById('mAuto').classList.toggle('active', m==='A');
      document.getElementById('mManual').classList.toggle('active', m==='M');
    }

    setInterval(function(){
      fetch('/status').then(function(r){return r.json();}).then(function(d){
        var dist = d.dist;
        document.getElementById('dist').innerText = (dist >= 999 ? '\u2014 cm' : dist.toFixed(1) + ' cm');

        var obst = document.getElementById('obst');
        obst.innerText = d.obst ? 'Detected' : 'Clear';
        obst.className = 'v ' + (d.obst ? 'bad' : 'ok');

        var act = document.getElementById('action');
        act.innerText = d.action;
        act.className = 'v ' + (d.obst ? 'bad' : 'ok');

        document.getElementById('mode').innerText = d.mode;
        document.getElementById('net').innerText  = d.net;
        document.getElementById('alert').style.display = d.obst ? 'flex' : 'none';
        document.getElementById('mAuto').classList.toggle('active', d.mode==='AUTO');
        document.getElementById('mManual').classList.toggle('active', d.mode==='MANUAL');
        document.getElementById('mute').classList.toggle('off', d.mute);

        var pct = dist >= 999 ? 0 : Math.max(0, Math.min(100, (1 - dist/50) * 100));
        var fill = document.getElementById('prox');
        fill.style.width = pct + '%';
        fill.style.background = d.obst ? 'var(--warn)' : 'var(--accent)';
      }).catch(function(){});
    }, 400);
  </script>
</body>
</html>
)rawhtml";
  return page;
}

// ============================================================
//  ROUTES
// ============================================================
void handleRoot() {
  server.send(200, "text/html; charset=UTF-8", getHTML());
}

void handleMute() {
  muted = !muted;
  if (muted) buzzerOff();
  else       beep(1, TONE_CLICK, 60, 60);   // confirm unmute
  server.send(200, "text/plain", "OK");
}

void handleCmd() {
  if (server.hasArg("c")) {
    char c = server.arg("c").charAt(0);

    if (c == 'M') { manualMode = true;  currentAction = "MANUAL"; beep(2, TONE_CLICK, 50, 60); }
    if (c == 'A') { manualMode = false; currentAction = "AUTO";   beep(1, TONE_CLICK, 50, 60); }

    if (manualMode && !obstacleDetected) {
      if (c == 'F') currentAction = "FORWARD";
      if (c == 'B') currentAction = "BACKWARD";
      if (c == 'L') currentAction = "LEFT";
      if (c == 'R') currentAction = "RIGHT";
      if (c == 'S') currentAction = "STOPPED";
    }

    // Block movement while an obstacle is present; always allow S / A / M
    if (!obstacleDetected || c == 'S' || c == 'A' || c == 'M') {
      sendToEDVON(c);
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  String json = "{";
  json += "\"dist\":"     + String(distance, 1) + ",";
  json += "\"obst\":"     + String(obstacleDetected ? "true" : "false") + ",";
  json += "\"mute\":"     + String(muted ? "true" : "false") + ",";
  json += "\"mode\":\""   + String(manualMode ? "MANUAL" : "AUTO") + "\",";
  json += "\"action\":\"" + currentAction + "\",";
  json += "\"net\":\""    + netInfo + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// ============================================================
//  WIFI
// ============================================================
bool startStation() {
  Serial.println("Station mode: joining router...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < STA_TIMEOUT_MS) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    netInfo = "STA " + String(STA_SSID) + "  |  " + WiFi.localIP().toString();
    Serial.println("Connected to router.");
    Serial.print("Open on your phone:  http://");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("Router connection FAILED.");
  return false;
}

void startAccessPoint() {
  Serial.println("Access Point mode: creating hotspot...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(300);

  IPAddress ip = WiFi.softAPIP();
  netInfo = "AP " + String(AP_SSID) + "  |  " + ip.toString();
  Serial.println("Hotspot started.");
  Serial.print("1) Connect phone to WiFi:  "); Serial.println(AP_SSID);
  Serial.print("2) Password:               "); Serial.println(AP_PASS);
  Serial.print("3) Open in browser:  http://"); Serial.println(ip);
}

void setupWiFi() {
#if   WIFI_MODE == 1
  if (!startStation()) Serial.println("STA-only: no network. Check SSID/password.");
#elif WIFI_MODE == 2
  startAccessPoint();
#else
  if (!startStation()) {
    Serial.println("Falling back to hotspot...");
    startAccessPoint();
  }
#endif
}

// ============================================================
//  SETUP & LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  buzzerSetup();

  setupWiFi();

  server.on("/",       handleRoot);
  server.on("/cmd",    handleCmd);
  server.on("/mute",   handleMute);
  server.on("/status", handleStatus);
  server.begin();
  Serial.println("Web server running.");

  sendToEDVON('A');
  beep(2, TONE_BOOT, 90, 110);   // ready
}

void loop() {
  server.handleClient();
  updateBuzzer();                 // non-blocking beep engine
  uint32_t now = millis();

  // ---- Ultrasonic: hysteresis + debounce ----
  if (now - lastUltraCheck > 70) {
    lastUltraCheck = now;
    distance = getDistance();

    if (distance > 0 && distance < OBST_ON_CM) { closeCount++; farCount = 0; }
    else if (distance > OBST_OFF_CM)           { farCount++;  closeCount = 0; }
    // 10-15cm dead-band: hold current state

    if (!obstacleDetected && closeCount >= CLOSE_CONFIRM) {
      obstacleDetected = true;
      currentAction = "OBSTACLE STOP";
      alarmMode = true;                        // start urgent alarm
      beep(1, TONE_ALARM, 120, 120);
      Serial.println("Obstacle -> STOP");
    }
    if (obstacleDetected && farCount >= CLEAR_CONFIRM) {
      obstacleDetected = false;
      currentAction = manualMode ? "MANUAL" : "AUTO RESUME";
      stopBeeping();                           // cancel alarm
      beep(1, TONE_CLEAR, 70, 70);             // single "all clear"
      Serial.println("Clear -> resume");
    }
  }

  // ---- Continuous command refresh (survives a dropped byte) ----
  if (obstacleDetected) {
    if (now - lastKeepAlive > 70)  { lastKeepAlive = now; sendToEDVON('X'); }
  } else if (!manualMode) {
    if (now - lastKeepAlive > 250) { lastKeepAlive = now; sendToEDVON('A'); }
  }
}
