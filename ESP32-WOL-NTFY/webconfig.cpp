#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "settings.h"
#include "webconfig.h"

namespace webconfig {
namespace {

WebServer server(80);
DNSServer dns;

bool apMode      = false;
bool authNeeded  = true;
bool running     = false;
bool savedFlag   = false;

const byte DNS_PORT = 53;

String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length());
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;
    }
  }
  return out;
}

const char PAGE_HEAD[] PROGMEM =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>WOL bot setup</title><style>"
    "body{font-family:system-ui,sans-serif;margin:0;padding:1rem;max-width:34rem;"
    "background:#111;color:#eee}"
    "h1{font-size:1.2rem}h2{font-size:.95rem;margin:1.4rem 0 .4rem;color:#8bd}"
    "label{display:block;margin:.6rem 0 .15rem;font-size:.85rem;color:#bbb}"
    "input[type=text],input[type=password],input[type=number]{width:100%;"
    "box-sizing:border-box;padding:.5rem;border:1px solid #444;border-radius:4px;"
    "background:#1c1c1c;color:#eee;font-size:1rem}"
    "small{color:#888;font-size:.75rem}"
    "button{margin-top:1.2rem;width:100%;padding:.7rem;font-size:1rem;border:0;"
    "border-radius:4px;background:#2a7;color:#022;font-weight:600}"
    ".row{display:flex;gap:.6rem}.row>div{flex:1}"
    ".err{background:#833;padding:.5rem;border-radius:4px;margin-bottom:.8rem}"
    "</style>";

// Returns false and sends the challenge if the request should be rejected.
bool authorized() {
  if (!authNeeded) return true;

  // No admin password stored. Leaving the page open would let anyone on the
  // LAN (or anyone in range of the fallback AP) repoint the device, so it stays
  // shut until one is set -- which needs the BOOT button, i.e. physical access.
  if (g_settings.adminPass.isEmpty()) {
    String p = FPSTR(PAGE_HEAD);
    p += "<h1>Settings locked</h1><p>No admin password is set yet. Press "
         "<b>EN</b> (reset) on the board, then immediately press and hold "
         "<b>BOOT</b> for two seconds. It opens the <b>" + htmlEscape(apSsid()) +
         "</b> network, where you can set one.</p>";
    server.send(403, "text/html", p);
    return false;
  }

  if (server.authenticate(ADMIN_USER, g_settings.adminPass.c_str())) return true;
  server.requestAuthentication();
  return false;
}

void handleScan() {
  if (!authorized()) return;

  int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n && i < 30; i++) {
    if (i) j += ",";
    j += "\"" + htmlEscape(WiFi.SSID(i)) + "\"";
  }
  WiFi.scanDelete();
  server.send(200, "application/json", j + "]");
}

void sendForm(const String& error) {
  const Settings& s = g_settings;

  String p = FPSTR(PAGE_HEAD);
  p += "<h1>WOL bot setup</h1>";
  if (!error.isEmpty()) p += "<div class=err>" + htmlEscape(error) + "</div>";
  p += "<form method=POST action=/save>";

  p += "<h2>WiFi</h2>";
  p += "<label>Network (2.4 GHz only)</label>";
  p += "<input type=text name=ssid list=nets value='" + htmlEscape(s.wifiSsid) +
       "' required><datalist id=nets></datalist>";
  p += "<label>Password</label><input type=password name=wpass>";
  p += "<small>Leave blank to keep the stored password.</small>";

  p += "<h2>ntfy server</h2>";
  p += "<label>Host</label><input type=text name=host value='" +
       htmlEscape(s.ntfyHost) + "' required>";
  p += "<div class=row><div><label>Port</label>"
       "<input type=number name=port min=1 max=65535 value='" +
       String(s.ntfyPort) + "' required></div>";
  p += "<div><label>TLS</label><input type=checkbox name=tls" +
       String(s.ntfyTls ? " checked" : "") + "> https</div></div>";
  p += "<label>Topic</label><input type=text name=topic value='" +
       htmlEscape(s.ntfyTopic) + "' required>";
  p += "<small>Commands and replies share this topic. Make it long and "
       "unguessable.</small>";

  p += "<h2>ntfy login <small>(blank for an open server)</small></h2>";
  p += "<label>Username</label><input type=text name=nuser value='" +
       htmlEscape(s.ntfyUser) + "'>";
  p += "<label>Password</label><input type=password name=npass>";
  p += "<label>or Token</label><input type=password name=token>";
  p += "<small>A token wins over username and password. Blank keeps stored "
       "values; tick below to remove them.</small>";
  p += "<label><input type=checkbox name=nclear> clear stored ntfy login</label>";

  p += "<h2>Passphrases</h2>";
  p += "<label>Command passphrase</label><input type=password name=cmdpass>";
  p += "<small>Prefix for every command sent from the ntfy app "
       "(<i>passphrase wake pc</i>). Required for add / remove / factoryreset.</small>";
  p += "<label>Admin password (this page)</label><input type=password name=admpass>";
  p += "<small>Login is <b>admin</b>. Keep it different from the command "
       "passphrase. Blank keeps the stored ones.</small>";

  p += "<button type=submit>Save &amp; restart</button></form>";

  // Populates the network picker. Typing a network name by hand is how the
  // SSID typo happened once already.
  p += "<script>fetch('/scan').then(r=>r.json()).then(a=>{"
       "document.getElementById('nets').innerHTML="
       "a.map(s=>'<option value=\"'+s+'\">').join('')}).catch(()=>{});</script>";

  server.send(200, "text/html", p);
}

