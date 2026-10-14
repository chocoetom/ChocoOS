# ChocoOS Roadmap — do Hello ao Shell

## Fase 0 — Hello Kernel ✅
- [x] Limine boot x86_64, higher-half
- [x] Framebuffer console + fonte 8x8 + tema Choco
- Critério: ISO boota no QEMU UEFI e mostra o gato.

## Fase 1 — Memória ✅ (v0.2 neste commit)
- [x] Ler memmap Limine (HHDM + memmap + kernel_address)
- [x] PMM: bitmap de frames 4KiB (`kernel/mem/pmm.cpp`)
- [x] Paging 4-níveis próprio + troca de CR3 (`kernel/mem/paging.cpp`)
- [x] Heap `kmalloc/kfree/realloc` + `new/delete` (`kernel/mem/heap.cpp`)
- [x] Self-test no boot (PMM alloc, translate do FB, heap alloc + new)
- Critério: tela mostra `[pmm]`, `[paging]`, `[heap] self-test OK` + resumo.

## Fase 2 — Interrupções + drivers base ✅ (v0.3 neste commit)
- [x] IDT 48 vetores + stubs NASM + `isr_dispatch` (`kernel/cpu/isr.asm`, `kernel/cpu/idt.cpp`)
- [x] PIC 8259 remap 32/40 + EOI (`kernel/cpu/pic.cpp`)
- [x] PIT 100Hz + contador de ticks/uptime (`kernel/cpu/pit.cpp`)
- [x] Teclado PS/2 set-1 → ASCII + buffer circular (`kernel/cpu/keyboard.cpp`)
- [x] Loop interativo: eco, backspace, barra de status com uptime/ticks
- Critério: tecla aparece na tela, timer incrementa — visível no QEMU.

## Fase 3 — Multitarefa ✅ (v0.4 neste commit)
- [x] GDT própria + TSS (rsp0, IST1 p/ double fault) (`kernel/cpu/gdt.cpp`, `gdt.asm`)
- [x] Troca de contexto callee-saved + trampolim (`kernel/cpu/switch.asm`)
- [x] Round-robin preemptivo via PIT (quantum 50ms, troca dentro do IRQ) (`kernel/cpu/sched.cpp`)
- [x] Demo: 2 contadores paralelos + prova no boot (ambos avançam)
- Critério: 2 tarefas contam em paralelo, `switches` cresce na status bar.

## Fase 4 — Shell CLI ✅ (v0.5 neste commit)
- [x] Linha editável + histórico 16 entradas (Up/Down) (`kernel/shell.cpp`)
- [x] Comandos: help, clear, mem, uptime, threads, echo, sleep, choco, reboot
- [x] Atalhos: Ctrl+L limpa, Ctrl+C cancela, Ctrl+U apaga a linha
- [x] Teclado estendido: setas via prefixo E0 + Ctrl+letra (`kernel/cpu/keyboard.cpp`)
- Critério: digitar `mem`, `uptime`, `choco` responde sem reboot.

## Fase 5 — User-mode + FS ✅ (v0.6, verificado E2E no QEMU)
- [x] Ring 3 via `iretq` (GDT user segs + TSS rsp0) + gate 0x80 DPL3
- [x] Syscalls: exit/write/read/sleep/ticks/yield, com validação de ponteiros
- [x] ELF64 loader mínimo (PT_LOAD → páginas USER, sem remapear página compartilhada)
- [x] initrd USTAR via módulo Limine + `ls/cat/run`; `run bin/hello` interativo
- [x] `user/hello.c` (write/read/sleep/exit) compilado pelo `scripts/build-user.sh`
- Critério: `/bin/hello` roda em user-mode, fala seu nome e sai com código 0. ✅ (transcrição em `build/serial.log` dos testes)

## Fase 6 — GUI ✅ (v0.7, verificado no QEMU)
- [x] Desktop: wallpaper (foto via `docs/choco.jpg` ou noite procedural + gato), taskbar com relógio
- [x] Assets de `icons/`: 3 landscapes selecionáveis (`wallpaper 1..3`), 21 ícones 24px com alpha (taskbar, botões X)
- [x] Terminal em janela (console retargetado) + janela sysmon (1 Hz)
- [x] Mouse PS/2 (IRQ12, 8042, cascade): cursor, foco, arrastar, X fecha/limpa, right-click alterna sysmon
- [x] Comandos `gui`, `mpos`, `wintest` (drag determinístico: PASS)
- Critério: shell em janela + `wintest: PASS` + screenshot do desktop.

## Fase 7 — Userland v2 ✅ (neste commit, verificado E2E no QEMU)- [x] Endereço privado por processo: PML4 próprio (metade alta clonada), CR3 por thread, páginas liberadas no exit (`paging::create/destroy_user_root`, `sched::spawn_user`)
- [x] `argc/argv` na pilha do usuário (`_start` passa `main(argc, argv)`; `run <path> [args...]`)
- [x] Syscalls 6→13: `open/close/read` no initrd, `sbrk` (heap por processo), `lseek/fsize`, `fscount/fsstat` (listar)
- [x] Programas de usuário: `bin/hello` (args+heap+arquivo), `bin/uls` (ls em ring 3), `bin/ucat` (cat em ring 3)
- [x] Runtime freestanding do kernel (`memset/memcpy/memmove/memcmp` em `kernel/mem/rt.cpp`)
- Bugs reais: `translate()` andava no PML4 do boot (todo ponteiro de usuário falhava); kstacks de `spawn_user` colidiam com a base do heap (sobrescrevia PTEs do initrd); array `argv` sobrepunha a última string
- Critério: `run bin/hello foo bar` + `run bin/uls` + `run bin/ucat motd.txt` + repetição, tudo exit 0 com FS íntegro.

