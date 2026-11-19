# Mapa de desenvolvimento do Oracis

Cada fase deve produzir uma entrega pequena, compilável, executável e verificável. Stubs e declarações sem implementação não contam como subsistema concluído.

## Regras permanentes

1. Trabalhar diretamente na branch `main`; não criar branches.
2. Não copiar, incorporar ou ligar componentes derivados de melonDS.
3. Usar somente o alvo estático `nds_recompile` em checkout externo e revisão fixada.
4. Implementar runtime, scheduler, memória e dispositivos do Oracis independentemente.
5. Não usar interpretador ARM geral como fallback de código dinâmico; misses devem falhar com diagnóstico.
6. ROM, BIOS, firmware, arquivos extraídos, saves e bancos gerados ficam fora do Git, CI e releases.
7. A reutilização de código de pret/pokeplatinum continua bloqueada até esclarecimento dos direitos.

## Fases

| Fase | Área | Entrega principal | Estado |
|---:|---|---|---|
| 0 | Baseline | Revisão/região e direitos avaliados | USA Rev 1 identificada; licença geral de pret/pokeplatinum não confirmada |
| 1 | NDSRecomp | Checkout fixado e compilação isolada | Bootstrap validado em CI; alvo nds_recompile compilou isoladamente; emissão de bancos reais pendente |
| 2 | Extração e bancos | Extrair ARM9/ARM7 e emitir bancos C | Metadados e hashes dos segmentos verificados localmente; emissão pendente |
| 3 | ABI e estado ARM | Estado ARM, CPSR/SPSR, exceções, flags e ponte C ABI | Estado/flags, User-bank helpers e CLZ implementados; CI multiplataforma e 21/21 testes confirmados na revisão 480651a |
| 4 | Memória e serviços base | Barramento, MMIO, IRQ, timers, DMA, IPC, CP15 e NitroSDK | Mapa inicial NDS retail: Main RAM 4MB/mirrors, WRAM ARM7 e modos WRAMCNT 0–3 (incluindo mirror ARM7 privado no modo 0); WRAMCNT ARM9 MMIO e troca transacional, sem alocação, das máscaras das sete janelas Shared WRAM, com validação integral antes da aplicação nos quatro modos implementados; IME/IE/IF, entrada IRQ condicional via `try_enter_irq` e Timer 0–3 por CPU implementados; `advance_bus_cycles(n)` avança as duas banks pelo mesmo clock global explícito; `NdsSystemClock` sincroniza os watermarks cumulativos ARM9/ARM7 na razão 2:1 e alimenta timers uma única vez pelos ciclos de barramento completos; `NdsMachine::run_dual_core_round()` agora reporta os dois cores até um watermark comum e trata HALT/yield como tempo ocioso; entrega IRQ na fronteira usando o provider de LR/vetor do host; ordenação geral dos demais eventos DS continua pendente; KEYINPUT compartilhado, KEYCNT independente por CPU e IRQ de teclado OR/AND implementados; `NdsMachine` reúne os estados ARM e dispositivos base, vincula/desvincula o barramento por dependência e reporta ciclos cumulativos ao relógio no fim de cada fatia; o orquestrador agora delimita/alternar as fatias em rounds e informa ciclos ociosos por core, com precisão de meio ciclo ARM9; HALT/wake básico por CPU (ARM7 via IE&IF, ARM9 via IME&IE&IF); conectado ao SWI NDS 06, SWI ARM7 1F CustomHalt (parâmetro R2) e CP15 WFI ARM9, com yield da fatia; HALTCNT MMIO direto, SLEEP/STOP e GBA mode não suportados; `NdsMachine::try_deliver_pending_irq()` implementado para fronteira segura, respeitando HALT wake, IME/IE/IF, CPSR.I, LR e vector_base fornecidos pelo driver; `run_dual_core_round()` agora consulta/entrega IRQs no início e no fim do round; host ainda precisa fornecer o executor real e o provider arquitetural; VRAMCNT_B, demais MMIO, CP15/TCM, VRAM e timing pendentes |
| 5 | Dispatch e scheduler | Tabelas por CPU, validação de conteúdo, interworking e scheduler dual-core cooperativo | Registry, validação de bytes, pilha de retorno e orçamento/yield por CPU implementados; `run_dual_core_round()` sincroniza ARM9/ARM7 em 2:1, alterna a ordem inicial, contabiliza HALT/yield e entrega IRQ via provider; `execute_registered_bank_slice()` liga a fatia ao `runtime_dispatch()`; a ponte C `oracis_host_runtime_run_round()` verifica a presença de bancos ARM9/ARM7 e aceita provider IRQ; testes sintéticos via C e C++ cobrem as fatias; SDL ainda não inicia rounds com bancos reais/provider arquitetural; scheduler geral de eventos DS, carregador de bancos e cache de links continuam pendentes |
| 6 | Boot visível e vídeo 2D | Primeiro quadro, VRAM, BGs, sprites, OAM e blending | Não iniciada |
| 7 | Vídeo 3D | Geometry Engine, transformações, iluminação, texturas e fog | Não iniciada |
| 8 | Áudio | SDAT, SSEQ/SBNK/SWAR/STRM, canais e mixagem | Não iniciada |
| 9 | Input e UX | Teclado, gamepad, toque e configurações | MMIO KEYINPUT/KEYCNT testados; ponte C retém estado até o vínculo do dispositivo e host SDL3 envia teclado desktop (X/Z, Enter/Backspace, setas, Q/W), com limpeza ao perder foco; o host inicializa `NdsMachine` pela ponte C ABI e conecta o teclado ao dispositivo vinculado; gamepad, EXTKEYIN e toque pendentes |
| 10 | Saves e dados | Save/load e cache local versionado | Não iniciada |
| 11 | Testes e desempenho | Comparação comportamental, frames, sanitizers e profiling | Testes unitários do estado/ABI iniciados |
| 12 | Empacotamento | Windows, Linux e posteriormente Android ARM64 | Não iniciada |


