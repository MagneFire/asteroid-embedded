#!/usr/bin/env python3
"""The OPPO Watch's coprocessor from the PC, through AsteroidOS on the
watch's application processor: swdtool is pushed there and run as root.

  tools/beluga/run.py probe
  tools/beluga/run.py backup DIR       saves the backup to DIR on the PC
  tools/beluga/run.py flash [IMAGE]    build/apollo3/asteroid.bin by default
  tools/beluga/run.py restore FILE.flash
  tools/beluga/run.py log
  tools/beluga/run.py shell

Before a flash the services that talk to the MCU or drive the panel are
stopped, and the panel is handed over the way WearOS does it for its
power saver mode.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, 'build', 'swdtool')
IMAGE = os.path.join(ROOT, 'build', 'apollo3', 'asteroid.bin')
REMOTE = '/tmp/swdtool'
# What has to leave the MCU and the panel alone while the firmware runs.
QUIET = [
    'setprop ctl.stop deamonserver',
    'systemctl stop asteroid-seconddisplayd',
    "su ceres -c 'export XDG_RUNTIME_DIR=/run/user/1000; systemctl --user stop asteroid-launcher'",
    'echo 0 > /sys/devices/virtual/graphics/fb0/mdss_fb_disp_en_gpio',
]


def adb(serial, *cmd, check=True, **kw):
    full = ['adb'] + (['-s', serial] if serial else []) + list(cmd)
    return subprocess.run(full, check=check, **kw)


def shell(serial, line, check=True, tty=False):
    return adb(serial, 'shell', *(['-t'] if tty else []), line, check=check)


def push_tool(serial):
    if not os.path.exists(TOOL):
        sys.exit('no build/swdtool: make swdtool SWD_CC=<beluga sysroot gcc>')
    adb(serial, 'push', TOOL, REMOTE, stdout=subprocess.DEVNULL)
    shell(serial, f'chmod 755 {REMOTE}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('-s', '--serial', help='adb serial of the watch')
    p.add_argument('cmd', choices=['probe', 'backup', 'flash', 'restore', 'log', 'shell'])
    p.add_argument('arg', nargs='?')
    a = p.parse_args()
    push_tool(a.serial)
    if a.cmd == 'probe':
        shell(a.serial, f'{REMOTE} probe')
    elif a.cmd == 'backup':
        if not a.arg:
            sys.exit('backup needs a directory on the PC')
        os.makedirs(a.arg, exist_ok=True)
        shell(a.serial, f'{REMOTE} backup /tmp/mcu-backup')
        for ext in ('.flash', '.info0'):
            adb(a.serial, 'pull', '/tmp/mcu-backup' + ext, os.path.join(a.arg, 'mcu-backup' + ext))
    elif a.cmd in ('flash', 'restore'):
        src = a.arg or (IMAGE if a.cmd == 'flash' else None)
        if not src or not os.path.exists(src):
            sys.exit(f'no image: {src}')
        adb(a.serial, 'push', src, '/tmp/mcu-image', stdout=subprocess.DEVNULL)
        for line in QUIET:
            shell(a.serial, line, check=False)
        shell(a.serial, f'{REMOTE} {a.cmd} /tmp/mcu-image')
    elif a.cmd == 'log':
        shell(a.serial, f'{REMOTE} log')
    else:
        shell(a.serial, f'{REMOTE} shell', tty=True)


if __name__ == '__main__':
    main()
