<!-- Oracis project page -->
<p align="center">
  <img src="assets/icon/oracis.svg" alt="Ícone do Oracis: estrela de platina e anel celeste" width="160">
</p>

<h1 align="center">Oracis</h1>

<p align="center">Um projeto independente de port nativo de Pokémon Platinum para plataformas modernas.</p>

<p align="center"><strong>Reconstruindo Sinnoh para além do hardware original.</strong></p>

> **Status:** Pokémon Platinum USA Rev 1 foi validado, extraído e recompilado localmente em 19/11/2026. O NDSRecomp fixado gerou os bancos C/dispatch ARM9 e ARM7: 78 funções ARM9 e 957 ARM7 (incluindo 8 Thumb). Os quatro arquivos C gerados (corpo e dispatch de cada CPU) foram compilados localmente em C99 contra a ABI Oracis e agrupados em objetos; os objetos e bancos permanecem fora do Git e da CI. Ainda não foram ligados ao runtime, registrados no dispatcher nem executados. A varredura ARM9 terminou por limite de passos com 12 leituras não mapeadas e 10 alvos indiretos desconhecidos; ARM7 terminou ao sair do módulo com 388 leituras não mapeadas e 30 alvos indiretos desconhecidos: a cobertura permanece parcial. O CP15 ARM9 tem suporte parcial a ID, TCM size, ITCM/DTCM e load modes; MPU/cache seguem pendentes. O timing uncached ARM9 cobre ITCM, Main RAM, WRAM, BIOS, os primeiros 4 KB de I/O, Palette/VRAM e OAM; refill de pipeline ARM9/ARM7 está implementado para essas classes, com Palette/VRAM no ARM9 e somente VRAM C/D atribuída ao ARM7 como Work RAM, custos distintos por estado ARM/Thumb e reclassificação ao cruzar regiões. `NdsVramControl` mapeia os nove bancos A-I em MST=0/LCDC para acesso direto ARM9; adiciona também as janelas CPU de BG/OBJ: A-D/E/F/G em MST=1/Main BG, A/B/E/F/G em MST=2/Main OBJ (F/G usam OFS.0 e OFS.1 para posicionar janelas de 16 KiB), C/D em MST=4/Sub BG/Sub OBJ, H/I em MST=1/Sub BG e I em MST=2/Sub OBJ. Colisões entre aliases que disputam o mesmo endereço CPU são recusadas transacionalmente. C/D também podem ser atribuídos ao ARM7 em MST=2/Work RAM. `NdsVideoMemory` mapeia 2 KiB de paletas padrão em `0x05000000` e 2 KiB de OAM em `0x07000000`, com quatro paletas de 512 B e duas tabelas OAM de 1 KiB acessíveis somente pelo ARM9; STRB é ignorado e STRH/STR são aceitos. O PPU, a avaliação de OAM/sprites e a composição 2D ainda não consomem essas regiões. `NdsDisplayControl` mantém `DISPCNT` e `BG0CNT–BG3CNT` dos motores A/B, modela `DISPSTAT/VCOUNT` compartilhados pelos CPUs e gera flags/IRQs de scanline sincronizados ao relógio dual-core. HBlank usa 1606 ciclos (ARM9) e 1613 (ARM7); VBlank cobre as linhas 192–261. Os oito `BGxHOFS/BGxVOFS` por engine também estão armazenados como valores de 9 bits, com registradores write-only de 16 bits. Os coeficientes PA/PB/PC/PD e os pontos X/Y de BG2/BG3 também são armazenados com formatos signed 8.8 e signed 19.8/28 bits, respectivamente. O estado CPU de WIN0/WIN1, WININ/WINOUT, MOSAIC, BLDCNT, BLDALPHA e BLDY também está armazenado com máscaras e permissões por registrador. O módulo `Nds2dEffects` avalia a região de janela por pixel, combina os masks com os enables master, calcula coordenadas de amostragem de mosaic e aplica alpha/brightness em RGB555. O novo `Nds2dTextBgFetcher` produz uma scanline de 256 pixels para cada BG de texto habilitado, lendo character/map/palette diretamente das regiões BG VRAM e palette RAM mapeadas. Ele suporta tiles 8×8, mapas por screen blocks, 4bpp/8bpp, flips, scroll, mosaic e transparência por índice zero. O `Nds2dTextBgFetcher` gera scanlines para BGs de texto 4bpp/8bpp. O `Nds2dAffineBgFetcher` rasteriza os BGs affine clássicos BG2/BG3 nos modos 1, 2 e 4, e os BGs affine estendidos por tiles nos modos 3/4 (BG3) e 5 (BG2/BG3). O formato clássico usa mapas quadrados de índices de 8 bits; o estendido usa entradas de mapa de 16 bits, índices de tile de 10 bits e flips horizontal/vertical, ambos com tiles 8bpp. Os dois caminhos usam coeficientes PA/PB/PC/PD signed 8.8, referências BGxX/BGxY signed 19.8, acumuladores PB/PD por scanline, overflow wrap/transparent e mosaic; paletas estendidas e bitmaps continuam fail-closed. O acumulador interno avança junto ao VCOUNT e escritas de referência durante a imagem substituem a origem da linha atual; a cópia das referências no limite de VBlank ainda usa um modelo simplificado de frame boundary. O `Nds2dObjFetcher` lê as duas tabelas OAM e rasteriza sprites tiled 4bpp/8bpp nas Engines A/B, com mapeamento 1D/2D, formas/tamanhos, clipping, flips não affine, mosaic, transparência, semitransparência, OBJ-window e matrizes affine signed 8.8 por pixel, incluindo double-size. Preserva os dois pixels OBJ mais próximos por X; o compositor ordena ambos com BGs e permite blending OBJ-sobre-OBJ. Os testes cobrem BG affine nas duas engines, identidade, escala, rotação com PB negativo/PC positivo, acumuladores PB/PD entre linhas, referência negativa, wraparound, mosaic e integração ao compositor, além dos testes de OBJ affine e segundo OBJ. Continuam pendentes BG bitmap estendidos (indexados 8bpp e direct-color 16bpp), bitmap OBJ, paletas estendidas, BG0 3D, limites de avaliação por scanline, arbitragem/timing de fetch PPU e apresentação final SDL/Android. Nenhum primeiro quadro ou boot do jogo foi comprovado. Entradas não suportadas retornam diagnóstico em vez de gerar pixels falsos. O timing DATA agora cobre parcialmente Main RAM, WRAM/BIOS/I/O/OAM, Palette/VRAM no ARM9, VRAM C/D como Work RAM no ARM7 e TCM normal, com conversão de ciclo ARM9 2:1; a ABI não separa loads/stores, então TCM load mode, Gamecard/GBA slot, Wi-Fi e N32 ARM7 VRAM não sequencial seguem fail-closed. Multiply ARM7TDMI por early termination e combinação de ciclos ARM7 (classes documentadas de CODE/DATA, sobreposição Main RAM e contexto da instrução/acesso) estão implementados; cache, contenção, direção load/store na ABI TCM e wait states dinâmicos permanecem pendentes. A ABI de observabilidade inclui um buffer circular de 512 eventos por CPU para tracing e transferências, com contador de eventos descartados. A CI do commit `9c7580d` passou nos quatro jobs: Desktop Linux com **40/40 CTests**, smoke de bancos sintéticos **1/1** e tooling Python **15 testes**; runtime ARM **40/40 CTests**; Android ARM64 e compilador estático NDSRecomp também passaram (**6/6 testes** do compilador). Os testes agora incluem fetch BG texto, OBJ não affine e affine nas duas engines, retenção dos dois objetos superiores, prioridade, OBJ-window e blending com segundo OBJ como alvo. Nenhuma ROM, segmento ou banco comercial entrou no Git ou na CI.




