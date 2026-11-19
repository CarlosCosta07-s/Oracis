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
| 1 | NDSRecomp | Checkout fixado e compilação isolada | Bootstrap/CI configurados; build do upstream pendente de execução |
| 2 | Extração e bancos | Extrair ARM9/ARM7 e emitir bancos C | Metadados e hashes dos segmentos verificados localmente; emissão pendente |
| 3 | ABI e estado ARM | Estado ARM, bancos, CPSR/SPSR, exceções, flags e ponte C ABI | Implementados/testados em harness local; dispatch real e símbolos de runtime pendentes |
| 4 | Memória e serviços base | Barramento, MMIO, IRQ, timers, DMA, IPC, CP15 e NitroSDK | GuestMemory configurável com aliases/permissões/testes; mapa DS, MMIO e ABI bus_* pendentes |
| 5 | Dispatch e scheduler | Tabelas por CPU, validação de conteúdo, interworking e scheduler dual-core cooperativo | Não iniciada |
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
4. Implementar MMIO e os wrappers C bus_read_*/bus_write_* com testes por CPU.
5. Implementar dispatch estático por CPU e modo ARM/Thumb, com validação de conteúdo.
6. Implementar retornos/chamadas, scheduler cooperativo, ciclos e sincronização.
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
