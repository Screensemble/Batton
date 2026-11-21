// Screensemble Batton — ESP32 cue device for Screensemble Conductor.
//
// Configured entirely through its own web page: connect to the device's
// IP, enter the conductor URL, press Pair, authorize it in the
// conductor's Battons page, and map GPIO pins to input ids. Pin events
// POST to /api/battons/<id>/event; the conductor's automations decide
// what happens — no reflash needed to change behavior.
//
// Build: PlatformIO (platformio.ini, board esp32dev). Copy
// include/config.h.example → include/config.h for WiFi credentials.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "config.h"

#define FW_VERSION "0.2.0"
#define MAX_INPUTS 16

struct Input { String id; uint8_t pin; bool activeHigh; };

Preferences nvs;
WebServer http(80);
DNSServer dns;

String serverUrl, battonId, battonToken, deviceName;
String wifiSsid, wifiPass;
bool apMode = false;
String apSsid;
Input inputs[MAX_INPUTS];
int inputCount = 0;

// Pairing state machine.
enum PairState { UNPAIRED, OFFERED, PAIRED, DENIED };
PairState pairState = UNPAIRED;
String pairId;
unsigned long lastPoll = 0, lastBeat = 0;

// Debounce state — sized to MAX_INPUTS, indexed parallel to inputs[].
int lastLevel[MAX_INPUTS];
unsigned long lastChange[MAX_INPUTS], lastFire[MAX_INPUTS];

String url(const String &path) { return serverUrl + path; }

// -- NVS -----------------------------------------------------------------

void saveState() {
  nvs.begin("batton", false);
  nvs.putString("server_url", serverUrl);
  nvs.putString("wifi_ssid", wifiSsid);
  nvs.putString("wifi_pass", wifiPass);
  nvs.putString("batton_id", battonId);
  nvs.putString("token", battonToken);
  nvs.putString("name", deviceName);
  JsonDocument doc;
  for (int i = 0; i < inputCount; i++) {
    doc[i]["id"] = inputs[i].id;
    doc[i]["pin"] = inputs[i].pin;
    doc[i]["edge"] = inputs[i].activeHigh ? "high" : "low";
  }
  String s; serializeJson(doc, s);
  nvs.putString("inputs", s);
  nvs.end();
}

void loadState() {
  nvs.begin("batton", true);
  serverUrl = nvs.getString("server_url", DEFAULT_SERVER_URL);
  // NVS first, config.h as the build-time fallback seed.
  wifiSsid = nvs.getString("wifi_ssid", WIFI_SSID);
  wifiPass = nvs.getString("wifi_pass", WIFI_PASS);
  battonId = nvs.getString("batton_id", "");
  battonToken = nvs.getString("token", "");
  deviceName = nvs.getString("name", DEFAULT_DEVICE_NAME);
  JsonDocument doc;
  if (!deserializeJson(doc, nvs.getString("inputs", "[]"))) {
    for (JsonObject o : doc.as<JsonArray>()) {
      if (inputCount >= MAX_INPUTS) break;
      inputs[inputCount++] = {
        String((const char *)(o["id"] | "")), (uint8_t)(o["pin"] | 0),
        String((const char *)(o["edge"] | "low")) == "high"
      };
    }
  }
  nvs.end();
  serverUrl.trim();
  while (serverUrl.endsWith("/")) serverUrl.remove(serverUrl.length() - 1);
  if (battonToken.length()) pairState = PAIRED;
}

void forgetPairing() {
  nvs.begin("batton", false);
  nvs.remove("batton_id"); nvs.remove("token");
  nvs.end();
  battonId = ""; battonToken = "";
  pairState = UNPAIRED; pairId = "";
}

// -- Conductor calls -------------------------------------------------------

String httpPost(const String &u, const String &body, bool withToken) {
  HTTPClient hc;
  hc.begin(u);
  hc.addHeader("Content-Type", "application/json");
  if (withToken) hc.addHeader("Authorization", "Bearer " + battonToken);
  int code = hc.POST(body);
  String out = code > 0 ? hc.getString() : "";
  hc.end();
  return code > 0 ? out : String("HTTP ") + code;
}

String inputsJson() {
  JsonDocument doc;
  for (int i = 0; i < inputCount; i++) {
    doc[i]["id"] = inputs[i].id;
    doc[i]["pin"] = inputs[i].pin;
    doc[i]["edge"] = inputs[i].activeHigh ? "high" : "low";
  }
  String s; serializeJson(doc, s);
  return s;
}

String clientUrl() {
  return String("http://") + WiFi.localIP().toString();
}

