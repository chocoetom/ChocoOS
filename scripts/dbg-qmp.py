#!/usr/bin/env python3
# QMP client: query x-query-virtio-status and dump it.
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
        # else: async event, keep waiting


def main():
    s = socket.create_connection((HOST, PORT), timeout=15)
    g = b""
    while b"\n" not in g:
        g += s.recv(4096)
    print("greeted ok")
    print(transact(s, {"execute": "qmp_capabilities"}))
    print("LIST:", json.dumps(transact(s, {"execute": "x-query-virtio"}))[:2000])
    p = "/machine/peripheral-anon/device[0]/virtio-backend"
    for q in (0, 1):
        r = transact(
            s,
            {"execute": "x-query-virtio-queue-status", "arguments": {"path": p, "queue": q}},
        )
        print("QUEUE", q, "last_avail:", r.get("return", {}).get("last-avail-idx"),
              "used:", r.get("return", {}).get("used-idx"))
    for idx in (0, 1):
        r = transact(
            s,
            {
                "execute": "x-query-virtio-queue-element",
                "arguments": {"path": p, "queue": 1, "index": idx},
            },
        )
        print("TXDESC", idx)
        print(json.dumps(r, indent=1)[:3000])
    s.close()


main()