## O projeto

Oracis busca executar Pokémon Platinum como código nativo em computadores modernos. O projeto utiliza recompilação estática dos códigos ARM7/ARM9 e um runtime independente para os serviços necessários do Nintendo DS. Não é um emulador com uma CPU guest interpretada como caminho principal.

## Direção técnica

- **NDSRecomp estático** para gerar bancos C ARM7/ARM9, em checkout externo e revisão fixada.
- **C99 + C++20** para bancos gerados e runtime independente.
- **CMake + Ninja** para o build.
- **SDL3** para janela, eventos, controles, toque e saída de áudio.
- **Python** para extração local e validação da cópia de jogo.
- **Rust** permanece apenas no gerador isolado do ícone.

Consulte [docs/TECH_STACK.md](docs/TECH_STACK.md), [docs/NDSRECOMP_INTEGRATION.md](docs/NDSRECOMP_INTEGRATION.md), [documentação do scheduler dual-core](docs/DUAL_CORE_SCHEDULER.md), o [mapa da ABI dos bancos](docs/RECOMP_ABI_MAP.md), o [contrato GuestMemory](docs/GUEST_MEMORY.md), a [documentação parcial do ARM9 TCM](docs/ARM9_TCM.md), o [timing parcial de busca ARM9](docs/ARM9_TIMING.md) e a [decisão ADR 0001](docs/adr/0001-ndsrecomp-static-compiler.md).

## Estado atual

A primeira revisão-alvo é **Pokémon Platinum USA Rev 1**, identificada localmente por SHA-1. ROM e arquivos derivados permanecem fora do Git, da CI e das releases.

