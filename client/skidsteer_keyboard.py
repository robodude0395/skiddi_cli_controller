#!/usr/bin/env python3
"""
skidsteer_keyboard.py -- keyboard control for the ESP32 skidsteer.

Talks to the firmware's REST API (the batched /i endpoint) over WiFi.
Connect your computer to the ESP32 (AP mode: SSID "SkidSteer",
password "skidsteer123", device at 192.168.4.1), then run:

    python3 skidsteer_keyboard.py
    python3 skidsteer_keyboard.py --host 192.168.4.1 --rate 50

No third-party libraries required (stdlib only). Linux/macOS terminal.

--------------------------------------------------------------------
Controls  (hold to move)
--------------------------------------------------------------------
  W / S ............ drive forward / reverse
  A / D ............ turn left / right     (combine, e.g. W+D, to arc)
  R / F ............ lift arm up / down
  T / G ............ bucket raise up / down (held position)
  Y / H ............ bucket tilt +/-        (held position)
  L ................ toggle lights
  SPACE ............ emergency stop
  Q or ESC ......... quit (sends stop first)

Responsiveness notes
--------------------
A terminal reports key *presses* (repeated by the OS while held) but
never *releases*. This client treats a key as held while repeats keep
arriving and auto-releases it after --hold-timeout seconds.

Two things make it feel snappy:
  1. Control packets are sent over UDP, fire-and-forget (like an RC
     transmitter). Nothing waits for a reply, so WiFi round-trip
     latency never stalls control. A background thread blasts the
     latest state ~50x/sec; dropped packets self-heal on the next one.
  2. On Linux/X11 the client speeds up the OS key-repeat while running
     (and restores it on exit) so held keys don't stutter during the
     initial repeat-delay. Disable with --no-autorepeat.
--------------------------------------------------------------------
"""

import argparse
import os
import socket
import subprocess
import sys
import threading
import time
import select
import termios
import tty

# ---- Button bit layout (must match the firmware) ----
BTN = {
    "A": 1 << 0, "B": 1 << 1, "X": 1 << 2, "Y": 1 << 3,
    "L": 1 << 4, "R": 1 << 5, "Start": 1 << 6, "Select": 1 << 7,
    "Up": 1 << 8, "Down": 1 << 9, "Left": 1 << 10, "Right": 1 << 11,
}

AXIS_STEP = 100     # full deflection for a held drive/arm key
# Bucket servo nudge per input poll while a key is held. Larger = the
# bucket sweeps its full -100..100 travel faster (more range per press).
TILT_STEP = 10      # bucket tilt  (Y/H) -- larger so tilt covers its range quickly
RAISE_STEP = 4      # bucket raise (T/G)


class KeyReader:
    """Raw, non-blocking single-key reader for a POSIX terminal."""

    def __init__(self):
        self.fd = sys.stdin.fileno()
        self._old = None

    def __enter__(self):
        self._old = termios.tcgetattr(self.fd)
        tty.setcbreak(self.fd)
        return self

    def __exit__(self, *exc):
        if self._old is not None:
            termios.tcsetattr(self.fd, termios.TCSADRAIN, self._old)

    def get_keys(self, wait=0.0):
        """Return buffered chars, blocking up to `wait` seconds for the first."""
        keys = []
        r, _, _ = select.select([sys.stdin], [], [], wait)
        while r:
            ch = sys.stdin.read(1)
            if not ch:
                break
            keys.append(ch)
            r, _, _ = select.select([sys.stdin], [], [], 0)
        return keys


class Sender(threading.Thread):
    """Background thread: fires the latest control state at the ESP32 over
    UDP. Fire-and-forget -- no reply is expected or waited for, exactly
    like an RC transmitter. Dropped packets don't matter because a fresh
    one follows ~20ms later, and the firmware fail-stops if they stop.
    """

    def __init__(self, host, port, rate):
        super().__init__(daemon=True)
        self.addr = (host, port)
        self.period = 1.0 / max(1.0, rate)

        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setblocking(False)   # never block the loop

        self._lock = threading.Lock()
        self._state = (0, 0, 0, 0, 0, 0)   # mask, x, y, a, tilt, raise
        self._running = True

    def set_state(self, mask, x, y, a, tilt, raise_):
        with self._lock:
            self._state = (mask, int(x), int(y), int(a),
                           int(tilt), int(raise_))

    def stop(self):
        self._running = False

    def _send_once(self, mask, x, y, a, tilt=0, raise_=0):
        # Plain text packet mirroring the /i args; the firmware parses the
        # same key=value tokens over UDP.
        pkt = ("b=%x x=%d y=%d a=%d t=%d p=%d"
               % (mask, x, y, a, tilt, raise_)).encode("ascii")
        try:
            self._sock.sendto(pkt, self.addr)
        except Exception:
            pass   # fire and forget: never let a send error stall control

    def run(self):
        next_t = time.time()
        while self._running:
            with self._lock:
                mask, x, y, a, tilt, raise_ = self._state
            self._send_once(mask, x, y, a, tilt, raise_)
            next_t += self.period
            sleep = next_t - time.time()
            if sleep > 0:
                time.sleep(sleep)
            else:
                next_t = time.time()  # fell behind; resync


