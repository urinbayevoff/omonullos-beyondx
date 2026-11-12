#!/usr/bin/env python3
"""zril: send commands to rild-lite (/dev/socket/zril) and print what comes back.

  zril.py [-t SECONDS] COMMAND...     e.g. zril.py "raw0 1" "rawi 23 1"
  zril.py -w SECONDS                  only listen (unsolicited messages)

Commands: raw0 REQ | rawi REQ INT... | raws REQ STR... | rawz REQ STR | dial NUMBER.
Requests: 1 SIM status, 9 calls, 12 hangup IDX, 19 signal, 20 voice reg,
22 operator, 23 radio power 0/1, 25 send SMS (SMSC PDU), 38 IMEI, 40 answer,
29 USSD (rawz 29 "*102#", answer = unsol 1006), 51 baseband version. Added for OmonullOS, October 2026.
"""
import socket, sys, time

args, wait = sys.argv[1:], 3.0
if args[:1] in (["-t"], ["-w"]):
    wait, args = float(args[1]), args[2:]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/dev/socket/zril")
for cmd in args:
    s.sendall(cmd.encode() + b"\n")
    time.sleep(0.3)
s.settimeout(0.5)
end = time.time() + wait
while time.time() < end:
    try:
        data = s.recv(65536)
    except socket.timeout:
        continue
    if not data:
        break
    sys.stdout.write(data.decode(errors="replace"))
    sys.stdout.flush()
