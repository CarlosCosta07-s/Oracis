<!-- Oracis project page -->
<p align="center">
  <img src="assets/icon/oracis.svg" alt="Ícone do Oracis: estrela de platina e anel celeste" width="160">
</p>

<h1 align="center">Oracis</h1>

<p align="center">Um projeto independente de port nativo de Pokémon Platinum para plataformas modernas.</p>

<p align="center"><strong>Reconstruindo Sinnoh para além do hardware original.</strong></p>

> **Status:** Pokémon Platinum USA Rev 1 foi validado, extraído e recompilado localmente em 19/11/2026. O NDSRecomp fixado gerou os bancos C/dispatch ARM9 e ARM7: 78 funções ARM9 e 957 ARM7 (incluindo 8 Thumb). Os quatro arquivos C gerados (corpo e dispatch de cada CPU) foram compilados localmente em C99 contra a ABI Oracis e agrupados em objetos; os objetos e bancos permanecem fora do Git e da CI. Ainda não foram ligados ao runtime, registrados no dispatcher nem executados. A varredura ARM9 terminou por limite de passos com 12 leituras não mapeadas e 10 alvos indiretos desconhecidos; ARM7 terminou ao sair do módulo com 388 leituras não mapeadas e 30 alvos indiretos desconhecidos: a cobertura permanece parcial. O CP15 ARM9 tem suporte parcial a ID, TCM size, ITCM/DTCM e load modes; MPU/cache seguem pendentes. O timing uncached ARM9 cobre ITCM, Main RAM, WRAM, BIOS, os primeiros 4 KB de I/O, Palette/VRAM e OAM; refill de pipeline ARM9/ARM7 está implementado para essas classes, com Palette/VRAM no ARM9 e somente VRAM C/D atribuída ao ARM7 como Work RAM, custos distintos por estado ARM/Thumb e reclassificação ao cruzar regiões. O timing DATA agora cobre parcialmente Main RAM, WRAM/BIOS/I/O/OAM, Palette/VRAM no ARM9, VRAM C/D como Work RAM no ARM7 e TCM normal, com conversão de ciclo ARM9 2:1; a ABI não separa loads/stores, então TCM load mode, Gamecard/GBA slot, Wi-Fi e N32 ARM7 VRAM não sequencial seguem fail-closed. Multiply ARM7TDMI por early termination e combinação de ciclos ARM7 (classes documentadas de CODE/DATA, sobreposição Main RAM e contexto da instrução/acesso) estão implementados; cache, contenção, direção load/store na ABI TCM e wait states dinâmicos permanecem pendentes. A ABI de observabilidade inclui um buffer circular de 512 eventos por CPU para tracing e transferências, com contador de eventos descartados. A CI da revisão `72a9c66` passou nos quatro jobs: 32/32 CTests, smoke de bancos sintéticos 1/1, 15 testes Python, runtime ARM, Android ARM64 e compilador fixado. Nenhuma ROM, segmento ou banco comercial entrou no Git ou na CI.




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
- GuestMemory: mapa de RAM NDS retail, Main RAM compartilhada/mirrors, WRAM ARM7 privada, quatro modos WRAMCNT e leitura ARM7 de WRAMSTAT; controlador parcial VRAM C/D com VRAMCNT_C/D e VRAMSTAT para Work RAM do ARM7; as sete janelas Shared WRAM são pré-mapeadas e suas máscaras de visibilidade por CPU são atualizadas de forma transacional, com o mapa validado antes da aplicação, sem desalocação nem perda dos bytes físicos;
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

A validação automatizada na revisão `72a9c66` passou nos quatro jobs da CI. Desktop Linux: **32/32 CTests**; smoke de ligação com bancos sintéticos: **1/1**; tooling Python: **15 testes**; runtime ARM, Android ARM64 e compilador estático NDSRecomp também passaram. Os testes de timing cobrem fetch, DATA, TCM, refill ARM9/ARM7 e multiply ARM7TDMI. [Execução da CI](https://github.com/CarlosCosta07-s/Oracis/actions/runs/44628908857).


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
