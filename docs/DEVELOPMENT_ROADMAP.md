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
| 4 | Memória e serviços base | Barramento, MMIO, IRQ, timers, DMA, IPC, CP15 e NitroSDK | Mapa NDS retail e WRAMCNT/WRAMSTAT; IME/IE/IF, IRQ fronteira, timers 0–3 por CPU, clock dual-core 2:1, HALT/wake básico; ARM9 CP15 parcial: ID/TCM size c0, V + ITCM/DTCM enable/load mode c1, regiões c9 e backings 32 KB ITCM/16 KB DTCM com espelhos e máscaras independentes; fetch uncached ARM9 e refill ARM9/ARM7 nas classes documentadas; DATA parcial NDS7/DATA e NDS9/DATA para Main RAM, WRAM/BIOS/I/O/OAM, Palette/VRAM no ARM9, VRAM C/D como Work RAM no ARM7 e TCM normal; direção load/store, TCM load mode, Gamecard/Wi-Fi, N32 ARM7 VRAM não sequencial, cache/MPU, contenção, callback LR arquitetural, extended palettes, textura, DMA/IPC e demais MMIO pendentes; `NdsDisplayControl` já gera DISPSTAT/VCOUNT sincronizados, com HBlank/VBlank e IRQs `NdsVramControl` implementa parcialmente VRAMCNT_A-I para MST=0/LCDC, A-D/E/F/G em MST=1/Main BG, A/B/E/F/G em MST=2/Main OBJ, C/D em MST=4/Sub BG/OBJ, H/I em MST=1/Sub BG, I em MST=2/Sub OBJ e C/D também em MST=2/Work RAM ARM7 (OFS=0/1), com aliases por CPU transacionais e rejeição de colisões; `NdsVideoMemory` instala 2 KiB de paletas padrão e 2 KiB de OAM como regiões ARM9-only, suprime STRB e permite STRH/STR; `NdsDisplayControl` mantém `DISPCNT` e `BG0CNT–BG3CNT` dos motores A/B; DISPSTAT/VCOUNT, scroll/affine, janelas, blending, PPU, avaliação OAM e arbitragem de scanline ainda pendentes |
| 5 | Dispatch e scheduler | Tabelas por CPU, validação de conteúdo, interworking e scheduler dual-core cooperativo | Registry, validação de bytes, pilha de retorno e orçamento/yield por CPU implementados; `run_dual_core_round()` sincroniza ARM9/ARM7 em 2:1 e contabiliza HALT/yield; a ponte C registra/remover tabelas locais opcionais e verifica presença dos bancos; CMake compila bancos locais; smoke test com fixtures sintéticas valida ligação e ciclo registro/remoção; API de tracing/live-transfer com buffer circular por CPU; API de carga em memória e carregador local verificado (tamanho/SHA-1 de ambos os segmentos antes de escrever) implementados; testes cobrem falta, truncamento, hash divergente e preservação do estado; host SDL desktop tenta carregar os segmentos no início quando bancos estáticos estão ligados; Android ainda não possui seleção de arquivos/diretório local; nenhuma plataforma executa rounds; provider arquitetural de LR ARM/Thumb, scheduler geral DS e cache de links pendentes |
| 6 | Boot visível e vídeo 2D | Primeiro quadro, VRAM, BGs, sprites, OAM e blending | Base de memória/configuração/temporização iniciada: VRAM A-I mapeada nos destinos CPU LCDC/BG/OBJ cobertos por MST/OFS; `NdsVideoMemory` oferece palette RAM padrão (2 KiB) e OAM (2 KiB) para ARM9, com STRB ignorados e STRH/STR aceitos; `NdsDisplayControl` mantém DISPCNT, BG0CNT–BG3CNT, scroll/affine, DISPSTAT/VCOUNT, janelas e registradores de blend. `Nds2dEffects` calcula máscaras de janela, mosaic e efeitos RGB555; `Nds2dTextBgFetcher` gera scanlines de BG texto 4bpp/8bpp; `Nds2dAffineBgFetcher` rasteriza BG2/BG3 affine clássicos nos modos 1, 2 e 4, com mapa quadrado row-major de índices de 8 bits, tiles 8bpp, coeficientes PA/PB/PC/PD signed 8.8, referências BGxX/BGxY signed 19.8, acumuladores PB/PD por scanline, overflow wrap/transparent e mosaic; o compositor seleciona o fetcher de acordo com DISPCNT. A acumulação PB/PD e escritas visíveis de BGxX/BGxY estão integradas a VCOUNT, mas o ponto exato de cópia das referências em VBlank ainda é uma aproximação de frame boundary. `Nds2dObjFetcher` rasteriza OBJ tiled 4bpp/8bpp não affine e affine nas Engines A/B, com OAM, mapeamento 1D/2D, formas/tamanhos, clipping, flips não affine, matrizes PA/PB/PC/PD signed 8.8 selecionadas por Attr1[9:13], transformação inversa por pixel e bounds double-size, além de mosaic, transparência, semitransparência e OBJ-window; preserva os dois pixels OBJ superiores por X. `Nds2dBgCompositor` ordena esses dois pixels junto aos BGs, por prioridade e desempate OAM, e permite OBJ semitransparente usar outro OBJ como segundo alvo de alpha blending; também aplica backdrop, janelas e alpha/brightness. Bitmap OBJ, BG extended/bitmap, paletas estendidas, BG0 3D, limites por scanline, arbitragem/timing de fetch PPU e apresentação final ainda faltam; nenhum primeiro quadro ou boot do jogo foi comprovado |
| 7 | Vídeo 3D | Geometry Engine, transformações, iluminação, texturas e fog | Não iniciada |
| 8 | Áudio | SDAT, SSEQ/SBNK/SWAR/STRM, canais e mixagem | Não iniciada |
| 9 | Input e UX | Teclado, gamepad, toque e configurações | MMIO KEYINPUT/KEYCNT testados; ponte C retém estado até o vínculo do dispositivo e host SDL3 envia teclado desktop (X/Z, Enter/Backspace, setas, Q/W), com limpeza ao perder foco; o host inicializa `NdsMachine` pela ponte C ABI e conecta o teclado ao dispositivo vinculado; gamepad, EXTKEYIN e toque pendentes |
| 10 | Saves e dados | Save/load e cache local versionado | Não iniciada |
| 11 | Testes e desempenho | Comparação comportamental, frames, sanitizers e profiling | Testes unitários do estado/ABI iniciados |
| 12 | Empacotamento | Windows, Linux e posteriormente Android ARM64 | Não iniciada |


**Validação confirmada em 20/11/2026:** o commit `1d249b1` passou nos quatro jobs da CI. Desktop Linux: **41/41 CTests**, smoke de bancos sintéticos **1/1** e tooling Python **15 testes**; runtime ARM independente: **41/41 CTests**; Android ARM64 e compilador estático NDSRecomp também passaram (suite do compilador **6/6**). O teste novo cobre BG affine clássico nas Engines A/B, mapa/tile 8bpp, identidade, escala, acumuladores PB/PD, escrita de referência na linha atual, overflow, mosaic e integração ao compositor. Os demais testes cobrem OBJ affine e blending OBJ-sobre-OBJ, além dos testes anteriores de memória, CPU, BG texto e composição. Os artefatos da ROM continuam fora do Git/CI. [Execução da CI](https://github.com/CarlosCosta07-s/Oracis/actions/runs/44774368023). A CI não comprova boot do jogo.

## Próxima ordem de implementação

1. Ligar os quatro objetos corpo/dispatch locais às bibliotecas Oracis e confirmar a resolução dos 48 símbolos externos do inventário.
2. Ampliar `runtime_mem_cycles` para direção load/store, TCM load mode, cache, contenção e wait states dinâmicos somente quando a ABI e o modelo forem testáveis.
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
