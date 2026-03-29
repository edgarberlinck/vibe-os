# VibeOS

VibeOS e um sistema operacional x86 BIOS em 32-bit com bootloader proprio, kernel hibrido orientado a servicos, AppFS modular para apps e uma arvore grande de ports reaproveitados de `compat/`.

O repositorio hoje ja nao e mais a demo minima original. O estado real do tree e:

- boot BIOS funcional com imagem particionada (`MBR -> FAT32 boot -> stage2 -> kernel`)
- kernel com scheduler, memoria paginada, ELF loader, VFS, IPC e servicos bootstrap
- shell externa via `userland.app` carregada do AppFS no boot normal
- desktop grafico, terminal, file manager, editor, task manager, jogos e apps modulares
- stack de audio e video em evolucao, com validacao forte em QEMU e rodadas em hardware real
- SMP em bring-up ativo, com foco atual em estabilizar hardware real antigo

## Estado atual

Fechado ou suficientemente estavel:

- bootloader e pipeline de boot
- imagem particionada + AppFS + area de persistencia
- boot modular `init -> userland.app`
- desktop e base de apps modulares
- scroll wheel do mouse de ponta a ponta no kernel e no desktop
- matriz principal de QEMU para boot/apps/audio/video

Ainda em fechamento:

- rede real completa
- audio robusto em hardware real, especialmente `compat-azalia`
- video nativo em hardware real fora do QEMU
- SMP estavel em hardware real multiprocessado
- alguns gaps de runtime/POSIX e smoke por app

O plano consolidado atual esta em [docs/FINALIZATION_EXECUTION_PLAN.md](/home/mel/Documentos/vibe-os/docs/FINALIZATION_EXECUTION_PLAN.md).

## Arquitetura

### Boot

- `boot/mbr.asm`: MBR BIOS
- `boot/boot.asm`: VBR/FAT32 bootstrap
- `boot/stage2.asm`: loader principal e handoff para o kernel

### Kernel

- `kernel/`: kernel principal
- `kernel/microkernel/`: limites de servico e bridges atuais
- `kernel/process/`: processos e scheduler
- `kernel/memory/`: paging, physmem e heap
- `kernel/drivers/`: input, video, storage, timer, debug, PCI, USB
- `kernel/cpu/`, `kernel/apic.c`, `kernel/smp.c`: topologia, LAPIC e bring-up SMP

### Userland e apps

- `userland/userland.c`: `userland.app`, shell externa autostartada no boot
- `userland/modules/`: runtime e bibliotecas comuns
- `userland/applications/`: desktop e apps nativos/modulares
- `applications/ported/`: utilitarios e apps portados

### Compat

- `compat/`: codigo herdado/reaproveitado de base UNIX/BSD para ports e referencias

### Docs importantes

- [docs/QUICK_BUILD.md](/home/mel/Documentos/vibe-os/docs/QUICK_BUILD.md): build e comandos rapidos
- [docs/MICROKERNEL_MIGRATION.md](/home/mel/Documentos/vibe-os/docs/MICROKERNEL_MIGRATION.md): estado real da migracao
- [docs/NETWORK_AUDIO_PANEL_PLAN.md](/home/mel/Documentos/vibe-os/docs/NETWORK_AUDIO_PANEL_PLAN.md): audio/rede
- [docs/VIDEO_BACKEND_REWRITE_PLAN.md](/home/mel/Documentos/vibe-os/docs/VIDEO_BACKEND_REWRITE_PLAN.md): video
- [docs/smp.md](/home/mel/Documentos/vibe-os/docs/smp.md): SMP

## Pre-requisitos

Minimo pratico:

- `nasm`
- `make`
- `python3`
- QEMU (`qemu-system-i386` ou `qemu-system-x86_64`)
- `mtools` e `mkfs.fat`/equivalente para gerar a imagem FAT32

Toolchain:

