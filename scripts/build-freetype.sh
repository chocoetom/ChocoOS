#!/usr/bin/env bash
# Builds FreeType 2.13.2 (third_party/freetype-2.13.2) into
# build/freetype/libfreetype.a for the x86_64-linux-musl target, so ring-3
# programs can rasterize TrueType glyphs at runtime (the ROADMAP 'fonts for the
# engine' gap). The archive is cached; delete it to force a rebuild.
#
# We compile the MINIMAL module set a TrueType engine needs (base + truetype +
# sfnt + psnames + raster + smooth) and ship our own ftmodule.h registering only
# those classes. The full default ftmodule.h pulls in lzw/gzip/exotic drivers
# that either fail under musl or we never use. FreeType uses its own fixed-point
# math, so no FPU is required by the library itself.
set -e
cd "$(dirname "$0")/.."
. scripts/host-env.sh

SRC=third_party/freetype-2.13.2
OUT=build/freetype
LIB=$OUT/libfreetype.a
MODULES="base truetype sfnt psnames raster smooth"
# Each module ships an "umbrella" <name>.c that #includes all its real sources
# (single-file build). We compile the INDIVIDUAL files instead, so we must skip
# the umbrellas or every symbol would appear twice at link. src/gzip/ftgzip.c is
# pulled in because sfnt's SVG/sfnt loader references FT_Gzip_Uncompress; its
# inflate is internal (no external zlib). lzw/bzip2 are not referenced, so we
# leave them out (ftlzw.c does not build under musl).
UMBRELLAS="$SRC/src/base/ftbase.c $SRC/src/truetype/truetype.c $SRC/src/sfnt/sfnt.c \
  $SRC/src/psnames/psnames.c $SRC/src/raster/raster.c $SRC/src/smooth/smooth.c"

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
  echo "ERROR: $SRC missing (FreeType sources not vendored)." >&2
  exit 1
fi

if [ ! -f "$LIB" ]; then
  echo "[freetype] compiling $MODULES -> $LIB ..."
  rm -rf "$OUT"
  mkdir -p "$OUT/obj" "$OUT/inc/freetype/config"

  # Override the default module list with exactly what we compile. This header
  # is #included by src/base/ftinit.c and must match the objects below.
  cat > "$OUT/inc/freetype/config/ftmodule.h" <<'EOF'
FT_USE_MODULE( FT_Driver_ClassRec,      tt_driver_class )
FT_USE_MODULE( FT_Module_Class,         sfnt_module_class )
FT_USE_MODULE( FT_Module_Class,         psnames_module_class )
FT_USE_MODULE( FT_Renderer_Class,       ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class,       ft_raster1_renderer_class )
/* EOF */
EOF

  n=0
  fail=0
  # Gather the individual sources (module dirs minus the umbrella files) plus the
  # gzip filter, then compile one object each.
  LIST=""
  for m in $MODULES; do
    for f in $(find "$SRC/src/$m" -name '*.c' | sort); do
      skip=0
      for u in $UMBRELLAS; do [ "$f" = "$u" ] && skip=1; done
      [ "$skip" = 1 ] && continue
      LIST="$LIST $f"
    done
  done
  LIST="$LIST $SRC/src/gzip/ftgzip.c"

  for c in $LIST; do
    o="$OUT/obj/$(echo "$c" | tr './' '__').o"
    # -I $OUT/inc FIRST so our ftmodule.h wins over the vendored default.
    if ! clang --target=x86_64-linux-musl -O2 -fno-stack-protector -w \
      $MUSL_INC -I "$OUT/inc" -I "$SRC/include" -I "$SRC/src" -DFT2_BUILD_LIBRARY \
      -c "$c" -o "$o"; then
      echo "[freetype] FAILED: $c" >&2
      fail=$((fail + 1))
    fi
    n=$((n + 1))
  done
  [ "$fail" -eq 0 ] || { echo "[freetype] $fail file(s) failed to compile" >&2; exit 1; }
  ar rc "$LIB" "$OUT"/obj/*.o
  ranlib "$LIB"
  echo "[freetype] $LIB ready from $n objects ($(du -h "$LIB" | cut -f1))"
else
  echo "[freetype] cached: $LIB"
fi
