#!/usr/bin/env bash
# Builds BearSSL (third_party/bearssl-0.6) into build/bearssl/libbearssl.a for
# the x86_64-linux-musl target, so ring-3 tools (bin/httpsget) can link TLS.
# The archive is cached; delete it to force a rebuild.
#
# BearSSL has no build-system dependency here: every .c under src/ is portable
# C and compiles together, and the linker drops the parts we never call.
set -e
cd "$(dirname "$0")/.."
. scripts/host-env.sh

SRC=third_party/bearssl-0.6
OUT=build/bearssl
LIB=$OUT/libbearssl.a

# musl headers, same sysroot build-musl.sh produces.
if [ -d build/musl-sysroot/include ]; then
  MUSL_INC="-I build/musl-sysroot/include"
elif [ -d build/musl-src/include ]; then
  MUSL_INC="-I build/musl-src/include -I build/musl-src/arch/x86_64 -I build/musl-src/arch/generic -I build/musl-src/obj/include -I build/musl-src/src/include -I build/musl-src/src/internal"
else
  echo "ERROR: musl sysroot missing; run scripts/build-musl.sh first." >&2
  exit 1
fi

if [ ! -d "$SRC/src" ]; then
  echo "ERROR: $SRC missing (BearSSL sources not vendored)." >&2
  exit 1
fi

if [ ! -f "$LIB" ]; then
  echo "[bearssl] compiling $SRC -> $LIB ..."
  rm -rf "$OUT"
  mkdir -p "$OUT/obj"
  n=0
  for c in $(find "$SRC/src" -name '*.c' | sort); do
    o="$OUT/obj/$(echo "$c" | tr './' '__').o"
    clang --target=x86_64-linux-musl -O2 -fno-stack-protector -w \
      $MUSL_INC -I "$SRC/src" -I "$SRC/inc" -c "$c" -o "$o"
    n=$((n + 1))
  done
  ar rc "$LIB" "$OUT"/obj/*.o
  ranlib "$LIB"
  echo "[bearssl] $LIB ready from $n objects ($(du -h "$LIB" | cut -f1))"
else
  echo "[bearssl] cached: $LIB"
fi