## Fase 8 — Desktop completo ✅ (neste commit, verificado com screenshots no QEMU)
- [x] Tema roxo estilo Win11: taskbar centralizada 44px, títulos em roxo, cantos arredondados, relógio HH:MM:SS + wifi no tray
- [x] Botão Start + menu (busca decorativa, 5 apps fixados, barra usuário + reboot funcional via 8042)
- [x] Ícones no desktop (Terminal/Files/Monitor, clique seleciona, duplo-clique abre)
- [x] Janela Files: lista o initrd com ícones, duplo-clique abre txt / executa ELF
- [x] Minimizar/restaurar (botão — + toggle na taskbar com sublinhado de foco)
- [x] Pins do menu funcionais: Terminal, Files, Monitor (toggle), Wallpaper (cicla), Hello (`run bin/hello`)
- Bugs reais: botões da taskbar indexavam `s_win[i-1]` ( Files abria sysmon); relógio sem mod 60; `mclick` sem `gui::tick` perdia a borda do clique
- Critério: screenshots (desktop, menu, files, minimize/restore, wallpaper 2) + `wintest: PASS` + reboot sai do QEMU.

## Fase 9 — Tema e polimento ✅ (neste commit, verificado com screenshots no QEMU)
- [x] `kernel/include/choco/theme.hpp`: paleta e métricas num lugar só (base do futuro tema claro)
- [x] Sombras suaves sob janelas e menu (escurece o que já foi pintado, com halo)
- [x] Barra de título refeita: ícone do app + texto escala 1 centralizado + botões com padding do tema
- [x] Ícones do desktop em grade uniforme, labels com sombra para contraste em qualquer wallpaper
- [x] `background4.png` é o wallpaper padrão; `background*` excluído dos ícones (antes inchava o binário)
- [x] Cantos via snapshot do fundo (compõe certo sobre janelas empilhadas, sem pixels soltos) + taskbar com cantos superiores arredondados
- [x] Console do terminal em escala 1 (denso, moderno); mensagens de boot curtas, sem spam de depuração
- [x] Start com logo à esquerda (acento roxo), apps recentralizados
- Critério: screenshots com o novo wallpaper + `wintest: PASS` + `smoke.sh` 8/8.

## Fase 10 — /proc + apps fora do kernel ✅ (neste commit, verificado E2E no QEMU)
- [x] `kernel/proc/procfs.cpp`: `proc/version`, `proc/meminfo`, `proc/uptime`, `proc/threads` gerados sob demanda em buffers estáticos
- [x] Servidos pelo `fs::read/stat/count` existente: **zero syscall nova**; `ls`, `cat`, `open/read/lseek`, `uls`, `ucat` enxergam sem mudar nada
- [x] `user/umon.c`: primeiro app-monitor fora do kernel (só open/read/close)
- [x] `sched::stat(i)` para introspecção de threads (nome, runs, atual)
- Critério: `run bin/umon` mostra as 4 seções + exit 0; `cat proc/version` no shell.

## Fase 11 — Display server ✅ (neste commit, verificado E2E no QEMU)
- [x] Syscalls 13–15: `win_create(w,h,title)` → endereço compartilhado; `win_present`, `win_close`
- [x] App desenha em shm (kernel mapeia os mesmos frames no espaço do processo em `0x800000+`; kernel lê via HHDM) e avisa com `present()`; input volta em anel de 32 eventos por janela (mouse + tecla)
- [x] WM trata janelas de usuário como de primeira classe: chrome, foco, drag, minimize, z-order; tecla vai pra janela focada (shell e waiter roteiam)
- [x] `user/bin/uwm`: demo animado (sai no clique/tecla/timeout); reap no exit limpa tudo (reuso provado em 3 runs seguidos)
- [x] Compositor mais rápido: sombra só nas faixas expostas, halo em banda pré-computada, `memcpy` em linha cheia, throttle ~10fps (sem o throttle o TCG rasgava o shot no meio do `paint`)
- [x] Quick wins de fluidez (2026-10-13): o movimento do mouse **não** força mais repaint de tela cheia — o cursor usa save-under e só a caixa dele é copiada (`present()` parcial); os contadores de boot (`counter-A/B`) não giram mais em busy-spin; `X` **encerra** o programa foreground (`proc::kill_foreground`), então fechar uma janela não bloqueia mais os próximos lançamentos; a arena de VA das janelas é **por slot** (não vaza mais ~1,2 MiB por janela, que travava após ~6 aberturas); `title`/`prog` são zerados ao reusar a célula. Teste headless `scripts/test-cursor-trail.py` (0 pixels de rastro).
- Limites honestos da v1: sem slot na taskbar, máx 4 janelas 640x480, recomposição ainda é full (dirty-rect no *present*, não na composição). `wintest` "edge resize" regride (pré-existente, independente dos quick wins).
- Critério: screenshot da janela `uwm demo` + `bye (input received)` + exit 0 + `wintest: PASS`.

## Fase 12 — Linux, primeira versão ✅ (tradutor v1 + BusyBox estático, verificado E2E no QEMU)
- [x] Entrada `syscall` de verdade: EFER.SCE + LSTAR + stub `kernel/cpu/linux.asm` (mesmo `cpu::Frame` do `isr.asm`, retorno via `iretq`, sem churn no GDT)
- [x] Tradutor com ~35 syscalls (`linux_dispatch` em `kernel/proc/syscall.cpp`): read/write/readv/writev/openat/close/lseek/fstat, brk/mmap/munmap/mprotect, arch_prctl(SET_FS/GET_FS), uname, clock_gettime, getrandom, ioctl→ENOTTY, sinais/futex→stub, exit/exit_group
- [x] Stack Linux: argc/argv/envp + auxv (PHDR/PHENT/PHNUM/ENTRY/UID/GID/CLKTCK/PAGESZ/RANDOM/EXECFN); `USTACK_PAGES` 8→16; `PT_TLS`/RELRO ignorados com segurança
- [x] SSE/FPU ligado no boot (musl emite SSE2; kernel segue integer-only). **Contexto de FPU/SSE por thread** (2026-10-13): `Thread.fpu[512]` (FXSAVE) salvo/restaurado em `do_switch` e baseline limpo no `thread_trampoline` — sem isso, dois threads com float compartilhavam o FPU e corrompiam um ao outro após uma preempção. Teste `bin/lfpu` (soma float no pai enquanto um clone martela os XMM; falha se o FPU não for por thread).
- [x] Sysroot musl 1.2.5 via clang (`scripts/build-musl.sh`, cache em `build/musl-src`, `ar` com response file no Windows)
- [x] Marco: `Hello from musl libc!` + exit 0 (`user/mhello.c` → `bin/mhello` estático de verdade); `lhello` + `ltest: PASS` (asm, sem libc)
- [x] BusyBox estático Debian: `busybox true` exit 0, `busybox echo hi` imprime `hi`, `busybox uname -a` imprime `Linux choco 5.4.0 ...`.
- [x] Entrada de processos Linux começa com GPRs zerados; isso evita vazar estado do kernel como `rtld_fini` para o teardown glibc.
- Bugs reais: `do_clock_gettime(f->rdi)` passava o clock id como buffer; teste asm destruía `rax` no marker (kernel inocente nas duas)
- Falta pro BusyBox: clone/futex/sinais, ioctl/TTY, pipes, `execve`, fs gravável, binário dinâmico
- Critério final: BusyBox estático rodando (ex.: `busybox echo hi`). ✅