def clamp(v, lo=-100, hi=100):
    return max(lo, min(hi, v))


class RepeatTuner:
    """Speed up X11 key-repeat while running, restore on exit.

    Uses `xset`. No-op if xset is missing or not on X11 (e.g. Wayland,
    macOS). This shrinks the initial repeat delay so held keys don't
    briefly look released.
    """

    def __init__(self, enabled):
        self.enabled = enabled and self._have_xset() and self._on_x11()
        self._restored = False

    @staticmethod
    def _have_xset():
        from shutil import which
        return which("xset") is not None

    @staticmethod
    def _on_x11():
        return bool(os.environ.get("DISPLAY")) and \
            os.environ.get("XDG_SESSION_TYPE", "").lower() != "wayland"

    def apply(self):
        if not self.enabled:
            return
        try:
            # delay 200ms before repeat, then 40 repeats/sec.
            subprocess.run(["xset", "r", "rate", "200", "40"],
                           check=False,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        except Exception:
            self.enabled = False

    def restore(self):
        if not self.enabled or self._restored:
            return
        self._restored = True
        try:
            subprocess.run(["xset", "r", "rate"], check=False,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(
        description="Keyboard control for the ESP32 skidsteer.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--host", default="192.168.4.1",
                    help="ESP32 IP/host (default: 192.168.4.1)")
    ap.add_argument("--port", type=int, default=4210,
                    help="ESP32 UDP control port (default: 4210)")
    ap.add_argument("--rate", type=float, default=50.0,
                    help="UDP send rate, packets/sec (default: 50)")
    ap.add_argument("--poll", type=float, default=120.0,
                    help="keyboard poll rate, Hz (default: 120)")
    ap.add_argument("--hold-timeout", type=float, default=0.12,
                    help="seconds without a repeat before a key releases. "
                         "With --autorepeat (default) the OS repeats every "
                         "~25ms, so 0.12 bridges the gap while keeping release "
                         "latency low. (default: 0.12)")
    ap.add_argument("--no-autorepeat", action="store_true",
                    help="don't touch the OS key-repeat rate. If you use this, "
                         "raise --hold-timeout to ~0.5 to avoid stutter.")
    args = ap.parse_args()

    poll_period = 1.0 / max(1.0, args.poll)

    tuner = RepeatTuner(enabled=not args.no_autorepeat)
    tuner.apply()

    sender = Sender(args.host, args.port, args.rate)
    sender.start()

    tilt = 0
    raise_ = 0
    held = {}   # char -> last-seen time

    def is_held(c, now):
        t = held.get(c)
        return t is not None and (now - t) <= args.hold_timeout

    print("Skidsteer keyboard control -> %s:%d (UDP, fire-and-forget)"
          % (args.host, args.port))
    print("W/S drive  A/D turn  R/F arm  T/G raise  Y/H tilt  "
          "L lights  SPACE stop  Q quit")
    if tuner.enabled:
        print("(sped up key-repeat via xset; will restore on exit)")

    try:
        with KeyReader() as kr:
            while True:
                now = time.time()
                keys = kr.get_keys(wait=poll_period)
                quit_now = False
                lights_pulse = False

                for ch in keys:
                    if ch in ("\x1b", "q", "Q"):
                        quit_now = True
                        continue
                    c = ch.lower()
                    if c == " ":
                        held.clear()
                        continue
                    if c == "l":
                        if not is_held("l", now):
                            lights_pulse = True
                        held["l"] = now
                        continue
                    if c == "t":
                        raise_ = clamp(raise_ + RAISE_STEP)
                    elif c == "g":
                        raise_ = clamp(raise_ - RAISE_STEP)
                    elif c == "y":
                        tilt = clamp(tilt + TILT_STEP)
                    elif c == "h":
                        tilt = clamp(tilt - TILT_STEP)
                    held[c] = now

                if quit_now:
                    break

                drive_y = (AXIS_STEP if is_held("w", now) else 0) - (AXIS_STEP if is_held("s", now) else 0)
                drive_x = (AXIS_STEP if is_held("d", now) else 0) - (AXIS_STEP if is_held("a", now) else 0)
                arm     = (AXIS_STEP if is_held("r", now) else 0) - (AXIS_STEP if is_held("f", now) else 0)

                mask = BTN["A"] if lights_pulse else 0
                # Bucket tilt/raise are absolute held positions, sent every
                # frame as t= and p= so the firmware drives the servos.
                sender.set_state(mask, drive_x, drive_y, arm, tilt, raise_)

                sys.stdout.write(
                    "\rx=%4d y=%4d arm=%4d | raise=%4d tilt=%4d | L=%s "
                    % (drive_x, drive_y, arm, raise_, tilt,
                       "^" if lights_pulse else " ")
                )
                sys.stdout.flush()
    finally:
        # Stop the robot, stop the sender, restore key-repeat.
        # Motors to zero, but hold the last bucket position.
        sender.set_state(0, 0, 0, 0, tilt, raise_)
        for _ in range(3):
            sender._send_once(0, 0, 0, 0, tilt, raise_)
            time.sleep(0.03)
        sender.stop()
        tuner.restore()
        print("\nStopped.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(0)