void offerPair() {
  if (!serverUrl.length() || WiFi.status() != WL_CONNECTED) return;
  JsonDocument doc;
  doc["name"] = deviceName;
  doc["hardware_id"] = WiFi.macAddress();
  doc["version"] = FW_VERSION;
  doc["client_url"] = clientUrl();
  String body;
  { String ij = inputsJson(); doc["inputs"] = serialized(ij);
    serializeJson(doc, body); }
  String res = httpPost(url("/api/battons/offer"), body, false);
  JsonDocument rd;
  if (!deserializeJson(rd, res) && rd["pair_id"].is<const char*>()) {
    pairId = rd["pair_id"].as<String>();
    pairState = OFFERED;
    lastPoll = 0;
    Serial.printf("pair offered: %s\n", pairId.c_str());
  }
}

void pollPair() {
  if (!pairId.length()) return;
  HTTPClient hc;
  hc.begin(url("/api/battons/poll?pair_id=" + pairId));
  int code = hc.GET();
  if (code != 200) { hc.end(); return; }
  JsonDocument doc;
  deserializeJson(doc, hc.getString());
  hc.end();
  String st = doc["status"] | "";
  if (st == "approved") {
    battonId = doc["batton_id"].as<String>();
    battonToken = doc["token"].as<String>();
    pairState = PAIRED; pairId = "";
    saveState();
    Serial.printf("paired as %s\n", battonId.c_str());
    httpPost(url("/api/battons/" + battonId + "/heartbeat"),
             "{\"inputs\":" + inputsJson() + "}", true);
  } else if (st == "denied" || st == "expired" || st == "unknown") {
    pairState = st == "denied" ? DENIED : UNPAIRED;
    pairId = "";
  }
}

void heartbeat() {
  if (pairState != PAIRED) return;
  JsonDocument doc;
  doc["version"] = FW_VERSION;
  doc["client_url"] = clientUrl();
  String body;
  { String ij = inputsJson(); doc["inputs"] = serialized(ij);
    serializeJson(doc, body); }
  httpPost(url("/api/battons/" + battonId + "/heartbeat"), body, true);
}

void fireInput(const String &inputId) {
  if (pairState != PAIRED) return;
  String res = httpPost(url("/api/battons/" + battonId + "/event"),
                        "{\"input\":\"" + inputId + "\"}", true);
  Serial.printf("event %s -> %s\n", inputId.c_str(),
                res.substring(0, 60).c_str());
}