void handleRoot() {
  if (!authorized()) return;
  sendForm("");
}

// ntfy topics are [-_A-Za-z0-9]{1,64}; anything else would also break the URL.
bool validTopic(const String& t) {
  if (t.isEmpty() || t.length() > 64) return false;
  for (unsigned int i = 0; i < t.length(); i++) {
    char c = t[i];
    if (!isalnum((unsigned char)c) && c != '-' && c != '_') return false;
  }
  return true;
}

void handleSave() {
  if (!authorized()) return;

  String ssid  = server.arg("ssid");
  String host  = server.arg("host");
  String topic = server.arg("topic");
  String nuser = server.arg("nuser");
  long   port  = server.arg("port").toInt();

  ssid.trim(); host.trim(); topic.trim(); nuser.trim();

  if (ssid.isEmpty())               { sendForm("WiFi network is required."); return; }
  if (host.isEmpty())               { sendForm("ntfy host is required."); return; }
  if (!validTopic(topic)) {
    sendForm("ntfy topic must be 1-64 letters, digits, - or _.");
    return;
  }
  if (port < 1 || port > 65535)     { sendForm("Port must be 1-65535."); return; }

  // Blank password fields mean "keep what is stored" -- the form never renders
  // a stored password back to the browser, so blank is the normal state.
  String wpass   = server.arg("wpass");
  String npass   = server.arg("npass");
  String token   = server.arg("token");
  String cmdpass = server.arg("cmdpass");
  String admpass = server.arg("admpass");
  cmdpass.trim();

  // Work on a copy so a rejected form leaves the running config untouched.
  Settings s = g_settings;

  if (!wpass.isEmpty())   s.wifiPass  = wpass;
  if (!cmdpass.isEmpty()) s.cmdPass   = cmdpass;
  if (!admpass.isEmpty()) s.adminPass = admpass;

  // On a first-time setup there is nothing to keep, so this must be supplied.
  if (!g_settings.provisioned && wpass.isEmpty()) {
    sendForm("WiFi password is required.");
    return;
  }
  if (s.cmdPass.isEmpty())   { sendForm("Command passphrase is required."); return; }
  if (s.adminPass.isEmpty()) { sendForm("Admin password is required."); return; }
  // Commands match the passphrase case-insensitively, so compare that way too.
  if (s.cmdPass.equalsIgnoreCase(s.adminPass)) {
    sendForm("Use a different admin password from the command passphrase.");
    return;
  }

  s.wifiSsid  = ssid;
  s.ntfyHost  = host;
  s.ntfyPort  = (uint16_t)port;
  s.ntfyTls   = server.hasArg("tls");
  s.ntfyTopic = topic;
  s.ntfyUser  = nuser;
  if (server.hasArg("nclear")) {
    s.ntfyUser  = "";
    s.ntfyPass  = "";
    s.ntfyToken = "";
  }
  if (!npass.isEmpty()) s.ntfyPass  = npass;
  if (!token.isEmpty()) s.ntfyToken = token;

  // settings_save() persists g_settings, so swap the copy in and roll back if
  // the write fails.
  Settings previous = g_settings;
  g_settings = s;
  if (!settings_save()) {
    g_settings = previous;
    sendForm("Could not write to flash.");
    return;
  }
  savedFlag = true;

  String p = FPSTR(PAGE_HEAD);
  p += "<h1>Saved</h1><p>Restarting. The device will join <b>" +
       htmlEscape(ssid) + "</b> and announce its address on your ntfy topic.</p>";
  server.send(200, "text/html", p);

  delay(1500);       // let the response actually reach the browser
  ESP.restart();
}

// Captive-portal probes (/generate_204, /hotspot-detect.html, /ncsi.txt ...)
// all land here. Answering them with a redirect is what makes phones pop the
// setup page on their own. AP mode only -- doing this in STA would hijack
// ordinary name resolution on the LAN.
void handleNotFound() {
  if (!apMode) { handleRoot(); return; }
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

// Once only: the device can move between AP and STA several times per boot,
// and WebServer::on() appends rather than replaces.
void routes() {
  static bool done = false;
  if (done) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  done = true;
}

}  // namespace

void beginAP(bool requireAuth) {
  stop();
  apMode     = true;
  authNeeded = requireAuth;

  // AP_STA rather than AP: in the fallback case the station side keeps
  // retrying in the background, so a router coming back brings the device
  // online without a reboot.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid().c_str());
  delay(100);

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(DNS_PORT, "*", WiFi.softAPIP());

  routes();
  server.begin();
  running = true;

  Serial.printf("[web] AP \"%s\" up, portal at http://%s (auth %s)\n",
                apSsid().c_str(), WiFi.softAPIP().toString().c_str(),
                requireAuth ? "on" : "off");
}

void beginSTA() {
  stop();
  apMode     = false;
  authNeeded = true;

  routes();
  server.begin();
  running = true;

  Serial.printf("[web] settings page at http://%s\n",
                WiFi.localIP().toString().c_str());
}

void stop() {
  if (!running) return;
  server.stop();
  if (apMode) dns.stop();
  running = false;
}

void tick() {
  if (!running) return;
  if (apMode) dns.processNextRequest();
  server.handleClient();
}

bool isAP() { return apMode; }

String apSsid() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof(buf), "WOL-Setup-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

String ip() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

String settingsUrl() { return "http://" + ip(); }

bool saved() { return savedFlag; }

}  // namespace webconfig
