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
| 3 | ABI e estado ARM | Estado ARM, CPSR/SPSR, exceções, flags e ponte C ABI | Estado/flags, User-bank helpers e CLZ implementados; CI multiplataforma e 21/21 testes confirmados na revisão 480651a |
| 4 | Memória e serviços base | Barramento, MMIO, IRQ, timers, DMA, IPC, CP15 e NitroSDK | Mapa NDS retail e WRAMCNT/WRAMSTAT; IME/IE/IF, IRQ fronteira, timers 0–3 por CPU, clock dual-core 2:1, HALT/wake básico; ARM9 CP15 parcial: ID/TCM size c0, V + ITCM/DTCM enable/load mode c1, regiões c9 e backings 32 KB ITCM/16 KB DTCM com espelhos e máscaras read/write/execute independentes; testes de load-mode e preservação da RAM subjacente passam; MPU, cache, callback LR arquitetural, VRAMCNT_B, demais MMIO, VRAM e timing pendentes |
| 5 | Dispatch e scheduler | Tabelas por CPU, validação de conteúdo, interworking e scheduler dual-core cooperativo | Registry, validação de bytes, pilha de retorno e orçamento/yield por CPU implementados; `run_dual_core_round()` sincroniza ARM9/ARM7 em 2:1 e contabiliza HALT/yield; a ponte C registra/remover tabelas locais opcionais e verifica presença dos bancos; CMake compila bancos locais; smoke test com fixtures sintéticas valida ligação e ciclo registro/remoção; API de carga em memória e carregador local verificado (tamanho/SHA-1 de ambos os segmentos antes de escrever) implementados; testes cobrem falta, truncamento, hash divergente e preservação do estado; host SDL desktop tenta carregar os segmentos no início quando bancos estáticos estão ligados; Android ainda não possui seleção de arquivos/diretório local; nenhuma plataforma executa rounds; provider arquitetural de LR ARM/Thumb, scheduler geral DS e cache de links pendentes |
| 6 | Boot visível e vídeo 2D | Primeiro quadro, VRAM, BGs, sprites, OAM e blending | Não iniciada |
| 7 | Vídeo 3D | Geometry Engine, transformações, iluminação, texturas e fog | Não iniciada |
| 8 | Áudio | SDAT, SSEQ/SBNK/SWAR/STRM, canais e mixagem | Não iniciada |
| 9 | Input e UX | Teclado, gamepad, toque e configurações | MMIO KEYINPUT/KEYCNT testados; ponte C retém estado até o vínculo do dispositivo e host SDL3 envia teclado desktop (X/Z, Enter/Backspace, setas, Q/W), com limpeza ao perder foco; o host inicializa `NdsMachine` pela ponte C ABI e conecta o teclado ao dispositivo vinculado; gamepad, EXTKEYIN e toque pendentes |
| 10 | Saves e dados | Save/load e cache local versionado | Não iniciada |
| 11 | Testes e desempenho | Comparação comportamental, frames, sanitizers e profiling | Testes unitários do estado/ABI iniciados |
| 12 | Empacotamento | Windows, Linux e posteriormente Android ARM64 | Não iniciada |


**Validação confirmada em 19/11/2026:** a revisão `3f52e1c` passou nos quatro jobs da CI. Desktop Linux: **30/30 CTests**; smoke de bancos sintéticos: **1/1**; tooling Python: **15 testes**; runtime ARM, Android ARM64 e compilador estático NDSRecomp passaram. O novo teste da fila de eventos por CPU também passou. Os artefatos da ROM continuam fora do Git/CI. [Execução da CI](https://github.com/CarlosCosta07-s/Oracis/actions/runs/44620734864). A CI não comprova boot do jogo.

## Próxima ordem de implementação

1. Ligar os quatro objetos corpo/dispatch às bibliotecas Oracis e confirmar a resolução dos 48 símbolos externos do inventário.
2. Completar os provedores de timing fail-closed (fetch, dados, multiply, refill e combinação ARM7) sem inventar custos; auditar alvos indiretos e overlays desconhecidos.
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
