# Build técnico do kernel (evolução incremental)

Este documento consolida o que já foi desenvolvido, como reproduzir o build/boot, e o estado técnico atual do kernel.

## 1. Ambiente e execução

### Pré-requisitos
- `gcc`, `g++`, `ld` (ou toolchain cross `i686-elf-*`)
- `grub-pc-bin` (fornece `grub-mkimage` e módulos `i386-pc`)
- `grub-efi-amd64-bin` (fornece `grub-mkstandalone` e módulos `x86_64-efi`)
- `xorriso`
- `qemu-system-i386`
- `qemu-system-x86_64`
- firmware OVMF/UEFI (`/usr/share/OVMF/OVMF_CODE_4M.fd`)

### Build e boot
```bash
make clean
make
make iso
make run
make run-uefi
```

### Direção de próxima geração
- A base atual continua BIOS+i386.
- O próximo alvo deve ser UEFI+x86_64 para pendrive live em PCs modernos.
- A meta é manter o fluxo atual enquanto a nova árvore de boot é introduzida em paralelo.
- A separação inicial de arquitetura já existe em `kernel/arch/x86_64`, ainda como scaffold.

### Boot live para pendrive
- A ISO agora é híbrida e contém boot BIOS e UEFI.
- Para testar em máquina virtual moderna, use `make run-uefi`.
- Para gravar em pendrive, copie a ISO diretamente para o dispositivo cru com `dd`.

### Fluxo de ISO atual
Em vez de `grub-mkrescue` (que depende de `mformat`), o projeto usa:
1. `grub-mkimage` para gerar `core.img` BIOS
2. concatenação `cdboot.img + core.img` em `bios.img`
3. `xorriso` para compor a ISO bootável

Isso remove dependência direta de `mtools` e mantém um pipeline estável no host.

## 2. Marcos implementados

## Bootstrap e observabilidade
- Header Multiboot2 + entrada `_start` em ASM.
- Linker script com símbolos `__kernel_start` e `__kernel_end`.
- `kernel_main` em C++ freestanding.
- Logger serial COM1 para telemetria de boot.

## Arquitetura x86 (base)
- GDT com segmentos kernel/user.
- TSS carregada (`ltr`) e stack de kernel (`esp0`) configurável.
- IDT com 256 entradas.
- Stubs de interrupção em ASM com handlers em C++.

## Interrupções e timer
- PIC remapeada para `0x20/0x28`.
- PIT configurado em 100 Hz.
- IRQ0 contabiliza ticks globais do kernel.
- Breakpoint (`int 0x03`) validado no boot.

## Syscalls
- Vetor `int 0x80` ativo na IDT (trap gate ring3).
- Dispatcher de syscall com números iniciais:
  - `kWrite`
  - `kExit`
  - `kSleep`
  - `kOpen`
  - `kRead`
  - `kClose`
  - `kGetPid`
  - `kGetTid`
  - `kProcCount`
  - `kMkdir`
  - `kGetUid`
  - `kGetGid`
- `sys_sleep` integrado ao scheduler com estado de sleep/wakeup.
- `sys_write` agora aceita `stdout/stderr` e FDs de arquivo do ramfs.
- Validação de ponteiros de userland (`kernel/user_safety.*`) aplicada em `write/open/read/mkdir`
  quando a syscall vem de task ring3.

## Processos/threads e sincronização
- Scheduler evoluído para manter tabela de processos e threads:
  - PID por processo
  - TID por thread
  - contadores de processos/threads ativos
- API nova de scheduler: `create_process_task(...)`, `current_process_id()`, `current_thread_id()`.
- `ramfs` protegido por spinlock para acesso concorrente entre tasks preemptivas.
- Primitivas de sync adicionadas em `kernel/sync/spinlock.hpp`.
- Cada processo recebe um page directory próprio (clone do kernel).
- Troca de contexto entre processos agora inclui troca de `cr3` no scheduler.

## VFS/ramfs inicial
- Camada VFS mínima em `kernel/fs/vfs.hpp`.
- Backend `ramfs` em memória (`kernel/fs/ramfs.*`).
- Arquivo de teste montado no boot: `/test.txt`.
- Caminho validado no boot via syscalls: `open -> read -> close`.
- Suporte a diretórios no ramfs com caminhos absolutos normalizados (`/dir/sub/file`).
- Nova syscall `kMkdir` para criação de diretórios.
- Fluxo validado no boot: criação de `/bin`, criação/escrita/leitura de `/bin/note.txt`.
- Rootfs inicial com identidade root e home: `/root`, `/home/root`, `/etc/passwd`, `/root/.profile`.
- Árvores padrão de usuário criadas no boot em `/root` e `/home/root`:
  - `Desktop`
  - `Documents`
  - `Downloads`
  - `Images`
  - `Music`
  - `Musics`
  - `Pictures`
  - `Public`
  - `Templates`
  - `Videos`
  - `Workspace`
  - `Projects`
  - `Docs`

