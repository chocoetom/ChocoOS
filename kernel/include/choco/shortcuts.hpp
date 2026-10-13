#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::shortcuts {

// Desktop entries, the freedesktop.org "Desktop Entry" subset (.desktop files).
// Shortcuts are *data on the filesystem*, not code: anything that can write a
// file (a user program through open/write, the FAT volume mounted at /disk, the
// initrd baked at build time) can put an icon on the desktop. The window manager
// only draws what this module reports and calls activate() on it.
enum class Kind : uint8_t { Application, Link };

// Where an entry is allowed to appear. Standard .desktop files carry no notion
// of "the desktop", so ChocoOS reads the `OnlyShowIn=` key against these three
// tokens (semicolon-separated, like the spec). No OnlyShowIn = all three.
enum : uint32_t {
    kShowDesktop = 1u << 0,
    kShowMenu = 1u << 1,
    kShowTaskbar = 1u << 2,
    kShowEverywhere = kShowDesktop | kShowMenu | kShowTaskbar,
};

struct Entry {
    char file[64];   // path in the FS, e.g. "desktop/files.desktop"
    char name[40];   // Name=
    char comment[48]; // Comment= (status bar / tooltip)
    char icon[32];   // Icon= without extension
    char exec[80];   // Exec= program + args, or a choco:// verb, or URL= for Link
    Kind kind = Kind::Application;
    bool terminal = false;  // Terminal=true: run it inside the terminal window
    bool valid = false;     // the target exists (TryExec/Exec/URL resolved)
    uint32_t show_in = kShowEverywhere;
    int order = 0;          // stable sort key (name), assigned by rescan()
};

// Re-read the desktop directory. Cheap enough to call from the paint loop, but
// it only walks the filesystem when something actually changed.
void rescan();

size_t count();
const Entry *at(size_t i);
// The list as one surface should show it: same order, filtered by kShow*.
size_t count_showing(uint32_t where);
const Entry *at_showing(size_t i, uint32_t where);
// -1 when no entry matches. Used by the shell's `shortcut` command.
long find(const char *name);

// The window manager owns the screen, so instead of depending on it this module
// calls back through hooks it is given at init: `verb` handles choco:// targets
// ("terminal", "files") and `open` opens a file or folder.
using VerbHook = bool (*)(const char *verb);
using OpenHook = bool (*)(const char *path);
void set_hooks(VerbHook verb, OpenHook open);

// Run the entry: choco:// verbs open kernel windows, links go to the open hook,
// everything else is launched through the process loader. False if invalid.
// A pointer from at()/at_showing() is preferred: the surfaces show filtered
// views of the table, so a filtered index would not identify the entry.
bool activate(const Entry *e);
bool activate_at(size_t i);

// The directory scanned, for diagnostics.
const char *dir();

} // namespace choco::shortcuts
