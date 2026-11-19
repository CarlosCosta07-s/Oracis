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
| 1 | NDSRecomp | Checkout fixado e compilação isolada | Bootstrap e alvo `nds_recompile` validados na revisão fixada; teste sintético extrator→NDSRecomp compila corpos e dispatch C99 contra ABI Oracis; identidade `nds-codegen-v3` exigida pelo emissor; CI `90d45fe` aprovada (28/28 CTests, 15 testes Python e build do compilador); emissão com a ROM real pendente |
| 2 | Extração e bancos | Extrair ARM9/ARM7 e emitir bancos C | Extrator/emissor locais publicados; manifesto, tamanho/hash, offsets, load address, entry point, TOML e identidade do codegen são conferidos; emissão dos dois bancos em diretório de staging com publicação transacional, remoção de arquivos gerados obsoletos e rollback preservando a saída anterior em falha ARM7; testes sintéticos rejeitam destinos com symlink, conteúdo não relacionado ou sobreposição com a extração; carregador local de arquivos ARM9/ARM7 agora valida tamanho e SHA-1 fixos antes de modificar a memória; a ROM USA Rev 1 fornecida foi validada pelo extrator versionado em 19/11 (SHA-1 da ROM e dos segmentos conferidos; saídas mantidas somente localmente); a emissão real dos bancos pelo NDSRecomp continua pendente |
| 3 | ABI e estado ARM | Estado ARM, CPSR/SPSR, exceções, flags e ponte C ABI | Estado/flags, User-bank helpers e CLZ implementados; CI multiplataforma e 21/21 testes confirmados na revisão 480651a |
| 4 | Memória e serviços base | Barramento, MMIO, IRQ, timers, DMA, IPC, CP15 e NitroSDK | Mapa inicial NDS retail: Main RAM 4MB/mirrors, WRAM ARM7 e modos WRAMCNT 0–3 (incluindo mirror ARM7 privado no modo 0); WRAMCNT ARM9 MMIO e troca transacional, sem alocação, das máscaras das sete janelas Shared WRAM, com validação integral antes da aplicação nos quatro modos implementados; IME/IE/IF, entrada IRQ condicional via `try_enter_irq` e Timer 0–3 por CPU implementados; `advance_bus_cycles(n)` avança as duas banks pelo mesmo clock global explícito; `NdsSystemClock` sincroniza os watermarks cumulativos ARM9/ARM7 na razão 2:1 e alimenta timers uma única vez pelos ciclos de barramento completos; `NdsMachine::run_dual_core_round()` agora reporta os dois cores até um watermark comum e trata HALT/yield como tempo ocioso; entrega IRQ na fronteira usando o provider de LR/vetor do host; ordenação geral dos demais eventos DS continua pendente; KEYINPUT compartilhado, KEYCNT independente por CPU e IRQ de teclado OR/AND implementados; `NdsMachine` reúne os estados ARM e dispositivos base, vincula/desvincula o barramento por dependência e reporta ciclos cumulativos ao relógio no fim de cada fatia; o orquestrador agora delimita/alternar as fatias em rounds e informa ciclos ociosos por core, com precisão de meio ciclo ARM9; HALT/wake básico por CPU (ARM7 via IE&IF, ARM9 via IME&IE&IF); conectado ao SWI NDS 06, SWI ARM7 1F CustomHalt (parâmetro R2) e CP15 WFI ARM9, com yield da fatia; HALTCNT MMIO direto, SLEEP/STOP e GBA mode não suportados; `NdsMachine::try_deliver_pending_irq()` implementado para fronteira segura, respeitando HALT wake, IME/IE/IF e CPSR.I; `run_dual_core_round()` consulta/entrega IRQs no início e no fim do round; ARM9 CP15 Control (`c1,c0,0`) agora lê/escreve o bit V (13), inicializado em 1 para o vetor BIOS alto do Nintendo DS (`0xFFFF0000`), e permite selecionar vetores baixos/altos; ARM7 permanece na base baixa; callback externo ainda precisa fornecer o LR arquitetural exato; MPU, cache, TCM, VRAMCNT_B, demais MMIO, VRAM e timing pendentes |
| 5 | Dispatch e scheduler | Tabelas por CPU, validação de conteúdo, interworking e scheduler dual-core cooperativo | Registry, validação de bytes, pilha de retorno e orçamento/yield por CPU implementados; `run_dual_core_round()` sincroniza ARM9/ARM7 em 2:1 e contabiliza HALT/yield; a ponte C registra/remover tabelas locais opcionais e verifica presença dos bancos; CMake compila bancos locais; smoke test com fixtures sintéticas valida ligação e ciclo registro/remoção; API de carga em memória e carregador local verificado (tamanho/SHA-1 de ambos os segmentos antes de escrever) implementados; testes cobrem falta, truncamento, hash divergente e preservação do estado; host SDL desktop tenta carregar os segmentos no início quando bancos estáticos estão ligados; Android ainda não possui seleção de arquivos/diretório local; nenhuma plataforma executa rounds; provider arquitetural de LR ARM/Thumb, scheduler geral DS e cache de links pendentes |
| 6 | Boot visível e vídeo 2D | Primeiro quadro, VRAM, BGs, sprites, OAM e blending | Não iniciada |
| 7 | Vídeo 3D | Geometry Engine, transformações, iluminação, texturas e fog | Não iniciada |
| 8 | Áudio | SDAT, SSEQ/SBNK/SWAR/STRM, canais e mixagem | Não iniciada |
| 9 | Input e UX | Teclado, gamepad, toque e configurações | MMIO KEYINPUT/KEYCNT testados; ponte C retém estado até o vínculo do dispositivo e host SDL3 envia teclado desktop (X/Z, Enter/Backspace, setas, Q/W), com limpeza ao perder foco; o host inicializa `NdsMachine` pela ponte C ABI e conecta o teclado ao dispositivo vinculado; gamepad, EXTKEYIN e toque pendentes |
| 10 | Saves e dados | Save/load e cache local versionado | Não iniciada |
| 11 | Testes e desempenho | Comparação comportamental, frames, sanitizers e profiling | Testes unitários do estado/ABI iniciados |
| 12 | Empacotamento | Windows, Linux e posteriormente Android ARM64 | Não iniciada |


