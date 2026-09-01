#!/usr/bin/env python3
"""
taranis-sitl-rc.py  --  EdgeTX Taranis (or any USB game controller) -> Betaflight SITL RC bridge.

Reads the radio as a USB HID joystick (EdgeTX: Model / SYS -> "USB Joystick (HID)")
and streams RC channels straight into the SITL native RC UDP server on 127.0.0.1:9004.

Wire format expected by SITL (src/platform/SIMULATOR/target/SITL/target.h, struct rc_packet):
    double   timestamp;         # seconds, 8 bytes, little-endian
    uint16_t channels[16];      # microseconds (1000..2000), 32 bytes
  => 40-byte packet, layout "<d16H".

Channel order SITL expects (from its own debug print "AETR ... AUX1-4"):
    ch0 = Roll (Aileron)   ch1 = Pitch (Elevator)
    ch2 = Throttle         ch3 = Yaw (Rudder)
    ch4 = AUX1  ch5 = AUX2  ch6 = AUX3  ch7 = AUX4 ...

Usage:
    pip3 install pygame
    python3 taranis-sitl-rc.py --list          # enumerate controllers
    python3 taranis-sitl-rc.py --monitor        # print live axes/buttons (calibration)
    python3 taranis-sitl-rc.py                   # run the bridge (Ctrl-C to stop)

Everything you normally need to tweak lives in the CONFIG block below.
"""

import argparse
import socket
import struct
import sys
import time

# ----------------------------------------------------------------------------
# CONFIG  --  edit these to match how YOUR Taranis presents itself.
# Run with --monitor first to see which axis index moves for each stick.
# ----------------------------------------------------------------------------

JOYSTICK_INDEX = 0          # which controller (see --list); usually 0

# Map a logical RC channel -> a joystick AXIS index (from --monitor).
# EdgeTX default USB-joystick order is often A,E,T,R = 0,1,2,3 but VERIFY.
AXIS_MAP = {
    "roll":     1,          # ch0  (right stick left/right, mode 2)
    "pitch":    2,          # ch1  (right stick up/down)
    "throttle": 0,          # ch2  (left stick up/down)
    "yaw":      3,          # ch3  (left stick left/right)
}

# Invert an axis if it moves the wrong way (True/False).
AXIS_INVERT = {
    "roll":     True,       # flipped: all attitude commands were reversed vs the Gazebo iris
    "pitch":    False,      # flipped
    "throttle": False,
    "yaw":      True,       # flipped
}

# AUX channels driven by joystick BUTTONS (momentary) or AXES (switches).
# EdgeTX exposes 2-pos switches as buttons and 3-pos/pots as axes.
# Format: channel_name -> ("button", index)  or  ("axis", index)
#   Suggested: AUX1 = ARM switch, AUX2 = POS HOLD switch.
AUX_MAP = {
    "aux1": ("axis", 4),    # ARM      -> EdgeTX CH5 switch
    "aux2": ("axis", 5),    # ANGLE    -> EdgeTX CH6 switch
    "aux3": ("axis", 6),    # POS HOLD -> EdgeTX CH7 switch
    "aux4": None,
}

# Endpoints. Betaflight arms/activates modes on channel thresholds.
RC_MIN, RC_MID, RC_MAX = 1000, 1500, 2000

# Failsafe defaults for unmapped / at-startup channels.
DEFAULT_THROTTLE = RC_MIN   # throttle low so it can't arm hot
DEFAULT_AUX      = RC_MIN    # switches low (disarmed / mode off)

DEST_HOST = "127.0.0.1"      # SITL host: 127.0.0.1 if local, or the VM's IP (e.g. UTM shared-net)
DEST_PORT = 9004
RATE_HZ = 50                 # SITL is happy anywhere 20-100 Hz
NUM_CHANNELS = 16
# ----------------------------------------------------------------------------


def clamp(v, lo, hi):
    return lo if v < hi and v < lo else (hi if v > hi else v)


def axis_to_us(value, invert):
    # pygame axis is -1.0 .. +1.0
    if invert:
        value = -value
    v = value  # -1..1
    us = int(RC_MID + v * (RC_MAX - RC_MIN) / 2.0)
    return max(RC_MIN, min(RC_MAX, us))


def build_packet(channels):
    ts = time.time()
    ch = list(channels) + [RC_MIN] * (NUM_CHANNELS - len(channels))
    return struct.pack("<d" + "H" * NUM_CHANNELS, ts, *ch[:NUM_CHANNELS])


