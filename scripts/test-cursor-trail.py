#!/usr/bin/env python3
"""Headless regression for the pointer pixel trail.

The desktop paints the cursor straight into the back buffer. A cursor-only
update erases the old glyph from a save-under box, so that box must cover the
whole glyph (the arrow spans x..x+10 by y..y+16). If it is one row short the
uncovered edge is never restored and the pointer smears a trail behind it.

This test parks the pointer, snapshots a clean frame, sweeps it horizontally
across bare wallpaper, snapshots again, and requires the swept band to be
pixel-identical to the clean frame (the two cursor boxes at the ends are
excluded). Any leftover glyph pixels show up as a diff.

Usage: python scripts/test-cursor-trail.py     (build first)
Exit 0 when the band is clean.
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "ChocoOS.iso")
RUN = os.getpid()
LOG = os.path.join(ROOT, "build", "serial-trail.%d.log" % RUN)
CLEAN = os.path.join(ROOT, "build", "trail_clean.%d.ppm" % RUN)
SWEPT = os.path.join(ROOT, "build", "trail_swept.%d.ppm" % RUN)

# The pointer walks this horizontal band across the bare desktop; the compared
# window trims the start box, the end box and the vertical padding around them.
Y = 400
X0, X1 = 300, 900
CMP_X0, CMP_X1 = 340, 860
CMP_Y0, CMP_Y1 = 388, 424


def find_qemu():
    for cand in ("/ucrt64/bin", "/c/msys64/ucrt64/bin", "/mnt/c/msys64/ucrt64/bin",
                 "C:/msys64/ucrt64/bin"):
        if os.path.isdir(cand):
            os.environ["PATH"] = cand + os.pathsep + os.environ.get("PATH", "")
    if not _which("qemu-system-x86_64"):
        sys.exit("FAIL: qemu-system-x86_64 not on PATH")


def _which(tool):
    for d in os.environ.get("PATH", "").split(os.pathsep):
        for ext in ("", ".exe"):
            p = os.path.join(d, tool + ext)
            if os.path.isfile(p):
                return p
    return None


class Qmp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.f = self.s.makefile("rw", encoding="utf-8", newline="")
        self.read()
        self.exec_("qmp_capabilities")

    def read(self):
        while True:
            line = self.f.readline()
            if not line:
                return None
            try:
                m = json.loads(line)
            except ValueError:
                continue
            if "return" in m or "error" in m or "QMP" in m:
                return m

    def exec_(self, cmd, **args):
        obj = {"execute": cmd}
        if args:
            obj["arguments"] = args
        self.f.write(json.dumps(obj) + "\n")
        self.f.flush()
        return self.read()

    def hmp(self, line):
        r = self.exec_("human-monitor-command", **{"command-line": line})
        if r is None:
            return "<no reply>"
        if "error" in r:
            return "QMP-ERR " + json.dumps(r["error"])[:160]
        return (r.get("return") or "").strip()


def move(q, dx, dy):
    # PS/2 carries at most +/-127 per packet; step well inside the limit.
    while dx or dy:
        sx = max(-100, min(100, dx))
        sy = max(-100, min(100, dy))
        q.hmp("mouse_move %d %d" % (sx, sy))
        dx -= sx
        dy -= sy
        time.sleep(0.15)


def goto(q, x, y):
    for _ in range(30):  # saturate the driver's clamp to reach (0,0)
        q.hmp("mouse_move -100 -100")
    time.sleep(0.5)
    move(q, x, y)


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"P6":
        raise ValueError("not a P6 ppm: " + path)
    pos, fields = 2, []
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1] in b" \t\r\n":
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos:pos + 1] not in b" \t\r\n":
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h = fields[0], fields[1]
    return w, h, data[pos:pos + w * h * 3]


def main():
    find_qemu()
    if not os.path.exists(ISO):
        sys.exit("FAIL: no ISO, build first")
    for p in (LOG, CLEAN, SWEPT):
        if os.path.exists(p):
            os.remove(p)
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.close()
    q = subprocess.Popen(
        [_which("qemu-system-x86_64"), "-cdrom", ISO, "-m", "256M", "-display", "none",
         "-accel", "tcg,tb-size=64", "-serial", "file:" + LOG, "-boot", "order=d",
         "-no-reboot", "-qmp", "tcp:127.0.0.1:%d,server,nowait" % port],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = False
    for _ in range(90):
        time.sleep(1)
        try:
            if "choco> " in open(LOG, encoding="utf-8", errors="replace").read():
                ok = True
                break
        except OSError:
            pass
    if not ok:
        q.kill()
        sys.exit("FAIL: shell prompt never appeared")
    c = Qmp(port)
    passed = failed = 0

    def check(cond, what):
        nonlocal passed, failed
        print(("PASS: " if cond else "FAIL: ") + what)
        if cond:
            passed += 1
        else:
            failed += 1

    # Clean frame: pointer parked at the top-left corner, away from the band.
    goto(c, 0, 0)
    c.hmp("screendump " + CLEAN.replace("\\", "/"))
    time.sleep(2)

    # Sweep the pointer left-to-right across the band, ending at the right.
    goto(c, X0, Y)
    time.sleep(0.5)
    move(c, X1 - X0, 0)
    time.sleep(1.0)
    c.hmp("screendump " + SWEPT.replace("\\", "/"))
    time.sleep(2)

    c.hmp("quit")
    try:
        q.wait(timeout=25)
    except subprocess.TimeoutExpired:
        q.kill()

    w0, h0, a = read_ppm(CLEAN)
    w1, h1, b = read_ppm(SWEPT)
    check(w0 == w1 and h0 == h1, "both frames are the same size (%dx%d)" % (w0, h0))

    diff = 0
    first = None
    for y in range(CMP_Y0, min(CMP_Y1, h0)):
        base = y * w0 * 3
        for x in range(CMP_X0, min(CMP_X1, w0)):
            i = base + x * 3
            if a[i:i + 3] != b[i:i + 3]:
                diff += 1
                if first is None:
                    first = (x, y)
    print("swept-band pixel diffs: %d%s" % (diff, "" if first is None else " (first at %s)" % (first,)))
    check(diff == 0, "pointer left no trail on the swept band")

    print("test-cursor-trail: %d passed, %d failed" % (passed, failed))
    if failed:
        print("frames kept: %s %s" % (CLEAN, SWEPT))
    else:
        for p in (CLEAN, SWEPT, LOG):
            try:
                os.remove(p)
            except OSError:
                pass
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
