#include "SkidSteer.h"

// ---- LEDC compatibility shim ----------------------------------
// Arduino-ESP32 core 3.x uses ledcAttach(pin, freq, res) and
// ledcWrite(pin, duty). Older 2.x cores use channel-based
// ledcSetup/ledcAttachPin/ledcWrite(channel, duty). We attach and
// write "by pin" everywhere and let this shim bridge to 2.x.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  static inline void ledcAttachPinCompat(uint8_t pin, uint32_t freq, uint8_t res) {
    ledcAttach(pin, freq, res);
  }
  static inline void ledcWritePinCompat(uint8_t pin, uint32_t duty) {
    ledcWrite(pin, duty);
  }
#else
  // Assign each EN pin its own channel on 2.x cores. Channels 13/14/15
  // are used to stay clear of the low channels ESP32Servo tends to grab.
  static inline uint8_t _chanFor(uint8_t pin) {
    if (pin == PIN_LEFT_EN)  return 13;
    if (pin == PIN_RIGHT_EN) return 14;
    return 15; // PIN_ARM_EN
  }
  static inline void ledcAttachPinCompat(uint8_t pin, uint32_t freq, uint8_t res) {
    uint8_t ch = _chanFor(pin);
    ledcSetup(ch, freq, res);
    ledcAttachPin(pin, ch);
  }
  static inline void ledcWritePinCompat(uint8_t pin, uint32_t duty) {
    ledcWrite(_chanFor(pin), duty);
  }
#endif

void SkidSteer::begin() {
  // Direction pins as outputs
  pinMode(PIN_LEFT_IN1, OUTPUT);
  pinMode(PIN_LEFT_IN2, OUTPUT);
  pinMode(PIN_RIGHT_IN3, OUTPUT);
  pinMode(PIN_RIGHT_IN4, OUTPUT);
  pinMode(PIN_ARM_IN5, OUTPUT);
  pinMode(PIN_ARM_IN6, OUTPUT);
  pinMode(PIN_LIGHTS, OUTPUT);

  digitalWrite(PIN_LIGHTS, LOW);

  // LEDC PWM on the three enable pins (attached by pin; see shim above).
  ledcAttachPinCompat(PIN_LEFT_EN,  LEDC_FREQ_HZ, LEDC_RESOLUTION);
  ledcAttachPinCompat(PIN_RIGHT_EN, LEDC_FREQ_HZ, LEDC_RESOLUTION);
  ledcAttachPinCompat(PIN_ARM_EN,   LEDC_FREQ_HZ, LEDC_RESOLUTION);

  ledcWritePinCompat(PIN_LEFT_EN, 0);
  ledcWritePinCompat(PIN_RIGHT_EN, 0);
  ledcWritePinCompat(PIN_ARM_EN, 0);

  // Servos on the ESP32Servo library (also LEDC-backed internally).
  _tilt.attach(PIN_SERVO_TILT, SERVO_MIN, SERVO_MAX);
  _raise.attach(PIN_SERVO_RAISE, SERVO_MIN, SERVO_MAX);
  _tilt.writeMicroseconds(SERVO_NEUTRAL);
  _raise.writeMicroseconds(SERVO_NEUTRAL);
}

// Map signed percent (-100..100) to signed duty (-255..255) with a
// power-scale applied.
static int percentToDuty(int percent, int scalePercent) {
  percent = constrain(percent, -100, 100);
  long scaled = (long)percent * scalePercent / 100;   // apply power cap
  return (int)map(scaled, -100, 100, -255, 255);
}

void SkidSteer::setLeftTrack(int power) {
  _leftTarget = percentToDuty(power, _drivePercent);
}

void SkidSteer::setRightTrack(int power) {
  _rightTarget = percentToDuty(power, _drivePercent);
}

void SkidSteer::drive(int throttle, int turn) {
  throttle = constrain(throttle, -100, 100);
  turn     = constrain(turn, -100, 100);

  // Standard skidsteer mix, clamped to +/-100.
  int left  = constrain(throttle + turn, -100, 100);
  int right = constrain(throttle - turn, -100, 100);

  setLeftTrack(left);
  setRightTrack(right);
}

void SkidSteer::setArm(int power) {
  _armTarget = percentToDuty(power, _armPercent);
}

void SkidSteer::setTilt(int position) {
  position = constrain(position, -100, 100);
  _tilt.writeMicroseconds(map(position, -100, 100, SERVO_MIN, SERVO_MAX));
}

void SkidSteer::setRaise(int position) {
  position = constrain(position, -100, 100);
  _raise.writeMicroseconds(map(position, -100, 100, SERVO_MIN, SERVO_MAX));
}

void SkidSteer::setLights(bool on) {
  _lightsOn = on;
  digitalWrite(PIN_LIGHTS, on ? HIGH : LOW);
}

void SkidSteer::stopMotors() {
  _leftTarget = _rightTarget = _armTarget = 0;
  _leftCurrent = _rightCurrent = _armCurrent = 0;
  ledcWritePinCompat(PIN_LEFT_EN, 0);
  ledcWritePinCompat(PIN_RIGHT_EN, 0);
  ledcWritePinCompat(PIN_ARM_EN, 0);
}

int SkidSteer::_ramp(int current, int target) {
  int diff = target - current;
  if (abs(diff) <= _rampRate) return target;
  return current + (diff > 0 ? _rampRate : -_rampRate);
}

// Sets direction pins from the sign of signedDuty and writes the
// magnitude to the enable PWM channel.
void SkidSteer::_applyMotor(int enPin, int inA, int inB,
                            int signedDuty, bool reverse) {
  if (reverse) signedDuty = -signedDuty;

  int duty = abs(signedDuty);
  if (duty > 255) duty = 255;

  if (signedDuty > 0) {
    digitalWrite(inA, HIGH);
    digitalWrite(inB, LOW);
  } else if (signedDuty < 0) {
    digitalWrite(inA, LOW);
    digitalWrite(inB, HIGH);
  } else {
    // Coast/brake: both low = coast on most H-bridges.
    digitalWrite(inA, LOW);
    digitalWrite(inB, LOW);
  }

  ledcWritePinCompat(enPin, duty);
}

void SkidSteer::update() {
  _leftCurrent  = _ramp(_leftCurrent,  _leftTarget);
  _rightCurrent = _ramp(_rightCurrent, _rightTarget);
  _armCurrent   = _ramp(_armCurrent,   _armTarget);

  _applyMotor(PIN_LEFT_EN,  PIN_LEFT_IN1,  PIN_LEFT_IN2,
              _leftCurrent,  _reverseLeft);
  _applyMotor(PIN_RIGHT_EN, PIN_RIGHT_IN3, PIN_RIGHT_IN4,
              _rightCurrent, _reverseRight);
  _applyMotor(PIN_ARM_EN,   PIN_ARM_IN5,   PIN_ARM_IN6,
              _armCurrent,   _reverseArm);
}
