// /bin/uwm - first display-server client (ring 3).
// Draws into a shared buffer, notifies with present(), reads input from
// the per-window queue. Click inside or press a key to quit.
#include "lib.h"

#define W 320
#define H 200

// The shared buffer is allocated at the server's capacity stride (640), not at
// the visible width, so always index rows by the pitch the header reports.
static int g_pitch = W;

static void rect(unsigned *px, int x0, int y0, int x1, int y1, unsigned c) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            px[y * g_pitch + x] = c;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    long addr = choco_win_create(W, H, "uwm demo");
    if (addr <= 0) {
        choco_print("uwm: win_create failed\n");
        return 1;
    }
    struct uw_win *win = (struct uw_win *)addr;
    unsigned *px = (unsigned *)(addr + UW_PIX_OFF);
    if (win->pitch >= W) g_pitch = (int)win->pitch;

    int bx = 20, by = 30, vx = 3, vy = 2;
    for (int frame = 0; frame < 400; frame++) {
        // gradient background
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                unsigned r = (unsigned)(x * 255 / W);
                unsigned g = (unsigned)(y * 255 / H);
                px[y * g_pitch + x] = (r << 16) | (g << 8) | 0x80;
            }
        // bouncing white box with purple border
        rect(px, bx, by, bx + 60, by + 44, 0x9D4EDD);
        rect(px, bx + 4, by + 4, bx + 56, by + 40, 0xFFFFFF);
        rect(px, bx + 8, by + 8, bx + 52, by + 36, 0x1E1332);
        bx += vx;
        by += vy;
        if (bx <= 0 || bx + 60 >= W) vx = -vx;
        if (by <= 0 || by + 44 >= H) vy = -vy;
        choco_win_present(addr);

        // drain the event queue: click or key quits
        while (win->tail != win->head) {
            struct uw_ev e = win->ev[win->tail % 32];
            win->tail++;
            if (e.type == UEV_MOUSE || e.type == UEV_KEY) {
                choco_win_close(addr);
                choco_print("uwm: bye (input received)\n");
                return 0;
            }
        }
        choco_sleep(50);
    }
    choco_win_close(addr);
    choco_print("uwm: bye (timeout)\n");
    return 0;
}
