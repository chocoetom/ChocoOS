# ChocoOS 🐈‍⬛

Hobby OS bare-metal em **C++20 freestanding**, **x86_64 UEFI**, bootloader **Limine**.
Mascote: **Choco, o gato preto**.

Estado atual: **Fase 15 — TTY Linux parcial** — tradutor Linux x86_64, musl estático, BusyBox estático e `ioctl` básico de terminal verificados E2E no QEMU. Fase 15 e o ramdisk FAT32 (`/disk`) com o File Explorer navegável estão verificados E2E (`scripts/test-fat.sh`, `scripts/test-fileman.sh`). As janelas — do terminal e dos apps — agora são **redimensionáveis** (arrastar borda/canto, maximizar, `win_resize`), com o arrasto real do mouse coberto por teste headless (`scripts/test-resize.py`). Ver `docs/ROADMAP.md`.

![foto do Choco aqui](docs/choco.jpg)

## Stack escolhida (suas respostas)
- Tipo: OS bare-metal completo
- Arch: x86_64 UEFI + Limine v12.9.3
- Dev: Windows nativo (MSYS2 UCRT64)
- Build: CMake + Ninja + NASM
- Runtime: freestanding puro (sem STL/exceptions/RTTI)
- MVP: Hello World Kernel + CLI texto (próximo passo)
- Licença: MIT

## Pré-requisitos (só Python instalado aqui)
Você está sem `cmake/gcc/qemu`. Rode **uma vez** no PowerShell:

```powershell
.\scripts\setup-windows.ps1
```

Isso instala MSYS2 via winget + `gcc/clang/cmake/ninja/nasm/qemu/xorriso/mtools/git`.

## Build + Run (no MSYS2 UCRT64)

```bash
cd /d/Downloads/ChocoOS   # ajuste se seu caminho for outro
bash scripts/build.sh     # baixa limine, compila kernel, gera build/ChocoOS.iso
bash scripts/run.sh       # abre QEMU
bash scripts/smoke.sh     # teste headless: boota e confere 8 marcos no serial
```

### Build sem downloads

Se as dependências e o Limine já estiverem instalados localmente, use no
MSYS2 UCRT64:

```bash
cd /d/Downloads/ChocoOS
bash scripts/build-local.sh
bash scripts/run.sh
```

Esse script não usa `winget`, `pacman`, `curl` nem baixa o Limine. Ele exige
as ferramentas no `PATH` e os arquivos locais em `build/limine/`.

No PowerShell, os atalhos equivalentes são:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\scripts\build-local.ps1
.\scripts\run-local.ps1
```

Esperado: tela azul-noite `1A1A2E`, barra `ChocoOS 0.5`, e o prompt:
```
choco> help
commands:
  help                 show this list
  ...
```

## Estrutura
```
CMakeLists.txt            # build do kernel + download limine.h
cmake/toolchain-x86_64-elf.cmake
kernel/boot.asm           # _start -> kmain
kernel/linker.ld          # higher-half 0xffffffff80000000
kernel/main.cpp           # boot sequence: mem -> ints -> sched -> shell
kernel/console.cpp        # framebuffer console, 8x8 font, kprintf, status bar
kernel/shell.cpp          # line editing, history, 9 built-in commands
kernel/cpu/               # gdt, idt/isr, pic, pit, keyboard, mouse, sched
kernel/mem/               # pmm bitmap, 4-level paging, heap + new/delete
kernel/proc/              # elf, fs (initrd+tmpfs+/disk), procfs, vfs, fat32 (real), linux, syscall, pci, virtio(_net)
kernel/gui/               # wm, wallpaper, icons (alpha blit)
icons/                    # PNGs 24px + backgroundN.jpg (vira kernel/gen/*.inc)
user/                     # hello/uls/ucat/umon/uwm/fileman/sysmon + mhello(musl)/linux-asm ring-3 + lib.h + root/ (initrd)
user/lib/                 # gui.h (desenho no shm), fs_client.h (listagem), font_ui.h + icons_ui.h (gerados)
assets/fonts/             # Inter.ttf (SIL OFL) - origem da fonte proporcional do UI
boot/limine.cfg           # entradas Limine (initrd.tar + fat.img como /disk)
scripts/setup-windows.ps1 # instala tudo no Windows
scripts/build.sh          # cmake + user + fatdisk + iso
scripts/make-fatdisk.sh   # gera build/fat.img (FAT32 real via mformat/mmd/mcopy)
scripts/build-font.py     # horneia Inter -> user/lib/font_ui.h + kernel/gen/uifont.inc
scripts/make-iso.sh       # xorriso BIOS+UEFI
scripts/run.sh            # qemu
scripts/test-fat.sh       # E2E headless do /disk (boot + teclas no monitor + serial)
scripts/test-fileman.sh   # smoke headless da GUI do File Explorer (screendump)
scripts/test-resize.py    # E2E: arrasta a borda do Explorer com mouse real (QMP)
scripts/host-env.sh       # PATH do toolchain + guarda contra artefatos velhos
docs/ROADMAP.md           # fases 0..7 até userland v2
```

## Roadmap (resumo)
0. Hello Kernel ✅
1. Memória: memmap, PMM, paging, kmalloc ✅
2. Interrupções: IDT, PIC, timer, teclado PS/2 ✅
3. Multitarefa: scheduler round-robin ✅
4. Shell CLI: `help/clear/mem/choco` ✅
5. User-mode + syscalls + FS ✅ (ring 3, `run bin/hello`, initrd)
6. GUI framebuffer (janelas) ✅ (desktop, mouse, taskbar)
7. Userland v2 ✅ (endereço privado, argv, 13 syscalls, `uls`/`ucat` em ring 3)
8. Desktop completo ✅ (Win11 roxo: taskbar, Start, ícones, Files, minimize)
9. Tema e polimento ✅ (theme.hpp, sombras, títulos, wallpaper padrão)
10. /proc + apps fora do kernel ✅ (procfs, `umon` em ring 3, zero syscall nova)
11. Display server ✅ (shm + present + eventos, `uwm`, janelas de usuário)
12. Linux, primeira versão ✅ (tradutor v1 + musl + BusyBox estático)

Extra (fora das fases): **/disk FAT32 real** ✅ (driver de leitura com BPB, cluster
chains, 8.3 + LFN; ramdisk gerado no build e montado pela camada `fs::`) e
**File Explorer navegável** ✅ (`bin/fileman`: toolbar, sidebar, lista, viewer de
texto e criar/renomear/deletar no FS gravável).

Detalhes em `docs/ROADMAP.md`.

## C++ freestanding — regras
- Sem `new/delete` (vamos implementar no roadmap fase 1).
- Sem `std::vector/string/iostream`, sem exceptions/RTTI (`-fno-exceptions -fno-rtti`).
- Só `<stdint.h>`, `<stddef.h>` e seu próprio código.
- `kmain()` é `extern "C"` chamado do ASM.

## Debug
- QEMU `-serial stdio`: futuro `printf serial` vai aqui.
- Se tela preta: confira `build/boot/chocoos.elf` existe e `limine.cfg` em `iso_root/`.
- UEFI real precisa de `edk2-ovmf` no MSYS2: `pacman -S mingw-w64-ucrt-x86_64-edk2-ovmf`.

MIT — faça bom uso, e dê um pet no Choco. 🐾
