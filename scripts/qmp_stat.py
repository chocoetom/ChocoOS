#!/usr/bin/env python3
# QMP one-shot: query-status + query-cpus (liveness/halt probe).
import json
import socket

HOST, PORT = "127.0.0.1", 4444


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


s = socket.create_connection((HOST, PORT), timeout=15)
g = b""
while b"\n" not in g:
    g += s.recv(4096)
print(transact(s, {"execute": "qmp_capabilities"}))
print("status:", transact(s, {"execute": "query-status"}))
cpus = transact(s, {"execute": "query-cpus"})
import json as _j

print("cpus:", _j.dumps(cpus)[:3000])
s.close()
