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
| 1 | NDSRecomp | Checkout fixado e compilação isolada | Bootstrap e alvo nds_recompile validados na revisão fixada; CI produz artefato temporário com compilador + scripts + headers ABI Oracis e checksum; identidade nds-codegen-v3 exigida pelo emissor; compilador usado localmente em 19/11 para emitir os bancos USA Rev 1 |
| 2 | Extração e bancos | Extrair ARM9/ARM7 e emitir bancos C | ROM USA Rev 1 identificada/extraída; emissão real local: 78 funções ARM9 e 957 ARM7 (8 Thumb); scan parcial (ARM9 12 leituras não mapeadas/10 indiretos desconhecidos; ARM7 388 leituras não mapeadas/30 indiretos desconhecidos); os quatro arquivos C corpo/dispatch foram compilados em C99 contra a ABI Oracis e agrupados em objetos locais; inventário: 40 referências externas ARM9, 45 ARM7, 48 nomes únicos; ligação ao runtime pendente; nenhum banco/segmento no Git ou CI |
| 3 | ABI e estado ARM | Estado ARM, CPSR/SPSR, exceções, flags e ponte C ABI | Estado/flags, User-bank helpers e CLZ implementados; tracing e transferência dinâmica registram em buffer circular de 512 eventos por CPU; CI 30/30 CTests e builds multiplataforma confirmados |
| 4 | Memória e serviços base | Barramento, MMIO, IRQ, timers, DMA, IPC, CP15 e NitroSDK | Mapa NDS retail e WRAMCNT/WRAMSTAT; IME/IE/IF, IRQ fronteira, timers 0–3 por CPU, clock dual-core 2:1, HALT/wake básico; ARM9 CP15 parcial: ID/TCM size c0, V + ITCM/DTCM enable/load mode c1, regiões c9 e backings 32 KB ITCM/16 KB DTCM com espelhos e máscaras independentes; fetch uncached ARM9: ITCM 1, Main RAM 18, WRAM/BIOS/I/O/OAM 8, Palette/VRAM 10 ciclos core; refill ARM9: duas N32 ou uma N32 para Thumb alinhado; refill ARM7: duas S32/S16; data timing, multiply, ARM7 combine, cache/MPU, callback LR arquitetural, VRAMCNT_B, DMA/IPC e demais MMIO pendentes |
| 5 | Dispatch e scheduler | Tabelas por CPU, validação de conteúdo, interworking e scheduler dual-core cooperativo | Registry, validação de bytes, pilha de retorno e orçamento/yield por CPU implementados; `run_dual_core_round()` sincroniza ARM9/ARM7 em 2:1 e contabiliza HALT/yield; a ponte C registra/remover tabelas locais opcionais e verifica presença dos bancos; CMake compila bancos locais; smoke test com fixtures sintéticas valida ligação e ciclo registro/remoção; API de tracing/live-transfer com buffer circular por CPU; API de carga em memória e carregador local verificado (tamanho/SHA-1 de ambos os segmentos antes de escrever) implementados; testes cobrem falta, truncamento, hash divergente e preservação do estado; host SDL desktop tenta carregar os segmentos no início quando bancos estáticos estão ligados; Android ainda não possui seleção de arquivos/diretório local; nenhuma plataforma executa rounds; provider arquitetural de LR ARM/Thumb, scheduler geral DS e cache de links pendentes |
| 6 | Boot visível e vídeo 2D | Primeiro quadro, VRAM, BGs, sprites, OAM e blending | Não iniciada |
| 7 | Vídeo 3D | Geometry Engine, transformações, iluminação, texturas e fog | Não iniciada |
| 8 | Áudio | SDAT, SSEQ/SBNK/SWAR/STRM, canais e mixagem | Não iniciada |
| 9 | Input e UX | Teclado, gamepad, toque e configurações | MMIO KEYINPUT/KEYCNT testados; ponte C retém estado até o vínculo do dispositivo e host SDL3 envia teclado desktop (X/Z, Enter/Backspace, setas, Q/W), com limpeza ao perder foco; o host inicializa `NdsMachine` pela ponte C ABI e conecta o teclado ao dispositivo vinculado; gamepad, EXTKEYIN e toque pendentes |
| 10 | Saves e dados | Save/load e cache local versionado | Não iniciada |
| 11 | Testes e desempenho | Comparação comportamental, frames, sanitizers e profiling | Testes unitários do estado/ABI iniciados |
| 12 | Empacotamento | Windows, Linux e posteriormente Android ARM64 | Não iniciada |