O bootstrap do NDSRecomp está fixado no commit `14b7e15023e9affc9dc375e4e611ac703f5c76ab` e limita o checkout a `recompiler/`, `cmake/` e ao núcleo ARM MIT. O runtime/oracle/frontend upstream e os componentes derivados de melonDS não são incorporados.

O Oracis já contém:
- estado ARM com registradores bancados e asserts de layout C99;
- GuestMemory: mapa de RAM NDS retail, Main RAM compartilhada/mirrors, WRAM ARM7 privada, quatro modos WRAMCNT e leitura ARM7 de WRAMSTAT; controlador parcial VRAM `VRAMCNT_A-I` (ARM9, 8-bit/write-only) e `VRAMSTAT` (ARM7, 8-bit/read-only), com os nove bancos mapeados em MST=0/LCDC nos endereços `0x06800000`–`0x068A3FFF`; acesso CPU a MST=1/Main BG dos bancos A-D/E/F/G, MST=2/Main OBJ A/B/E/F/G, MST=4/Sub BG/Sub OBJ C/D, MST=1/Sub BG H/I e MST=2/Sub OBJ I; colisões de alias são rejeitadas; stores de 8 bits em VRAM ARM9 são ignorados e escritas de 16/32 bits são aceitas; C/D também suportam MST=2/Work RAM ARM7, onde stores por byte são permitidos nas janelas `0x06000000`/`0x06020000`, OFS=0/1, status independente do OFS e rejeição transacional de colisão no mesmo slot; as sete janelas Shared WRAM são pré-mapeadas e suas máscaras de visibilidade por CPU são atualizadas de forma transacional, com o mapa validado antes da aplicação, sem desalocação nem perda dos bytes físicos;
- wrappers C bus_read_*/bus_write_* que encaminham para RAM guest mapeada e falham com diagnóstico em endereços inválidos;
- controlador MMIO `NdsInterruptControl` com IME/IE/IF independentes por CPU, IRQ requests latcheados, IF W1C e `try_enter_irq` condicionado a `CPSR.I`; `run_dual_core_round` consulta e entrega IRQs nas fronteiras; o callback host fornece o LR ARM/Thumb e a máquina seleciona a base vetorial pelo ARM9 CP15 Control.V;
- `NdsTimerControl` com Timer 0–3 para cada CPU, reload, prescalers, cascata e pedidos de IRQ; `NdsSystemClock` converte os contadores cumulativos ARM9/ARM7 na razão 2:1, preserva meio ciclo e chama `advance_bus_cycles(n)` uma única vez por intervalo de barramento completo;
- carregador local de programas que lê os dois binários extraídos, exige tamanhos e SHA-1 conhecidos de ARM9/ARM7 e só entrega as imagens à máquina depois de validar ambas; módulo SHA-1 próprio coberto por vetores conhecidos;
- `NdsMachine` como contexto proprietário dos dois estados ARM e dos dispositivos base; instalação do mapa retail, vínculo C ABI em ordem de dependência, rollback/desvínculo reverso, início/fim de fatias com relatório automático dos contadores cumulativos, contabilização de ciclos ociosos com precisão de core e rounds dual-core sincronizados em 2:1; HALT/wake básico por CPU conectado ao BIOS SWI `0x06`, SWI ARM7 `0x1F` (parâmetro em R2) e CP15 WFI do ARM9; CP15 ARM9 parcial: ID e tamanho do TCM em c0; V, enable e load-mode ITCM/DTCM em c1; janelas c9 com backing privado de 32 KB ITCM/16 KB DTCM, prioridade sobre a RAM coberta e máscaras independentes de leitura/escrita/busca. Vetores altos/baixos seguem Control.V; MPU, caches e outros registradores CP15 continuam pendentes. Os providers `runtime_code_cycles`, `arm9_refill_cycles` e `arm7_refill_cycles` modelam fetch uncached ARM9 e refill ARM/Thumb nas regiões documentadas, incluindo Palette/VRAM no ARM9 e VRAM C/D no ARM7. A primeira halfword Thumb recebe o fetch 32-bit integral e a segunda custa zero; os refills contam os fetches sequenciais do destino e reclassificam cada fetch ao cruzar regiões. Timing DATA parcial de Main RAM, WRAM/BIOS/I/O/OAM, Palette/VRAM no ARM9, VRAM C/D como Work RAM no ARM7 e TCM normal está implementado; a ABI não distingue loads/stores, então TCM load mode, Gamecard/Wi-Fi e N32 ARM7 VRAM não sequencial continuam fail-closed. Combinação ARM7 cobre classes documentadas de CODE/DATA; cache, contenção e wait states dinâmicos permanecem pendentes;
- tracing ABI limitado por CPU: `runtime_trace_event` e `runtime_live_transfer` gravam em um buffer circular de 512 eventos por core; o host consulta a fila por cursor e recebe a contagem de eventos sobrescritos;
- operações CPSR/SPSR e entrada/retorno de exceções;
- avaliação de condições e atualização de flags NZCV;
- helpers de acesso aos registradores do User bank e instrução `CLZ` na ABI C;
- ponte ABI C para selecionar o estado ARM9 ou ARM7 via `g_cpu`, wrappers de flags/PSR e especialização `NDS_STATIC_CPU`;
- asserts do layout de `NdsStaticValidation`, `NdsDispatchEntry` e `NdsLinkSlot`;
- registro/remoção de bancos estáticos por ARM9/ARM7, lookup ARM/Thumb e `runtime_dispatch` com falha fechada;
- callback de identidade que compara bytes do banco e dependências contra `GuestMemory`, exigindo proveniência para RAM gravável;
- pilha de retorno guest separada por CPU, com chave PC+ARM/Thumb e desenrolamento de retornos não locais;
- contadores de ciclos e controle cooperativo de fatias ARM9/ARM7 por orçamento, yield e unwind automático durante o retorno dos frames C gerados;
- `NdsKeypadControl` com KEYINPUT compartilhado, KEYCNT independente por CPU, condições OR/AND e IRQ de teclado; o host SDL3 mapeia X/Z, Enter/Backspace, setas e Q/W, limpa entradas ao perder foco e encaminha estado pela ponte C;
- entradas `runtime_link_*` encaminhadas ao dispatcher normal; otimização de slots e folhas inline ainda desabilitada.

