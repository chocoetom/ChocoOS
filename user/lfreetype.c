// lfreetype.c - prove runtime TrueType rasterization in ring 3.
//
// The ROADMAP 'fonts for the engine' gap: until now the UI font was BAKED at
// build time (scripts/build-font.py) because the kernel/user build was
// integer-only, so a real engine (NetSurf/Dillo) had no glyph rasterizer. Here
// we link libfreetype.a (built by scripts/build-freetype.sh) and rasterize a
// glyph from a TTF loaded from the initrd at run time - exactly the path a
// browser's font stack uses (FT_New_Face -> stdio open/read -> syscalls).
//
// FreeType does its own fixed-point math, so it needs no FPU; the run-ltests
// gate just checks the marker 'ftfont: PASS'.
#include <stdio.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static const char *FONT = "/usr/share/fonts/Inter.ttf";

int main(void) {
    FT_Library lib = 0;
    FT_Face face = 0;
    int err;

    err = FT_Init_FreeType(&lib);
    if (err) { printf("ftfont: FAIL init err=%d\n", err); return 1; }

    err = FT_New_Face(lib, FONT, 0, &face);
    if (err) { printf("ftfont: FAIL new_face err=%d\n", err); FT_Done_FreeType(lib); return 1; }

    err = FT_Set_Pixel_Sizes(face, 0, 24);
    if (err) { printf("ftfont: FAIL set_size err=%d\n", err); FT_Done_Face(face); FT_Done_FreeType(lib); return 1; }

    // Render 'A' into an 8-bit anti-aliased bitmap.
    err = FT_Load_Char(face, 'A', FT_LOAD_RENDER);
    if (err) { printf("ftfont: FAIL load_char err=%d\n", err); FT_Done_Face(face); FT_Done_FreeType(lib); return 1; }

    FT_Bitmap *bm = &face->glyph->bitmap;
    unsigned ink = 0;
    for (unsigned y = 0; y < bm->rows; y++) {
        const unsigned char *row = bm->buffer + (size_t)y * bm->pitch;
        for (unsigned x = 0; x < bm->width; x++)
            if (row[x]) ink++;
    }

    printf("ftfont: glyph A %ux%u ink=%u\n", bm->width, bm->rows, ink);

    int ok = bm->rows > 0 && bm->width > 0 && ink > 0;
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    if (!ok) { printf("ftfont: FAIL empty bitmap\n"); return 1; }
    printf("ftfont: PASS\n");
    return 0;
}
