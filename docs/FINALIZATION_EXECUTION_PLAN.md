# Plano Consolidado de Finalizacao do VibeOS

Data da consolidacao: 2026-03-29

## Premissas desta consolidacao

- `VibeLoader` deve ser tratado como 100% funcional para o plano principal. O que ficou em `docs/VIBELOADER_PLAN.md` e `docs/VIBELOADER_BIOS_DEBUG_HANDOFF.md` entra como backlog documental/diagnostico, nao como bloqueio de entrega.
- O kernel continua sendo o `VibeKernel`; a meta de compatibilidade 1:1 vale para drivers, servicos, userland e apps herdados de `compat`.
- O objetivo deste documento e juntar o que ainda falta nos outros `.md` em uma fila executavel, sem misturar backlog opcional com bloqueio real.

## O que ja pode sair da frente

- Bootloader / pipeline de boot: tratado como fechado.
- Base AppFS / modularizacao principal: funcional, com pendencias de smoke e aliases.
- GPU em QEMU: suficientemente valida para nao ser o primeiro bloqueio de entrega.
- Port base de apps CLI e jogos de `compat`: ja existe massa critica boa o bastante para mudar o foco para hardware, UX e integracao.

## O que ainda falta de verdade

### Bloco 1: rede e internet real

Origem principal:
- `docs/NETWORK_AUDIO_PANEL_PLAN.md`
- `docs/COMPAT_PLAN.md`
- `docs/COMPAT_PLAN2.md`

Falta fechar:
- driver cabeado real com attach/enum/pacotes de verdade vindos de `compat`
- DHCP + DNS funcionando
- caminho de socket/rede real em vez de control-plane parcial
- Wi-Fi com scan, senha e conexao
- navegador integrado ao desktop usando a rede real

### Bloco 2: audio real em hardware

Origem principal:
- `docs/NETWORK_AUDIO_PANEL_PLAN.md`

Falta fechar:
- `compat-azalia` robusto em notebook real
- captura real validada fora do fallback de QEMU
- `compat-uaudio` sair de substrate/MVP e virar backend confiavel
- matriz real de hardware com backend certo por maquina

### Bloco 3: video nativo e validacao em hardware

Origem principal:
- `docs/VIDEO_BACKEND_REWRITE_PLAN.md`

Falta fechar:
- primeiro backend real de hardware promovido com seguranca
- validacao widescreen real
- mais testes fora do QEMU
- decidir escopo minimo real de i915/radeon/nouveau sem inflar promessa

### Bloco 4: fechamento de integracao modular

Origem principal:
- `docs/MODULAR_APP_INTEGRATION_PLAN.md`

Falta fechar:
- smoke test por app
- aliases e paths explicitos (`/bin/...`, `/compat/bin/...`)
- assets de DOOM/Craft na matriz completa

### Bloco 5: compat / runtime / POSIX que ainda falta

Origem principal:
- `docs/COMPAT_PLAN.md`
- `docs/COMPAT_PLAN2.md`
- `docs/java.md`

Falta fechar:
- semantica POSIX faltante em pontos do VFS/runtime
- execucao nativa de binarios a partir do VFS quando isso deixar de depender do caminho atual de AppFS
- dependencias de plataforma para JVM

### Bloco 6: SMP e robustez multiprocessada

Origem principal:
- `docs/smp.md`

Falta fechar:
- validacao confiavel de `2+ CPUs` em QEMU/hardware
- decidir se o proximo degrau e suporte a topologia ACPI/MP mais amplo ou outra forma de detectar SMP nas maquinas alvo
- endurecer o que ja entrou para nao ficar dependente de um unico ambiente de QEMU

### Bloco 7: UX de input e desktop

Origem principal:
- observacao do tree atual
- necessidade nova deste pedido

Falta fechar:
- scroll wheel do mouse no kernel
- scroll wheel atravessando syscall/ABI ate o userland
- scroll funcionando de verdade nas interfaces

## Ordem recomendada para finalizar

### Etapa 0: congelar o que ja esta bom

