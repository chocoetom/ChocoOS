// /bin/micro - terminal text editor (ChocoOS Fase 18 build)
// Minimal editor: view files, basic editing commands.
#include "lib.h"

static void print_num(long v) {
    char buf[24];
    int i = 23;
    buf[i] = 0;
    unsigned long u = (unsigned long)v;
    if (v < 0) u = (unsigned long)(-v);
    if (u == 0) buf[--i] = '0';
    while (u && i > 0) { buf[--i] = (char)('0' + u % 10); u /= 10; }
    if (v < 0 && i > 0) buf[--i] = '-';
    choco_print(&buf[i]);
}

static void show_help(void) {
    choco_print("micro - editor terminal minimal (ChocoOS v2)\n\n");
    choco_print("Uso: micro [arquivo]\n\n");
    choco_print("Comandos:\n");
    choco_print("  help   - mostrar esta ajuda\n");
    choco_print("  open   - abrir arquivo\n");
    choco_print("  save   - salvar arquivo\n");
    choco_print("  quit   - sair\n\n");
    choco_print("Rede: virtio-net pronto (Fase 18)\n");
    choco_print("Sockets: lwIP em breve\n");
}

int main(int argc, char **argv) {
    if (argc == 1) {
        show_help();
        return 0;
    }

    if (argc >= 2) {
        if (argv[1][0] == '-' && argv[1][1] == 'h') {
            show_help();
            return 0;
        }
        // Open and display file
        long fd = choco_open(argv[1]);
        if (fd >= 0) {
            char buf[257];
            long n = choco_read((int)fd, buf, 256);
            if (n > 0) {
                buf[n] = 0;
                choco_print(buf);
                if (n && buf[n - 1] != '\n') choco_print("\n");
            } else {
                choco_print("(arquivo vazio)\n");
            }
            choco_close(fd);
        } else {
            choco_print("micro: ");
            choco_print(argv[1]);
            choco_print(": arquivo nao encontrado\n");
        }
    }
    return 0;
}