- recomendado: `i686-elf-*`
- fallback suportado em Linux: toolchain host GNU 32-bit (`gcc`, `ld`, `objcopy`, `nm`, `ar`, `ranlib`)

## Build

Build principal:

```bash
make
```

Isso gera, entre outros:

- `build/kernel.bin`
- `build/kernel.elf`
- `build/data-partition.img`
- `build/boot.img`

Referencia curta de comandos: [docs/QUICK_BUILD.md](/home/mel/Documentos/vibe-os/docs/QUICK_BUILD.md).

## Rodando no QEMU

Execucao normal:

```bash
make run
```

O perfil padrao de `make run` agora e propositalmente mais proximo de um notebook antigo classe T61:

- `-cpu core2duo`
- `-smp 2,sockets=1,cores=2,threads=1,maxcpus=2`
- `-machine pc`
- `-vga std`

Exemplos de override:

```bash
make run QEMU_RUN_SMP=1
make run QEMU_RUN_CPU=pentium
make run QEMU_RUN_MACHINE=q35
```

Debugs uteis:

```bash
make run-debug
make run-headless-debug
make run-headless-core2duo-debug
make run-headless-ahci-debug
```

## Fluxo de boot atual

O fluxo esperado hoje e:

1. BIOS carrega a imagem e entra pelo caminho `MBR -> FAT32 boot`.
2. `stage2` prepara o ambiente e entrega controle ao kernel.
3. O kernel sobe servicos bootstrap.
4. O `init` tenta carregar `userland.app` do AppFS.
5. A shell externa entra como caminho normal de boot.
6. `startx` sobe o desktop grafico.

O shell embutido permanece como fallback/rescue path, nao como steady-state esperado.

## O que ja existe no runtime

- shell externa modular no AppFS
- desktop grafico e launcher de apps
- terminal, editor, file manager, task manager, calculator, image viewer, audio player
- jogos nativos e ports, incluindo DOOM/Craft
- layout de teclado em runtime (`loadkeys`)
- scroll wheel entregue do PS/2 ate userland/desktop
- boot e smoke automatizados em varias matrizes de QEMU

## Validacao

Alvos uteis:

```bash
make validate-phase6
make validate-smp
make validate-audio-stack
make validate-audio-hda-startup
make validate-gpu-backends
```

Esses fluxos escrevem relatorios em `build/`.

## Hardware real

Para iterar no bootloader sem regravar a imagem inteira, existe um fluxo de baixo desgaste documentado em [docs/QUICK_BUILD.md](/home/mel/Documentos/vibe-os/docs/QUICK_BUILD.md).

Resumo:

```bash
make build/stage2.bin build/boot.bin
python3 tools/patch_boot_sectors.py --target /dev/sdX --vbr build/boot.bin --stage2 build/stage2.bin
```

Se voce mudou `kernel.bin`, assets ou o conteudo FAT32/AppFS, gere `build/boot.img` de novo.

## Limitacoes honestas

- BIOS legado apenas, sem UEFI
- o kernel ainda e um hibrido: existem limites de servico reais, mas parte do backend ainda vive em bridges kernel-side
- SMP ainda esta em estabilizacao para hardware real
- rede real completa ainda nao esta fechada
- audio em notebook real ainda exige endurecimento por chipset/backend
- video real fora do QEMU ainda esta em consolidacao

## Estrutura resumida

```text
.
├── boot/
├── build/
├── compat/
├── docs/
├── headers/
├── kernel/
├── kernel_asm/
├── linker/
├── tools/
├── userland/
└── applications/
```

## Licenca

O repositorio usa GPLv3 no root em [LICENSE](vibe-os/LICENSE).

Arvores importadas de terceiros dentro de `compat/`, `lang/vendor/` e similares podem manter suas licencas originais. Veja tambem [THIRD_PARTY_LICENSES.md](vibe-os/THIRD_PARTY_LICENSES.md).
