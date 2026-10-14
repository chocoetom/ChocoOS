#!/usr/bin/env bash
# Shared environment for the headless test scripts.
#
# The scripts are written for the MSYS2 UCRT64 shell, where the toolchain lives
# at /ucrt64/bin. From a plain git-bash that path does not exist, and a hardcoded
# `export PATH=/ucrt64/bin:/usr/bin:/bin` silently leaves qemu/cmake/python off
# PATH: the emulator never starts, the previous run's serial log is still on
# disk, and every marker greps green. Resolve the real prefix instead.
for _cand in /ucrt64/bin /c/msys64/ucrt64/bin /mnt/c/msys64/ucrt64/bin \
             /c/msys64/mingw64/bin; do
  [ -d "$_cand" ] && PATH="$_cand:$PATH"
done
export PATH="$PATH:/usr/bin:/bin"

# need <tool> [package] - abort with a message instead of passing on stale data.
need() {
  command -v "$1" >/dev/null 2>&1 && return 0
  echo "FAIL: '$1' not found on PATH. Install it (pacman -S $2) or run these"
  echo "      scripts from the MSYS2 UCRT64 shell."
  exit 1
}

# fresh <file> - remove an artifact and prove it is really gone. A leftover
# QEMU keeps serial logs open on Windows, so a failed rm would feed the
# previous run's output to the assertions below.
fresh() {
  rm -f "$1"
  if [ -e "$1" ]; then
    echo "FAIL: cannot delete $1 - a stale emulator still holds it open."
    echo "      kill qemu-system-x86_64 (taskkill //F //IM qemu-system-x86_64.exe) and rerun."
    exit 1
  fi
}