**Validação da revisão em andamento:** os novos testes de `run_dual_core_round()` cobrem a razão 2:1, yield antecipado, avanço de tempo ocioso durante HALT e despertar/entrega de IRQ ARM7. A CI desses commits ainda não foi confirmada; não tratar os testes como aprovados até a execução reproduzível.

## Próxima ordem de implementação

1. Implementar o provider arquitetural de IRQ a partir do estado real ARM/Thumb e CP15; validar LR e base vetorial em testes de exceção.
2. Executar o pipeline NDSRecomp fixado e emitir os bancos reais ARM9/ARM7 para Pokémon Platinum USA Rev 1.
3. Inventariar símbolos indefinidos e implementar os serviços/runtime exigidos pelos bancos.
4. Criar o carregador local dos bancos, validar hashes/dependências e registrá-los no dispatcher sem incluir arquivos derivados no Git.
5. Acionar `oracis_host_runtime_run_round()` no loop SDL após a carga dos dois bancos e instalar o provider IRQ validado.
6. Implementar ordenação geral de eventos DS e integrar DMA/IPC sem avançar dispositivos fora do watermark comum.
7. Completar MMIO/CP15/TCM e temporização necessários ao boot; só então avançar para vídeo, áudio, saves e UX.

## Código dinâmico e overlays

Código executável materializado em RAM deverá ser identificado, recompilado e validado como outro banco. Não haverá interpretador geral ou JIT como fallback. Um alvo desconhecido deve parar com diagnóstico de CPU, PC, modo e banco.

## Critério de primeiro boot nativo

- Cópia USA Rev 1 identificada.
- ARM9 e ARM7 extraídos e recompilados em bancos C.
- Bancos compilados e ligados à ABI Oracis sem runtime melonDS.
- Estado e dispatch de ambos os processadores funcionais.
- Memória, exceções, timers e serviços necessários ao boot validados.
- Tela inicial apresentada pelo host SDL3, com logs e evidências reproduzíveis.

Não anunciar compatibilidade, desempenho ou conclusão de subsistemas sem testes reproduzíveis.