// -- Web UI ----------------------------------------------------------------

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Screensemble Batton</title>
<style>
body{font:15px system-ui;background:#0a1428;color:#dce9f5;max-width:520px;margin:20px auto;padding:0 14px}
h1{font-size:20px;color:#4ecbe8}h2{font-size:14px;color:#4ecbe8;margin:22px 0 6px}
.card{background:#10203c;border:1px solid #1d3a63;border-radius:8px;padding:14px;margin-bottom:12px}
input,select{background:#060e1e;border:1px solid #1d3a63;color:#dce9f5;border-radius:6px;padding:7px 9px;font-size:14px}
input[type=text],input[type=url]{width:100%;box-sizing:border-box}
button{background:#4ecbe8;border:0;color:#0a1428;font-weight:700;border-radius:6px;padding:8px 16px;cursor:pointer;font-size:14px}
button.ghost{background:#1d3a63;color:#dce9f5}
.dim{color:#7e95b3;font-size:12px}.row{display:flex;gap:8px;margin:6px 0;align-items:center}
.chip{display:inline-block;background:#1d3a63;border-radius:10px;padding:2px 10px;margin:2px;font-size:12px}
#toast{position:fixed;bottom:14px;left:50%;transform:translateX(-50%);background:#4ecbe8;color:#0a1428;font-weight:700;border-radius:8px;padding:8px 16px;opacity:0;transition:opacity .3s}
</style>
<h1>Screensemble Batton</h1>
<div class="card" id="status"></div>
<h2>WiFi</h2>
<div class="card">
<div id="wifiMsg" class="dim"></div>
<div id="nets"></div>
<div class="row" style="margin-top:8px">
<input id="ssid" type="text" placeholder="network name" style="flex:1">
<input id="wpass" type="password" placeholder="password" style="flex:1">
</div>
<div class="row"><button onclick="saveWifi()">Connect</button>
<button class="ghost" onclick="scan()">Scan</button></div>
</div>
<h2>Conductor</h2>
<div class="card">
<label class="dim">Server URL</label>
<input id="url" type="url" placeholder="http://192.168.0.10:8090">
<div class="row" style="margin-top:8px">
<button onclick="saveServer()">Save</button>
<button class="ghost" id="pairBtn" onclick="startPair()">Pair</button>
<a id="openLink" target="_blank" style="color:#4ecbe8;font-size:13px"></a>
</div>
<div class="dim" id="pairMsg"></div>
</div>
<h2>Inputs — GPIO pin → input id</h2>
<div class="card"><div id="inrows"></div>
<button class="ghost" onclick="addInput()">+ add input</button>
<button onclick="saveInputs()" style="float:right">Save inputs</button>
</div>
<h2>Device</h2>
<div class="card">
<label class="dim">Name</label><input id="name" type="text">
<div class="row" style="margin-top:8px">
<button onclick="saveName()">Save</button>
<button class="ghost" onclick="forget()">Forget pairing</button>
</div></div>
<div id="toast"></div>
<script>
const $=i=>document.getElementById(i);
const esc=s=>String(s??"").replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
function toast(m){const t=$("toast");t.textContent=m;t.style.opacity=1;setTimeout(()=>t.style.opacity=0,2500)}
let inputs=[];
async function api(p,o){const r=await fetch(p,o);return r.json().catch(()=>({}))}
async function refresh(){
 const s=await api("/api/state");
 $("status").innerHTML=
  `<b>${esc(s.name)}</b> <span class="dim">v${esc(s.version)}</span><br>
   <span class="dim">WiFi:</span> ${s.wifi_mode=="ap"
     ?`<b>setup AP</b> — connect to "${esc(s.ap_ssid)}", any page opens here`
     :`${esc(s.ssid)} · ${esc(s.ip)}`}<br>
   <span class="dim">State:</span> <b>${esc(s.pair_state)}</b>
   ${s.batton_id?`<span class="dim mono">(${esc(s.batton_id)})</span>`:""}`;
 $("url").value=s.server_url||"";
 $("name").value=s.name||"";
 $("openLink").href=s.server_url+"/battons.html";
 $("openLink").textContent=s.server_url?"open conductor →":"";
 $("pairBtn").textContent=s.pair_state=="offered"?"Pairing…":"Pair";
 $("wifiMsg").textContent = s.wifi_mode=="ap"
   ?"Setup mode: pick your network below — the device reboots onto it."
   :`Connected to ${s.ssid||"—"}. Change network:`;
 $("pairMsg").textContent=
  s.pair_state=="offered"?"Waiting — authorize this Batton on the conductor's Battons page.":
  s.pair_state=="denied"?"Pair request was denied on the conductor.":
  s.pair_state=="paired"?"Paired. Pin events fire automations on the conductor.":"";
 if(!inputs.length)inputs=s.inputs||[];
 renderInputs();
}
function renderInputs(){
 $("inrows").innerHTML=inputs.map((x,i)=>`<div class="row">
  <input type="text" placeholder="id (e.g. door)" value="${esc(x.id)}" onchange="inputs[${i}].id=this.value" style="width:34%">
  <input type="number" placeholder="GPIO" value="${x.pin}" onchange="inputs[${i}].pin=+this.value" style="width:20%">
  <select onchange="inputs[${i}].edge=this.value">
   <option value="low" ${x.edge=="high"?"":"selected"}>LOW (to GND)</option>
   <option value="high" ${x.edge=="high"?"selected":""}>HIGH</option></select>
  <button class="ghost" onclick="inputs.splice(${i},1);renderInputs()">✕</button></div>`).join("");
}
async function scan(){
 $("nets").innerHTML='<span class="dim">scanning…</span>';
 const r=await api("/api/scan");
 $("nets").innerHTML=(r.nets||[]).sort((a,b)=>b.rssi-a.rssi)
  .map(n=>`<span class="chip" style="cursor:pointer"
   onclick="$('ssid').value='${esc(n.ssid)}'">
   ${esc(n.ssid)} ${n.open?"":"🔒"}</span>`).join("")||'<span class="dim">none found</span>';
}
async function saveWifi(){
 const r=await api("/api/wifi",{method:"POST",headers:{"Content-Type":"application/json"},
  body:JSON.stringify({ssid:$("ssid").value.trim(),pass:$("wpass").value})});
 toast(r.error||"saved — rebooting onto the network");
}
function addInput(){inputs.push({id:"in-"+(inputs.length+1),pin:13,edge:"low"});renderInputs()}
async function saveServer(){
 const r=await api("/api/server",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({url:$("url").value.trim()})});
 toast(r.error||"saved");refresh();
}
async function startPair(){
 await saveServer();
 const r=await api("/api/pair",{method:"POST"});
 toast(r.error||"pair request sent — authorize on the conductor");refresh();
}
async function saveInputs(){
 const r=await api("/api/inputs",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({inputs})});
 toast(r.error||"inputs saved");refresh();
}
async function saveName(){
 await api("/api/name",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({name:$("name").value.trim()})});
 toast("saved");refresh();
}
async function forget(){
 if(!confirm("Forget pairing and conductor URL stays, but the device must re-pair?"))return;
 await api("/api/forget",{method:"POST"});toast("forgotten");refresh();
}
refresh();setInterval(refresh,3000);
</script>)HTML";

void sendJson(int code, const String &s) {
  http.send(code, "application/json", s);
}

void apiState() {
  JsonDocument doc;
  doc["name"] = deviceName;
  doc["version"] = FW_VERSION;
  doc["ssid"] = apMode ? "" : WiFi.SSID();
  doc["ip"] = (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString();
  doc["wifi_mode"] = apMode ? "ap" : "sta";
  if (apMode) doc["ap_ssid"] = apSsid;
  doc["mac"] = WiFi.macAddress();
  doc["server_url"] = serverUrl;
  doc["batton_id"] = battonId;
  doc["pair_state"] = pairState == PAIRED ? "paired"
    : pairState == OFFERED ? "offered"
    : pairState == DENIED ? "denied" : "unpaired";
  String s;
  { String ij = inputsJson(); doc["inputs"] = serialized(ij);
    serializeJson(doc, s); }
  sendJson(200, s);
}

bool readBody(JsonDocument &doc) {
  return !deserializeJson(doc, http.arg("plain"));
}

void setupHttp() {
  http.on("/", HTTP_GET, [] { http.send_P(200, "text/html", PAGE); });
  http.on("/api/state", HTTP_GET, apiState);

  http.on("/api/server", HTTP_POST, [] {
    JsonDocument doc;
    if (!readBody(doc)) return sendJson(400, "{\"error\":\"bad json\"}");
    serverUrl = doc["url"].as<String>();
    serverUrl.trim();
    while (serverUrl.endsWith("/")) serverUrl.remove(serverUrl.length() - 1);
    if (serverUrl.length() && !serverUrl.startsWith("http"))
      serverUrl = "http://" + serverUrl;
    saveState();
    sendJson(200, "{\"ok\":true}");
  });

  http.on("/api/pair", HTTP_POST, [] {
    if (!serverUrl.length())
      return sendJson(400, "{\"error\":\"set the server URL first\"}");
    if (pairState == PAIRED)
      return sendJson(200, "{\"ok\":true,\"already\":true}");
    offerPair();
    sendJson(pairState == OFFERED ? 200 : 502,
             pairState == OFFERED ? "{\"ok\":true}"
                                  : "{\"error\":\"conductor unreachable\"}");
  });

  http.on("/api/name", HTTP_POST, [] {
    JsonDocument doc;
    if (!readBody(doc)) return sendJson(400, "{\"error\":\"bad json\"}");
    deviceName = doc["name"].as<String>();
    deviceName.trim();
    if (!deviceName.length()) deviceName = DEFAULT_DEVICE_NAME;
    saveState();
    sendJson(200, "{\"ok\":true}");
  });

  http.on("/api/inputs", HTTP_POST, [] {
    JsonDocument doc;
    if (!readBody(doc)) return sendJson(400, "{\"error\":\"bad json\"}");
    inputCount = 0;
    for (JsonObject o : doc["inputs"].as<JsonArray>()) {
      int pin = o["pin"] | 0;
      if (pin <= 0 || inputCount >= MAX_INPUTS) continue;
      inputs[inputCount] = {
        String((const char *)(o["id"] | "")),
        (uint8_t)pin,
        String((const char *)(o["edge"] | "low")) == "high"
      };
      if (!inputs[inputCount].id.length())
        inputs[inputCount].id = "pin-" + String(pin);
      pinMode(pin, inputs[inputCount].activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
      lastLevel[inputCount] = digitalRead(pin);
      inputCount++;
    }
    saveState();
    sendJson(200, "{\"ok\":true,\"count\":" + String(inputCount) + "}");
  });

  http.on("/api/forget", HTTP_POST, [] {
    forgetPairing();
    sendJson(200, "{\"ok\":true}");
  });

  http.on("/api/wifi", HTTP_POST, [] {
    JsonDocument doc;
    if (!readBody(doc)) return sendJson(400, "{\"error\":\"bad json\"}");
    wifiSsid = doc["ssid"].as<String>();
    wifiPass = doc["pass"].as<String>();
    wifiSsid.trim();
    if (!wifiSsid.length()) return sendJson(400, "{\"error\":\"ssid required\"}");
    saveState();
    sendJson(200, "{\"ok\":true}");
    delay(600);            // let the response leave before the radio resets
    ESP.restart();
  });

  http.on("/api/scan", HTTP_GET, [] {
    int n = WiFi.scanNetworks();
    JsonDocument doc;
    JsonArray arr = doc["nets"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = WiFi.SSID(i);
      o["rssi"] = WiFi.RSSI(i);
      o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    }
    String s; serializeJson(doc, s);
    sendJson(200, s);
  });

  // Captive-portal detection endpoints — iOS/Android/Windows probe URLs.
  // Anything else in AP mode also lands on the setup page.
  auto portal = [] { http.send(302, "text/plain", ""); };
  http.on("/generate_204", HTTP_GET, portal);          // Android
  http.on("/gen_204", HTTP_GET, portal);
  http.on("/hotspot-detect.html", HTTP_GET,
          [] { http.send_P(200, "text/html", PAGE); }); // iOS/macOS
  http.on("/library/test/success.html", HTTP_GET,
          [] { http.send_P(200, "text/html", PAGE); }); // iOS alt
  http.on("/ncsi.txt", HTTP_GET,
          [] { http.send_P(200, "text/html", PAGE); }); // Windows
  http.on("/connecttest.txt", HTTP_GET,
          [] { http.send_P(200, "text/html", PAGE); }); // Windows alt
  http.on("/fwlink", HTTP_GET,
          [] { http.send_P(200, "text/html", PAGE); }); // Windows alt
  http.onNotFound([] {
    if (apMode) {                                        // DNS catches all
      http.sendHeader("Location", "http://192.168.4.1/");
      http.send(302, "text/plain", "");
    } else {
      sendJson(404, "{\"error\":\"not found\"}");
    }
  });

  http.begin();
}

// -- Pins --------------------------------------------------------------------

void setupPins() {
  for (int i = 0; i < inputCount; i++) {
    pinMode(inputs[i].pin, inputs[i].activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
    lastLevel[i] = digitalRead(inputs[i].pin);
    lastChange[i] = lastFire[i] = 0;
  }
}

void watchPins() {
  unsigned long now = millis();
  for (int i = 0; i < inputCount; i++) {
    int level = digitalRead(inputs[i].pin);
    if (level != lastLevel[i]) {
      lastLevel[i] = level;
      lastChange[i] = now;
      continue;
    }
    // 40ms debounce, 1s per-input re-fire floor (conductor cooldown is policy).
    int active = inputs[i].activeHigh ? HIGH : LOW;
    if (level == active && now - lastChange[i] > 40 && now - lastFire[i] > 1000) {
      lastFire[i] = now;
      fireInput(inputs[i].id);
    }
  }
}

// -- Setup / loop --------------------------------------------------------------

bool connectWifi() {
  if (!wifiSsid.length()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  Serial.printf("wifi %s", wifiSsid.c_str());
  for (int i = 0; i < 50 && WiFi.status() != WL_CONNECTED; i++) {
    delay(400); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n%s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("\nconnect failed");
  return false;
}

// No saved network (or it vanished): become an open AP with a captive
// portal — every DNS answer points at us, so any page a phone tries to
// open lands on the setup UI.
void startAp() {
  WiFi.mode(WIFI_AP);
  apSsid = "Batton-" + WiFi.macAddress().substring(9);
  apSsid.replace(":", "");
  WiFi.softAP(apSsid.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  apMode = true;
  Serial.printf("AP %s @ %s\n", apSsid.c_str(),
                WiFi.softAPIP().toString().c_str());
}

unsigned long wifiDown = 0;

void setup() {
  Serial.begin(115200);
  loadState();
  setupPins();
  if (!connectWifi()) startAp();
  setupHttp();
  Serial.printf("web ui: http://%s/\n",
    (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str());
}

void loop() {
  http.handleClient();
  if (apMode) { dns.processNextRequest(); return; }
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lost = 0;
    if (now - lost > 30000) { lost = now; WiFi.reconnect(); }
    // ~2 min with no network: re-offer ourselves as a setup AP so a
    // device moved to a new site recovers without a serial console.
    if (!wifiDown) wifiDown = now;
    if (now - wifiDown > 120000) { wifiDown = 0; startAp(); }
    return;
  }
  wifiDown = 0;
  if (pairState == OFFERED && now - lastPoll > 3000) {
    lastPoll = now;
    pollPair();
  }
  if (now - lastBeat > 60000) {
    lastBeat = now;
    heartbeat();
  }
  watchPins();
}