## Plano completo — continuação

### Fase 13 — Modelo de processos Linux 🚧
- [x] Pipes anônimos: `pipe`, `pipe2`, buffer compartilhado e descritores redirecionáveis.
- [x] `dup2`, `dup3` e `fcntl` básico.
- [ ] Tabela de processos privada por tarefa, com PID/PPID e estado `RUNNING/ZOMBIE`.
- [x] `clone` mínimo com `CLONE_VM`, PID filho e retorno 0 no filho.
- [x] `wait4` mínimo, status de saída e reap de zombies.
- [x] Teste `bin/lclone`: cria filho, espera e retorna 0 no QEMU.
- [x] Teste `bin/lclonepipe`: filho herda pipe, escreve, pai lê e aguarda no QEMU.
- [x] `clone` sem `CLONE_VM` duplica o espaço de endereços e herda a tabela de descritores.
- [x] Teste `bin/lfork`: espaço independente, filho, espera e status validados no QEMU.
- [x] CR3 independente é liberado pelo `wait4` no reap do filho.
- [x] Tabela de descritores é privada por processo, com herança no `clone`.
- [ ] Tabela de processos completa, com grupos, referências de arquivos e recursos por processo.
- Critério final: programa de teste cria filho, redireciona pipe, espera e retorna 0.

### Fase 14 — `execve` e shell Linux 🚧
- [x] `execve` mínimo substitui imagem, stack, auxv, CR3 e FS do processo atual.
- [x] Filho com CR3 independente pode executar `execve` de ELF raw.
- [x] Teste `bin/lexecclone`: filho independente executa `/bin/lhello` e o pai aguarda no QEMU.
- [x] **Tabela por-processo** (`kernel/proc/proc.cpp`): o `s_fg` global virou `Proc s_procs[16]` + `s_fg_pid`; `cur()` resolve a thread atual por pid e `fg()` o job do console. `exec_current` opera **in-place no processo atual** — um filho forked+exec não sobrescreve mais o pai. `clone`/`exit`/`run_args` alocam/liberam o `Proc`. Acessadores `proc_*` → `cur()`; novos `foreground_*` para o `procfs`.
- [x] **Pilha de syscall e de IRQ por thread** (`kernel/cpu/linux.asm` + `kernel/cpu/sched.cpp` + `kernel/cpu/gdt.cpp`): `linux_syscall_rsp` aponta para a pilha de syscall da thread corrente; o `rsp0` do TSS passa a ser o topo da pilha de kernel da thread, atualizado em toda troca (`gdt::set_rsp0`). **Motivo:** uma pilha de syscall e um `rsp0` globais faziam o frame de uma thread ser sobrescrito pelo de outra quando dois processos rodavam (o pai voltava para o `rip` do filho).
- [x] `exit_group` publica status para o pai.
- [x] `busybox sh -c 'echo hi'`-style: `bin/lbusyexecpipe` (clone + `dup2` num pipe + `execve /bin/busybox echo busyexec-ok`) imprime o resultado e sai 0 no QEMU.
- [x] Pipeline `busybox echo hi | busybox wc -c` no QEMU: `bin/lbusypipe` cria um pipe real; dois filhos independentes fazem `dup2`+`execve /bin/busybox` (`echo hi` → `wc -c`) e o pai lê o `3` de um segundo pipe. A leitura de pipe agora **bloqueia** até haver dados e devolve EOF só quando todos os writers fecham (antes retornava 0 com o pipe vazio — o `wc` pararia cedo).
- **Critério da fase atingido:** dois processos BusyBox trocam dados por um pipe no QEMU.

### Fase 15 — TTY e sinais Linux
- [x] **TTY completo (FEATURE-TTY-FULL)**: disciplina de linha em `kernel/cpu/tty.cpp` + `choco/tty.hpp` — canonical/raw, echo/erase/kill/EOF, `TCSETS` **aplicando de verdade** (era no-op), `TIOCGWINSZ`/`TIOCSWINSZ`, `TIOCSTI`, `FIONREAD`, `TCXONC`/`TCFLSH`, setas→ANSI, IXON/IXOFF; ISIG (`SIGINT`/`SIGQUIT`/`SIGTSTP`) e `SIGWINCH`; sinais pendentes entregues no retorno de syscall (`proc::raise_signal`/`deliver_pending`). Teste `scripts/test-tty.sh` 9/9.
- [x] `ioctl` mínimo de terminal: `TCGETS`, `TCSETS`, `TIOCGWINSZ` e `FIONREAD`; isso já atende `isatty`, `tcgetattr` e `tcsetattr` básicos.
- [x] Pipes sem leitores retornam `-EPIPE`; teste `bin/lpipebroken` validado no QEMU.
- [x] `kill(2)` encerra o alvo com `SIGINT`, `SIGTERM`, `SIGKILL` ou `SIGABRT`; `SIGTERM`/status 143 validado por `bin/lkillself` no QEMU.
- [x] `SIGCHLD` é representado pelo estado `ZOMBIE/DEAD` observado por `wait4`, com status codificado no formato Linux.
- [x] `rt_sigaction`/`rt_sigprocmask` básicos: máscaras por processo, `SIG_IGN` e proteção de `SIGKILL`/`SIGSTOP`; `bin/lsigconfig` validado no QEMU.
- [x] Entrega síncrona de handler e retorno transparente via `rt_sigreturn`, restaurando `rip`, `rsp`, registradores e máscara; `bin/lhandler` validado no QEMU.
- [x] Grupos básicos de processo: `getpgrp`, `getpgid`, `setpgid`, `kill(0/-pgid)` e Ctrl+C direcionado ao foreground process group.
- [x] TTY acompanha o foreground process group via `TIOCGPGRP`/`TIOCSPGRP`; `bin/lttygrp` validado no QEMU.
- [ ] Handlers POSIX reais, entrega assíncrona de sinais e `SIGPIPE` como sinal (em vez de apenas `-EPIPE`).
- [ ] Grupo de processo, foreground process group e Ctrl+C.
- Critério: `busybox sh` interativo aceita Ctrl+C sem derrubar o kernel.