**Validação confirmada em 19/11/2026:** a revisão `90d45fe` passou nos quatro jobs da CI. Desktop Linux: **28/28 CTests**; build e smoke test opcional de bancos sintéticos: **1/1**; tooling Python: **15 testes**; runtime ARM, Android ARM64 e compilador estático NDSRecomp também passaram. Os testes novos do carregador local rejeitam arquivo ausente, truncado e com hash divergente sem carregar o programa; os vetores SHA-1 conhecidos também passam. [Execução da CI](https://github.com/CarlosCosta07-s/Oracis/actions/runs/44611707658). Isso não comprova emissão de bancos comerciais nem boot do jogo.

## Próxima ordem de implementação

1. Executar o extrator e o emissor NDSRecomp versionados contra a cópia local real de Pokémon Platinum USA Rev 1.
2. Compilar os bancos gerados e inventariar símbolos indefinidos, serviços requeridos e código dinâmico/overlays.
3. Implementar e testar os helpers exigidos pelos bancos: bus, contagem de ciclos, exceções/CP15, DMA, IPC e serviços BIOS/NitroSDK.
4. Implementar o provider arquitetural de LR IRQ para ARM/Thumb e validar exceções em ambas as CPUs; a seleção do vetor ARM9 via CP15 Control.V já está ligada à máquina.
5. Executar e auditar a inicialização desktop com os artefatos reais; adicionar seleção segura de ROM/segmentos e caminho de armazenamento no Android.
6. Acionar rounds apenas quando imagem, bancos, serviços e provider IRQ estiverem prontos; manter a falha explícita para dispatch ausente.
7. Completar ordenação geral de eventos DS, MMIO/TCM e temporização para avançar ao boot visível; vídeo, áudio, saves e UX vêm depois.

## Código dinâmico e overlays

Código executável materializado em RAM deverá ser identificado, recompilado e validado como outro banco. Não haverá interpretador geral ou JIT como fallback. Um alvo desconhecido deve parar com diagnóstico de CPU, PC, modo e banco.

## Critério de primeiro boot nativo

- Cópia USA Rev 1 identificada.
- ARM9 e ARM7 extraídos e recompilados em bancos C.
- Bancos compilados e ligados à ABI Oracis sem runtime melonDS.
- Estado e dispatch de ambos os processadores funcionais.
- Memória, exceções, timers e serviços necessários ao boot validados.
- Tela inicial apresentada pelo host SDL3, com logs e evidências reproduzíveis.


### Verificação local da ROM USA Rev 1 (19/11/2026)

O script versionado `tools/ndsrecomp/extract_nds_programs.py` foi executado sobre a cópia local fornecida para desenvolvimento. A identidade completa conferiu (`0862ec35b24de5c7e2dcb88c9eea0873110d755c`), e o script gerou os dois segmentos fora da árvore Git, verificando os arquivos após a gravação:

- ARM9: offset `0x00004000`, load `0x02000000`, entry `0x02000800` (ARM), tamanho `0x001023F8`, SHA-1 `7ec963e737f0f61efcbbf5c7a3d33cbf378143f8`.
- ARM7: offset `0x00409800`, load `0x02380000`, entry `0x02380000` (ARM), tamanho `0x000277FC`, SHA-1 `082bb349ae7d057edbbd91a07316b1402b18b803`.

Os arquivos `.bin`, TOML e manifesto permaneceram no ambiente local e não foram enviados ao repositório ou à CI. Esta etapa confirma a extração dos segmentos, não a recompilação: ainda falta construir o NDSRecomp fixado, emitir os bancos reais, compilá-los e auditar os símbolos/serviços exigidos.

Não anunciar compatibilidade, desempenho ou conclusão de subsistemas sem testes reproduzíveis.