**Validação confirmada em 19/11/2026:** a revisão `ed035ab` passou nos quatro jobs da CI. Desktop Linux: **31/31 CTests**; smoke de bancos sintéticos: **1/1**; tooling Python: **15 testes**; runtime ARM, Android ARM64 e compilador estático NDSRecomp passaram. Os testes incluem tracing por CPU, CP15/TCM/load modes, fetch uncached ARM9 e refill ARM9/ARM7 em ARM/Thumb nas regiões documentadas. Os artefatos da ROM continuam fora do Git/CI. [Execução da CI](https://github.com/CarlosCosta07-s/Oracis/actions/runs/44624426075). A CI não comprova boot do jogo.

## Próxima ordem de implementação

1. Ligar os quatro objetos corpo/dispatch locais às bibliotecas Oracis e confirmar a resolução dos 48 símbolos externos do inventário.
2. Completar `runtime_mem_cycles` (data read/write), `runtime_mul_cycles` e `arm7_cycle_combine`; expandir fetch/refill ARM9 para cache/MPU/Gamecard e wait states dinâmicos somente com modelo testável.
3. Auditar e implementar os alvos indiretos/overlays desconhecidos e os SWIs/serviços BIOS/NitroSDK requeridos pela inicialização; completar DMA, IPC e MMIO.
4. Implementar o provider arquitetural de LR IRQ para ARM/Thumb e validar exceções nas duas CPUs.
5. Registrar os bancos reais, confirmar identidade das entradas e executar testes controlados de dispatch sem abrir rounds de jogo.
6. Habilitar rounds somente após os serviços mínimos de boot estarem presentes e observáveis.
7. Implementar VRAM/PPU e apresentar o primeiro quadro; áudio, saves e UX avançam depois.


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

O script versionado `tools/ndsrecomp/extract_nds_programs.py` foi executado sobre a cópia local fornecida para desenvolvimento. A identidade da ROM conferiu (`0862ec35b24de5c7e2dcb88c9eea0873110d755c`), e os segmentos foram verificados depois da gravação, sempre fora da árvore Git:

- ARM9: offset `0x00004000`, load `0x02000000`, entry `0x02000800` (ARM), tamanho `0x001023F8`, SHA-1 `7ec963e737f0f61efcbbf5c7a3d33cbf378143f8`.
- ARM7: offset `0x00409800`, load `0x02380000`, entry `0x02380000` (ARM), tamanho `0x000277FC`, SHA-1 `082bb349ae7d057edbbd91a07316b1402b18b803`.

O NDSRecomp fixado gerou 78 funções ARM9 e 957 ARM7 (8 Thumb) em bancos C/dispatch. A varredura foi parcial: ARM9 atingiu step-cap com 12 leituras não mapeadas e 10 alvos indiretos desconhecidos; ARM7 saiu do módulo com 388 leituras não mapeadas e 30 alvos indiretos desconhecidos. Os quatro arquivos C gerados foram compilados localmente em C99 contra a ABI Oracis e agrupados em objetos. A análise `nm -u` encontrou 40 referências externas ARM9 e 45 ARM7 (48 nomes únicos). Nenhum segmento, banco ou objeto gerado foi enviado ao repositório ou à CI. A emissão e a compilação C são marcos reais, mas não demonstram ligação ou boot.


Não anunciar compatibilidade, desempenho ou conclusão de subsistemas sem testes reproduzíveis.