### Fase 16 — Sistema de arquivos gravável
- [x] VFS mínimo em memória com arquivos regulares até 256 KiB (`WRITABLE_CAP`), `O_CREAT`, `O_TRUNC`, `O_APPEND`, `write`, `unlink`, `rename` e reabertura; `bin/lfswrite` validado no QEMU.
- [x] Diretórios virtuais simples, `mkdir`, `rmdir` e tipo `S_IFDIR` em `newfstatat`; `bin/lmkdir` validado no QEMU.
- [x] Hierarquia de caminhos simples com diretórios-pai, arquivos aninhados e `rename`; `bin/lhier` validado no QEMU.
- [x] `stat`/`lstat`/`fstat` básicos com tipo, permissões, tamanho, links, timestamps e blocos; `bin/lstat` validado no QEMU.
- [x] Volume persistente separado do initrd (`kernel/proc/fs.cpp` `disk_persistence_*`, volume cru no canal IDE secundário, `bin/lpersist`).
- [x] Mount de um volume persistente separado do initrd (FAT32 read/write via ATA, `test-netdisk.sh` 6/6).
- [ ] `openat` com `O_CREAT`, `O_TRUNC`, `O_APPEND` e permissões básicas.
- Critério: BusyBox cria, lê, altera e remove arquivo após reboot do QEMU.

### Fase 16B — Unificação do VFS (pré-requisito para os itens restantes da 16)
- [ ] Interface de mount única: tabela path → driver de fs.
- [ ] initrd, procfs, tmpfs em memória e futuro volume persistente como backends da mesma interface.
- [ ] Resolução de caminhos unificada (`openat` com dirfd, `..`, decisão explícita sobre symlinks).
- [ ] Nenhum `if/else` por fs solto em `syscall.cpp`; `fs::read/stat/count` falam só com a camada VFS.
- Critério: os testes existentes (`lfs`, `lfswrite`, `lmkdir`, `lhier`, `lstat`) continuam PASS sem mudança de código de fs depois da refatoração.

### Fase 17 — ELF e userland Linux ampliados
- [x] RTC CMOS lido no boot (`kernel/cpu/rtc.cpp`, portas 0x70/0x71, BCD e 12h tratados) + relógio de parede real: `rtc::now()` = epoch do CMOS + uptime do PIT. O PIT segue fixo em 100 Hz (sem calibração).
- [x] `clock_gettime` com `CLOCK_REALTIME` de verdade (epoch real); `time(2)`, `gettimeofday` e os timestamps do `stat` também passaram a usar o relógio de parede. Comando de shell `date`; teste `bin/ldate` (`date: PASS`).
- [ ] Relocações ELF necessárias e suporte a PIE.
- [ ] `PT_TLS` real por processo/thread.
- [ ] Loader dinâmico (`PT_INTERP`) e bibliotecas compartilhadas mínimas.
- [x] `mmap` file-backed (MAP_PRIVATE: copia o conteúdo do arquivo para páginas novas, zero após o EOF) e `mprotect` com permissões reais (`paging::protect_as` aplica R/W nos PTEs; `PROT_NONE` remove o present). Teste `bin/lmmap` (mapeia `proc/version` e lê). Sem NX ainda (falta `EFER.NXE`).
- Critério: binário dinâmico pequeno compilado para Linux inicia e sai 0.

### Fase 18 — Rede e ferramentas de sistema
- [x] Endurecimento pré-rede: auditoria de todos os ponteiros de usuário no tradutor Linux (`linux_dispatch`/`linux_abi`). Um bug real achado e corrigido: `fstat` preenchia o `struct stat` em `f->rdi` (o fd!) em vez de `f->rsi`; teste `bin/lfstat` (`fstat: PASS`).
- [x] Limites de recurso por processo: teto de 128 MiB na arena `mmap` por processo (`kernel/proc/linux.cpp`); descritores limitados a `FD_N=8` por processo.
- [x] `docs/LINUX-ABI.md` publicado: syscalls, sinais, flags de `clone`, ioctls de TTY e limitações conhecidas.
- [x] **Sockets TCP cliente** de ring-3: syscalls `socket`/`connect`/`send`/`recv`/`close` (AF_INET/SOCK_STREAM) sobre `net::tcp`; `read`/`write`/`close` roteados por fd. Teste `bin/lsock` conecta em `10.0.2.2:18080` e lê uma resposta `HTTP`. Pegadinha resolvida: `connect` zerava os globais do scheduler ao construir um `Tcb` (~32 KiB) como temporário na pilha de syscall (16 KiB) — corrigido zerando o TCB in-place.
- [x] `poll`/`select` mínimo (syscalls 7/23): prontidão de tty, sockets, pipes e arquivos; teste `bin/lpoll`.
- [x] `socketpair` (AF_UNIX + SOCK_STREAM): par conectado implementado sobre pipes (syscall 53); teste `bin/lsp`.
- [x] Lado **servidor TCP** (`bind`/`listen`/`accept`/`accept4`): listener + estados `LISTEN`/`SYN-RCVD` no motor (`net::tcp::listen`/`accept`); teste `scripts/test-sock-serv.sh` (servidor in-guest + cliente no host via `hostfwd`) — **critério da fase atingido** (cliente e servidor trocam dados no QEMU).
- [x] Sockets Unix **por caminho** (`bind`/`connect` AF_UNIX): registro de endpoints nomeados no `syscall.cpp` (path → listener com fila de conexões pendentes); `bind` reserva o nome, `listen` marca joinable, `connect` empurra a ponta servidora na fila e `accept` a puxa para um fd novo (mesmo mecanismo de pipes de um socketpair). Teste `bin/lusock` (servidor por caminho + cliente trocam PING/PONG). Limite honesto: o nome é só uma chave no kernel, não cria nó no filesystem.
- [x] Virtio-net como backend de rede (`kernel/proc/virtio_net.cpp`) — ver "Extra — pilha de rede".
- [x] `ifconfig`/`arp`/`dhcp` no shell; `run.sh` agora anexa a NIC por padrão (`-netdev user` + `virtio-net-pci`).
- Critério: cliente e servidor trocam dados no QEMU.

