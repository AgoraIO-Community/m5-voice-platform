#!/usr/bin/env python3
"""Run timed A-equivalent console commands and retain only diagnostic log lines."""
import argparse
import re
import shutil
import subprocess
import time
from pathlib import Path
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', default='/dev/ttyACM0')
parser.add_argument('--duration', type=float, default=55)
parser.add_argument('--at', action='append', default=[], metavar='SECONDS:COMMAND')
parser.add_argument('--output', type=Path, default=Path('/tmp/m5stack-serial-sanitized.log'))
args = parser.parse_args()
schedule = []
for command in args.at:
    when, key = command.split(':', 1)
    if key not in ('a', 'b', 'q', 's', 'm', 'p', 'h', 'd'):
        parser.error('commands must be a, b, q, s, m, p, h or d')
    schedule.append((float(when), key.encode()))
schedule.sort()
if shutil.which('fuser'):
    busy = subprocess.run(['fuser', args.port], capture_output=True, check=False)
    if busy.stdout.strip():
        parser.error('serial port is in use; finish flashing or close the other monitor first')
port = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2, exclusive=True)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
keep = ('m5stack:', 'voice_audio:', 'voice_transport:', 'voice_ui:', 'voice_heap:',
        'Guru Meditation',
        'assert failed', 'Backtrace:', 'abort()', 'ESP-ROM:', 'rst:', 'task_wdt:')
start, buffer = time.monotonic(), b''
with port, args.output.open('w') as log:
    def emit(text):
        print(text, flush=True)
        log.write(text + '\n')
        log.flush()
    while time.monotonic() - start < args.duration:
        elapsed = time.monotonic() - start
        while schedule and elapsed >= schedule[0][0]:
            _, key = schedule.pop(0)
            port.write(key)
            emit('TEST +%.1fs command=%s' % (elapsed, key.decode()))
        buffer += port.read(8192)
        while b'\n' in buffer:
            line, buffer = buffer.split(b'\n', 1)
            text = re.sub(r'\x1b\[[0-9;]*m', '', line.decode(errors='replace')).strip()
            if any(marker in text for marker in keep):
                emit(text)