- tratar `VibeLoader` como fechado e parar de gastar ciclo no bootloader
- manter GPU/QEMU/AppFS verdes durante qualquer rodada grande
- toda rodada grande precisa terminar com `boot.img` gerada e pelo menos smoke de boot

### Etapa 1: melhorar UX base imediatamente

- implementar scroll wheel no mouse
- plugar scroll no desktop, start menu, listas e dialogs
- usar isso como melhoria de usabilidade e como exercicio de ABI/input sem risco alto

### Etapa 2: fechar audio de hardware real

- atacar `compat-azalia` ate sair do estado fraco em notebook real
- promover `compat-uaudio` para fallback USB util de verdade
- consolidar a ordem de fallback por backend

### Etapa 3: fechar rede real

- transformar o caminho atual de Ethernet em datapath real
- subir DHCP e DNS
- depois subir Wi-Fi e UX de senha/conexao
- por fim integrar navegador real ao desktop

### Etapa 4: fechar validacao modular e runtime

- smoke per-app
- aliases e paths explicitos
- jogos com assets
- reduzir gaps POSIX mais gritantes

### Etapa 5: fechar hardware/video/SMP

- promover um backend de video real com escopo curto e validado
- fechar a matriz de modos reais
- retomar SMP com deteccao/topologia suficiente para `2+ CPUs` no ambiente de validacao

## Definicao pratica de "acabou"

O projeto pode ser considerado fechado quando, ao mesmo tempo:

- Ethernet sobe, recebe lease e resolve DNS
- Wi-Fi lista redes, pede senha e conecta
- audio toca em QEMU e em hardware real no backend correto
- fallback USB audio e util de verdade
- navegador abre pelo desktop e usa a rede real
- apps modulares principais passam em smoke
- video sobe em QEMU e em pelo menos um backend real de hardware
- mouse scroll funciona no desktop e nas listas/dialogs principais

## Plano especifico: scroll do mouse

### Meta

Adicionar wheel scroll no caminho completo:

- controlador PS/2 ou outro backend de mouse
- syscall/ABI
- desktop/UI
- apps que dependem de lista/viewport

### Estado atual

- `mouse_state` so carrega `x`, `y`, `dx`, `dy` e `buttons`
- o driver PS/2 do kernel usa pacote de 3 bytes
- o desktop ja possui varios pontos de scroll manual por drag/teclado, mas nao recebe delta de wheel

### Etapas tecnicas

#### Fase A: kernel/input

- detectar e habilitar protocolo de wheel no mouse PS/2 (`IntelliMouse`, pacote de 4 bytes)
- estender `mouse_state` com delta de wheel
- garantir que a fila de eventos preserve esse campo

#### Fase B: ABI/syscall

- propagar o novo campo em `headers/include/userland_api.h`
- atualizar syscall/input service/runtime sem quebrar o polling atual
- manter compatibilidade com mouse sem wheel

#### Fase C: desktop e UI base

- ligar wheel no start menu
- ligar wheel em listas de apps/resultados
- ligar wheel em file dialogs
- ligar wheel em areas com scroll futuro do file manager/editor/terminal quando houver viewport/lista adequada

#### Fase D: apps e compat wrappers

- verificar jogos/apps que ja esperam callback de scroll
- avaliar se o compat de GLFW deve expor wheel para apps como Craft

#### Fase E: validacao

- QEMU com wheel
- mouse USB/PS2 real
- teste de regressao de clique/movimento sem wheel

### Criterio de pronto para scroll

- wheel sobe no kernel sem quebrar movimento/clique
- desktop responde a scroll em pelo menos start menu, listas e dialogs
- regressao zero para mouse sem wheel

## Primeira sequencia de execucao recomendada

1. Implementar scroll wheel do mouse.
2. Fechar `compat-azalia` no hardware real alvo.
3. Fazer Ethernet real com DHCP/DNS.
4. Integrar navegador e UX de rede.
5. Fechar smoke modular e gaps POSIX mais visiveis.
6. Voltar para video real e SMP com matriz de hardware/QEMU mais honesta.
