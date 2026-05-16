# kernigham Kernel

Kernel minimal em C++ (freestanding), com boot via GRUB2/Multiboot2.

## Pré-requisitos
- `gcc`, `g++`, `ld` (ou toolchain `i686-elf-*`)
- `grub-mkimage` (pacote GRUB para i386-pc)
- `xorriso` (usado para montar a ISO final)
- `qemu-system-i386`

## Build
```bash
make
```

## Gerar ISO
```bash
make iso
```

## Executar no QEMU
```bash
make run
```

## Estrutura
- `boot/`: entrada em ASM, linker e configuração do GRUB
- `kernel/`: núcleo C++ (entrypoint, serial e base de interrupções x86)
- `kernel/boot/`: parsing de estruturas de boot (Multiboot2)
- `kernel/mm/`: PMM bitmap, VMM paginado e heap virtual do kernel
- `kernel/task/`: scheduler round-robin e contexto de tasks de kernel
- `kernel/sync/`: primitivas de sincronização (`spinlock`, `LockGuard`)
- `kernel/syscall/`: dispatcher de syscalls (`int 0x80`)
- `kernel/fs/`: VFS mínimo com backend `ramfs`
- `docs/SPEC_V0.md`: especificação técnica da fase inicial
- `docs/build.md`: histórico técnico consolidado das implementações

## Estado atual
- Build do kernel (`make`) já funcional.
- Base de interrupções com PIC/PIT e tick via IRQ0 implementada.
- Base de GDT/TSS implementada para evolução de contexto de tarefas.
- Parser do mapa de memória Multiboot2 implementado com logs de regiões.
- PMM bitmap inicial implementado com teste de alocação de frame no boot.
- Paginação (VMM) habilitada com map/unmap e tradução virtual->físico.
- Heap virtual do kernel ativo, incluindo `operator new/delete` global.
- `sys_sleep` funcional com base no timer IRQ0.
- Scheduler round-robin com tasks de kernel já ativo em runtime.
- Scheduler evoluído para modelo inicial **processo/thread** (PID/TID distintos).
- Criação de processos de kernel com thread inicial (`create_process_task`).
- Cada processo agora possui **address space próprio** (page directory clonado do kernel).
- Scheduler troca `cr3` ao alternar processos.
- VFS/ramfs com caminhos aninhados e diretórios (`/bin/note.txt`) implementado.
- Syscalls de arquivo adicionadas: `open`, `read`, `close` e `write` para FDs de arquivo.
- Rootfs inicial com `/root`, `/home/root`, `/etc/passwd` e `/root/.profile`.
- Novas syscalls de processo/FS: `getpid`, `gettid`, `proc_count`, `mkdir`, `getuid`, `getgid`.
- Loader ELF já executa `/bin/init.elf` em ring3 com `int 0x80`.
- Geração de ISO (`make iso`) funcional no fluxo atual.

## Dependências no Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install -y xorriso qemu-system-x86 grub-pc-bin
```

## Toolchain acadêmica (macro-montador, montador, ligador e executor)

Implementada em C++ e compilada como `build/macro-montador`:

- **Processador de macros (1 passagem)**: suporta macros aninhadas e chamadas aninhadas.
- **Montador (2 passagens)**: gera objeto com símbolos e tabela de relocação.
- **Ligador (2 passagens)**:
  - `--mode absolute`: ligador-relocador com endereço de carga conhecido.
  - `--mode relocatable`: apenas ligação, relocação final para o carregador relocador.
- **Executor (VM)**: carrega e executa o código ligado.

Exemplo completo:

```bash
make toolchain
./build/macro-montador build \
  tools/examples/main.asm tools/examples/lib.asm \
  --out-dir build/toolchain --mode absolute --load-address 0x1000 --run
```
