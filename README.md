# ESP32 Skidsteer Controller

A universal, REST-controlled firmware for an ESP32-driven skidsteer. The ESP32
runs as a WiFi access point and exposes an HTTP GET API in the **same format as
the [3ds-to-esp32](https://github.com/robodude0395/3ds-to-esp32) project**, so
any client that can make HTTP requests can drive the machine: a Nintendo 3DS, a
phone web page, a gamepad bridge, or a plain `curl`/Python script.

```
┌──────────────┐   WiFi (HTTP GET)   ┌──────────────┐
│  Any client  │ ──────────────────► │    ESP32      │
│  3DS / phone │   button + axes     │   (AP mode)   │
│  / script    │ ◄────────────────── │  web server   │
└──────────────┘      "OK"           └───────┬───────┘
                                             │
                                             ▼
                                   drive motors, lift arm,
                                   bucket servos, lights
```

## Architecture

The firmware is split so the transport and the hardware are independent — swap
the controller, keep the robot code.

```
lib/SkidSteerController/   WiFi AP + DNS + HTTP REST API -> SkidSteerInput
lib/SkidSteer/             pin mapping + PWM/servo/direction hardware control
src/main.cpp               maps SkidSteerInput onto the SkidSteer outputs
platformio.ini             esp32dev + ESP32Servo
```

## Pin mapping

Silkscreen labels (`Dxx`) equal the GPIO number on the usual ESP32 WROOM DevKits.

| Function | Silkscreen | GPIO |
|---|---|---|
| Left drive – Enable (PWM) | `D25` | 25 |
| Right drive – Enable (PWM) | `D26` | 26 |
| Lift arm – Enable (PWM) | `D27` | 27 |
| Left drive – Direction IN1 | `D32` | 32 |
| Left drive – Direction IN2 | `D33` | 33 |
| Right drive – Direction IN3 | `D18` | 18 |
| Right drive – Direction IN4 | `D19` | 19 |
| Lift arm – Direction IN5 | `D23` | 23 |
| Lift arm – Direction IN6 | `D5` | 5 |
| Bucket tilt – Servo | `D13` | 13 |
| Bucket raise/lower – Servo | `D14` | 14 |
| Lights – Digital on/off | `D4` | 4 |

The three enable pins use LEDC PWM (20 kHz) for speed control. Each motor is a
1-enable + 2-direction driver (L298N style).

## Quick start

Open in PlatformIO and upload:

```
pio run --target upload
```

### Uploading from VS Code

Install the **PlatformIO IDE** extension in VS Code, then open this folder.
PlatformIO reads `platformio.ini` automatically. Use the toolbar/status-bar
buttons (or the PlatformIO sidebar):

- **Build** (checkmark) — compile.
- **Upload** (right arrow) — flash the connected ESP32.
- **Serial Monitor** (plug icon) — opens at 115200 baud; watch here to see
  which WiFi mode the board chose and the IP to connect to.

### WiFi mode: join a network, or host an AP

Set this at the top of [`src/main.cpp`](src/main.cpp):

```cpp
#define STA_SSID ""            // your WiFi name, or "" for AP mode
#define STA_PASS ""            // your WiFi password ("" if open)
```

- **Leave `STA_SSID` empty (default):** the ESP32 hosts its own access point.
  - SSID `SkidSteer`, password `skidsteer123`, IP `192.168.4.1`.
- **Fill in `STA_SSID`/`STA_PASS`:** the ESP32 joins that network and gets an
  IP from your router. If the join fails, it automatically falls back to
  hosting its own AP so you're never locked out.

Either way, the serial monitor prints the exact IP to point your controller at,
for example `Connect clients to 192.168.1.42`. In AP mode it's always
`192.168.4.1`.

Serial monitor is at 115200 baud.

## REST API

All requests are HTTP GET on port 80. The device answers `OK` (or an echoed
config string). Point your client at `192.168.4.1`.

### Batched input (preferred — one request per frame)

```
GET /i?b=<hexmask>&x=<-100..100>&y=<-100..100>&a=<-100..100>
```

| Arg | Meaning | Range |
|---|---|---|
| `b` | button bitmask (hex) | see below |
| `x` | drive turn | -100 (left) .. 100 (right) |
| `y` | drive throttle | -100 (reverse) .. 100 (forward) |
| `a` | arm lift axis (optional) | -100 (down) .. 100 (up) |

Button bitmask bits (matches 3ds-to-esp32):

```
bit0=A  bit1=B  bit2=X  bit3=Y
bit4=L  bit5=R  bit6=Start bit7=Select
bit8=Up bit9=Down bit10=Left bit11=Right
```

### Individual endpoints

```
GET /b?i=<ID>&s=<0|1>     single button (ID: A B X Y L R ST SE U D LT RT)
GET /s?x=<X>&y=<Y>&a=<A>   single axis update
GET /status               human-readable current state (open in a browser)
```

### Config (tune live, same style as the 3DS repo's /cfg)

```
GET /cfg?dp=..&ap=..&rl=..&rr=..&ra=..&rt=..
GET /cfg_get              read current values
```

| Arg | Meaning | Range |
|---|---|---|
| `dp` | drive power cap (%) | 0..100 |
| `ap` | arm power cap (%) | 0..100 |
| `rl` | reverse left track | 0/1 |
| `rr` | reverse right track | 0/1 |
| `ra` | reverse arm | 0/1 |
| `rt` | PWM ramp rate (duty/tick) | 1..255 |

## Default control scheme

| Input | Action |
|---|---|
| Drive stick X/Y | tank drive (throttle + turn mix) |
| Arm axis `a` | lift arm motor |
| R / L buttons | arm up / down (if no `a` axis sent) |
| Up / Down | bucket raise servo |
| Left / Right | bucket tilt servo |
| A button | toggle lights (rising edge) |

## Safety

- **Fail-stop:** if no input arrives for 1000 ms, inputs are cleared and the
  motors ramp to a stop. Tune with `controller.setInputTimeout(ms)`.
- **Ramping:** motor PWM ramps toward its target (`rt`) to soften current
  spikes and reduce brown-outs.

## Example (curl)

```sh
# Full forward
curl "http://192.168.4.1/i?y=100"

# Spin right in place
curl "http://192.168.4.1/i?x=100"

# Raise arm + press A (lights): b=0x20 (R) | 0x01 (A) = 0x21
curl "http://192.168.4.1/i?b=21&a=100"

# Cap drive power to 60%
curl "http://192.168.4.1/cfg?dp=60"
```

## Keyboard client (`client/skidsteer_keyboard.py`)

A stdlib-only CLI that drives the skidsteer from your keyboard. Connect your
computer to the `SkidSteer` WiFi AP first, then:

```sh
python3 client/skidsteer_keyboard.py
python3 client/skidsteer_keyboard.py --host 192.168.4.1 --rate 30
```

Controls:

| Key | Action |
|---|---|
| W / S | drive forward / reverse |
| A / D | turn left / right (combine with W/S to arc) |
| R / F | lift arm up / down |
| T / G | bucket raise up / down |
| Y / H | bucket tilt +/- |
| L | toggle lights |
| SPACE | emergency stop |
| Q / ESC | quit (sends stop first) |

Terminals report key presses but not releases, so the client treats a key as
held while the OS key-repeat keeps firing and auto-releases it after
`--hold-timeout` seconds (default 0.12). Keep key-repeat enabled for smooth
driving. No third-party packages needed.

## Hardware notes

- Power servos from a separate 5V rail with a common ground — not the ESP32 3.3V.
- Use logic level shifting between the 3.3V ESP32 and 5V driver inputs if your
  driver needs it.
- Requires Arduino-ESP32 core 3.0+ (the pinned `ESP32Servo@^3.0.5`). The LEDC
  code also compiles on 2.x cores via a compatibility shim in `SkidSteer.cpp`.
