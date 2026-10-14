#!/usr/bin/env bash
# Builds user-space programs and packs initrd.tar (run inside MSYS2 UCRT64).
# Needs a bare-metal-capable compiler: x86_64-elf-gcc or clang+lld.
set -e
cd "$(dirname "$0")/.."

TARGET_FLAGS=""
LD_FLAGS=""
if command -v x86_64-elf-gcc >/dev/null 2>&1; then
  CC=x86_64-elf-gcc
elif command -v clang >/dev/null 2>&1; then
  CC=clang
  TARGET_FLAGS="--target=x86_64-unknown-elf"
  LD_FLAGS="-fuse-ld=lld"
else
  echo "ERROR: need x86_64-elf-gcc or clang for user programs." >&2
  echo "Run scripts/setup-windows.ps1 first." >&2
  exit 1
fi
echo "[user] compiler: $CC $TARGET_FLAGS"

mkdir -p build/user build/user-root/bin

nasm -f elf64 user/start.asm -o build/user/start.o

# No FPU/SSE in ChocoOS user mode yet (kernel keeps CR0.EM set): integer
# code only, exactly like the kernel build.
USER_CFLAGS="-ffreestanding -O2 -m64 -mno-red-zone -fno-pic -fno-pie \
  -fno-stack-protector -mno-80387 -mno-mmx -mno-3dnow -mno-sse -mno-sse2 -Wall"

# shellcheck disable=SC2086
build_one() {
  $CC $TARGET_FLAGS $USER_CFLAGS -Iuser \
    -c "user/$1.c" -o "build/user/$1.o"
  $CC $TARGET_FLAGS -nostdlib -static -no-pie -fno-pic -fno-pie \
    $LD_FLAGS -T user/hello.ld \
    -o "build/user-root/bin/$1" build/user/start.o "build/user/$1.o"
}

# Linux-ABI tests (pure asm, no libc): nasm + link as Linux x86-64 ELF.
# shellcheck disable=SC2086
build_linux() {
  nasm -f elf64 "user/linux/$1.asm" -o "build/user/$1.o"
  $CC --target=x86_64-unknown-linux-gnu -nostdlib -static -no-pie \
    -fno-pic -fno-pie -mno-red-zone $LD_FLAGS -T user/linux/linux.ld \
    -o "build/user-root/bin/$1" "build/user/$1.o"
}

build_one fileman
build_one browser
build_one lnk
build_one sysmon
build_one ttytest
build_one hello
build_one micro
build_one lua
build_one micropython
build_one uls
build_one ucat
build_one umon
build_one uwm
build_linux lhello
build_linux ltest
build_linux lfs
build_linux lguard
build_linux lpipe
build_linux lexec
build_linux lclone
build_linux lclonepipe
build_linux lfork
build_linux lexecclone
build_linux ltty
build_linux lpipebroken
build_linux lkillself
build_linux lsigconfig
build_linux lhandler
build_linux lttygrp
build_linux lfswrite
build_linux lmkdir
build_linux lhier
build_linux lbusyexecpipe
build_linux lbusypipe
build_linux lfpu
build_linux lstat
build_linux lfstat
build_linux lsock
build_linux ldate
build_linux lmmap
build_linux lpoll
build_linux lsp
build_linux lusock
build_linux lechoserv
build_linux lpersist
build_linux lrand
build_linux lmemfd

cp user/root/motd.txt build/user-root/
# Trust anchor(s) for bin/httpsget: a PEM CA bundle read at run time.
rm -rf build/user-root/etc
cp -r user/root/etc build/user-root/
# Desktop shortcuts are data, not code: they ship as .desktop files in the
# initrd, and the window manager scans them at run time (kernel/proc/shortcuts.cpp).
rm -rf build/user-root/desktop
cp -r user/root/desktop build/user-root/
# A real TrueType face for runtime rasterization (bin/lfreetype now, a browser
# font stack later). Inter, SIL Open Font License 1.1 - assets/fonts/INTER-OFL.txt.
rm -rf build/user-root/usr
mkdir -p build/user-root/usr/share/fonts
cp assets/fonts/Inter.ttf build/user-root/usr/share/fonts/Inter.ttf
bash scripts/build-musl.sh
tar --format=ustar -cf build/initrd.tar -C build/user-root bin desktop etc usr motd.txt
echo "[user] initrd.tar ready ($(du -h build/initrd.tar | cut -f1)): $(ls build/user-root/bin | tr '\n' ' ')"
