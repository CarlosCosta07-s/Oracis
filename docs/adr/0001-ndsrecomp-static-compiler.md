# ADR 0001 — NDSRecomp como compilador estático externo

- **Status:** Aceito
- **Data:** 19/11/2026
- **Escopo:** Oracis — port nativo de Pokémon Platinum

## Contexto

O objetivo do Oracis é executar a lógica de Pokémon Platinum como código nativo, sem depender de uma CPU Nintendo DS interpretada em uma máquina virtual. O NDSRecomp oferece decodificação ARM7TDMI/ARMv4T e ARM946E-S/ARMv5TE, emissão de bancos nativos e um executável de recompilação separado do runner.

O runner upstream inclui componentes derivados de melonDS sob GPL-3.0-or-later. A regra de projeto do Oracis é não incorporar esses componentes.

## Decisão

1. Usar apenas o alvo NDSRecomp `nds_recompile` como ferramenta externa.
2. Fixar o upstream no commit `14b7e15023e9affc9dc375e4e611ac703f5c76ab`.
3. Construir o recompilador em checkout irmão, fora da árvore Git do Oracis.
4. Inicializar apenas o submódulo MIT `external/arm-recomp-core`.
5. Não configurar, compilar, copiar, ligar ou distribuir `runner`, `oracle`, frontend, shims ou componentes derivados de melonDS.
6. Implementar um runtime Oracis independente em C++20, com ABI C própria para os bancos recompilados.
7. Não usar interpretador ARM geral como fallback para código guest não recompilado; falhas de cobertura devem ser explícitas e diagnosticáveis.
8. Manter ROM, BIOS, firmware, segmentos extraídos, TOMLs por cópia e bancos gerados fora do Git, CI e releases.

## Licenças e proveniência

Na revisão fixada, o código próprio do recompilador está sob PolyForm Noncommercial 1.0.0 e permissões adicionais documentadas pelo upstream. O submódulo `arm-recomp-core` está sob MIT. Partes do runner upstream e de seu oracle estão sob GPL-3.0-or-later devido à derivação de melonDS e estão excluídas desta decisão.

Uma futura atualização do pin exige nova leitura de LICENSE, PERMISSIONS.md, THIRD_PARTY_ATTRIBUTION.md, .gitmodules e CMake do alvo. A licença do recompilador não concede direitos sobre Pokémon Platinum, sua ROM, firmware ou assets.

A reutilização de código pret/pokeplatinum permanece bloqueada até que a autorização/licença seja esclarecida.

## Consequências

- O Oracis precisa implementar o estado ARM9/ARM7, dispatch, memória, scheduler e serviços de dispositivo independentemente.
- O contrato C ABI será estudado em `recompiler/armv4t/runtime_arm.h`, mas o runtime upstream não será copiado.
- Overlays e código copiado para RAM precisam ser inventariados e convertidos em bancos recompilados validados.
- A primeira prova de integração precisa emitir e compilar bancos reais ARM9 e ARM7 da cópia local USA Rev 1.
- SDL3 continua sendo o frontend de host; o esqueleto atual ainda não está conectado ao runtime.

## Alternativas rejeitadas

- **Usar o runner completo do NDSRecomp:** rejeitado por trazer código derivado de melonDS e impor a fronteira GPL ao runtime do Oracis.
- **Copiar o runtime do NDSRecomp:** rejeitado, mesmo em partes que não sejam derivadas de melonDS; a ABI será implementada independentemente.
- **Executar código dinâmico por interpretador ARM geral:** rejeitado para manter o requisito de recompilação/execução nativa.
- **Incorporar pret/pokeplatinum agora:** rejeitado enquanto não houver base de autorização clara.
