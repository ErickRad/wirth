# Roadmap técnico do kernel

Este documento organiza a próxima etapa do kernel em uma ordem pragmática. A ideia é evitar ampliar a superfície do sistema antes de fechar os fundamentos de boot, memória, processo e armazenamento.

## Princípio de execução

1. Fechar o boot/runtime base primeiro.
2. Tornar memória e processo confiáveis antes de avançar para userland complexo.
3. Consolidar armazenamento e filesystem persistente antes de USB, rede e UI mais rica.
4. Só então expandir input, framebuffer, rede e o restante do ecossistema de userland.

## Fase 1: boot e runtime base

Objetivo: sair do boot mínimo e chegar a um kernel que entende o hardware básico e consegue reiniciar/desligar de forma controlada.

Itens:
- UEFI runtime services.
- PE loader correto.
- Long mode definitivo.
- ACPI parsing.
- APIC e IOAPIC.
- Reboot/shutdown via ACPI.
- EFI variable support.
- Framebuffer GOP estável.

Critério de pronto:
- boot estável em UEFI;
- enumeração de tabelas ACPI;
- interrupções baseadas em APIC;
- reboot/shutdown sem depender de hacks de plataforma.

## Fase 2: memória e processo

Objetivo: transformar o kernel em uma base que consegue sustentar processos reais, troca de contexto e memória virtual séria.

Itens:
- Heap real.
- Free real.
- Slab allocator.
- Buddy allocator.
- Virtual memory regions.
- `mmap` e `munmap`.
- Page fault handler completo.
- Demand paging.
- Copy-on-write.
- Shared memory.
- Userspace stack growth.
- Guard pages.
- Huge pages.
- Swap.
- Page cache.
- Memory reclaim.
- OOM killer.
- `fork`.
- `execve`.
- `waitpid`.
- Signals.
- Pipes.
- Sessions e process groups.
- Priorities.
- Futex.
- Mutexes, semaphores e condition variables.
- `epoll` / `poll` / `select`.
- Zombie reaping.
- Idle task.
- Kernel worker threads.
- ABI estável.
- `errno`.
- File descriptor table real.
- `dup` / `dup2`.
- `ioctl`.
- `fcntl`.
- `stat`.
- `lseek`.
- `chmod` / `chown`.
- `getcwd`.
- `rename`.
- `link` / `symlink` / `readlink`.
- Syscall/sysret.
- Context switch completo x86_64.
- FPU save/restore.
- SSE/AVX context switching.
- TLS.
- Per-CPU structures.
- Interrupt affinity.
- Kernel preemption.
- High resolution timers.
- TSC calibration.

Critério de pronto:
- processos ring3 com troca de contexto estável;
- memória virtual com falhas tratadas corretamente;
- API de syscall suficiente para userland real;
- kernel apto a rodar serviços e bibliotecas mais complexas.

## Fase 3: storage e filesystem persistente

Objetivo: tornar o disco uma base confiável para persistência, instalação e execução de userland.

Itens:
- Block layer.
- Partition parser.
- MBR.
- GPT.
- AHCI.
- NVMe.
- SATA.
- DMA support.
- Request queues.
- Disk scheduler.
- Removable media support.
- Persistent FS decente.
- VFS completo.
- Inode layer.
- Dentry cache.
- Mountpoints.
- Permissions.
- Ownership.
- Timestamps.
- Symlinks.
- Hardlinks.
- Journaling.
- Block cache.
- Buffered IO.
- Async writeback.
- Ext2/ext4-like FS.
- FAT32 driver.
- `fsck` tooling.

Critério de pronto:
- disco persistente montado no boot;
- leitura/escrita confiáveis;
- árvore de arquivos estável para sistema e userland;
- caminho claro para recuperação e integridade.

## Fase 4: USB, input, vídeo e rede

Objetivo: completar periféricos e I/O interativo depois que o núcleo já sustenta boot, memória e disco.

Itens:
- USB/xHCI.
- Transfer rings completos.
- Event rings corretos.
- TRB cycle handling correto.
- Device enumeration robusta.
- USB hubs.
- HID.
- Keyboard.
- Mouse.
- USB storage estável.
- Interrupt transfers.
- Isochronous transfers.
- Hotplug.
- MSI/MSI-X.
- PCI enumeration completa.
- Driver model.
- Device manager.
- Framebuffer driver.
- Terminal driver.
- PS/2.
- Serial robusto.
- RTC.
- HPET.
- NIC driver.
- Packet buffers.
- Ethernet.
- ARP.
- IPv4.
- ICMP.
- UDP.
- TCP.
- Sockets.
- DNS.
- DHCP.
- Routing.
- Loopback.
- TTY subsystem.
- PTY.
- ANSI escape sequences.

Critério de pronto:
- input e display utilizáveis;
- USB confiável;
- rede mínima funcional;
- console interativo consistente.

## Fase 5: userland e distribuição

Objetivo: transformar o kernel em um sistema utilizável de ponta a ponta.

Itens:
- Shell userland real.
- Login manager.
- Keyboard layouts.
- Unicode.
- Framebuffer terminal.
- Users/groups.
- Capability model.
- Secure syscall validation.
- SMEP/SMAP.
- NX.
- ASLR.
- Stack canaries.
- Init process.
- Coreutils.
- Package manager real.
- Compiler/toolchain integrada.
- Text editor.
- Service manager.
- Logging daemon.
- Kernel panic reports.
- Stack traces.
- Symbol resolver.
- Tracing.
- Profiling.
- Crash dumps.
- GDB stub.
- Performance counters.
- Watchdog.
- Recovery mode.

Critério de pronto:
- boot até login sem intervenção manual;
- ferramentas básicas do usuário funcionando;
- diagnóstico, recuperação e manutenção disponíveis.