def init_pygame():
    try:
        import pygame  # noqa
    except ImportError:
        sys.exit("pygame not installed.  Run:  pip3 install pygame")
    import pygame
    pygame.init()
    pygame.joystick.init()
    return pygame


def open_joystick(pygame, index):
    count = pygame.joystick.get_count()
    if count == 0:
        sys.exit("No controllers found. Put the Taranis in EdgeTX 'USB Joystick (HID)' mode "
                 "and reconnect USB (not DFU/flash mode).")
    if index >= count:
        sys.exit(f"Controller index {index} not present ({count} found). Use --list.")
    js = pygame.joystick.Joystick(index)
    js.init()
    return js


def cmd_list(pygame):
    n = pygame.joystick.get_count()
    print(f"{n} controller(s):")
    for i in range(n):
        j = pygame.joystick.Joystick(i)
        j.init()
        print(f"  [{i}] {j.get_name()}  axes={j.get_numaxes()} buttons={j.get_numbuttons()} hats={j.get_numhats()}")


def cmd_monitor(pygame, js):
    print(f"Monitoring '{js.get_name()}'.  Move each stick/switch; Ctrl-C to stop.\n")
    try:
        while True:
            pygame.event.pump()
            axes = [round(js.get_axis(i), 3) for i in range(js.get_numaxes())]
            btns = [js.get_button(i) for i in range(js.get_numbuttons())]
            sys.stdout.write("\r axes=" + str(axes) + "  buttons=" + str(btns) + "        ")
            sys.stdout.flush()
            time.sleep(0.05)
    except KeyboardInterrupt:
        print("\nstopped.")


def read_aux(js, spec):
    if spec is None:
        return DEFAULT_AUX
    kind, idx = spec
    if kind == "button":
        return RC_MAX if js.get_button(idx) else RC_MIN
    if kind == "axis":                    # 3-pos switch / pot mapped to an axis
        return axis_to_us(js.get_axis(idx), False)
    return DEFAULT_AUX


def cmd_run(pygame, js, dest):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    period = 1.0 / RATE_HZ
    print(f"Bridging '{js.get_name()}' -> SITL {dest[0]}:{dest[1]} @ {RATE_HZ} Hz.  Ctrl-C to stop.")
    print("SAFETY: throttle stays low until you move it; keep ARM switch OFF until GPS lock.\n")
    try:
        while True:
            pygame.event.pump()

            def ax(name):
                return axis_to_us(js.get_axis(AXIS_MAP[name]), AXIS_INVERT[name])

            ch = [RC_MIN] * NUM_CHANNELS
            ch[0] = ax("roll")
            ch[1] = ax("pitch")
            ch[2] = ax("throttle")
            ch[3] = ax("yaw")
            ch[4] = read_aux(js, AUX_MAP["aux1"])
            ch[5] = read_aux(js, AUX_MAP["aux2"])
            ch[6] = read_aux(js, AUX_MAP["aux3"])
            ch[7] = read_aux(js, AUX_MAP["aux4"])

            sock.sendto(build_packet(ch), dest)
            sys.stdout.write("\r R:%d P:%d T:%d Y:%d A1:%d A2:%d   "
                             % (ch[0], ch[1], ch[2], ch[3], ch[4], ch[5]))
            sys.stdout.flush()
            time.sleep(period)
    except KeyboardInterrupt:
        print("\nstopped.")


def main():
    ap = argparse.ArgumentParser(description="Taranis -> Betaflight SITL RC bridge (UDP 9004).")
    ap.add_argument("--list", action="store_true", help="list controllers and exit")
    ap.add_argument("--monitor", action="store_true", help="print live axes/buttons for calibration")
    ap.add_argument("--index", type=int, default=JOYSTICK_INDEX, help="controller index")
    ap.add_argument("--host", default=DEST_HOST, help="SITL host IP (VM IP if SITL runs in a VM)")
    ap.add_argument("--port", type=int, default=DEST_PORT, help="SITL RC UDP port (default 9004)")
    args = ap.parse_args()

    pygame = init_pygame()

    if args.list:
        cmd_list(pygame)
        return

    js = open_joystick(pygame, args.index)

    if args.monitor:
        cmd_monitor(pygame, js)
    else:
        cmd_run(pygame, js, (args.host, args.port))


if __name__ == "__main__":
    main()
