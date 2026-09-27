#include "SkidSteerController.h"

// Nintendo connectivity-check response. Kept so a real Nintendo 3DS
// (or any client that pings conntest.nintendowifi.net) passes its
// connection test when its DNS is pointed at this ESP32.
static const char* CONN_RESPONSE =
  "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" "
  "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\r\n"
  "<html>\r\n<head>\r\n<title>HTML Page</title>\r\n</head>\r\n"
  "<body bgcolor=\"#FFFFFF\">\r\nThis is test.\r\n</body>\r\n</html>\r\n";

void SkidSteerController::setAP(const char* ssid, const char* password) {
  _ssid = ssid;
  _pass = password;
}

void SkidSteerController::setStation(const char* ssid, const char* password,
                                     unsigned long timeoutMs) {
  _staSsid = ssid ? ssid : "";
  _staPass = password ? password : "";
  _staTimeout = timeoutMs;
}

void SkidSteerController::setUdpPort(uint16_t port) {
  _udpPort = port;
}

String SkidSteerController::ipAddress() const {
  return (_mode == SS_WIFI_STA) ? WiFi.localIP().toString()
                                : WiFi.softAPIP().toString();
}

void SkidSteerController::begin() {
  bool joined = false;

  // Try to join an existing network first, if station creds were given.
  if (_staSsid && _staSsid[0] != '\0') {
    Serial.printf("[SkidSteer] Joining network \"%s\"...\n", _staSsid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(_staSsid, _staPass);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < _staTimeout) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      joined = true;
      _mode = SS_WIFI_STA;
      Serial.printf("[SkidSteer] Connected. IP: %s\n",
                    WiFi.localIP().toString().c_str());
    } else {
      Serial.println("[SkidSteer] Join failed, falling back to AP mode.");
      WiFi.disconnect(true);
    }
  }

  // Fall back to hosting our own access point.
  if (!joined) {
    _mode = SS_WIFI_AP;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(_ssid, _pass);
    // DNS captive portal only makes sense as an AP (needed for the 3DS
    // connectivity check). Skip it in station mode.
    _dns.start(53, "*", WiFi.softAPIP());
    _dnsActive = true;
  }

  // Batched input endpoint (preferred - one request per frame)
  _server.on("/i", HTTP_GET, [this]() { _handleInput(); });

  // Legacy individual endpoints (still supported)
  _server.on("/b", HTTP_GET, [this]() { _handleButton(); });
  _server.on("/s", HTTP_GET, [this]() { _handleStick(); });

  // Human-readable state dump (handy from a browser)
  _server.on("/status", HTTP_GET, [this]() { _handleStatus(); });

  // Connectivity checks / captive-portal catch-all
  _server.on("/", HTTP_ANY, [this]() { _handleConnTest(); });
  _server.onNotFound([this]() { _handleConnTest(); });

  _server.begin();

  // Start the fast fire-and-forget UDP control listener.
  if (_udpPort != 0) {
    _udpActive = _udp.begin(_udpPort);
    if (_udpActive) {
      Serial.printf("[SkidSteer] UDP control on port %u\n", _udpPort);
    } else {
      Serial.println("[SkidSteer] UDP listener failed to start");
    }
  }

  if (_mode == SS_WIFI_STA) {
    Serial.printf("[SkidSteer] Station mode on \"%s\" @ %s\n",
                  _staSsid, WiFi.localIP().toString().c_str());
  } else {
    Serial.printf("[SkidSteer] AP \"%s\" @ %s\n",
                  _ssid, WiFi.softAPIP().toString().c_str());
  }
}

void SkidSteerController::update() {
  if (_dnsActive) _dns.processNextRequest();
  if (_udpActive) _pollUdp();
  _server.handleClient();

  // Safety fail-stop: clear inputs if the client goes quiet.
  if (_timeout > 0 && _lastInput > 0 && millis() - _lastInput > _timeout) {
    _input.clear();
    _lastInput = 0;
    if (_callback) _callback(_input);  // let main.cpp push neutral out
  }
}