## Ordem recomendada de implementação

1. ACPI + APIC/IOAPIC + runtime UEFI.
2. Page fault completo + heap real + regiões de memória.
3. `fork` / `execve` / `waitpid` + syscall/sysret + contexto x86_64.
4. Block layer + particionamento + AHCI/NVMe.
5. FS persistente com mountpoints e permissões.
6. xHCI + HID + terminal framebuffer.
7. Rede básica.
8. Userland completo.

## Plano de execução em 3 marcos

### Marco 1: boot/runtime e base de arquitetura

Entregas:
- UEFI runtime services funcionando.
- Long mode definitivo e loader PE correto.
- ACPI parseado e exposto ao kernel.
- APIC/IOAPIC no caminho normal de interrupções.
- Reboot/shutdown via ACPI ou caminho equivalente estável.
- GOP/framebuffer estável para debug visual.

Dependências principais:
- [boot/boot64.S](boot/boot64.S)
- [boot/linker64.ld](boot/linker64.ld)
- [kernel/kmain64.cpp](kernel/kmain64.cpp)
- [kernel/arch/x86_64/gdt.cpp](kernel/arch/x86_64/gdt.cpp)
- [kernel/arch/x86_64/interrupts.cpp](kernel/arch/x86_64/interrupts.cpp)
- [kernel/arch/x86/gdt.cpp](kernel/arch/x86/gdt.cpp)
- [kernel/arch/x86/interrupts.cpp](kernel/arch/x86/interrupts.cpp)
- [kernel/arch/x86/pic.cpp](kernel/arch/x86/pic.cpp)
- [kernel/arch/x86/pit.cpp](kernel/arch/x86/pit.cpp)
- [kernel/boot/multiboot2.cpp](kernel/boot/multiboot2.cpp)
- [tools/efiImageBuilder/main.cpp](tools/efiImageBuilder/main.cpp)

### Marco 2: memória, processo e syscall

Entregas:
- Heap real com free utilizável.
- Regiões de memória virtual explícitas.
- Page fault completo.
- `fork`, `execve`, `waitpid` e `syscall/sysret`.
- Troca de contexto completa em x86_64.
- FPU/SSE/AVX save/restore e TLS.
- Preempção, timers de alta resolução e TSC calibrado.

Dependências principais:
- [kernel/mm/pmm.cpp](kernel/mm/pmm.cpp)
- [kernel/mm/vmm.cpp](kernel/mm/vmm.cpp)
- [kernel/mm/heap.cpp](kernel/mm/heap.cpp)
- [kernel/mm/new_delete.cpp](kernel/mm/new_delete.cpp)
- [kernel/task/scheduler.cpp](kernel/task/scheduler.cpp)
- [kernel/task/scheduler64_stub.cpp](kernel/task/scheduler64_stub.cpp)
- [kernel/syscall/syscall.cpp](kernel/syscall/syscall.cpp)
- [kernel/loader/userland.cpp](kernel/loader/userland.cpp)
- [kernel/loader/userland.hpp](kernel/loader/userland.hpp)
- [kernel/user_safety.cpp](kernel/user_safety.cpp)
- [kernel/user_safety.hpp](kernel/user_safety.hpp)
- [kernel/arch/x86_64/interrupts.cpp](kernel/arch/x86_64/interrupts.cpp)
- [kernel/arch/x86_64/gdt.cpp](kernel/arch/x86_64/gdt.cpp)

### Marco 3: storage, FS e periféricos essenciais

Entregas:
- Block layer estável.
- Particionamento MBR/GPT.
- AHCI e NVMe utilizáveis.
- FS persistente com mountpoints e permissões.
- xHCI/HID/input estáveis.
- Framebuffer terminal e shell com console útil.
- Rede básica e caminhos iniciais de userland.

Dependências principais:
- [kernel/block.cpp](kernel/block.cpp)
- [kernel/block.hpp](kernel/block.hpp)
- [kernel/storage.cpp](kernel/storage.cpp)
- [kernel/storage.hpp](kernel/storage.hpp)
- [kernel/ahci.cpp](kernel/ahci.cpp)
- [kernel/ahci.hpp](kernel/ahci.hpp)
- [kernel/nvme.cpp](kernel/nvme.cpp)
- [kernel/nvme.hpp](kernel/nvme.hpp)
- [kernel/fs/vfs.hpp](kernel/fs/vfs.hpp)
- [kernel/fs/ramfs.cpp](kernel/fs/ramfs.cpp)
- [kernel/efifs.cpp](kernel/efifs.cpp)
- [kernel/xhci.cpp](kernel/xhci.cpp)
- [kernel/xhci.hpp](kernel/xhci.hpp)
- [kernel/usb_mass_storage.cpp](kernel/usb_mass_storage.cpp)
- [kernel/usb_mass_storage.hpp](kernel/usb_mass_storage.hpp)
- [kernel/pci.cpp](kernel/pci.cpp)
- [kernel/pci.hpp](kernel/pci.hpp)
- [kernel/video.cpp](kernel/video.cpp)
- [kernel/video.hpp](kernel/video.hpp)
- [kernel/shell.cpp](kernel/shell.cpp)

## Critério de corte entre marcos

- Não começar o Marco 2 antes de o Marco 1 bootar com estabilidade e interrupções previsíveis.
- Não expandir USB/input/rede antes de memória, processo e bloco estarem confiáveis.
- Não chamar userland de pronto enquanto o caminho de `execve`, FDs e FS persistente ainda depender de atalhos do kernel.

## Observação prática

USB/xHCI, input e framebuffer parecem urgentes porque afetam a experiência imediata, mas eles dependem de uma base confiável de boot, interrupção, memória e bloco. Se antecipados demais, viram uma cadeia longa de bugs sem uma plataforma estável para depuração.