// /bin/lnk - create a desktop shortcut from userland.
//
// The desktop is data: `lnk` writes a freedesktop .desktop file into desktop/,
// which the window manager rescans on its own. Anything able to write a file
// can put an icon on the desktop - that ability is the whole design, so this
// program exists mainly to prove it.
//
//   lnk <Name> <exec-or-path> [icon] [-l] [-t]
//     -l  Type=Link (second argument is a path, not a command)
//     -t  Terminal=true (run the command inside the terminal window)
#include "lib.h"

static char path[160];
static char body[512];
static unsigned blen;

static void put(const char *s) {
    for (unsigned i = 0; s[i] && blen + 1 < sizeof(body); i++) body[blen++] = s[i];
    body[blen] = 0;
}

static void put_kv(const char *key, const char *val) {
    put(key);
    put(val);
    put("\n");
}

// desktop/my-shortcut.desktop: lowercase, anything else collapses to one dash.
static void set_path(const char *name) {
    unsigned n = 0;
    const char *pre = "desktop/";
    for (unsigned i = 0; pre[i]; i++) path[n++] = pre[i];
    int prev_dash = 1;
    for (unsigned i = 0; name[i] && n + 16 < sizeof(path); i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        const int alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum) {
            if (prev_dash) continue;
            path[n++] = '-';
            prev_dash = 1;
            continue;
        }
        path[n++] = c;
        prev_dash = 0;
    }
    while (n > 8 && path[n - 1] == '-') n--;
    if (n <= 8) return;  // name was all punctuation: refuse rather than collide
    const char *suf = ".desktop";
    for (unsigned i = 0; suf[i]; i++) path[n++] = suf[i];
    path[n] = 0;
}

int main(int argc, char **argv) {
    int link = 0, term = 0, pos = 0;
    const char *arg[3] = {0, 0, 0};
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'l') link = 1;
        else if (argv[i][0] == '-' && argv[i][1] == 't') term = 1;
        else if (pos < 3) arg[pos++] = argv[i];
    }
    if (pos < 2) {
        choco_print("usage: lnk <Name> <exec-or-path> [icon] [-l] [-t]\n");
        return 2;
    }
    const char *icon = pos > 2 ? arg[2] : (link ? "folder-icon" : "basic-file");

    set_path(arg[0]);
    if (!path[8]) {
        choco_print("lnk: name has no letters or digits\n");
        return 2;
    }
    blen = 0;
    body[0] = 0;
    put("[Desktop Entry]\n");
    put_kv("Type=", link ? "Link" : "Application");
    put_kv("Name=", arg[0]);
    put_kv(link ? "URL=" : "Exec=", arg[1]);
    put_kv("Icon=", icon);
    if (term) put("Terminal=true\n");

    // O_CREAT | O_WRONLY
    const long fd = choco_open_flags(path, 0x40 | 0x01);
    if (fd < 0) {
        choco_print("lnk: cannot create ");
        choco_print(path);
        choco_print("\n");
        return 1;
    }
    if (choco_write((int)fd, body, blen) < 0) {
        choco_print("lnk: write failed\n");
        choco_close((int)fd);
        return 1;
    }
    choco_close((int)fd);
    choco_print("shortcut created: ");
    choco_print(path);
    choco_print("  (");
    choco_print(arg[0]);
    choco_print(")\n");
    return 0;
}
