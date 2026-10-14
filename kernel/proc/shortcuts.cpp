// Desktop entries (.desktop) scanned from the filesystem.
//
// The window manager used to own a hardcoded array of three icons and a switch
// statement of what clicking them did. That made it impossible for a program -
// or the FAT volume, or the initrd - to add a shortcut, so the shortcut list
// became data: files under `desktop/` in the freedesktop.org Desktop Entry
// format, which real Linux software already writes.
#include <choco/shortcuts.hpp>

#include <choco/user.hpp>

namespace choco::shortcuts {
namespace {

constexpr size_t MAX_ENTRIES = 24;
constexpr size_t MAX_ARGS = 8;
constexpr size_t MAX_LINE = 200;
constexpr size_t MAX_FILE = 4096;
constexpr const char *kDir = "desktop/";
constexpr const char *kSuffix = ".desktop";

Entry s_entries[MAX_ENTRIES]{};
size_t s_count = 0;
uint64_t s_stamp = 0;   // signature of the desktop folder, to skip idle rescans

VerbHook s_verb = nullptr;
OpenHook s_open = nullptr;

size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
char low(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (low(*a) != low(*b)) return false;
    return !*a && !*b;
}
bool ci_prefix(const char *s, const char *pre) {
    for (; *pre; s++, pre++)
        if (low(*s) != low(*pre)) return false;
    return true;
}
// Exact key match: Desktop Entry keys are case-sensitive, and a locale suffix
// ("Name[pt_BR]") must never be taken for the plain key.
bool is_key(const char *k, const char *want) {
    for (; *k && *want; k++, want++)
        if (*k != *want) return false;
    return !*k && !*want;
}
bool ends_ci(const char *s, const char *suffix) {
    const size_t ls = slen(s), lf = slen(suffix);
    if (ls < lf) return false;
    const char *tail = s + ls - lf;
    for (size_t i = 0; i < lf; i++)
        if (low(tail[i]) != low(suffix[i])) return false;
    return true;
}
bool truthy(const char *v) { return ci_equal(v, "true") || v[0] == '1'; }

void copy_field(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

void trim(char *s) {
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) {
        size_t j = 0;
        for (; s[i]; i++, j++) s[j] = s[i];
        s[j] = 0;
    }
    size_t n = slen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

// Only the root `desktop/` is the desktop, like $XDG_DESKTOP_DIR: one folder.
bool under_desktop(const char *path) {
    return ci_prefix(path, kDir) && path[slen(kDir)] != 0;
}

bool path_exists(const char *path) {
    if (!path[0]) return false;
    const char *p = path[0] == '/' ? path + 1 : path;
    const size_t want = slen(p);
    const size_t total = fs::count();
    for (size_t i = 0; i < total; i++) {
        const char *nm = nullptr;
        size_t sz = 0;
        if (!fs::stat(i, &nm, &sz) || !nm) continue;
        if (slen(nm) != want) continue;
        size_t k = 0;
        while (k < want && nm[k] == p[k]) k++;
        if (k == want) return true;
    }
    return false;
}

uint32_t parse_show_in(const char *v) {
    uint32_t mask = 0;
    const char *p = v;
    while (*p) {
        char tok[16];
        size_t n = 0;
        while (p[n] && p[n] != ';' && n + 1 < sizeof(tok)) { tok[n] = p[n]; n++; }
        tok[n] = 0;
        p += n;
        while (*p == ';') p++;
        if (ci_equal(tok, "Desktop")) mask |= kShowDesktop;
        else if (ci_equal(tok, "StartMenu")) mask |= kShowMenu;
        else if (ci_equal(tok, "Taskbar")) mask |= kShowTaskbar;
    }
    return mask;
}

// Drop the Desktop Entry field codes (%f, %U, %i, ...): nothing here passes a
// selection to an app, and leaving them would end up in argv.
void strip_codes(char *s) {
    char *o = s;
    for (; *s;) {
        if (*s == '%' && s[1] &&
            (s[1] == 'f' || s[1] == 'F' || s[1] == 'u' || s[1] == 'U' || s[1] == 'i' ||
             s[1] == 'c' || s[1] == 'k' || s[1] == 'v' || s[1] == 'm')) {
            s += 2;
            continue;
        }
        *o++ = *s++;
    }
    *o = 0;
    // Codes left runs of two spaces behind.
    o = s;
    char *r = s;
    while (*r) {
        if (r[0] == ' ' && r[1] == ' ') { r++; continue; }
        *o++ = *r++;
    }
    *o = 0;
    trim(s);
}

void parse_one(const char *path, const char *data, size_t size, Entry &e) {
    e = Entry{};
    copy_field(e.file, sizeof(e.file), path);
    e.show_in = kShowEverywhere;

    // Fallback name: the base of the path without the suffix, the way a file
    // manager labels an entry with no Name=.
    const char *base = path;
    for (const char *q = path; *q; q++)
        if (*q == '/') base = q + 1;
    char stem[40];
    copy_field(stem, sizeof(stem), base);
    if (ends_ci(stem, kSuffix)) stem[slen(stem) - slen(kSuffix)] = 0;
    copy_field(e.name, sizeof(e.name), stem);

    size_t pos = 0;
    char line[MAX_LINE];
    while (pos < size) {
        size_t n = 0;
        while (pos < size && data[pos] != '\n') {
            if (n + 1 < sizeof(line)) line[n++] = data[pos];
            pos++;
        }
        if (pos < size) pos++;
        line[n] = 0;
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == '[') continue;
        char *eq = line;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        *eq++ = 0;
        trim(eq);
        const char *key = line;
        const char *val = eq;

        if (is_key(key, "Name")) {
            if (val[0]) copy_field(e.name, sizeof(e.name), val);
        } else if (is_key(key, "Comment")) {
            copy_field(e.comment, sizeof(e.comment), val);
        } else if (is_key(key, "Icon")) {
            copy_field(e.icon, sizeof(e.icon), val);
            if (ends_ci(e.icon, ".png")) e.icon[slen(e.icon) - 4] = 0;
        } else if (is_key(key, "Type")) {
            e.kind = ci_equal(val, "Link") ? Kind::Link : Kind::Application;
        } else if (is_key(key, "Exec")) {
            copy_field(e.exec, sizeof(e.exec), val);
            strip_codes(e.exec);
        } else if (is_key(key, "URL")) {
            copy_field(e.exec, sizeof(e.exec), val);
            e.kind = Kind::Link;
        } else if (is_key(key, "Terminal")) {
            e.terminal = truthy(val);
        } else if (is_key(key, "Hidden") || is_key(key, "NoDisplay")) {
            if (truthy(val)) e.show_in = 0;
        } else if (is_key(key, "OnlyShowIn")) {
            const uint32_t m = parse_show_in(val);
            if (m) e.show_in = m;
        } else if (is_key(key, "TryExec")) {
            if (val[0] && !path_exists(val)) e.show_in = 0;
        }
    }
}

void resolve(Entry &e) {
    if (e.kind == Kind::Link) {
        e.valid = e.exec[0] && (path_exists(e.exec) || fs::writable_is_dir(e.exec));
        return;
    }
    if (ci_prefix(e.exec, "choco://")) { e.valid = true; return; }
    char prog[80];
    const char *src = e.exec;
    const bool quoted = src[0] == '"';
    if (quoted) src++;
    size_t i = 0;
    while (src[i] && src[i] != (quoted ? '"' : ' ') && i + 1 < sizeof(prog)) {
        prog[i] = src[i];
        i++;
    }
    prog[i] = 0;
    e.valid = prog[0] && path_exists(prog);
}

int name_cmp(const Entry &a, const Entry &b) {
    for (size_t i = 0;; i++) {
        const char ca = low(a.name[i]), cb = low(b.name[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
}

// Cheap invalidation: fs::count() adds up slot counters, it does not touch any
// file. Deliberately not using fs::stat() here - procfs regenerates a virtual
// file's buffer on every stat() call, so a per-tick sweep of the index would
// rewrite /proc content under a running sysmon. Creating, deleting or renaming
// a shortcut changes the count and triggers a rescan; rewriting one with
// exactly the same length does not, which is the documented trade-off.
uint64_t signature() { return fs::count(); }

void sort_entries() {
    for (size_t i = 1; i < s_count; i++) {
        Entry key = s_entries[i];
        size_t j = i;
        while (j && name_cmp(s_entries[j - 1], key) > 0) {
            s_entries[j] = s_entries[j - 1];
            j--;
        }
        s_entries[j] = key;
    }
}

// Split the command line into argv in place. Quotes group arguments, which is
// what the spec's quoting rules do and what a path with a space needs.
size_t split_args(char *buf, const char **argv, size_t max_args) {
    size_t argc = 0;
    char *p = buf;
    while (*p && argc < max_args) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *start = p;
        if (*p == '"') {
            p++;
            char *o = p;
            while (*p && *p != '"') *o++ = *p++;
            *o = 0;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ') p++;
            if (*p == ' ') *p++ = 0;
        }
        argv[argc++] = start;
    }
    return argc;
}

} // namespace

void set_hooks(VerbHook v, OpenHook o) {
    s_verb = v;
    s_open = o;
}

void rescan() {
    const uint64_t stamp = signature();
    if (stamp == s_stamp) return;
    s_stamp = stamp;

    Entry tmp[MAX_ENTRIES];
    size_t n = 0;
    const size_t total = fs::count();
    for (size_t i = 0; i < total && n < MAX_ENTRIES; i++) {
        const char *nm = nullptr;
        size_t sz = 0;
        if (!fs::stat(i, &nm, &sz)) continue;
        if (!nm || !under_desktop(nm) || !ends_ci(nm, kSuffix)) continue;
        const void *data = nullptr;
        size_t size = 0;
        if (!fs::read(nm, &data, &size) || !data) continue;
        if (size > MAX_FILE) size = MAX_FILE;
        parse_one(nm, (const char *)data, size, tmp[n]);
        if (!tmp[n].show_in) continue;
        resolve(tmp[n]);
        n++;
    }
    // Publish only a complete list: a half-parsed file must not leave the
    // desktop showing a truncated row.
    for (size_t i = 0; i < n; i++) s_entries[i] = tmp[i];
    s_count = n;
    sort_entries();
    for (size_t i = 0; i < s_count; i++) s_entries[i].order = (int)i;
}

size_t count() { return s_count; }

const Entry *at(size_t i) { return i < s_count ? &s_entries[i] : nullptr; }

size_t count_showing(uint32_t where) {
    size_t k = 0;
    for (size_t i = 0; i < s_count; i++)
        if (s_entries[i].show_in & where) k++;
    return k;
}

const Entry *at_showing(size_t i, uint32_t where) {
    size_t k = 0;
    for (size_t j = 0; j < s_count; j++) {
        if (!(s_entries[j].show_in & where)) continue;
        if (k == i) return &s_entries[j];
        k++;
    }
    return nullptr;
}

long find(const char *name) {
    for (size_t i = 0; i < s_count; i++)
        if (ci_equal(s_entries[i].name, name)) return (long)i;
    return -1;
}

const char *dir() { return kDir; }

bool activate(const Entry *e) {
    // Only pointers that are still in the live table may launch: an Entry copy
    // held across a rescan could name a file that no longer exists.
    if (!e || e < s_entries || e >= s_entries + s_count) return false;
    if (!e->valid) return false;
    if (e->kind == Kind::Link) return s_open && s_open(e->exec);
    if (ci_prefix(e->exec, "choco://")) return s_verb && s_verb(e->exec + 8);

    char buf[80];
    copy_field(buf, sizeof(buf), e->exec);
    const char *argv[MAX_ARGS];
    const size_t argc = split_args(buf, argv, MAX_ARGS);
    if (!argc) return false;
    if (e->terminal && s_verb) s_verb("terminal");
    // Non-blocking: several shortcuts can be launched and co-exist, and the
    // click that launched this one returns to the WM immediately.
    return proc::spawn_job(argv[0], (int)argc - 1, (char **)argv + 1, nullptr) >= 0;
}

bool activate_at(size_t i) { return activate(i < s_count ? &s_entries[i] : nullptr); }

} // namespace choco::shortcuts