## Tasking e scheduler
- Scheduler round-robin acionado por IRQ0 (timer).
- Quantum fixo por ticks para seleção da próxima task.
- Tasks de kernel com stack dedicada e contexto inicial montado para `iret`.
- Trampoline de entrada para executar função da task.
- Integração com `sys_sleep` para bloquear/despertar task atual.
- Context switch de ring3 no timer amadurecido:
  - primeira entrada em userland monta frame IRET de troca de privilégio;
  - retomadas seguintes reutilizam o contexto salvo da interrupção;
  - `TSS.esp0` é atualizado para stack de kernel da task ring3 selecionada.

## Boot info e memória física
- Parser Multiboot2 de mapa de memória (tag type 6).
- Log de regiões de RAM por serial.
- PMM com bitmap de frames 4 KiB:
  - reserva da low memory (primeiro 1 MiB),
  - reserva da faixa ocupada pelo kernel,
  - `alloc_frame` e `free_frame`.

## Memória virtual e heap
- VMM com diretório/tabelas de página 4 KiB.
- Identity map inicial para bootstrap estável.
- API mínima:
  - `map_page`
  - `unmap_page`
  - `virt_to_phys`
  - `enable_paging`
- Heap virtual do kernel (bump allocator) sobre PMM+VMM:
  - mapeamento sob demanda de páginas,
  - métricas de bytes usados/mapeados.
- `operator new/delete` global apontando para heap do kernel.

## 3. Evidências atuais no boot (serial)

Sequência típica validada em QEMU:
1. bootstrap OK + endereço MBI
2. regiões de memória Multiboot2
3. PMM total/livre + frame de teste
4. `paging enabled`
5. mapeamento virtual de teste (`virt -> phys`)
6. alocação em heap + teste de `new`
7. GDT/TSS, IDT, PIT, breakpoint
8. syscall write/sleep
9. leitura do ramfs no boot (`[kernigham] ramfs read: Hello from ramfs!`)
10. arquivo aninhado no ramfs (`[kernigham] ramfs nested read: Nested file OK`)
11. carga de `/bin/init.elf` e entrada em ring3 via loader (`sys_exit code=0x0000002A`)
12. identidade root disponível por syscall (`uid=0`, `gid=0`) e leitura de arquivos de conta

## 4. Organização atual de código

- `boot/`
  - `boot.S`: entrada Multiboot2
  - `interrupts.S`: stubs de ISR/IRQ/syscall
  - `gdt.S`: flush de GDT/TSS
  - `linker.ld`: layout e símbolos de kernel
- `kernel/arch/x86/`
  - `gdt.*`, `interrupts.*`, `pic.*`, `pit.*`, `io.hpp`
- `kernel/boot/`
  - `multiboot2.*`
- `kernel/mm/`
  - `pmm.*`, `vmm.*`, `heap.*`, `new_delete.cpp`
- `kernel/task/`
  - `scheduler.*`
- `kernel/sync/`
  - `spinlock.hpp`
- `kernel/syscall/`
  - `syscall.*`
- `kernel/fs/`
  - `vfs.hpp`, `ramfs.*`

## 5. Limitações atuais

- Ainda não há permissões POSIX, listagem de diretório ou FS em disco.
- Heap é bump allocator (sem free de blocos).
- VMM/scheduler ainda são núcleos iniciais (sem `fork/exec`, sem sinais, sem IPC).
- Userland ainda não está conectado a multitarefa ELF completa via scheduler (caminho atual de ELF segue execução direta pelo loader).

## 6. Userland e ELF loader (v0.1.5)

Adicionado suporte básico para:
- Parser de header ELF 32-bit e program headers.
- Loader que valida binários ELF, mapeia segmentos PT_LOAD como páginas user e zera BSS.
- Execução direta em ring3 a partir do `e_entry` carregado (`iret` para CPL3).
- Libc mínimo com wrappers de syscall para userland.
- Programa teste userland em ASM puro (syscalls via `int 0x80`).

## 7. Ring3 execution infrastructure (v0.1.6)

Adicionado:
- Assembly function `enter_ring3_simple` que prepara frame IRET e salta para ring3.
- Scheduler função `create_ring3_task` para criar tarefas em ring3.
- Extensão Task struct com campos `ring` e `user_esp0` para suporte ring3.
- GDT já possui user code (0x1B) e user data (0x23) selectors.
- TSS configurado com ring0 stack (esp0, ss0) para syscalls/interrupts.

Status:
- ✅ Boot continua estável
- ✅ Ring3 jump funcional com código em página user (`U/S=1`) e stack user mapeada
- ✅ Syscall `int 0x80` executada a partir de CPL3 (validação com `sys_exit code=0x2A`)
- ✅ ELF real em `/bin/init.elf` carregado do ramfs e executado em CPL3
- ⏳ Scheduler ainda precisa realizar context switch completo de tasks ring3 no caminho normal do timer

