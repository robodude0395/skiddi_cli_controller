#ifndef SKIDSTEER_CONTROLLER_H
#define SKIDSTEER_CONTROLLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <WebServer.h>

// =============================================================
// SkidSteerController
// -------------------------------------------------------------
// Hosts a WiFi access point + HTTP REST API in the same format
// as the 3ds-to-esp32 project. Any client (a 3DS, a phone, a
// laptop, a Python script) can drive the skidsteer by issuing
// simple HTTP GET requests.
//
// This class only owns the transport + input state. It does NOT
// touch any motor/servo pins -- your main.cpp reads getInput()
// and maps it onto the hardware. That keeps the program
// "universal": swap the controller, keep the firmware.
//
// === REST API ===
//
//   GET /i?b=<hexmask>&x=<-100..100>&y=<-100..100>&a=<-100..100>
//       Batched input (preferred, one request per frame).
//       b = hex button bitmask (see bit layout below)
//       x = drive stick X (turn)      -100..100
//       y = drive stick Y (throttle)  -100..100
//       a = arm axis (lift up/down)   -100..100  (optional)
//
//   GET /b?i=<ID>&s=<0|1>     Single button (ID list below)
//   GET /s?x=<X>&y=<Y>&a=<A>  Single axis update
//   GET /cfg?...              Set tunable params (see main.cpp)
//   GET /cfg_get              Read tunable params
//   GET /status              Human-readable current input state
//
// === Button bit layout (matches 3ds-to-esp32) ===
//   bit0=A  bit1=B  bit2=X  bit3=Y
//   bit4=L  bit5=R  bit6=Start bit7=Select
//   bit8=Up bit9=Down bit10=Left bit11=Right
//
// === Button IDs (for /b) ===
//   A B X Y L R ST SE U D LT RT
// =============================================================

struct SkidSteerInput {
  // Buttons (true = pressed)
  bool A = false;       // suggested: lights toggle / action
  bool B = false;
  bool X = false;
  bool Y = false;
  bool L = false;       // suggested: arm down
  bool R = false;       // suggested: arm up
  bool Start = false;
  bool Select = false;
  bool Up = false;       // D-pad (bucket / arm nudge)
  bool Down = false;
  bool Left = false;
  bool Right = false;

  // Analog axes (-100 to 100)
  int driveX = 0;    // turn:      -100 = full left,  100 = full right
  int driveY = 0;    // throttle:  -100 = full back,  100 = full forward
  int armAxis = 0;   // arm lift:  -100 = down,       100 = up

  // Bucket servo positions (-100 to 100). These are absolute positions,
  // not deltas: the client holds the position and sends it each frame.
  int tiltPos = 0;   // bucket tilt   servo target
  int raisePos = 0;  // bucket raise  servo target

  bool anyActive() const {
    return A || B || X || Y || L || R || Start || Select ||
           Up || Down || Left || Right ||
           driveX != 0 || driveY != 0 || armAxis != 0;
    // Note: tiltPos/raisePos are held positions, not "activity", so they
    // are intentionally excluded here.
  }

  void clear() {
    A = B = X = Y = L = R = Start = Select = false;
    Up = Down = Left = Right = false;
    driveX = driveY = armAxis = 0;
    // Do NOT reset tiltPos/raisePos on input timeout: a bucket should
    // hold its last commanded position when the client goes quiet,
    // rather than snapping back to center.
  }
};

typedef void (*SkidSteerInputCallback)(const SkidSteerInput& input);

// WiFi mode the controller ended up running in after begin().
// (Prefixed to avoid clashing with the ESP-IDF wifi_mode_t constants
//  WIFI_MODE_AP / WIFI_MODE_STA that WiFi.h pulls in.)
enum SkidSteerWifiMode {
  SS_WIFI_AP,   // hosting its own access point (fallback / default)
  SS_WIFI_STA   // joined an existing network as a client
};

class SkidSteerController {
public:
  // Configure the fallback access-point credentials (call before begin).
  void setAP(const char* ssid, const char* password);

  // Configure credentials for an EXISTING network to join. If set (and
  // non-empty), begin() tries to connect as a station first, and only
  // falls back to hosting its own AP if the connection fails.
  //   ssid     - network to join
  //   password - network password (use "" for an open network)
  //   timeoutMs- how long to wait for a connection before falling back
  void setStation(const char* ssid, const char* password,
                  unsigned long timeoutMs = 15000);

  // Set the UDP port used for fast, fire-and-forget control packets
  // (call before begin). Set to 0 to disable UDP. Default: 4210.
  void setUdpPort(uint16_t port);

  // Start WiFi (STA if station creds were set and connect succeeds,
  // otherwise AP) + DNS + web server + UDP control listener.
  void begin();

  // Which mode begin() settled on, and the reachable IP address.
  SkidSteerWifiMode wifiMode() const { return _mode; }
  String ipAddress() const;

  // Call every loop() - services DNS + HTTP clients, handles timeout
  void update();

  // Current input state
  const SkidSteerInput& getInput() const;

  // True if a client sent input within timeoutMs
  bool isConnected(unsigned long timeoutMs = 3000) const;

  // Callback fired whenever new input arrives
  void onInput(SkidSteerInputCallback callback);

  // Auto-clear inputs after this many ms of silence (0 = never).
  // This is a SAFETY feature: if the client drops off, the robot
  // stops instead of running away with its last command.
  void setInputTimeout(unsigned long ms);

  // Access the internal server to register extra endpoints
  WebServer& getServer();
  void addEndpoint(const char* uri, HTTPMethod method,
                   WebServer::THandlerFunction handler);

private:
  // Fallback access-point credentials
  const char* _ssid = "SkidSteer";
  const char* _pass = "skidsteer123";

  // Station (join-an-existing-network) credentials. Empty _staSsid
  // means "AP mode only".
  const char* _staSsid = "";
  const char* _staPass = "";
  unsigned long _staTimeout = 15000;

  SkidSteerWifiMode _mode = SS_WIFI_AP;

  DNSServer _dns;
  bool _dnsActive = false;

  // UDP fast-control channel
  WiFiUDP _udp;
  uint16_t _udpPort = 4210;
  bool _udpActive = false;
  WebServer _server{80};
  SkidSteerInput _input;
  SkidSteerInputCallback _callback = nullptr;
  unsigned long _lastInput = 0;
  unsigned long _timeout = 1000;  // shorter than the 3DS project: a
                                  // moving robot should fail-stop fast

  void _handleInput();
  void _handleButton();
  void _handleStick();
  void _handleStatus();
  void _handleConnTest();
  bool _isNintendoCheck();

  // Parse a fire-and-forget control packet ("b=.. x=.. y=.. a=.. t=.. p=..")
  // and apply it to _input. Shared by UDP (and reusable elsewhere).
  void _applyPacket(const char* data, size_t len);
  void _pollUdp();
};

#endif