### Fase 19 — Userland ChocoOS
- [ ] libc freestanding documentada e headers estáveis.
- [ ] Shell nativo com jobs, pipes, histórico e redirecionamento: pipes/redirecionamento/histórico prontos (Tiers 1-3), e **`&` (background)** entregue (`tokenize` → `T_BG`; programa vira `proc::spawn_job` e é reapado pelo loop ambiente; `test-shell3.sh` 7/7). Faltam `jobs`/`fg`/`bg` e job control por sinais.
- [ ] Coreutils nativos: `cat`, `ls`, `cp`, `mv`, `rm`, `mkdir`, `ps`.
- [ ] Sistema de pacotes/initrd versionado.
- Critério: sistema inicializa até um shell utilizável sem comandos do kernel.

### Fase 20 — Display server v2 e aplicações
- [ ] Dirty rectangles no compositor.
- [ ] Event queues por janela e shared memory robusta.
- [ ] Widget kit, fonte própria e ícones Material-style.
- [ ] Apps nativos: terminal, arquivos, monitor, editor e configurações.
- Critério: desktop completo com apps em userland e sem repaint full desnecessário.

### Fase 21 — Robustez e manutenção
- [ ] Decisão explícita sobre SMP cedo: scheduler, VM por processo e reap de zombies assumem single-core; se SMP não entra, `#ifdef` + nota na ABI, se entra, ajuste antes de APIC.
- [x] Runner headless de regressão `scripts/run-ltests.sh`: sobe o QEMU uma vez (com NIC + servidor HTTP + volume FAT32), roda todos os `bin/l*` e reporta PASS/FAIL (XFAIL para os sabidamente incompletos). Achou e ajudou a corrigir dois bugs reais: **vazamento de slot de thread** (o thread foreground nunca era reciclado → "no thread slots left" após ~13 runs; fix `sched::reap_pid` no `run_args`) e **o kernel dava panic num fault de ring 3** (agora `isr_dispatch` mata o processo com SIGSEGV e segue).
- [ ] Mini-framework de asserts unificado no kernel (estilo o self-test do heap) + contador de testes visível.
- [ ] SMP ou limitação single-core explicitamente isolada.
- [ ] Testes automatizados de boot, syscalls, memória, processos e GUI.
- [ ] Sanitização de ponteiros, auditoria de limites e mensagens de erro.
- [ ] Documentação de ABI, formato de disco e processo de release.
- Critério: smoke test completo repetido sem regressões e ISO reproduzível.

## Fase 22 — Threads, futex e eventos
- [ ] `clone` com `CLONE_THREAD`, `CLONE_SETTLS`, `CLONE_CHILD_CLEARTID`, `set_tid_address`, `tgkill`
- [ ] TLS por thread (`FS` salvo na troca de contexto)
- [ ] `futex` de verdade (WAIT/WAKE/REQUEUE), stubs de `rseq` e `set_robust_list`
- [ ] Entrega assíncrona de sinais
- [ ] `epoll_create1/ctl/pwait`, `eventfd2`, `timerfd`, `poll/ppoll/pselect6`, `clock_nanosleep`
- Critério: programa pthreads (musl) com mutex, cond e loop `epoll` passa no QEMU.

## Fase 23 — Python
- [ ] CPython estático (musl), stdlib em `/usr/lib/python3.x` no disco
- [ ] `python3 -c 'print(2+2)'` e script que cria/lê arquivos
- [ ] REPL interativo no terminal (TTY)
- [ ] `os`, `time`, `threading`, `subprocess` (fork+exec), depois `socket` e `ssl`
- Critério: REPL funcionando e `urllib.request.urlopen('http://...')` com rede.

## Fase 24 — Node.js
- [ ] Node (musl ou glibc) com `--jitless` primeiro, depois JIT (`mprotect` PROT_EXEC)
- [ ] libuv: threads, epoll, eventfd, pipes
- [ ] `node -e 'console.log(1)'`
- Critério: servidor HTTP respondendo ao host via `hostfwd` do QEMU.

## Fase 25 — Janelas para apps grandes (display v2+)
- [x] Janelas redimensionáveis (arrastar borda/canto + maximizar) — ver Extra abaixo.
- [ ] Janelas maiores que 640x480 e sem o limite de 4 (o shm ainda aloca pela
      capacidade `UW_MAX_W`x`UW_MAX_H`, então redimensionar não remapeia nada)
