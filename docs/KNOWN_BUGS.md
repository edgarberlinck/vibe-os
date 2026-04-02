# Known Bugs

Este documento lista os problemas conhecidos mais críticos na base atual.
A migração para microkernel foi concluída, mas ainda existem regressões em várias áreas do desktop.

## Status geral

- Migração para microkernel: concluída
- Estabilidade de vídeo: instável em alguns casos de entrada e apresentação
- Áudio: sem saída de som após migração
- Rede: implementação incompleta / falta de integração completa
- Jogos: problemas de recursos e caminhos de dados

## Bugs atuais

### 1. Teclado está saindo do modo de vídeo para o shell e causando piscar de tela

- Comportamento: pressionar teclas durante o desktop faz o foco escapar para o shell de texto ou para um modo de apresentação indesejado.
- Sintoma: a tela pisca e o desktop perde o estado visual esperado.
- Possível causa: falha na linha de path de apresentação de vídeo ou no fallback de `sys_video_present_submit()`.

### 2. Som não está saindo

- Comportamento: o desktop inicia normalmente, mas não há áudio de sistema ou de apps.
- Sintoma: mixagem de áudio inativa e sem saída de backend.
- Áreas afetadas: reprodução de áudio do desktop, apps de som e notificações.

### 3. Internet / rede está incompleta

- Comportamento: a página de configuração de rede abre, mas a integração de scan/conexão não está completa.
- Sintoma: não é possível conectar de forma confiável a redes Wi-Fi ou aplicar perfis salvos.
- Áreas afetadas: applet de rede, backend de gerenciamento de perfil e reconciliação de status.

### 4. Doom não encontra os arquivos do jogo

- Comportamento: ao iniciar Doom, o app falha na localização dos assets do port.
- Sintoma: erro de "arquivo não encontrado" ou carregamento incompleto do jogo.
- Possível causa: caminhos de dados estáticos não resolvidos ou assets não empacotados corretamente.

### 5. Craft está alterando as cores do desktop

- Comportamento: o jogo Craft modifica o tema do desktop ou a paleta global durante a execução.
- Sintoma: após sair do jogo, o desktop pode ficar com cores erradas ou inconsistentes.
- Possível causa: compartilhamento de estado gráfico ou de paleta entre apps e desktop.

### 6. Ports BSD de jogos abrem um terminal em vez de iniciar o jogo

- Comportamento: os ports BSD de jogos são iniciados, mas acabam abrindo uma janela de terminal em vez de carregar o jogo.
- Sintoma: todos os ports BSD exibem um terminal vazio ou um prompt de shell em vez da interface do jogo.
- Possível causa: caminhos de stub ou launch incorretos para os ports, redirecionamento para o terminal como fallback ou falha na detecção do binário do jogo.

## Observações

- Esses bugs devem ser priorizados na ordem em que impactam mais diretamente a experiência do usuário.
- A migração para microkernel está concluída, então o foco agora é estabilizar a camada de apresentação, entrada, áudio e integração de apps.
- Documentar cada item com reprodução exata e local de código ajuda no rastreamento e correção.
