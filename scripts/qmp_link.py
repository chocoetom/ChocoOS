#!/usr/bin/env python3
# One-shot QMP: set_link n0 up|down. Usage: qmp_link.py [up|down]
import json
import socket
import sys

want_up = not (len(sys.argv) > 1 and sys.argv[1] == "down")


def transact(s, obj):
    s.sendall((json.dumps(obj) + "\n").encode())
    buf = b""
    while True:
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                return {"error": "eof"}
            buf += chunk
        line, _, buf = buf.partition(b"\n")
        try:
            msg = json.loads(line.decode())
        except Exception:
            continue
        if "return" in msg or "error" in msg:
            return msg


s = socket.create_connection(("127.0.0.1", 4444), timeout=15)
g = b""
while b"\n" not in g:
    g += s.recv(4096)
print(transact(s, {"execute": "qmp_capabilities"}))
print(transact(s, {"execute": "set_link", "arguments": {"name": "n0", "up": want_up}}))
s.close()