const SkidSteerInput& SkidSteerController::getInput() const {
  return _input;
}

bool SkidSteerController::isConnected(unsigned long timeoutMs) const {
  if (_lastInput == 0) return false;
  return (millis() - _lastInput) < timeoutMs;
}

void SkidSteerController::onInput(SkidSteerInputCallback callback) {
  _callback = callback;
}

void SkidSteerController::setInputTimeout(unsigned long ms) {
  _timeout = ms;
}

WebServer& SkidSteerController::getServer() {
  return _server;
}

void SkidSteerController::addEndpoint(const char* uri, HTTPMethod method,
                                      WebServer::THandlerFunction handler) {
  _server.on(uri, method, handler);
}

// Read all pending UDP control packets. Only the LAST one matters (it's
// the freshest state), but we drain the queue so it can't back up.
void SkidSteerController::_pollUdp() {
  static char buf[128];
  int packetSize;
  while ((packetSize = _udp.parsePacket()) > 0) {
    int n = _udp.read(buf, sizeof(buf) - 1);
    if (n <= 0) continue;
    buf[n] = '\0';
    _applyPacket(buf, (size_t)n);
  }
}

// Parse a fire-and-forget control packet and apply it to _input.
// Format (whitespace-separated key=value, any subset, any order):
//   b=<hexmask> x=<-100..100> y=<..> a=<..> t=<..> p=<..>
// This mirrors the /i query args so both channels behave identically.
void SkidSteerController::_applyPacket(const char* data, size_t len) {
  (void)len;
  // Tiny hand parser: scan for "<key>=" tokens.
  const char* s = data;
  while (*s) {
    // skip separators
    while (*s == ' ' || *s == '&' || *s == ',' || *s == '\t' ||
           *s == '\r' || *s == '\n') s++;
    if (!*s) break;

    char key = *s;
    const char* eq = s + 1;
    if (*eq != '=') {          // not a key=value token; skip to next sep
      while (*s && *s != ' ' && *s != '&' && *s != ',') s++;
      continue;
    }
    const char* valStr = eq + 1;

    if (key == 'b') {
      unsigned int m = strtoul(valStr, NULL, 16);
      _input.A      = (m & (1 << 0)) != 0;
      _input.B      = (m & (1 << 1)) != 0;
      _input.X      = (m & (1 << 2)) != 0;
      _input.Y      = (m & (1 << 3)) != 0;
      _input.L      = (m & (1 << 4)) != 0;
      _input.R      = (m & (1 << 5)) != 0;
      _input.Start  = (m & (1 << 6)) != 0;
      _input.Select = (m & (1 << 7)) != 0;
      _input.Up     = (m & (1 << 8)) != 0;
      _input.Down   = (m & (1 << 9)) != 0;
      _input.Left   = (m & (1 << 10)) != 0;
      _input.Right  = (m & (1 << 11)) != 0;
    } else {
      int v = constrain((int)strtol(valStr, NULL, 10), -100, 100);
      switch (key) {
        case 'x': _input.driveX   = v; break;
        case 'y': _input.driveY   = v; break;
        case 'a': _input.armAxis  = v; break;
        case 't': _input.tiltPos  = v; break;
        case 'p': _input.raisePos = v; break;
        default: break;
      }
    }

    // advance past this token
    while (*s && *s != ' ' && *s != '&' && *s != ',' &&
           *s != '\t' && *s != '\r' && *s != '\n') s++;
  }

  _lastInput = millis();
  if (_callback) _callback(_input);
}