- [ ] Clipboard, cursores, layouts de teclado (US, ABNT2), eventos mais ricos, frame callbacks
- [x] Protocolo de janelas para apps portados — **1º recorte (Wayland mínimo)**: `bin/wlrun` (`user/wlrun.c`) é um compositor Wayland ring-3 que fala `wl_display`/`wl_registry`/`wl_compositor`/`wl_shm`/`wl_shm_pool`/`wl_surface`/`wl_buffer` sobre um socket AF_UNIX e blita o buffer shm do cliente numa janela nativa (`win_create`/`win_present`, expostos ao ABI Linux como syscalls 500/501/502). Um cliente de teste com marshalling cru (fork do mesmo binário) desenha um gradiente e commita; verificado por screenshot + `run-ltests` (`wlrun`: "composite OK"). **Pré-requisitos de kernel entregues:** `memfd_create` (319) + `ftruncate` (77) + `mmap MAP_SHARED` (`kernel/mem/memfd.cpp`), `sendmsg`/`recvmsg` com **SCM_RIGHTS** (`kernel/proc/syscall.cpp`), `fork`/`vfork` (57/58) + cópia do estado Linux no fork (`linux::clone_state`: FS base + arena mmap). **Falta:** `xdg_wm_base`/`xdg_surface`/`xdg_toplevel`, `wl_seat` (mouse/teclado), frame callbacks reais e mais de um cliente; sem isso apps reais (GTK/Qt) ainda não rodam.
- [x] Fontes para o engine: **FreeType 2.13.2** vendorizado em `third_party/`, `scripts/build-freetype.sh` gera `build/freetype/libfreetype.a` (padrão BearSSL: clang `--target=x86_64-linux-musl`); `Inter.ttf` (OFL) ship em `/usr/share/fonts` do initrd; teste `bin/lfreetype` faz `FT_New_Face`→`FT_Set_Pixel_Sizes`→`FT_Load_Char('A')` e rasteriza AA real em ring-3 (`ftfont: glyph A 16x18 ink=134`), marker no `run-ltests` (32/0). FreeType usa aritmética fixa própria, não exige FPU.
- Critério: app Wayland simples abre janela, redimensiona e recebe teclado e mouse.

## Fase 26 — Navegadores em degraus
- [ ] Navegador de texto (links ou w3m): valida rede + TTY
- [ ] NetSurf: primeiro navegador gráfico, leve
- [ ] Chromium `--headless --no-sandbox --disable-gpu`: valida syscalls, threads, mmap e TLS sem gráfico
- [ ] Chromium com janela (Ozone/Wayland), renderização por software
- Critério: Chromium abre https://example.com numa janela do ChocoOS (QEMU com 4 GB+ de RAM).

## Fase 27 — Hardware real (opcional)
- [ ] APIC/IOAPIC, HPET, ACPI básico
- [ ] AHCI/NVMe, xHCI + USB HID (teclado e mouse), drivers de Ethernet reais
- [ ] SMP (ou limitação single-core isolada)

## Fase 28 — Ecossistema de desenvolvimento
- [ ] Gerenciador de pacotes simples e repositório
- [ ] make, git, tcc/clang rodando dentro do ChocoOS
- [ ] Auto-hospedagem: compilar o ChocoOS dentro do próprio ChocoOS (meta distante)

## Fase 29 — EXEs (Windows)
- [ ] Loader PE + `ntdll` próprio, quase como uma camada de abstração, ou mesmo um "wine nativo", o código aberto do wine pode ser utilizado para poupar tempo
- [ ] Reaproveitar código do Wine por cima para o Win32
- Critério: EXE de console, depois app simples com janela.

## Fase 30 — Encerramento e manutenção de sistema
- [ ] ACPI mínimo para `poweroff` limpo (hoje só reboot via 8042 no menu Start).
- [ ] Watchdog do kernel: timeout de boot, pânico com mensagem útil no serial e framebuffer.
- Critério: `poweroff` no shell desliga o QEMU sem matar o processo na mão.

## Fase 31 — Userland visual (em paralelo, opcional)
- [ ] Editor de texto nativo com save para o volume persistente.
- [ ] Navegador de rede depois da Fase 18, antes de Chromium (degraus já descritos na 26).
- Critério: texto editado e salvo sobrevive a reboot.

Cada fase = 1 PR/commit com teste no QEMU.

## Extra — /disk FAT32 e File Explorer (fora das fases)

Trabalho feito fora da sequência das fases, endereçando os itens "críticos"
(FS/FAT32 stub e File Explorer fake):

- [x] `scripts/make-fatdisk.sh`: gera `build/fat.img`, um **FAT32 real** (mformat/mcopy),
      embarcado como segundo módulo Limine (`fatdisk`).
- [x] `kernel/proc/fat32.cpp`: driver de leitura de verdade — parse do BPB,
      caminhada do cluster chain (FAT), diretórios com entradas 8.3 + LFN,
      achatados em caminhos `disk/...` e plugados na camada `fs::` (zero syscall nova).
- [x] `user/fileman.c`: File Explorer navegável (toolbar, sidebar, lista com tipos,
      viewer de texto, criar/renomear/deletar no FS gravável).
- [x] UI Fluent do Explorer e do chrome do desktop: tokens Win11 em `user/lib/gui.h`,
      texto proporcional **Inter** gravado em build time (`scripts/build-font.py` →
      `user/lib/font_ui.h` + `kernel/gen/uifont.inc`) porque kernel e ring 3 rodam
      sem FPU, ícones 512px normalizados para 24px com tint controlado por luminância.
- [x] **Janelas redimensionáveis**: `win_resize()` único caminho para gesto e app,
      bordas/cantos (`EZ_L/R/T/B`), mínimo 320x200, maximizar por duplo-clique no
      título e Aero snap nas bordas. No shm o buffer é alocado pela **capacidade**
      (`UW_MAX_W`x`UW_MAX_H`) com `pitch` fixo no cabeçalho, então redimensionar
      nunca remapeia página sob o app: `N_WIN_RESIZE` (21) e `UEV_RESIZE` (3) só
      avisam o novo tamanho. `user/fileman.c` e `user/uwm.c` relayoutam no evento.
- [x] Testes headless: `scripts/test-fat.sh` (boot + teclas no monitor + serial) e
      `scripts/test-fileman.sh` (screendump + checagem de cores).
- [x] `scripts/test-resize.py`: E2E do **arrasto real do mouse** numa janela de app
      (QMP + `human-monitor-command`: `mouse_move`/`mouse_button`), medindo a largura
      do conteúdo pela faixa de `UI_SEL` no framebuffer. 7/7.
      `wintest` cobre o mesmo gesto em janelas do kernel (5/5).

