#include <Arduino.h>
#include <SkidSteerController.h>
#include <SkidSteer.h>

// =============================================================
// ESP32 Skidsteer - universal REST-controlled firmware
// -------------------------------------------------------------
// Architecture (mirrors the 3ds-to-esp32 project):
//
//   Any client --HTTP GET--> SkidSteerController (AP + REST API)
//                                   |
//                            SkidSteerInput struct
//                                   |
//                            main.cpp maps inputs
//                                   |
//                                SkidSteer  --> GPIO / PWM / servos
//
// Because control is just HTTP GET requests, the "controller" can
// be a Nintendo 3DS, a phone web page, a gamepad bridge, or a
// Python script. The firmware doesn't care where input comes from.
//
// === Default control scheme ===
//   Drive stick X/Y  -> tank drive (throttle + turn mix)
//   Arm axis (a)     -> lift arm motor
//   R button         -> arm up      (if no analog arm axis sent)
//   L button         -> arm down
//   Up / Down        -> bucket raise servo
//   Left / Right     -> bucket tilt servo
//   A button         -> toggle lights (edge-triggered)
// =============================================================

// =============================================================
// WiFi configuration
// -------------------------------------------------------------
// Leave STA_SSID empty ("") to make the ESP32 host its OWN access
// point (default). Controllers connect directly to it.
//
// Fill in STA_SSID / STA_PASS to make the ESP32 JOIN an existing
// network instead. If the join fails (bad password, network down,
// out of range) it automatically falls back to hosting its own AP,
// so you're never locked out.
//
//   Join a network:   set STA_SSID and STA_PASS below.
//   Host own AP:       leave STA_SSID as "".
// =============================================================
#define STA_SSID "#VM3605823"            // <-- your WiFi name, or "" for AP mode
#define STA_PASS "#mt5Xbjhrxpqj"            // <-- your WiFi password ("" if open)

#define AP_SSID  "SkidSteer"    // fallback access-point name
#define AP_PASS  "skidsteer123" // fallback access-point password (>= 8 chars)

SkidSteerController controller;
SkidSteer robot;

// Bucket servo positions come straight from the client as absolute
// positions (in.tiltPos / in.raisePos), so no local state is needed here.

// Edge detection for the lights toggle
bool lastA = false;

void setup() {
  Serial.begin(115200);

  robot.begin();
  robot.stopMotors();

  controller.setAP(AP_SSID, AP_PASS);
  // Fast fire-and-forget control channel (UDP). The keyboard client
  // blasts state here ~50x/sec and never waits for a reply.
  controller.setUdpPort(4210);
  // Only enables station mode if STA_SSID is non-empty; otherwise this
  // is a no-op and the controller stays in AP mode.
  if (sizeof(STA_SSID) > 1) {
    controller.setStation(STA_SSID, STA_PASS);
  }
  controller.begin();

  // Fail-stop fast if the client drops out (safety).
  controller.setInputTimeout(1000);

  // --- Config endpoint: tune params live (same style as /cfg in
  //     the 3DS repo). All args optional; echoes current values. ---
  controller.addEndpoint("/cfg", HTTP_GET, []() {
    WebServer& srv = controller.getServer();
    if (srv.hasArg("dp")) robot.setDrivePower(srv.arg("dp").toInt());
    if (srv.hasArg("ap")) robot.setArmPower(srv.arg("ap").toInt());
    if (srv.hasArg("rl")) robot.setReverseLeft(srv.arg("rl").toInt() != 0);
    if (srv.hasArg("rr")) robot.setReverseRight(srv.arg("rr").toInt() != 0);
    if (srv.hasArg("ra")) robot.setReverseArm(srv.arg("ra").toInt() != 0);
    if (srv.hasArg("rt")) robot.setRampRate(srv.arg("rt").toInt());

    char buf[160];
    snprintf(buf, sizeof(buf),
      "dp=%d&ap=%d&rl=%d&rr=%d&ra=%d&rt=%d",
      robot.drivePower(), robot.armPower(),
      robot.reverseLeft() ? 1 : 0, robot.reverseRight() ? 1 : 0,
      robot.reverseArm() ? 1 : 0, robot.rampRate());
    srv.send(200, "text/plain", buf);
  });

  controller.addEndpoint("/cfg_get", HTTP_GET, []() {
    WebServer& srv = controller.getServer();
    char buf[160];
    snprintf(buf, sizeof(buf),
      "dp=%d&ap=%d&rl=%d&rr=%d&ra=%d&rt=%d",
      robot.drivePower(), robot.armPower(),
      robot.reverseLeft() ? 1 : 0, robot.reverseRight() ? 1 : 0,
      robot.reverseArm() ? 1 : 0, robot.rampRate());
    srv.send(200, "text/plain", buf);
  });

  // Tell the user exactly where to point their controller/client.
  if (controller.wifiMode() == SS_WIFI_STA) {
    Serial.printf("[SkidSteer] Ready (joined network). Connect clients to %s\n",
                  controller.ipAddress().c_str());
  } else {
    Serial.printf("[SkidSteer] Ready (AP \"%s\"). Connect clients to %s\n",
                  AP_SSID, controller.ipAddress().c_str());
  }
}

void loop() {
  controller.update();

  const SkidSteerInput& in = controller.getInput();

  // --- Drive: tank mix from the drive stick ---
  robot.drive(in.driveY, in.driveX);

  // --- Lift arm: prefer the analog axis, fall back to L/R buttons ---
  if (in.armAxis != 0) {
    robot.setArm(in.armAxis);
  } else if (in.R) {
    robot.setArm(100);
  } else if (in.L) {
    robot.setArm(-100);
  } else {
    robot.setArm(0);
  }

  // --- Bucket servos: absolute positions sent by the client ---
  robot.setRaise(in.raisePos);
  robot.setTilt(in.tiltPos);

  // --- Lights: toggle on the rising edge of A ---
  if (in.A && !lastA) {
    robot.setLights(!robot.lightsOn());
  }
  lastA = in.A;

  // Advance motor PWM ramping and push to hardware.
  robot.update();

  delay(5);  // ~200 Hz control loop: low latency + fast ramp advance
}
