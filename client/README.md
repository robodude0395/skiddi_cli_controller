# Skidsteer Keyboard Client — Quickstart

Drive the ESP32 skidsteer from your keyboard over WiFi. Pure Python standard
library, no third-party packages required.

## 1. Flash and power the ESP32

Make sure the firmware from this repo is running (see the top-level
[`README.md`](../README.md)). The firmware runs in one of two WiFi modes,
depending on what you set for `STA_SSID` in `src/main.cpp`:

- **Access-point mode (default):** the ESP32 hosts its own network.

  | | |
  |---|---|
  | SSID | `SkidSteer` |
  | Password | `skidsteer123` |
  | Device IP | `192.168.4.1` |

- **Station mode:** the ESP32 joins your existing WiFi and your router assigns
  it an IP. Read that IP from the serial monitor at boot (it prints
  `Connect clients to <ip>`).

## 2. Get on the same network as the ESP32

- **AP mode:** connect your computer to the `SkidSteer` network. Your machine
  loses normal internet while on the robot's AP — that's expected.
- **Station mode:** just stay on the same WiFi you told the ESP32 to join.

## 3. (Optional) Set up a virtual environment

The client has no dependencies, but a venv keeps things tidy:

```sh
cd client
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt   # no-op today; here for future extras
```

Requires Python 3.6+ on Linux or macOS (uses a POSIX terminal for raw key
input).

## 4. Run it

```sh
python3 skidsteer_keyboard.py
```

The default host is `192.168.4.1` (AP mode). In **station mode**, pass the IP
the serial monitor printed:

```sh
python3 skidsteer_keyboard.py --host 192.168.1.42 --rate 30
```

## Controls

| Key | Action |
|---|---|
| `W` / `S` | drive forward / reverse |
| `A` / `D` | turn left / right (combine with W/S to arc) |
| `R` / `F` | lift arm up / down |
| `T` / `G` | bucket raise up / down |
| `Y` / `H` | bucket tilt +/- |
| `L` | toggle lights |
| `SPACE` | emergency stop (zero all motion) |
| `Q` / `ESC` | quit (sends a stop first) |

The bottom line shows live axis values, bucket positions, lights state, and
whether the last request reached the ESP32 (`OK` / `ERR`).

## How "holding" a key works

A terminal reports key *presses* (repeated by the OS while held) but never key
*releases*. The client treats a key as held while its repeats keep arriving and
auto-releases it after `--hold-timeout` seconds (default `0.12`).

For a responsive feel the client does two things automatically:

- **UDP fire-and-forget.** Control packets are sent over UDP like an RC
  transmitter, no reply is expected or waited for, so WiFi round-trip latency
  never stalls control. A background thread blasts the latest state ~50x/sec;
  a dropped packet is harmless because a fresh one follows ~20ms later. If the
  client stops entirely, the firmware fail-stops on its own after ~1s.
- **Faster key-repeat (Linux/X11 only).** While running it sets a short repeat
  delay via `xset` so held keys don't briefly look released, then restores your
  setting on exit. On Wayland/macOS (or without `xset`) this is a no-op.

Tuning flags:

| Flag | Default | Purpose |
|---|---|---|
| `--port` | `4210` | ESP32 UDP control port |
| `--rate` | `50` | UDP send rate (packets/sec) |
| `--poll` | `120` | keyboard poll rate (Hz) |
| `--hold-timeout` | `0.12` | key auto-release delay |
| `--no-autorepeat` | off | don't touch OS key-repeat (then raise `--hold-timeout` to ~`0.5`) |

If motion still stutters when holding a key, either keep the default
autorepeat behavior, or run `--no-autorepeat --hold-timeout 0.5`.

## Safety

- **Emergency stop:** `SPACE` zeroes drive and arm instantly.
- **Clean exit:** `Q`, `ESC`, and `Ctrl-C` send stop frames before quitting.
- **Firmware fail-stop:** if the client stops sending (crash, WiFi drop), the
  ESP32 halts the motors after ~1 second on its own.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Every frame shows `ERR` | Confirm you're on the same network as the ESP32 and `--host` matches its IP (AP mode: `192.168.4.1`; station mode: the IP from the serial monitor). |
| Keys do nothing | Run in a real terminal (not an IDE console); raw key input needs a TTY. |
| Robot keeps creeping | Increase `--hold-timeout`, or press `SPACE`. |
| `termios`/`tty` import error | You're on Windows; this client targets Linux/macOS terminals. |
