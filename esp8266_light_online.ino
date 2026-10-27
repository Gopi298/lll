/*
  ESP8266 Smart Light Controller
  ------------------------------------------------------------
  - Single relay light ON/OFF
  - Control 1: Web dashboard (type the ESP IP address in a browser)
  - Control 2: Touch sensor switch (TTP223)
  - Login page   -> user: priya   password: priya@143
  - Dashboard    -> bulb image (OFF) / fire-glow image (ON), toggle switch
  - AUTO button  -> square button, set ON/OFF time, date and days (3 schedules)
                    Schedules are saved in EEPROM (survive power cut)
  - Time comes from the internet (NTP), shown as Chennai / India time (IST, UTC+5:30)
  - ONLINE: also controlled from the Vercel website through Firebase (see README.md)

  WIRING (NodeMCU / Wemos D1 mini)
    Relay IN   -> D1 (GPIO5)
    Touch OUT  -> D2 (GPIO4)
    Relay VCC  -> 5V (VIN)     Touch VCC -> 3.3V
    GND        -> GND (common)
    Light      -> through relay COM and NO terminals

  Board: "NodeMCU 1.0 (ESP-12E Module)" or "LOLIN(WEMOS) D1 R2 & mini"
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>      // Library Manager -> "ArduinoJson" by Benoit Blanchon (v6 or v7)
#include <EEPROM.h>
#include <time.h>

// ---------------- USER SETTINGS ----------------
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Online control (Firebase Realtime Database). See README.md
const char* FIREBASE_URL    = "https://app-registration-2f6cb-default-rtdb.firebaseio.com";   // no slash at the end
const char* FIREBASE_API_KEY = "AIzaSyC9VfyxSYHpOPDFx7IPxQoiT9DsshF3TfI";                     // from your Firebase web config

const char* LOGIN_USER = "priya";
const char* LOGIN_PASS = "priya@143";

#define RELAY_PIN          5      // D1
#define TOUCH_PIN          4      // D2
#define RELAY_ACTIVE_LOW   true   // most relay modules turn ON with LOW. Set false if yours is opposite.
#define TOUCH_ACTIVE_HIGH  true   // TTP223 gives HIGH when touched

#define GMT_OFFSET_SEC     19800  // India IST = 5.5 hours
// -----------------------------------------------

#define NUM_SLOTS   3
#define EEPROM_MAGIC 0xA7

struct Slot {
  uint8_t  enabled;    // 0/1
  uint8_t  days;       // bit0=Sun ... bit6=Sat
  uint16_t onMin;      // minutes from midnight
  uint16_t offMin;     // minutes from midnight
  uint8_t  dateOn;     // 1 = use specific date
  uint16_t year;
  uint8_t  month;
  uint8_t  day;
};

struct Settings {
  uint8_t magic;
  uint8_t autoEnabled;
  Slot    slots[NUM_SLOTS];
};

Settings cfg;
ESP8266WebServer server(80);

bool    lightOn = false;
String  sessionToken = "";
int8_t  slotWas[NUM_SLOTS];   // -1 = not checked yet, 0 = was outside the ON..OFF window, 1 = was inside
unsigned long lastAutoCheck = 0;
unsigned long lastTouchTime = 0;
bool    lastTouchState = false;

// cloud sync state
long    lastCmdId = -1;            // last online command already handled (-1 = none yet)
long    lastSchedVer = 0;          // version of the schedule shared with the cloud
bool    statePending = true;       // relay state must be sent to the cloud
bool    schedPushPending = false;  // local schedule must be sent to the cloud
unsigned long lastPoll = 0;
unsigned long lastBeat = 0;

const char* DAY_NAMES[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

// ======================= HTML PAGES =======================

const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Light Login</title>
<style>
*{box-sizing:border-box;font-family:Segoe UI,Arial,sans-serif}
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
background:linear-gradient(135deg,#0f2027,#203a43,#2c5364)}
.card{background:rgba(255,255,255,.08);backdrop-filter:blur(10px);padding:36px 30px;border-radius:20px;
width:90%;max-width:340px;color:#fff;box-shadow:0 10px 40px rgba(0,0,0,.5);text-align:center}
.card h2{margin:10px 0 24px}
input{width:100%;padding:13px 15px;margin-bottom:14px;border:none;border-radius:10px;font-size:16px;outline:none}
button{width:100%;padding:13px;border:none;border-radius:10px;font-size:17px;font-weight:600;color:#fff;
background:linear-gradient(135deg,#ff9800,#ff5722);cursor:pointer}
button:active{transform:scale(.97)}
.err{color:#ff8a80;margin-bottom:12px;display:none}
.icon{font-size:54px}
</style></head><body>
<form class="card" method="POST" action="/login">
<div class="icon">&#128161;</div>
<h2>Smart Light Login</h2>
<div class="err" id="err">Wrong username or password</div>
<input name="user" placeholder="Username" autocomplete="username" required>
<input name="pass" type="password" placeholder="Password" autocomplete="current-password" required>
<button type="submit">LOGIN</button>
</form>
<script>if(location.search.indexOf('err=1')>-1)document.getElementById('err').style.display='block';</script>
</body></html>
)rawliteral";

const char DASH_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Light Dashboard</title>
<style>
*{box-sizing:border-box;font-family:Segoe UI,Arial,sans-serif}
body{margin:0;min-height:100vh;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);color:#fff;
display:flex;flex-direction:column;align-items:center;padding:20px}
body.on{background:linear-gradient(135deg,#3a1c00,#7a3300,#b34700)}
.top{width:100%;max-width:420px;display:flex;justify-content:space-between;align-items:center}
.top a{color:#fff;text-decoration:none;background:rgba(255,255,255,.15);padding:7px 14px;border-radius:20px;font-size:14px}
.clock{text-align:right;font-size:13px;opacity:.9}
.clock b{font-size:20px;display:block}
.panel{margin-top:18px;width:100%;max-width:420px;background:rgba(255,255,255,.1);border-radius:24px;
padding:26px;text-align:center;box-shadow:0 10px 40px rgba(0,0,0,.4)}
h1{margin:0 0 6px;font-size:24px}
.state{font-size:18px;margin:8px 0 14px;font-weight:600}
.img{height:190px;display:flex;align-items:center;justify-content:center}
.img svg{height:180px;width:auto}
#fireImg{display:none;animation:flick 1s infinite alternate}
@keyframes flick{from{transform:scale(1) rotate(-1deg);filter:drop-shadow(0 0 18px #ffb300)}
to{transform:scale(1.06) rotate(1deg);filter:drop-shadow(0 0 34px #ff6d00)}}
body.on #fireImg{display:block}
body.on #bulbImg{display:none}
/* toggle switch */
.sw{position:relative;display:inline-block;width:92px;height:48px;margin-top:10px}
.sw input{display:none}
.sl{position:absolute;inset:0;background:#6b7a86;border-radius:48px;cursor:pointer;transition:.3s}
.sl:before{content:"";position:absolute;width:38px;height:38px;left:5px;top:5px;background:#fff;
border-radius:50%;transition:.3s;box-shadow:0 2px 6px rgba(0,0,0,.4)}
input:checked+.sl{background:#ff9800}
input:checked+.sl:before{transform:translateX(44px)}
.row{display:flex;gap:14px;justify-content:center;margin-top:22px}
.sq{width:110px;height:110px;border:none;border-radius:16px;color:#fff;font-size:17px;font-weight:700;
background:linear-gradient(135deg,#00bcd4,#2196f3);cursor:pointer;box-shadow:0 6px 18px rgba(0,0,0,.4)}
.sq small{display:block;font-weight:400;font-size:12px;margin-top:4px;opacity:.9}
.sq:active{transform:scale(.95)}
.sq.autoon{background:linear-gradient(135deg,#4caf50,#2e7d32)}
.sq .ic{font-size:30px;display:block}
/* modal */
.modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.7);align-items:flex-start;justify-content:center;
overflow:auto;padding:16px;z-index:9}
.mbox{background:#1d2b36;border-radius:20px;padding:20px;width:100%;max-width:440px;margin:auto}
.mbox h2{margin:0 0 6px}
.slot{background:rgba(255,255,255,.07);border-radius:14px;padding:14px;margin-top:12px}
.slot h3{margin:0 0 8px;font-size:16px;display:flex;justify-content:space-between;align-items:center}
.f{display:flex;gap:10px;margin-top:8px;flex-wrap:wrap}
.f label{font-size:12px;opacity:.85;display:block;margin-bottom:3px}
.f input[type=time],.f input[type=date]{padding:8px;border-radius:8px;border:none;font-size:15px;width:100%}
.f>div{flex:1;min-width:130px}
.days{display:flex;gap:5px;margin-top:10px;flex-wrap:wrap}
.days label{flex:1;min-width:42px;text-align:center;background:rgba(255,255,255,.12);padding:7px 0;
border-radius:8px;font-size:13px;cursor:pointer}
.days input{display:none}
.days input:checked+span{color:#ff9800;font-weight:700}
.days label:has(input:checked){background:rgba(255,152,0,.25);outline:1px solid #ff9800}
.mini{position:relative;width:46px;height:26px;display:inline-block}
.mini input{display:none}
.mini span{position:absolute;inset:0;background:#6b7a86;border-radius:26px;transition:.3s}
.mini span:before{content:"";position:absolute;width:20px;height:20px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.3s}
.mini input:checked+span{background:#4caf50}
.mini input:checked+span:before{transform:translateX(20px)}
.btns{display:flex;gap:10px;margin-top:16px}
.btns button{flex:1;padding:12px;border:none;border-radius:10px;font-size:16px;font-weight:600;color:#fff;cursor:pointer}
.save{background:#4caf50}.close{background:#78909c}
.note{font-size:12px;opacity:.7;margin-top:8px}
#msg{margin-top:10px;font-size:13px;color:#a5d6a7;min-height:16px}
</style></head><body>

<div class="top">
  <a href="/logout">&#8592; Logout</a>
  <div class="clock"><b id="clk">--:--:--</b><span id="dt">Waiting for time...</span></div>
</div>

<div class="panel">
  <h1>Smart Light</h1>
  <div class="state" id="stateTxt">LIGHT OFF</div>

  <div class="img">
    <!-- OFF : bulb image -->
    <svg id="bulbImg" viewBox="0 0 100 140">
      <path d="M50 8C28 8 12 25 12 47c0 14 7 24 15 33 5 6 7 11 7 18h32c0-7 2-12 7-18 8-9 15-19 15-33C88 25 72 8 50 8z"
        fill="#cfd8dc" stroke="#90a4ae" stroke-width="3"/>
      <path d="M40 60l10 14 10-14M50 74v24" fill="none" stroke="#90a4ae" stroke-width="3" stroke-linecap="round"/>
      <rect x="34" y="100" width="32" height="9" rx="3" fill="#78909c"/>
      <rect x="36" y="111" width="28" height="9" rx="3" fill="#607d8b"/>
      <rect x="42" y="122" width="16" height="9" rx="4" fill="#455a64"/>
    </svg>
    <!-- ON : fire image -->
    <svg id="fireImg" viewBox="0 0 100 140">
      <defs>
        <radialGradient id="g1" cx="50%" cy="70%" r="60%">
          <stop offset="0" stop-color="#fff59d"/><stop offset=".5" stop-color="#ffb300"/><stop offset="1" stop-color="#e64a19"/>
        </radialGradient>
      </defs>
      <path d="M50 4C56 28 82 40 80 74c-1 26-16 44-30 44S21 100 20 74c-1-16 8-26 16-36 2 12 8 16 12 16-4-20-2-34 2-50z"
        fill="url(#g1)"/>
      <path d="M50 52c4 14 18 22 17 40-1 14-8 22-17 22s-16-8-17-22c-1-10 6-16 10-22 1 7 4 9 7 9-2-12-1-20 0-27z"
        fill="#fff8e1" opacity=".9"/>
      <rect x="34" y="118" width="32" height="8" rx="3" fill="#78909c"/>
      <rect x="40" y="128" width="20" height="8" rx="4" fill="#455a64"/>
    </svg>
  </div>

  <label class="sw"><input type="checkbox" id="tg"><span class="sl"></span></label>

  <div class="row">
    <button class="sq" id="autoBtn" onclick="openAuto()"><span class="ic">&#9200;</span>AUTO<small id="autoSm">Set timer</small></button>
  </div>
  <div id="msg"></div>
</div>

<!-- AUTO MODAL -->
<div class="modal" id="modal"><div class="mbox">
  <h2>&#9200; Automatic Timer</h2>
  <div style="display:flex;justify-content:space-between;align-items:center">
    <span>Auto mode</span>
    <label class="mini"><input type="checkbox" id="autoMaster"><span></span></label>
  </div>
  <div id="slots"></div>
  <div class="note">Pick days (Sun-Sat) OR a specific date. If a date is chosen, the day buttons are ignored.
  Leave the date empty to repeat on the selected days every week.</div>
  <div class="btns"><button class="save" onclick="saveAuto()">SAVE</button>
  <button class="close" onclick="closeAuto()">CLOSE</button></div>
</div></div>

<script>
var DN=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
var tg=document.getElementById('tg');
function $(i){return document.getElementById(i)}

function setUI(on){
  document.body.className=on?'on':'';
  tg.checked=on;
  $('stateTxt').innerText=on?'LIGHT ON':'LIGHT OFF';
}
function refresh(){
  fetch('/status').then(function(r){if(r.status==401){location.href='/';return}return r.json()}).then(function(j){
    if(!j)return;
    setUI(j.on);
    $('clk').innerText=j.synced?j.time:'--:--:--';
    $('dt').innerText=j.synced?(j.day+', '+j.date):'Waiting for internet time...';
    $('autoBtn').className='sq'+(j.auto?' autoon':'');
    $('autoSm').innerText=j.auto?'Auto ON':'Auto OFF';
  }).catch(function(){});
}
tg.addEventListener('change',function(){
  fetch('/toggle?state='+(tg.checked?1:0)).then(refresh);
});
setInterval(refresh,1500);refresh();

function openAuto(){
  fetch('/getauto').then(function(r){return r.json()}).then(function(j){
    $('autoMaster').checked=!!j.auto;
    var h='';
    for(var i=0;i<j.slots.length;i++){
      var s=j.slots[i];
      h+='<div class="slot"><h3>Schedule '+(i+1)+
        '<label class="mini"><input type="checkbox" id="e'+i+'" '+(s.e?'checked':'')+'><span></span></label></h3>'+
        '<div class="f"><div><label>Light ON time</label><input type="time" id="on'+i+'" value="'+s.on+'"></div>'+
        '<div><label>Light OFF time</label><input type="time" id="off'+i+'" value="'+s.off+'"></div></div>'+
        '<div class="f"><div><label>Specific date (optional)</label><input type="date" id="dt'+i+'" value="'+s.date+'"></div></div>'+
        '<div class="days">';
      for(var d=0;d<7;d++){
        h+='<label><input type="checkbox" id="d'+i+'_'+d+'" '+((s.d>>d)&1?'checked':'')+'><span>'+DN[d]+'</span></label>';
      }
      h+='</div></div>';
    }
    $('slots').innerHTML=h;
    $('modal').style.display='flex';
  });
}
function closeAuto(){$('modal').style.display='none'}
function saveAuto(){
  var anyOn=false;
  for(var q=0;q<3;q++){
    if($('e'+q).checked){
      anyOn=true;
      if(!$('on'+q).value||!$('off'+q).value){alert('Schedule '+(q+1)+': please set both ON time and OFF time');return;}
    }
  }
  var p=new URLSearchParams();
  p.append('auto',$('autoMaster').checked?1:0);
  for(var i=0;i<3;i++){
    var m=0;
    for(var d=0;d<7;d++){if($('d'+i+'_'+d).checked)m|=(1<<d)}
    p.append('e'+i,$('e'+i).checked?1:0);
    p.append('on'+i,$('on'+i).value||'00:00');
    p.append('off'+i,$('off'+i).value||'00:00');
    p.append('d'+i,m);
    p.append('dt'+i,$('dt'+i).value||'');
  }
  fetch('/setauto',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()})
  .then(function(r){return r.text()}).then(function(){
    closeAuto();
    $('msg').innerText=(anyOn&&!$('autoMaster').checked)?'Saved, but the Auto mode switch is OFF. Turn it ON.':'Auto schedule saved';
    setTimeout(function(){$('msg').innerText=''},5000);refresh();
  });
}
</script></body></html>
)rawliteral";

// ======================= HELPERS =======================

void setRelay(bool on) {
  lightOn = on;
  statePending = true;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(RELAY_PIN, level ? HIGH : LOW);
}

bool timeSynced() {
  return time(nullptr) > 1700000000;
}

// Current Chennai / India time (IST = UTC + 5:30). Works on every ESP8266 core version.
struct tm* istNow() {
  time_t t = time(nullptr) + GMT_OFFSET_SEC;
  return gmtime(&t);
}

bool isAuthed() {
  if (sessionToken.length() == 0) return false;
  if (!server.hasHeader("Cookie")) return false;
  String c = server.header("Cookie");
  return c.indexOf("SID=" + sessionToken) != -1;
}

String two(int v) { return v < 10 ? "0" + String(v) : String(v); }

String minToStr(uint16_t m) { return two(m / 60) + ":" + two(m % 60); }

uint16_t strToMin(String s) {
  int h = 0, m = 0;
  sscanf(s.c_str(), "%d:%d", &h, &m);
  if (h < 0 || h > 23) h = 0;
  if (m < 0 || m > 59) m = 0;
  return h * 60 + m;
}

void loadSettings() {
  EEPROM.begin(128);
  EEPROM.get(0, cfg);
  if (cfg.magic != EEPROM_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = EEPROM_MAGIC;
    for (int i = 0; i < NUM_SLOTS; i++) {
      cfg.slots[i].days = 0x7F;
      cfg.slots[i].onMin = 18 * 60;
      cfg.slots[i].offMin = 23 * 60;
    }
    EEPROM.put(0, cfg);
    EEPROM.commit();
  }
}

void saveSettings() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ======================= WEB HANDLERS =======================

void handleRoot() {
  if (isAuthed()) server.send_P(200, "text/html", DASH_HTML);
  else server.send_P(200, "text/html", LOGIN_HTML);
}

void handleLogin() {
  if (server.arg("user") == LOGIN_USER && server.arg("pass") == LOGIN_PASS) {
    sessionToken = String(ESP.getChipId(), HEX) + String(micros(), HEX) + String(random(100000, 999999));
    server.sendHeader("Set-Cookie", "SID=" + sessionToken + "; Path=/; HttpOnly");
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  } else {
    server.sendHeader("Location", "/?err=1");
    server.send(302, "text/plain", "");
  }
}

void handleLogout() {
  sessionToken = "";
  server.sendHeader("Set-Cookie", "SID=; Path=/; Max-Age=0");
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleToggle() {
  if (!isAuthed()) { server.send(401, "text/plain", "login"); return; }
  if (server.hasArg("state")) setRelay(server.arg("state") == "1");
  else setRelay(!lightOn);
  server.send(200, "text/plain", lightOn ? "ON" : "OFF");
}

void handleStatus() {
  if (!isAuthed()) { server.send(401, "text/plain", "login"); return; }
  String j = "{\"on\":" + String(lightOn ? "true" : "false");
  j += ",\"auto\":" + String(cfg.autoEnabled ? "true" : "false");
  bool s = timeSynced();
  j += ",\"synced\":" + String(s ? "true" : "false");
  if (s) {
    struct tm* t = istNow();
    int h12 = t->tm_hour % 12;
    if (h12 == 0) h12 = 12;
    String ampm = (t->tm_hour >= 12) ? "PM" : "AM";
    j += ",\"time\":\"" + two(h12) + ":" + two(t->tm_min) + ":" + two(t->tm_sec) + " " + ampm + "\"";
    j += ",\"date\":\"" + String(t->tm_year + 1900) + "-" + two(t->tm_mon + 1) + "-" + two(t->tm_mday) + "\"";
    j += ",\"day\":\"" + String(DAY_NAMES[t->tm_wday]) + "\"";
  }
  j += "}";
  server.send(200, "application/json", j);
}

void handleGetAuto() {
  if (!isAuthed()) { server.send(401, "text/plain", "login"); return; }
  String j = "{\"auto\":" + String(cfg.autoEnabled ? 1 : 0) + ",\"slots\":[";
  for (int i = 0; i < NUM_SLOTS; i++) {
    Slot& s = cfg.slots[i];
    String date = "";
    if (s.dateOn) date = String(s.year) + "-" + two(s.month) + "-" + two(s.day);
    if (i) j += ",";
    j += "{\"e\":" + String(s.enabled) + ",\"on\":\"" + minToStr(s.onMin) + "\",\"off\":\"" + minToStr(s.offMin) +
         "\",\"d\":" + String(s.days) + ",\"date\":\"" + date + "\"}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleSetAuto() {
  if (!isAuthed()) { server.send(401, "text/plain", "login"); return; }
  cfg.autoEnabled = server.arg("auto") == "1";
  for (int i = 0; i < NUM_SLOTS; i++) {
    String n = String(i);
    Slot& s = cfg.slots[i];
    s.enabled = server.arg("e" + n) == "1";
    s.onMin   = strToMin(server.arg("on" + n));
    s.offMin  = strToMin(server.arg("off" + n));
    s.days    = (uint8_t)server.arg("d" + n).toInt() & 0x7F;
    String d  = server.arg("dt" + n);
    int y = 0, m = 0, dd = 0;
    if (d.length() >= 10 && sscanf(d.c_str(), "%d-%d-%d", &y, &m, &dd) == 3) {
      s.dateOn = 1; s.year = y; s.month = m; s.day = dd;
    } else {
      s.dateOn = 0; s.year = 0; s.month = 0; s.day = 0;
    }
    slotWas[i] = -1;          // check this schedule again from scratch
  }
  saveSettings();
  schedPushPending = true;   // share the new schedule with the online dashboard
  Serial.println("Schedule saved. Auto=" + String(cfg.autoEnabled ? "ON" : "OFF"));
  for (int i = 0; i < NUM_SLOTS; i++) {
    Slot& s = cfg.slots[i];
    Serial.println("  Schedule " + String(i + 1) + ": " + (s.enabled ? "enabled " : "disabled ") +
                   minToStr(s.onMin) + " -> " + minToStr(s.offMin) +
                   (s.dateOn ? " on date " + String(s.year) + "-" + two(s.month) + "-" + two(s.day)
                             : " days mask " + String(s.days)));
  }
  server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// ======================= AUTO SCHEDULER =======================

// true if this schedule is valid on the day that contains this (IST-shifted) time
bool dayMatches(const Slot& s, time_t shifted) {
  struct tm tmv;
  gmtime_r(&shifted, &tmv);
  if (s.dateOn) {
    return (s.year == tmv.tm_year + 1900 && s.month == tmv.tm_mon + 1 && s.day == tmv.tm_mday);
  }
  return (s.days >> tmv.tm_wday) & 1;
}

// true if "now" is inside the ON..OFF window of this schedule.
// Also works past midnight (for example ON 22:00, OFF 06:00).
bool slotActive(const Slot& s, time_t shifted, int curMin) {
  if (!s.enabled || s.onMin == s.offMin) return false;
  if (s.onMin < s.offMin) {
    return dayMatches(s, shifted) && curMin >= s.onMin && curMin < s.offMin;
  }
  return (dayMatches(s, shifted) && curMin >= s.onMin) ||
         (dayMatches(s, shifted - 86400) && curMin < s.offMin);
}

void checkAuto() {
  static unsigned long lastDbg = 0;

  if (!timeSynced()) {
    if (millis() - lastDbg > 10000UL) {
      lastDbg = millis();
      Serial.println("Waiting for internet time (NTP)...");
    }
    return;
  }

  time_t shifted = time(nullptr) + GMT_OFFSET_SEC;     // Chennai / India time
  struct tm tmv;
  gmtime_r(&shifted, &tmv);
  int curMin = tmv.tm_hour * 60 + tmv.tm_min;

  // status line in the Serial Monitor every 30 seconds
  if (millis() - lastDbg > 30000UL) {
    lastDbg = millis();
    Serial.printf("IST %02d:%02d:%02d | auto=%d | light=%d\n",
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, cfg.autoEnabled ? 1 : 0, lightOn ? 1 : 0);
  }

  if (!cfg.autoEnabled) {
    for (int i = 0; i < NUM_SLOTS; i++) slotWas[i] = -1;   // check again from scratch when Auto is turned ON
    return;
  }

  for (int i = 0; i < NUM_SLOTS; i++) {
    bool inside = slotActive(cfg.slots[i], shifted, curMin);
    if (slotWas[i] == -1) {
      // first check after boot / after saving: if we are inside the window, light must be ON
      if (inside && !lightOn) {
        setRelay(true);
        Serial.println("AUTO: light ON (schedule " + String(i + 1) + ", inside window)");
      }
    } else if (inside && !slotWas[i]) {
      setRelay(true);
      Serial.println("AUTO: light ON (schedule " + String(i + 1) + ")");
    } else if (!inside && slotWas[i]) {
      setRelay(false);
      Serial.println("AUTO: light OFF (schedule " + String(i + 1) + ")");
    }
    slotWas[i] = inside ? 1 : 0;
  }
}

// ======================= ONLINE (FIREBASE) SYNC =======================

String idToken = "";
String refreshToken = "";
unsigned long tokenExpireAt = 0;
unsigned long lastSignInTry = 0;

// Gets (or renews) the Firebase login token. No secret needed: anonymous sign-in with the project API key.
bool fbAuth() {
  if (idToken.length() > 0 && (long)(millis() - tokenExpireAt) < 0) return true;   // token still valid
  if (lastSignInTry != 0 && millis() - lastSignInTry < 15000UL) return false;      // wait between tries
  lastSignInTry = millis();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  bool refreshing = refreshToken.length() > 0;
  String url, body;
  if (refreshing) {
    url  = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;
    body = "grant_type=refresh_token&refresh_token=" + refreshToken;
  } else {
    url  = String("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=") + FIREBASE_API_KEY;
    body = "{\"returnSecureToken\":true}";
  }
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", refreshing ? "application/x-www-form-urlencoded" : "application/json");
  int code = http.POST(body);
  String out = (code == 200) ? http.getString() : "";
  http.end();
  if (code != 200) {
    Serial.println("Firebase login failed, HTTP " + String(code) + " (enable Anonymous sign-in in Firebase Authentication)");
    if (refreshing) refreshToken = "";     // try a fresh sign-in next time
    return false;
  }
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, out)) return false;
  const char* tok = doc[refreshing ? "id_token" : "idToken"] | "";
  const char* ref = doc[refreshing ? "refresh_token" : "refreshToken"] | "";
  if (strlen(tok) == 0) return false;
  idToken = tok;
  refreshToken = ref;
  tokenExpireAt = millis() + 50UL * 60UL * 1000UL;   // renew after 50 minutes
  Serial.println("Firebase login OK");
  return true;
}

String fbUrl(const String& path) {
  return String(FIREBASE_URL) + "/" + path + ".json?auth=" + idToken;
}

// method: "GET", "PUT" or "PATCH". Returns true when the server answered 2xx.
bool fbRequest(const char* method, const String& url, const String& body, String& out) {
  WiFiClientSecure client;
  client.setInsecure();                 // no certificate check (simple, works on ESP8266)
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, url)) return false;
  int code;
  if (strcmp(method, "GET") == 0) {
    code = http.GET();
  } else {
    http.addHeader("Content-Type", "application/json");
    code = http.sendRequest(method, body);
  }
  bool ok = (code >= 200 && code < 300);
  if (ok) out = http.getString();
  http.end();
  if (code == 401 || code == 403) {
    idToken = "";                       // token rejected: log in again
    Serial.println("Firebase says access denied (HTTP " + String(code) + "). Check the database Rules.");
  }
  return ok;
}

String scheduleJson(long ver) {
  String j = "{\"auto\":" + String(cfg.autoEnabled ? 1 : 0) + ",\"ver\":" + String(ver) + ",\"slots\":[";
  for (int i = 0; i < NUM_SLOTS; i++) {
    Slot& s = cfg.slots[i];
    String date = "";
    if (s.dateOn) date = String(s.year) + "-" + two(s.month) + "-" + two(s.day);
    if (i) j += ",";
    j += "{\"e\":" + String(s.enabled) + ",\"on\":\"" + minToStr(s.onMin) + "\",\"off\":\"" + minToStr(s.offMin) +
         "\",\"d\":" + String(s.days) + ",\"date\":\"" + date + "\"}";
  }
  j += "]}";
  return j;
}

void applyCloudSchedule(JsonObject sc) {
  cfg.autoEnabled = (sc["auto"] | 0) == 1;
  JsonArray arr = sc["slots"];
  for (int i = 0; i < NUM_SLOTS && i < (int)arr.size(); i++) {
    Slot& s = cfg.slots[i];
    JsonObject o = arr[i];
    s.enabled = (o["e"] | 0) == 1;
    const char* on  = o["on"]  | "00:00";
    const char* off = o["off"] | "00:00";
    s.onMin  = strToMin(String(on));
    s.offMin = strToMin(String(off));
    s.days   = (uint8_t)(o["d"] | 127) & 0x7F;
    const char* d = o["date"] | "";
    int y = 0, m = 0, dd = 0;
    if (strlen(d) >= 10 && sscanf(d, "%d-%d-%d", &y, &m, &dd) == 3) {
      s.dateOn = 1; s.year = y; s.month = m; s.day = dd;
    } else {
      s.dateOn = 0; s.year = 0; s.month = 0; s.day = 0;
    }
    slotWas[i] = -1;
  }
  saveSettings();
  Serial.println("Cloud: schedule updated");
}

void cloudPull() {
  String out;
  if (!fbRequest("GET", fbUrl("light"), "", out)) return;
  if (out == "null") return;
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, out)) return;

  // 1) on/off command from the website
  JsonObject cmd = doc["cmd"];
  if (!cmd.isNull()) {
    long id = cmd["id"] | 0L;
    int v = cmd["v"] | 0;
    if (lastCmdId == -1) {
      lastCmdId = id;                    // first poll after boot: remember, do not apply old command
    } else if (id != lastCmdId) {
      lastCmdId = id;
      setRelay(v == 1);
      Serial.println(v == 1 ? "Cloud: light ON" : "Cloud: light OFF");
    }
  } else if (lastCmdId == -1) {
    lastCmdId = 0;
  }

  // 2) schedule changed on the website
  JsonObject sc = doc["schedule"];
  if (!sc.isNull()) {
    long ver = sc["ver"] | 0L;
    if (ver != lastSchedVer) {
      lastSchedVer = ver;
      applyCloudSchedule(sc);
    }
  }
}

void cloudLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!fbAuth()) return;
  unsigned long ms = millis();
  String out;

  if (schedPushPending && timeSynced()) {
    long ver = (long)time(nullptr);
    if (fbRequest("PUT", fbUrl("light/schedule"), scheduleJson(ver), out)) {
      lastSchedVer = ver;
      schedPushPending = false;
    }
  }

  if (statePending || ms - lastBeat > 30000UL) {
    String body = "{\"state\":" + String(lightOn ? 1 : 0);
    if (timeSynced()) body += ",\"seen\":" + String((long)time(nullptr));
    body += "}";
    if (fbRequest("PATCH", fbUrl("light"), body, out)) {
      statePending = false;
      lastBeat = ms;
    }
  }

  if (ms - lastPoll >= 3000UL) {
    lastPoll = ms;
    cloudPull();
  }
}

// ======================= SETUP / LOOP =======================

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(TOUCH_PIN, INPUT);
  setRelay(false);

  loadSettings();
  for (int i = 0; i < NUM_SLOTS; i++) { slotWas[i] = -1; }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Open this IP in browser: http://");
  Serial.println(WiFi.localIP());

  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");   // get UTC, IST is added in istNow()

  server.collectHeaders("Cookie");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/toggle", HTTP_GET, handleToggle);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/getauto", HTTP_GET, handleGetAuto);
  server.on("/setauto", HTTP_POST, handleSetAuto);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();

  // Touch sensor switch (toggle on each touch)
  bool touched = (digitalRead(TOUCH_PIN) == (TOUCH_ACTIVE_HIGH ? HIGH : LOW));
  if (touched && !lastTouchState && millis() - lastTouchTime > 400) {
    lastTouchTime = millis();
    setRelay(!lightOn);
    Serial.println(lightOn ? "Touch: light ON" : "Touch: light OFF");
  }
  lastTouchState = touched;

  // Auto schedule check once per second
  if (millis() - lastAutoCheck >= 1000) {
    lastAutoCheck = millis();
    checkAuto();
  }

  // Online control (website -> Firebase -> this board)
  cloudLoop();
}
