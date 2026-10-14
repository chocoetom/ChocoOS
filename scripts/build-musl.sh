#!/usr/bin/env bash
# Builds the musl sysroot once (cached in build/musl-src) and compiles
# user/mhello.c against it. Called by build-user.sh before packing initrd.
# Needs network on first run (musl.libc.org tarball); reuses the cache after.
set -e
cd "$(dirname "$0")/.."
. scripts/host-env.sh

SYSROOT=build/musl-sysroot
SRC=build/musl-src
MUSL_VER="1.2.5"

if [ ! -f "$SRC/lib/libc.a" ]; then
  echo "[musl] bootstrapping sysroot (musl $MUSL_VER)..."
  mkdir -p build
  if [ ! -f build/musl.tgz ]; then
    curl -sL --max-time 120 "https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz" \
      -o build/musl.tgz
  fi
  rm -rf "$SRC"
  tar xzf build/musl.tgz -C build
  mv "build/musl-$MUSL_VER" "$SRC"
  cd "$SRC"
  CC="clang --target=x86_64-linux-musl" \
  CFLAGS="--target=x86_64-linux-musl -fuse-ld=lld -fno-stack-protector" \
  LDFLAGS="--target=x86_64-linux-musl -fuse-ld=lld -nostdlib" \
    ./configure --prefix="/d/Downloads/ChocoOS/$SYSROOT" \
      --target=x86_64-linux-musl --disable-shared >/dev/null
  # shellcheck disable=SC2086
  make -j2 CROSS_COMPILE= AR=ar RANLIB=ranlib >/dev/null 2>&1 || true
  # 'ar' on Windows chokes on the 1300-object command line: archive manually.
  if [ ! -f lib/libc.a ]; then
    find obj -name '*.lo' | sort > objs.rsp
    ar rc lib/libc.a @objs.rsp
    ranlib lib/libc.a
  fi
  make install CROSS_COMPILE= AR=ar RANLIB=ranlib >/dev/null 2>&1 || true
  cd ../..
fi

if [ ! -f "$SYSROOT/lib/libc.a" ] && [ -f "$SRC/lib/libc.a" ]; then
  # Fallback when 'make install' did not run: use the build tree directly.
  SYSROOT="$SRC"
fi

echo "[musl] compiling user/mhello.c..."
# shellcheck disable=SC2086
clang --target=x86_64-linux-musl -O2 -fno-stack-protector -mno-red-zone \
  -I "$SYSROOT/include" -I "$SRC/arch/x86_64" -I "$SRC/arch/generic" \
  -I "$SRC/obj/include" -I "$SRC/src/include" -I "$SRC/src/internal" \
  -c user/mhello.c -o build/user/mhello.o
LIBC="$SYSROOT/lib/libc.a"
[ -f "$LIBC" ] || LIBC="$SRC/lib/libc.a"
CRT="$SYSROOT/lib"
[ -f "$CRT/crt1.o" ] || CRT="$SRC/lib"
clang --target=x86_64-linux-musl -nostdlib -static -no-pie -fuse-ld=lld \
  "$CRT/crt1.o" "$CRT/crti.o" build/user/mhello.o "$LIBC" "$CRT/crtn.o" \
  -o build/user-root/bin/mhello
echo "[musl] bin/mhello ready"

echo "[musl] compiling user/lscm.c..."
# shellcheck disable=SC2086
clang --target=x86_64-linux-musl -O2 -fno-stack-protector -mno-red-zone \
  -I "$SYSROOT/include" -I "$SRC/arch/x86_64" -I "$SRC/arch/generic" \
  -I "$SRC/obj/include" -I "$SRC/src/include" -I "$SRC/src/internal" \
  -c user/lscm.c -o build/user/lscm.o
clang --target=x86_64-linux-musl -nostdlib -static -no-pie -fuse-ld=lld \
  "$CRT/crt1.o" "$CRT/crti.o" build/user/lscm.o "$LIBC" "$CRT/crtn.o" \
  -o build/user-root/bin/lscm
echo "[musl] bin/lscm ready"

echo "[musl] compiling user/wlrun.c..."
# shellcheck disable=SC2086
clang --target=x86_64-linux-musl -O2 -fno-stack-protector -mno-red-zone \
  -I "$SYSROOT/include" -I "$SRC/arch/x86_64" -I "$SRC/arch/generic" \
  -I "$SRC/obj/include" -I "$SRC/src/include" -I "$SRC/src/internal" \
  -c user/wlrun.c -o build/user/wlrun.o
clang --target=x86_64-linux-musl -nostdlib -static -no-pie -fuse-ld=lld \
  "$CRT/crt1.o" "$CRT/crti.o" build/user/wlrun.o "$LIBC" "$CRT/crtn.o" \
  -o build/user-root/bin/wlrun
echo "[musl] bin/wlrun ready"

# TLS-capable client: needs the BearSSL static archive.
bash scripts/build-bearssl.sh
BEARSSL=third_party/bearssl-0.6
echo "[musl] compiling user/httpsget.c..."
# shellcheck disable=SC2086
clang --target=x86_64-linux-musl -O2 -fno-stack-protector -mno-red-zone \
  -I "$SYSROOT/include" -I "$SRC/arch/x86_64" -I "$SRC/arch/generic" \
  -I "$SRC/obj/include" -I "$SRC/src/include" -I "$SRC/src/internal" \
  -I "$BEARSSL/inc" \
  -c user/httpsget.c -o build/user/httpsget.o
clang --target=x86_64-linux-musl -nostdlib -static -no-pie -fuse-ld=lld \
  "$CRT/crt1.o" "$CRT/crti.o" build/user/httpsget.o build/bearssl/libbearssl.a \
  "$LIBC" "$CRT/crtn.o" -o build/user-root/bin/httpsget
echo "[musl] bin/httpsget ready"

# Runtime TrueType rasterizer: links the FreeType static archive (ROADMAP
# 'fonts for the engine'). FT_New_Face opens the TTF from the initrd via stdio.
bash scripts/build-freetype.sh
FT=third_party/freetype-2.13.2
echo "[musl] compiling user/lfreetype.c..."
# shellcheck disable=SC2086
clang --target=x86_64-linux-musl -O2 -fno-stack-protector -mno-red-zone \
  -I "$SYSROOT/include" -I "$SRC/arch/x86_64" -I "$SRC/arch/generic" \
  -I "$SRC/obj/include" -I "$SRC/src/include" -I "$SRC/src/internal" \
  -I "$FT/include" \
  -c user/lfreetype.c -o build/user/lfreetype.o
clang --target=x86_64-linux-musl -nostdlib -static -no-pie -fuse-ld=lld \
  "$CRT/crt1.o" "$CRT/crti.o" build/user/lfreetype.o build/freetype/libfreetype.a \
  "$LIBC" "$CRT/crtn.o" -o build/user-root/bin/lfreetype
echo "[musl] bin/lfreetype ready"