## Extra — Pilha de rede e driver virtio-net (fora das fases)

Stack IPv4 no kernel, feito fora da sequência das fases (mesmo padrão do Extra
de FAT32). Hoje é **kernel-side**: nenhum destes caminhos usa syscalls — abrir
`socket()` para programas ring-3 é o alvo da Fase 18.

- [x] Driver **virtio-net**: `kernel/proc/virtio_net.cpp` (+ `kernel/proc/pci.cpp`) — probe PCI, virtqueue RX/TX e IRQ; MAC/MTU/identidade do link via `net::if_*`.
- [x] **ARP + IPv4 + ICMP**: `kernel/net/net.cpp` — `send_ipv4`, `ping`, tabela ARP e contadores; `ping 10.0.2.2` no shell.
- [x] **UDP + DHCP + DNS**: `kernel/net/{udp,dhcp,dns}.cpp` — cliente DHCP (defaults QEMU `10.0.2.x` como fallback) e resolução de nomes A sobre UDP.
- [x] **TCP**: `kernel/net/tcp.cpp` — `connect`/`send`/`recv` com retransmissão e FIN; base do `httpget` (HTTP/1.0 GET).
- [x] Comandos de shell: `ifconfig`, `arp [flush]`, `ping <ip> [n]`, `dns <name>`, `dhcp [renew]`, `udpstat`, `tcpstat`, `httpget <host> [port] [path] [outfile]`.
- [x] Testes headless de rede: `scripts/net-test.sh`, `net-test-dhcp.sh`, `net-test-dns.sh`, `net-test-nodhcp.sh`; captura com `scripts/pcap.py`.
- [x] **Sockets TCP cliente** de ring-3 sobre este stack (syscalls `socket`/`connect`/`send`/`recv`/`close`; ver Fase 18). Ainda **sem** lado servidor (`bind`/`listen`/`accept`), sockets Unix e `poll`/`select`; o `httpget` do shell segue kernel-side.
- [x] `scripts/host-env.sh`: resolve o PATH do toolchain e recusa artefatos de uma
      sessão anterior. **Motivo:** os scripts fixavam `export PATH=/ucrt64/bin`, que
      não existe fora do shell MSYS2 — o QEMU nem subia, e o serial.log *de outra
      sessão* fazia todas as asserções passarem.
- Limite honesto: mutações do FS gravável em RAM (raiz) não valem para o initrd;
  `/disk` FAT32 agora é **read/write com persistência real via ATA** (escreve em
  `s_img` e faz flush write-through sector-a-sector; cria/deleta/renomeia diretórios
  e arquivos com LFN). A imagem de exemplo é pequena (16/32 MiB) — mtools cria e
  o driver lê e escreve. Integração TCP+disco coberta por `scripts/test-netdisk.sh`
  (2 boots QEMU: baixa 211 KB via HTTP → salva em `/disk` → reboot → verifica;
  o wait por prompt no serial elimina a race que matava o QEMU no meio do flush
  PIO de ~400 setores — a falha "intermitente" era do teste, não do driver).
  O redimensionar ainda respeita os limites do display v1: máx. 640x480 de conteúdo
  (a capacidade do shm) e 4 janelas de app; o preview visual do Aero snap não foi
  feito. O arrasto em janela de app está coberto por teste (7/7), o redimensionar
  por canto em janela de app não.

## Extra — RAM: o kernel já usa mais que 512 MiB (fora das fases)

Pré-requisito do alvo "browser" (Fase 26). Medido empiricamente: o PMM é um bitmap
dimensionado pelo topo do memmap do Limine e o espelho HHDM mapeia entrada por
entrada, então **não há teto de 512 MiB** no kernel.

- [x] Boot verificado em 512M/1G/2G: tempo até o prompt ~7,7 s nos três casos (o
      espelho HHDM página-a-página **não** é gargalo — a hipótese de otimizar
      `pmm::alloc`/huge pages foi descartada por medição, não por suposição).
- [x] **4 GB validado**: boot ~7,9 s até o prompt, `run-ltests` **30/0 com
      `-m 4096M`**. Com `-m 4G` o QEMU divide a RAM em torno do "buraco" PCI:
      uma faixa usable abaixo de 4G e outra em `0x100000000..0x140000000`
      (1 GiB **acima** de 4G). O HHDM espelha ambas sem falha.
- [x] `run-ltests` passa **30/0 com `-m 1024M`** — nada regride com mais RAM; o
      bitmap maior e a colocação continuam válidos.
- [x] `pmm::total_bytes()` agora reporta a **soma das páginas usable**, não o
      span de endereço. **Motivo:** em 4G o topo do endereço (5 GiB) é maior que
      a RAM real por causa do buraco PCI — o `mem`/`/proc/meminfo` mostravam
      `total=5120 MiB` e um `used` inflado. Corrigido: `total=4095 MiB`,
      `free=4093`, `used=24` (o que o kernel realmente ocupa).
- [x] Superfície coerente: em 1G o boot mostra `[pmm] total=1023 MiB` e `mem`
      mostra `[mem] ram total=1023 MiB` (262112 páginas).
- [x] `scripts/run.sh` sobe o default interativo de **512M → 1024M** (ramificações
      UEFI e BIOS) para dar folga ao userland. Os testes headless seguem em 256M
      por velocidade.
- Limite honesto: o Chromium da Fase 26 pede "QEMU com 4 GB+"; 4G está validado,
  acima disso (6/8G) ainda não medido. Instabilidade observada: a QEMU TCG no
  Windows morre no meio da suíte em execuções consecutivas rápidas (falsa falha
  ~25/6); um re-run limpo volta a 30/0. Não é regressão do kernel.

## Extra — Entropia real + TLS (fora das fases)

Habilita HTTPS (requisito de qualquer browser). Fora da ordem das fases, mesmo
padrão dos Extras acima.

