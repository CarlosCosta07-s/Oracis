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
| 3 | ABI e estado ARM | Estado ARM, CPSR/SPSR, exceções, flags e ponte C ABI | Estado/flags, User-bank helpers e CLZ implementados; 20/20 testes de runtime confirmados na CI da revisão d7e9e23 |
| 4 | Memória e serviços base | Barramento, MMIO, IRQ, timers, DMA, IPC, CP15 e NitroSDK | Mapa inicial NDS retail: Main RAM 4MB/mirrors, WRAM ARM7 e modos WRAMCNT 0–3 (incluindo mirror ARM7 privado no modo 0); WRAMCNT ARM9 MMIO e troca transacional, sem alocação, das máscaras das sete janelas Shared WRAM, com validação integral antes da aplicação nos quatro modos implementados; IME/IE/IF com IRQ pending consultável por CPU implementados; WRAMSTAT, entrada de exceção IRQ pelo scheduler, demais MMIO, CP15/TCM, VRAM e timing pendentes |
| 5 | Dispatch e scheduler | Tabelas por CPU, validação de conteúdo, interworking e scheduler dual-core cooperativo | Registry, validação de bytes, pilha de retorno e orçamento/yield por CPU implementados; carregador de bancos, cache de links e eventos DS pendentes |
| 6 | Boot visível e vídeo 2D | Primeiro quadro, VRAM, BGs, sprites, OAM e blending | Não iniciada |
| 7 | Vídeo 3D | Geometry Engine, transformações, iluminação, texturas e fog | Não iniciada |
| 8 | Áudio | SDAT, SSEQ/SBNK/SWAR/STRM, canais e mixagem | Não iniciada |
| 9 | Input e UX | Teclado, gamepad, toque e configurações | Host SDL3 de teste existe; integração ao runtime não iniciada |
| 10 | Saves e dados | Save/load e cache local versionado | Não iniciada |
| 11 | Testes e desempenho | Comparação comportamental, frames, sanitizers e profiling | Testes unitários do estado/ABI iniciados |
| 12 | Empacotamento | Windows, Linux e posteriormente Android ARM64 | Não iniciada |

## Próxima ordem de implementação

1. Compilar o NDSRecomp fixado e emitir os bancos reais.
2. Inventariar todos os símbolos indefinidos dos bancos ARM9/ARM7.
3. Mapear os segmentos reais de memória da revisão DS e adicionar aliases/mirrors confirmados.
4. Implementar MMIO e timing por faixa de endereço, mantendo as RAM transfers já existentes.
5. Integrar o emissor NDSRecomp ao registro e carregar tabelas ARM9/ARM7 reais.
6. Implementar chamadas/retornos, link slots e scheduler cooperativo com ciclos.
7. Ligar SWI/IRQ/CP15 e serviços essenciais conforme os caminhos reais de boot exigirem.
8. Só então avançar para vídeo, áudio, saves e UX do jogo.

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
