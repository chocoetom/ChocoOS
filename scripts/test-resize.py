#!/usr/bin/env python3
"""Headless E2E for window gestures: kernel test + a real PS/2 drag on an app.

Phase 1 runs the guest's own `wintest`, which drives the window manager through
synthesized pointer motion (drag, drag back, edge resize, maximize, restore).
Phase 2 does the gesture a human actually makes, through QEMU's PS/2 mouse
instead of the kernel hook: grab the Explorer's right border and pull it.

QEMU's legacy HMP commands do reach the guest with -display none (mouse_move
accumulates screen-relative deltas, mouse_button 1 presses, mouse_button 0
releases), so this drives the real i8042 path and measures the result in the
framebuffer: the selected list row is filled with UI_SEL, which nothing else on
the desktop uses, so its horizontal span *is* the window's content width.

Usage: python scripts/test-resize.py     (build first)
Exit 0 when every assertion holds.
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
LOG = os.path.join(ROOT, "build", "serial-resize.%d.log" % RUN)
BEFORE = os.path.join(ROOT, "build", "rsz_before.%d.ppm" % RUN)
AFTER = os.path.join(ROOT, "build", "rsz_after.%d.ppm" % RUN)
BACK = os.path.join(ROOT, "build", "rsz_back.%d.ppm" % RUN)

# Explorer geometry, fixed by gui::user_create (slot 0) and user/fileman.c.
WIN_X, WIN_Y = 120, 110
CW, CH = 620, 460          # content size the app asks for
BORDER, TITLE = 2, 28      # theme.hpp
OUTER_W = CW + 2 * BORDER
# Client area origin, and the first list row (TOOLBAR_H 48 + HDR_H 26, ROW_H 32).
CLI_X, CLI_Y = WIN_X + BORDER, WIN_Y + TITLE
ROW_Y0, ROW_Y1 = CLI_Y + 74 + 4, CLI_Y + 74 + 25
SEL = (0x2D, 0x2A, 0x40)   # UI_SEL: the selected row's fill (user/lib/gui.h)
FOLDER = (0xF5, 0xA5, 0x24)  # UI_FOLDER: the row icons
DRAG = 100                 # px the right border is pulled left


def find_qemu():
    # Both spellings: the scripts run under MSYS2 bash (POSIX prefix) or a plain
    # Windows python (drive-letter prefix).
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


KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}


def type_line(q, s, wait=1.5):
    for ch in s:
        k = ch.lower() if ch.isalnum() else KEYMAP.get(ch)
        if k:
            q.hmp("sendkey " + ("shift-" + k if ch.isupper() else k))
    q.hmp("sendkey ret")
    time.sleep(wait)


def move(q, dx, dy):
    # PS/2 carries at most +/-127 per packet; QEMU splits larger deltas, but
    # step well inside the limit so a dropped packet cannot desync the pointer.
    while dx or dy:
        sx = max(-100, min(100, dx))
        sy = max(-100, min(100, dy))
        q.hmp("mouse_move %d %d" % (sx, sy))
        dx -= sx
        dy -= sy
        time.sleep(0.15)


def goto(q, x, y):
    # Park in the top-left corner by *saturating* the driver's clamp. A single
    # oversized mouse_move is dropped (PS/2 carries at most +/-127 per packet),
    # which is what left the pointer nowhere near the border on the first run.
    for _ in range(30):
        q.hmp("mouse_move -100 -100")
    time.sleep(0.5)
    move(q, x, y)


def save_png(img, path):
    """PPM -> PNG with stdlib only (the committed scripts must not need PIL)."""
    import struct
    import zlib
    w, h, px = img
    rows = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(rows, 6)))
        fh.write(chunk(b"IEND", b""))


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"P6":
        raise ValueError("not a P6 ppm: " + path)
    pos, fields = 2, []
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1] in b" \t\r\n":
            pos += 1
        if data[pos:pos + 1] == b"#":  # comment line
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos:pos + 1] not in b" \t\r\n":
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # the single whitespace byte that ends the header
    w, h = fields[0], fields[1]
    return w, h, data[pos:pos + w * h * 3]


def span(img, y0, y1, color, x_from=0):
    """min/max x and count of an exact color inside rows [y0,y1)."""
    w, h, px = img
    lo, hi, n = None, None, 0
    r, g, b = color
    for y in range(max(0, y0), min(h, y1)):
        base = y * w * 3
        for x in range(x_from, w):
            i = base + x * 3
            if px[i] == r and px[i + 1] == g and px[i + 2] == b:
                n += 1
                if lo is None:
                    lo = x
                hi = x
    return (lo if lo is not None else -1, hi if hi is not None else -1, n)


def count(img, color):
    _w, _h, px = img
    r, g, b = color
    return sum(1 for i in range(0, len(px) - 2, 3)
               if px[i] == r and px[i + 1] == g and px[i + 2] == b)


def main():
    find_qemu()
    if not os.path.exists(ISO):
        sys.exit("FAIL: no ISO, build first")
    for p in (LOG, BEFORE, AFTER, BACK):
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

    # Phase 1: the kernel's own deterministic gesture test (drag, drag back,
    # edge resize, maximize, restore). It existed for a while but no script ever
    # asserted its result, so "wintest: PASS" was a manual step nobody repeated.
    type_line(c, "wintest", wait=10)
    log = open(LOG, encoding="utf-8", errors="replace").read().replace("\r", "")
    wt = [l for l in log.split("\n")
          if l.startswith("[wintest]") and (": PASS" in l or ": FAIL" in l)]
    for line in wt:
        check(": FAIL" not in line, "wintest " + line.split(": ", 1)[1])
    check(len(wt) == 5, "wintest ran all five gestures (got %d)" % len(wt))

    # Phase 2: a real PS/2 drag on a shared-memory (user) window.
    type_line(c, "run bin/fileman", wait=8)
    c.hmp("screendump " + BEFORE.replace("\\", "/"))
    time.sleep(2)

    # Grab the right border (a couple of px inside the 6px band) and pull it left.
    grab_x, grab_y = WIN_X + OUTER_W - 2, WIN_Y + TITLE + CH // 2
    goto(c, grab_x, grab_y)
    GRAB = os.path.join(ROOT, "build", "rsz_grab.%d.ppm" % RUN)
    c.hmp("screendump " + GRAB.replace("\\", "/"))
    time.sleep(1.5)
    c.hmp("mouse_button 1")
    time.sleep(0.6)
    move(c, -DRAG, 0)
    c.hmp("mouse_button 0")
    time.sleep(4)
    c.hmp("screendump " + AFTER.replace("\\", "/"))
    time.sleep(2)

    # And back out again: a resize must be reversible, not a one-way clamp.
    goto(c, grab_x - DRAG, grab_y)
    c.hmp("mouse_button 1")
    time.sleep(0.6)
    move(c, DRAG, 0)
    c.hmp("mouse_button 0")
    time.sleep(4)
    c.hmp("screendump " + BACK.replace("\\", "/"))
    time.sleep(2)

    # Hand the app back a clean exit, then leave the VM.
    goto(c, grab_x - DRAG + 300, grab_y)
    c.hmp("quit")
    try:
        q.wait(timeout=25)
    except subprocess.TimeoutExpired:
        q.kill()

    pre, mid, post = read_ppm(BEFORE), read_ppm(AFTER), read_ppm(BACK)
    lo0, hi0, n0 = span(pre, ROW_Y0, ROW_Y1, SEL)
    lo1, hi1, n1 = span(mid, ROW_Y0, ROW_Y1, SEL)
    lo2, hi2, n2 = span(post, ROW_Y0, ROW_Y1, SEL)
    print("selected row  before: x %d..%d (%d px)" % (lo0, hi0, n0))
    print("selected row  after : x %d..%d (%d px)" % (lo1, hi1, n1))
    print("selected row  back  : x %d..%d (%d px)" % (lo2, hi2, n2))

    # The row must start where fileman.c draws it: content_x+10 = 182 in client px.
    check(abs(lo0 - (CLI_X + 182)) <= 4,
          "explorer drew its first row at the expected x (%d, want ~%d)" % (lo0, CLI_X + 182))
    check(hi0 - lo0 >= CW - 192 - 8,
          "row fill spans the full content width before the drag (%d px)" % (hi0 - lo0))
    check(abs((hi0 - hi1) - DRAG) <= 3,
          "mouse drag shrank the window by %d px (got %d)" % (DRAG, hi0 - hi1))
    check(abs(lo1 - lo0) <= 2,
          "the untouched left edge stayed put (%d -> %d)" % (lo0, lo1))
    check(n1 < n0 and n1 > 0,
          "app repainted at the smaller size (selection fill %d -> %d px)" % (n0, n1))
    check(count(mid, FOLDER) > 0,
          "explorer is still alive and drawing icons after the resize")
    check(abs(hi2 - hi0) <= 3,
          "drag back restored the original width (%d -> %d -> %d)" % (hi0, hi1, hi2))

    print("test-resize: %d passed, %d failed" % (passed, failed))
    if failed:
        # Keep the evidence and make it viewable: these are the three frames.
        for src, name in ((GRAB, "rsz_grab"), (BEFORE, "rsz_before"),
                          (AFTER, "rsz_after"), (BACK, "rsz_back")):
            if os.path.exists(src):
                save_png(read_ppm(src), os.path.join(ROOT, "build", name + ".png"))
        print("frames kept as build/rsz_*.png for inspection")
    else:
        for p in (GRAB, BEFORE, AFTER, BACK, LOG):
            try:
                os.remove(p)
            except OSError:
                pass
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
