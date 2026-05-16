# Especificação técnica v0.1

## Escopo desta entrega
- Kernel freestanding em C++ com boot via GRUB2/Multiboot2.
- Inicialização mínima com ponto de entrada `_start` e `kernel_main`.
- Observabilidade por serial COM1 para diagnóstico de boot.
- Base de interrupções com IDT carregada e stub comum de tratamento.
- Timer periódico com PIC remapeada + PIT em 100 Hz.
- Base de segmentação com GDT e TSS carregadas.
- Esqueleto de syscall via `int 0x80` com dispatcher central.
- Parser inicial de mapa de memória Multiboot2 com telemetria por serial.
- VMM com paginação 4 KiB e mapeamento dinâmico básico.
- Heap virtual do kernel (bump allocator) com integração PMM/VMM.
- Scheduler round-robin inicial com troca de contexto por timer IRQ0.
- VFS mínimo com backend ramfs em memória.
- Modelo inicial de processos e threads de kernel.
- Espaço de endereçamento por processo com troca de `cr3`.

## Arquitetura (marco inicial)
- Bootstrap: i686 (Multiboot2) como base atual.
- Evolução alvo: x86_64 em UEFI para mídia live moderna e melhor compatibilidade com PCs atuais.
- Modo de execução atual: múltiplos processos de kernel com uma thread inicial por processo.
- Tratamento de erro atual: halt em loop com mensagem de diagnóstico.
- Interrupções: IDT com 256 entradas e handler genérico para validação de trap.
- IRQ0 (timer): contador de ticks do kernel atualizado por interrupção.
- Segmentação: GDT kernel/user inicial e TSS preparada para troca de privilégio futura.
- Tasking: tasks de kernel com stacks dedicadas e preempção por quantum.
- Identidade de execução: PID (processo) e TID (thread) expostos por syscall.
- Cada processo possui page directory próprio clonado das mappings de kernel.

## ABI mínima do kernel (próximos marcos)
1. `sys_write(fd, buf, len)`
2. `sys_exit(code)`
3. `sys_sleep(ms)`
4. `sys_open(path, flags)`
5. `sys_read(fd, buf, len)`
6. `sys_close(fd)`
7. `sys_getpid()`
8. `sys_gettid()`
9. `sys_proc_count()`
10. `sys_mkdir(path)`
11. `sys_getuid()`
12. `sys_getgid()`

### Status da ABI
- `sys_write` e `sys_exit` com implementação inicial no dispatcher.
- `sys_sleep` implementada com base em ticks do timer.
- `sys_sleep` integrada ao estado de task (sleep/wakeup pelo scheduler).
- `sys_open`, `sys_read` e `sys_close` implementadas para VFS/ramfs.
- `sys_getpid`, `sys_gettid` e `sys_proc_count` implementadas para introspecção de execução.
- `sys_mkdir` implementada para criação de diretórios no ramfs.
- `sys_getuid` e `sys_getgid` implementadas (credenciais iniciais de root).

## Modelo de memória (planejado)
1. PMM baseado em bitmap de frames.
2. VMM paginado (4 KiB pages).
3. Heap de kernel com allocator simples (first-fit).

### Status de memória
- Mapa de memória Multiboot2 já parseado e exibido no boot.
- PMM bitmap inicial implementado (alocação de frames físicos com reserva de low memory e kernel).
- Paginação ativada com identity map de bootstrap e suporte a `map_page`/`unmap_page`.
- Heap virtual inicial ativo em faixa dedicada do kernel.

## Drivers essenciais planejados
1. Timer (PIT/APIC).
2. Teclado PS/2.
3. Framebuffer/VGA texto.
4. Armazenamento inicial em ramfs/tarfs.

## Critérios de aceite v0.1
1. Gerar `kernel.elf` freestanding.
2. Gerar ISO bootável com GRUB2.
3. Executar no QEMU e imprimir mensagens de boot na serial.
4. Carregar IDT e capturar ao menos uma interrupção de software (`int 0x03`).
5. Configurar PIT e receber IRQ0 com contagem de ticks.
6. Carregar GDT/TSS sem falha durante bootstrap.
7. Ativar paginação sem quebrar bootstrap e validar mapeamento virtual.
8. Alocar memória no heap virtual do kernel.
9. Executar múltiplas tasks de kernel com round-robin por IRQ0.

## Rota de portabilidade (próxima fase)
1. Adotar um boot path UEFI/BIOS dual (preferência: UEFI x86_64 como caminho principal para pendrive live).
2. Separar dependências de arquitetura em `kernel/arch/x86` e `kernel/arch/x86_64`.
3. Migrar entrada, GDT/TSS, IDT, PIC/APIC, paging e syscalls para o modo longo de 64 bits.
4. Atualizar a imagem de boot para gerar ISO e pendrive live compatíveis com firmware moderno.
5. Manter a imagem i386 apenas como fallback de laboratório enquanto a nova porta amadurece.