A validação automatizada na revisão `72a9c66` passou nos quatro jobs da CI. Desktop Linux: **32/32 CTests**; smoke de ligação com bancos sintéticos: **1/1**; tooling Python: **15 testes**; runtime ARM, Android ARM64 e compilador estático NDSRecomp também passaram. Os testes de timing cobrem fetch, DATA, TCM, refill ARM9/ARM7 e multiply ARM7TDMI. [Execução da CI](https://github.com/CarlosCosta07-s/Oracis/actions/runs/44638378456).


O extrator e o emissor local validam a identidade dos segmentos, os metadados TOML, os hashes ARM9/ARM7 e a identidade do gerador. A emissão dos dois bancos é transacional. Na revisão USA Rev 1, o gerador produziu localmente 78 funções ARM9 e 957 ARM7; os quatro arquivos C gerados foram compilados em C99 contra os headers ABI Oracis e agrupados em objetos locais. O inventário `nm -u` encontrou 40 referências externas ARM9, 45 ARM7 e 48 nomes únicos combinados. As duas rotinas ausentes (`runtime_live_transfer` e `runtime_trace_event`) agora têm implementações reais. Ainda falta fazer a ligação completa com as bibliotecas Oracis, confirmar todos os símbolos em conjunto e resolver os serviços que deliberadamente falham em timing/BIOS. Continuam pendentes combinação de ciclos ARM7, direção load/store na ABI de timing, TCM load mode, caches/MPU, contenção e wait states dinâmicos de Gamecard/Wi-Fi.
 
**Ainda não foi concluído:** ligar os objetos reais às bibliotecas/runtime, registrar as tabelas ARM9/ARM7, tratar os alvos indiretos desconhecidos e completar serviços BIOS/NitroSDK, DMA, IPC e vídeo. O timing de dados permanece parcial, com as limitações documentadas em ARM9_TIMING.md. O carregador local e a validação de identidade têm testes sintéticos; o Android ainda não possui seleção de arquivos e caminho de dados integrado. Nenhum teste comprova boot ou compatibilidade jogável.


## Princípios

- Recompilação estática e execução nativa; sem interpretador ARM geral como fallback.
- Runtime, scheduler e dispositivos implementados independentemente.
- Não incorporar código ou assets de emuladores terceiros.
- Não incorporar código derivado de melonDS, direta ou indiretamente.
- ROM, BIOS, firmware, extrações, bancos gerados e saves ficam fora do repositório, CI e releases.
- Não presumir autorização a partir da disponibilidade pública de uma descompilação.
- Não declarar compatibilidade, desempenho ou conclusão sem testes reproduzíveis.

## Plataformas planejadas

Linux x86-64 e Android ARM64 já passam pelos builds da CI; isso comprova compilação, não execução jogável. Windows x64 continua planejado e requer validação própria.

## Aviso

Oracis é um projeto independente, sem afiliação ou endosso da Nintendo, The Pokémon Company ou Game Freak. Pokémon e seus elementos relacionados pertencem aos respectivos titulares. O projeto não distribui o jogo nem conteúdo proprietário.