// GET /i?b=<hexmask>&x=<X>&y=<Y>&a=<A>
void SkidSteerController::_handleInput() {
  unsigned int btnMask = 0;
  if (_server.hasArg("b")) {
    btnMask = strtoul(_server.arg("b").c_str(), NULL, 16);
  }

  _input.A      = (btnMask & (1 << 0)) != 0;
  _input.B      = (btnMask & (1 << 1)) != 0;
  _input.X      = (btnMask & (1 << 2)) != 0;
  _input.Y      = (btnMask & (1 << 3)) != 0;
  _input.L      = (btnMask & (1 << 4)) != 0;
  _input.R      = (btnMask & (1 << 5)) != 0;
  _input.Start  = (btnMask & (1 << 6)) != 0;
  _input.Select = (btnMask & (1 << 7)) != 0;
  _input.Up     = (btnMask & (1 << 8)) != 0;
  _input.Down   = (btnMask & (1 << 9)) != 0;
  _input.Left   = (btnMask & (1 << 10)) != 0;
  _input.Right  = (btnMask & (1 << 11)) != 0;

  if (_server.hasArg("x")) _input.driveX   = constrain(_server.arg("x").toInt(), -100, 100);
  if (_server.hasArg("y")) _input.driveY   = constrain(_server.arg("y").toInt(), -100, 100);
  if (_server.hasArg("a")) _input.armAxis  = constrain(_server.arg("a").toInt(), -100, 100);
  if (_server.hasArg("t")) _input.tiltPos  = constrain(_server.arg("t").toInt(), -100, 100);
  if (_server.hasArg("p")) _input.raisePos = constrain(_server.arg("p").toInt(), -100, 100);

  _lastInput = millis();
  if (_callback) _callback(_input);

  _server.send(200, "text/plain", "OK");
}

// GET /b?i=<ID>&s=<0|1>
void SkidSteerController::_handleButton() {
  String id = _server.arg("i");
  bool pressed = _server.arg("s").toInt() == 1;

  if (id == "A") _input.A = pressed;
  else if (id == "B") _input.B = pressed;
  else if (id == "X") _input.X = pressed;
  else if (id == "Y") _input.Y = pressed;
  else if (id == "L") _input.L = pressed;
  else if (id == "R") _input.R = pressed;
  else if (id == "ST") _input.Start = pressed;
  else if (id == "SE") _input.Select = pressed;
  else if (id == "U") _input.Up = pressed;
  else if (id == "D") _input.Down = pressed;
  else if (id == "LT") _input.Left = pressed;
  else if (id == "RT") _input.Right = pressed;

  _lastInput = millis();
  if (_callback) _callback(_input);

  _server.send(200, "text/plain", "OK");
}

// GET /s?x=<X>&y=<Y>&a=<A>
void SkidSteerController::_handleStick() {
  if (_server.hasArg("x")) _input.driveX  = constrain(_server.arg("x").toInt(), -100, 100);
  if (_server.hasArg("y")) _input.driveY  = constrain(_server.arg("y").toInt(), -100, 100);
  if (_server.hasArg("a")) _input.armAxis = constrain(_server.arg("a").toInt(), -100, 100);

  _lastInput = millis();
  if (_callback) _callback(_input);

  _server.send(200, "text/plain", "OK");
}

void SkidSteerController::_handleStatus() {
  char buf[256];
  snprintf(buf, sizeof(buf),
    "driveX=%d driveY=%d armAxis=%d\n"
    "A=%d B=%d X=%d Y=%d L=%d R=%d Start=%d Select=%d\n"
    "Up=%d Down=%d Left=%d Right=%d\n"
    "connected=%d\n",
    _input.driveX, _input.driveY, _input.armAxis,
    _input.A, _input.B, _input.X, _input.Y, _input.L, _input.R,
    _input.Start, _input.Select,
    _input.Up, _input.Down, _input.Left, _input.Right,
    isConnected() ? 1 : 0);
  _server.send(200, "text/plain", buf);
}

void SkidSteerController::_handleConnTest() {
  if (_isNintendoCheck()) {
    _server.sendHeader("X-Organization", "Nintendo");
    _server.send(200, "text/html", CONN_RESPONSE);
  } else {
    _server.send(200, "text/plain", "SkidSteer Controller Server");
  }
}

bool SkidSteerController::_isNintendoCheck() {
  String host = _server.hostHeader();
  return host.indexOf("nintendo") >= 0 || host.indexOf("conntest") >= 0;
}
