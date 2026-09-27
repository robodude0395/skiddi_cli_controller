#ifndef SKIDSTEER_H
#define SKIDSTEER_H

#include <Arduino.h>
#include <ESP32Servo.h>

// =============================================================
// SkidSteer
// -------------------------------------------------------------
// Owns all the physical outputs of the skidsteer and exposes a
// clean high-level API. Nothing here knows about WiFi or HTTP --
// it just takes signed commands (-100..100) and drives hardware.
//
// Outputs (12 total), matching the agreed silkscreen mapping:
//
//   Motor drivers (e.g. L298N style: 1 enable + 2 direction each)
//     Left drive   EN=D25   IN1=D32  IN2=D33
//     Right drive  EN=D26   IN3=D18  IN4=D19
//     Lift arm     EN=D27   IN5=D23  IN6=D5
//
//   Servos
//     Bucket tilt        D13
//     Bucket raise/lower D14
//
//   Lights (digital on/off)  D4
//
// The three EN pins are driven with LEDC PWM for speed control.
// =============================================================

// ---- Pin mapping (silkscreen Dxx == GPIO xx on WROOM DevKits) ----
#define PIN_LEFT_EN    25
#define PIN_LEFT_IN1   32
#define PIN_LEFT_IN2   33

#define PIN_RIGHT_EN   26
#define PIN_RIGHT_IN3  18
#define PIN_RIGHT_IN4  19

#define PIN_ARM_EN     27
#define PIN_ARM_IN5    23
#define PIN_ARM_IN6     5

#define PIN_SERVO_TILT 13
#define PIN_SERVO_RAISE 14

#define PIN_LIGHTS      4

// ---- LEDC (PWM) config for the enable pins ----
// We drive the three EN pins directly by pin (Arduino-ESP32 core 3.x
// LEDC API auto-allocates a channel per pin, so there is no risk of
// colliding with the channels ESP32Servo grabs for the two servos).
#define LEDC_FREQ_HZ    20000   // 20 kHz: above audible range for motors
#define LEDC_RESOLUTION 8       // 8-bit duty (0-255)

// ---- Servo pulse widths (microseconds) ----
#define SERVO_MIN      1000
#define SERVO_MAX      2000
#define SERVO_NEUTRAL  1500

class SkidSteer {
public:
  void begin();

  // Differential drive from throttle/turn (-100..100 each).
  // Positive throttle = forward, positive turn = right.
  void drive(int throttle, int turn);

  // Individual track control (-100..100), if you'd rather mix
  // the tracks yourself.
  void setLeftTrack(int power);
  void setRightTrack(int power);

  // Lift arm motor (-100..100). Positive = raise, negative = lower.
  void setArm(int power);

  // Bucket servos, commanded as signed position (-100..100).
  // -100 = SERVO_MIN, 0 = neutral, 100 = SERVO_MAX.
  void setTilt(int position);
  void setRaise(int position);

  // Lights on/off.
  void setLights(bool on);
  bool lightsOn() const { return _lightsOn; }

  // Stop all motors immediately (servos hold position).
  void stopMotors();

  // --- Tunables (settable from the /cfg endpoint) ---
  void setDrivePower(int percent) { _drivePercent = constrain(percent, 0, 100); }
  void setArmPower(int percent)   { _armPercent   = constrain(percent, 0, 100); }
  void setReverseLeft(bool r)     { _reverseLeft  = r; }
  void setReverseRight(bool r)    { _reverseRight = r; }
  void setReverseArm(bool r)      { _reverseArm   = r; }
  void setRampRate(int r)         { _rampRate     = constrain(r, 1, 255); }

  int  drivePower() const { return _drivePercent; }
  int  armPower()   const { return _armPercent; }
  bool reverseLeft()  const { return _reverseLeft; }
  bool reverseRight() const { return _reverseRight; }
  bool reverseArm()   const { return _reverseArm; }
  int  rampRate()   const { return _rampRate; }

  // Call every loop to advance PWM ramping toward targets.
  void update();

private:
  Servo _tilt;
  Servo _raise;

  bool _lightsOn = false;

  // Tunables
  int  _drivePercent = 100;  // scales drive() output
  int  _armPercent   = 100;  // scales setArm() output
  bool _reverseLeft  = false;
  bool _reverseRight = false;
  bool _reverseArm   = false;
  int  _rampRate     = 40;   // max duty change per update() tick.
                             // 40/tick at a ~5ms loop reaches full power
                             // (255) in ~32ms -- snappy but still ramped
                             // enough to soften current spikes. Raise via
                             // /cfg?rt= for instant, lower for gentler.

  // Current (ramped) and target duty, signed -255..255
  int _leftTarget = 0,  _leftCurrent = 0;
  int _rightTarget = 0, _rightCurrent = 0;
  int _armTarget = 0,   _armCurrent = 0;

  void _applyMotor(int enPin, int inA, int inB,
                   int signedDuty, bool reverse);
  int  _ramp(int current, int target);
};

#endif