- [x] **CSPRNG do kernel** `kernel/crypto/rand.cpp` (+ `choco/rand.hpp`): DRBG
      HMAC-SHA256 no estilo SP800-90A (SHA-256 implementado aqui, inteiro-only,
      sem dependência). Semeadura com RTC + TSC + jitter de timing (no IRQ0, via
      `rand::stir()` a cada tick) e **RDRAND/RDSEED** quando o CPUID anuncia.
      `getrandom(2)` agora devolve bytes do CSPRNG (teto 4096/call) e o
      `AT_RANDOM` do carregador ELF usa a mesma fonte. Diferente do LCG do PIT
      antigo, o estado **avança** a cada chamada.
- [x] Teste headless `bin/lrand` (`user/linux/lrand.asm`): puxa 1024 bytes
      (prova o teto maior), exige buffer não-zero e que duas chamadas produzam
      bytes diferentes. Marker `rand: PASS`. `run-ltests` passou a **31/0**.
- [x] **open(2) faltando no ABI Linux** (`kernel/proc/syscall.cpp`): a musl usa
      `syscall 2` (não `openat 257`) em `open()`, e o kernel não tinha o `case 2`
      → `fopen` do musl falhava com `ENOSYS`. Adicionado `case 2 -> do_open`.
- [x] **BearSSL vendorizado** em `third_party/bearssl-0.6/` (MIT); build
      `scripts/build-bearssl.sh` compila os 277 `.c` para
      `build/bearssl/libbearssl.a` (target musl).
- [x] **`bin/httpsget`** (`user/httpsget.c`, musl + BearSSL): resolve/connecta,
      faz handshake TLS 1.2, **valida cadeia X.509** contra um CA em PEM passado
      em linha de comando (âncora de confiança decodificada em runtime via
      `br_pem_decoder` + `br_x509_decoder`), injeta entropia do `getrandom` no
      HMAC-DRBG do engine, envia GET e imprime o corpo. Uso:
      `httpsget <url> [cafile.pem] [server_name]`.
- [x] CA de teste em `user/root/etc/ssl/cacert.pem` (self-signed, SAN
      `localhost`/`10.0.2.2`) empacotada no initrd; chave/cert do servidor em
      `scripts/tls/`. Teste E2E `scripts/test-tls.sh` (openssl `s_server` no
      host): **2/2** — fetch válido imprime `CHOCO-TLS-OK`, e nome de servidor
      errado é **rejeitado** (`err=56` BAD_SERVER_NAME).
- Limites honestos: **TLS 1.2** (o BearSSL 0.6 não faz 1.3); a validação de
  horário depende do RTC do host (o modo `set_time` do BearSSL conta dias desde
  o ano 1, não 1970 — deixamos o engine buscar `time(NULL)`). O CA é um bundle
  único de teste, não ainda um conjunto de raízes do mundo real. Não há browser
  de texto ainda — `httpsget` é o tijolo de rede.

## Extra — browser gráfico simples com UI custom (fora das fases)

Primeiro passo concreto da Fase 26 ("Navegadores em degraus"), feito do zero para
a UI do ChocoOS (não é um port do NetSurf/w3m). Verificado E2E no QEMU com NIC.

- [x] **Syscall `fetch` (`N_FETCH=22`, int 0x80)** em `kernel/proc/syscall.cpp`
      (`do_fetch`): reusa o caminho do `httpget` do kernel — parseia
      `[http://]host[:port][/path]`, resolve nome via `net::dns::resolve`, faz
      HTTP/1.0 GET por `net::tcp::connect/send/recv` e devolve a resposta crua
      (headers + corpo) no buffer do chamador. Como expõe DNS no kernel, o app
      ring-3 não precisa de socket UDP (ainda inexistente). Respeita os dois
      gotchas da rede: `cpu::sti()` nos bloqueios e nenhum temporário grande na
      stack da syscall. `https://` retorna `EOPNOTSUPP` (TLS é só em userland).
- [x] **`/bin/browser`** (`user/browser.c`, ABI choco, `build_one`): janela com
      chrome Fluent (barra de endereço `gfx_input`, botões Voltar/Home/Go, barra
      de status). Busca via `choco_fetch`, tira os headers, faz **parse de um
      subconjunto de HTML** e layout com word-wrap: títulos `h1..h6` (escala 2 +
      peso), `b/strong`, `i/em` (faux negrito/itálico por superposição/cisalha —
      sem FreeType/FPU no caminho), `p/div/li/br/hr`, links `<a href>` (cor de
      destaque + sublinhado, clicáveis com resolução relativa), entidades
      (`&amp; &lt; &gt; &quot; &#..; &nbsp;`). Salta `head/script/style/textarea`
      (skip por nome do elemento, ignorando o texto bruto até o fechamento).
      Rolagem por setas/tecla/space e clique na scrollbar.
- [x] Atalho `desktop/browser.desktop` (aparece no desktop, visto no screenshot).
- [x] Teste E2E `scripts/test-browser.sh` (servidor HTTP no host `:8080` → guest
      `10.0.2.2:8080`, screendump + contagem de cor): **5/5** — canvas branco,
      chrome, **link** (`1D6FBF` do sublinhado sólido) e **heading** (`241F3A`)
      pintados, sem erro de conexão. Screenshot inspecionado: layout correto,
      sem vazamento de CSS/JS.
- Limites honestos (v1 "simples"): **sem imagens** (não há decodificador
  PNG/JPEG), **sem CSS** (regras ignoradas por design), **sem JS**, **sem
  HTTPS** (a syscall é HTTP puro; o `bin/httpsget` com BearSSL existe mas não
  está ligado ao browser), HTTP/1.0 `Connection: close` (sem keep-alive/pipeline),
  um documento por vez (sem abas), e layout de tabela/float não suportado
  (`<td>` etc. viram texto corrido). É um leitor de HTML de texto formatado, não
  um motor de render.

Observações:

ícones podem e devem ser domínio publico, baixe online, garanta que toda UI focada no visual tenha ícones.
Não precisa scriptar tudo do zero, pode pegar códigos já feitos da internet, desde que seja uso livre (domínio publico, sem licença ou atribuição)