## 8. Próxima prioridade

1. **Userland init/shell**: processo inicial em userland para leitura de `/etc/passwd`, HOME e loop de comandos.
2. **Execução via scheduler**: ligar ELF loader à criação de tasks ring3 agendáveis (não apenas entrada direta).
3. **VFS avançado**: listagem de diretório e metadados básicos de inode.
4. **FS persistente**: migrar de ramfs puro para backend em disco.

## 9. Shell interativo no kernel (terminal serial)

O boot agora inicia um shell interativo no serial (`qemu -serial stdio`) com prompt:

`root@kernigham:/root#`

O parser do shell foi refatorado para tokenização com suporte a aspas (`"..."` e `'...'`) e despacho por tabela de comandos
(sem cadeia grande de `if/else`).

Total atual: **186 comandos/aliases**.

Comandos-família implementados:
- Navegação/listagem: `ls`, `ld`, `dir`, `cd`, `pwd`, `tree`
- Diretórios/arquivos: `mkdir/md`, `rmdir/rd`, `touch`, `rm/unlink`, `cp`, `mv`
- Leitura/inspeção: `cat/print`, `head`, `tail`, `wc`, `cmp`, `stat`
- Ambiente/sistema: `whoami`, `id`, `env`, `uname`
- Utilitários: `echo`, `clear`, `help`, `halt`
- Novos utilitários (lote +30): `basename`, `dirname`, `exists`, `isfile`, `isdir`, `size`,
  `write`, `append`, `truncate`, `nl`, `hexdump`, `find`, `grep`, `sleep`, `tick`, `uptime`,
  `ps`, `meminfo`, `pathjoin`, `realpath`, `seq`, `repeat`, `catb`, `countdir`,
  `touchmany`, `rmmany` (com aliases Linux-like extras).

Exemplos de aliases adicionais (entre os 132): `ll`, `la`, `listdir`, `type`, `more`, `less`, `copy`,
`move`, `rename`, `compare`, `fileinfo`, `printenv`, `version`, `shutdown`.

No kernel/VFS também foram adicionados:
- `readdir(path, entries, max_entries)`
- `rmdir(path)`
- `unlink(path)`

e syscalls:
- `kReaddir = 13`
- `kRmdir = 14`
- `kUnlink = 15`

## 10. Empacotar em pendrive (próximo passo prático)

Com o ISO gerado em `build/kernigham.iso`, grave em USB (Linux host):

```bash
make iso
lsblk
sudo dd if=build/kernigham.iso of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

Troque `/dev/sdX` pelo dispositivo correto do pendrive (sem número de partição).

## 11. Pacotes e integração apt-like

O shell agora expõe um gerenciador de pacotes local com comandos:

- `apt update`
- `apt search <termo>`
- `apt list [all|installed|available]`
- `apt show <pacote>` / `apt info <pacote>`
- `apt files <pacote>`
- `apt install <pacote>`
- `apt remove <pacote>`
- `apt policy <pacote>`
- `apt upgrade`

Catálogo inicial disponível no boot:

- `base-system`: metadados do sistema e bootstrap do repositório
- `home-layout`: documentos e estrutura padrão de home
- `devtools`: documentação de shell e pacotes
- `demo-docs`: payload demo instalado em `/opt/demo`

Os pacotes são locais ao `ramfs` por enquanto; a próxima evolução natural é conectar isso a um backend persistente em disco.

## 11. Macro-montador, montador, ligador e executor (fase acadêmica)

Foi adicionada uma toolchain completa em C++, gerada por `make toolchain` como `build/macro-montador`:

1. **MacroProcessor (1 passagem)**  
   - Processamento antes da montagem, acionado pelo módulo integrador.  
   - Suporte a **definições aninhadas** de macro e **chamadas aninhadas**.
   - Entrada: arquivo fonte; saída: arquivo fonte expandido.

2. **TwoPassAssembler (2 passagens)**  
   - Passo 1: tabela de símbolos e tamanhos.  
   - Passo 2: geração de objeto (`kernigham-obj-v1`) com código, símbolos, globais/externos e relocações.

3. **TwoPassLinker (2 passagens)**  
   - Passo 1: layout de módulos e resolução global.  
   - Passo 2: aplicação de relocações e geração de executável (`kernigham-exe-v1`).  
   - Modos:
     - `absolute`: ligação + relocação completa (ligador-relocador, endereço de carga conhecido).
     - `relocatable`: ligação com relocações pendentes para o carregador relocador.

4. **Executor (VM)**  
   - Executa o formato ligado, incluindo aplicação de relocação pendente no modo relocatable.

5. **Módulo principal integrador**
  - `./build/macro-montador build ...`
   - Orquestra macro -> asm -> link -> exec.

Arquivos de exemplo:
- `tools/examples/main.asm`
- `tools/examples/lib.asm`
