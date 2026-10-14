#!/usr/bin/env python3
"""Boot the ISO headless, drive it, save framebuffer shots as PNG.

Interactive helper for GUI work: it does not assert anything, it shows the
screen and the serial log, which is what iterating on a layout needs.

  python scripts/shot.py out.png "run bin/fileman" "sleep 3"
  python scripts/shot.py shot.png "click 50 432" "sleep 2"      # real PS/2 mouse
  python scripts/shot.py shot.png "drag 742 368 642 368"        # press, move, release

Steps are strings. 'sleep N' waits, 'hmp <cmd>' runs a monitor command,
'move|click|drag' drive the i8042 mouse, anything else is typed into the shell.
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "ChocoOS.iso")
KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
          ":": "shift-semicolon", "=": "equal"}


def find_qemu():
    for cand in ("/ucrt64/bin", "/c/msys64/ucrt64/bin", "C:/msys64/ucrt64/bin",
                 "/mnt/c/msys64/ucrt64/bin"):
        if os.path.isdir(cand):
            os.environ["PATH"] = cand + os.pathsep + os.environ.get("PATH", "")
    for tool in ("qemu-system-x86_64",):
        if not which(tool):
            sys.exit("FAIL: %s not on PATH" % tool)


def which(tool):
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
            return "ERR " + json.dumps(r["error"])[:200]
        return (r.get("return") or "").strip()


def type_line(c, s, wait=1.5):
    for ch in s:
        k = ch.lower() if ch.isalnum() else KEYMAP.get(ch)
        if k:
            c.hmp("sendkey " + ("shift-" + k if ch.isupper() else k))
        time.sleep(0.05)
    c.hmp("sendkey ret")
    time.sleep(wait)


def mouse_move(c, dx, dy):
    # PS/2 carries at most +/-127 per packet and QEMU drops an oversized step.
    while dx or dy:
        sx = max(-100, min(100, dx))
        sy = max(-100, min(100, dy))
        c.hmp("mouse_move %d %d" % (sx, sy))
        dx -= sx
        dy -= sy
        time.sleep(0.12)


def mouse_goto(c, x, y):
    for _ in range(30):
        c.hmp("mouse_move -100 -100")  # the driver clamps: that parks at 0,0
    time.sleep(0.4)
    mouse_move(c, x, y)


def mouse_step(c, spec):
    """'click x y', 'move x y' or 'drag x0 y0 x1 y1' through the real i8042."""
    words = spec.split()
    kind, nums = words[0], [int(v) for v in words[1:]]
    if kind == "move":
        mouse_goto(c, nums[0], nums[1])
    elif kind == "click":
        mouse_goto(c, nums[0], nums[1])
        time.sleep(0.3)
        c.hmp("mouse_button 1")
        time.sleep(0.3)
        c.hmp("mouse_button 0")
        time.sleep(0.6)
    elif kind == "double":
        mouse_goto(c, nums[0], nums[1])
        time.sleep(0.3)
        for _ in range(2):
            c.hmp("mouse_button 1")
            time.sleep(0.1)
            c.hmp("mouse_button 0")
            time.sleep(0.1)
        time.sleep(0.6)
    elif kind == "drag":
        mouse_goto(c, nums[0], nums[1])
        c.hmp("mouse_button 1")
        time.sleep(0.4)
        mouse_move(c, nums[2] - nums[0], nums[3] - nums[1])
        c.hmp("mouse_button 0")
        time.sleep(0.6)
    else:
        raise SystemExit("unknown mouse step: %s" % spec)


def ppm_to_png(src, dst):
    import struct
    import zlib
    data = open(src, "rb").read()
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
    px = data[pos:pos + w * h * 3]
    rows = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))
    with open(dst, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(rows, 6)))
        fh.write(chunk(b"IEND", b""))


def main():
    find_qemu()
    if not os.path.exists(ISO):
        sys.exit("FAIL: no ISO, build first")
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    out = args[0]
    steps = args[1:]
    if not os.path.isabs(out):
        out = os.path.join(ROOT, out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    run = os.getpid()
    log = os.path.join(ROOT, "build", "shot-%d.log" % run)
    raw = os.path.join(ROOT, "build", "shot-%d.ppm" % run)
    open(log, "w").close()
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.close()
    # VOLUME=<raw FAT32 image> attaches the persistent ATA volume like run.sh, so
    # /disk is writable (otherwise the read-only boot-module ramdisk is used).
    vol = os.environ.get("VOLUME")
    volargs = (["-drive", "file=%s,format=raw,if=none,id=hdrvol" % vol,
                "-device", "ide-hd,bus=ide.0,unit=0,drive=hdrvol"] if vol else [])
    q = subprocess.Popen(
        [which("qemu-system-x86_64"), "-cdrom", ISO, "-m", "256M", "-display", "none",
         "-accel", "tcg,tb-size=64", "-serial", "file:" + log, "-boot", "order=d",
         "-no-reboot", "-qmp", "tcp:127.0.0.1:%d,server,nowait" % port] + volargs +
        (["-nic", "user,model=virtio,hostname=chocohost"] if os.environ.get("NIC") else []),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = False
    for _ in range(90):
        time.sleep(1)
        if "choco> " in open(log, encoding="utf-8", errors="replace").read():
            ok = True
            break
    if not ok:
        q.kill()
        sys.exit("FAIL: shell prompt never appeared (see %s)" % log)
    c = Qmp(port)
    shots = [(out, None)]

    def grab(dst):
        raw_one = os.path.join(ROOT, "build", "shot-%d-%d.ppm" % (run, len(shots)))
        c.hmp("screendump " + raw_one.replace("\\", "/"))
        time.sleep(2)
        shots.append((dst, raw_one))

    for s in steps:
        if s.startswith("shot "):
            dst = s[5:]
            grab(dst if os.path.isabs(dst) else os.path.join(ROOT, dst))
        elif s.startswith("sleep "):
            time.sleep(float(s.split()[1]))
        elif s.startswith("hmp "):
            print("hmp %s -> %s" % (s[4:], c.hmp(s[4:])))
        elif s.startswith(("move ", "click ", "double ", "drag ")):
            mouse_step(c, s)
        else:
            type_line(c, s)
    grab(raw)
    try:
        c.hmp("quit")
        q.wait(timeout=20)
    except Exception:
        # The guest died (a kernel panic exits QEMU under -no-reboot). Keep the
        # log: it holds the exception frame that explains why.
        try:
            q.kill()
        except Exception:
            pass
    log_txt = open(log, encoding="utf-8", errors="replace").read().replace("\r","")
    if "[EXC" in log_txt:
        print("=== guest exception ===")
        for ln in log_txt.split("\n"):
            if ln.startswith("[EXC") or "vec=" in ln or ln.strip().startswith(("rax", "cr2", "Choco")):
                print(ln)
    made = []
    for dst, src in shots:
        if src and os.path.exists(src):
            d = dst if os.path.isabs(dst) else os.path.join(ROOT, dst)
            os.makedirs(os.path.dirname(d), exist_ok=True)
            ppm_to_png(src, d)
            os.remove(src)
            made.append(d)
    if not made:
        print("=== last serial lines ===")
        print("\n".join([l for l in log_txt.split("\n") if l.strip()][-15:]))
        os.remove(log)
        sys.exit("FAIL: no screendump produced")
    for d in made:
        print("shot -> %s" % d)
    print("=== serial tail ===")
    print("\n".join([l for l in log_txt.split("\n") if l.strip()][-25:]))
    os.remove(log)
    return 0


if __name__ == "__main__":
    sys.exit(main())
