# Mapa de desenvolvimento do Oracis

Cada fase deve produzir uma entrega pequena, compilável, executável e verificável. Stubs, interfaces e código gerado sem execução não contam como subsistema concluído.

## Regras permanentes

1. Trabalhar diretamente na branch `main`; não criar branches.
2. Não copiar, incorporar ou ligar componentes derivados de melonDS.
3. Usar somente o alvo de compilação estática `nds_recompile` do NDSRecomp, em checkout externo e revisão fixada.
4. Implementar runtime, scheduler, memória e dispositivos do Oracis de forma independente.
5. Não usar um interpretador ARM geral como fallback de código dinâmico; regiões não recompiladas devem falhar com diagnóstico explícito.
6. ROM, BIOS, firmware, arquivos extraídos, saves e bancos gerados ficam fora do repositório, CI e releases.
7. A reutilização de código de pret/pokeplatinum continua bloqueada até esclarecimento dos direitos.
8. Logs e testes reproduzíveis devem acompanhar cada mudança relevante.

## Fases

| Fase | Área | Entrega principal | Estado |
|---:|---|---|---|
| 0 | Baseline | Revisão/região identificada; fontes e direitos avaliados | USA Rev 1 identificada; pret/pokeplatinum continua sem licença geral confirmada |
| 1 | NDSRecomp | Checkout externo fixado e compilação apenas do alvo nds_recompile | Script de bootstrap adicionado; execução/build pendentes |
| 2 | Extração e bancos | Validar header/segmentos ARM9 e ARM7; gerar TOMLs próprios; emitir e compilar bancos | Extração e hashes dos dois segmentos validados; emissão dos bancos pendente |
| 3 | Runtime e ABI | ABI C independente, estados ARM9/ARM7, dispatch estático e scheduler dual-CPU | Layout POD ARM, troca de modos/registradores bancados e operações CPSR/SPSR implementados/testados; ABI de helpers emitidos, dispatch e scheduler pendentes |
| 4 | Serviços base | Memória, exceções, IRQ, timers, DMA, IPC e serviços NitroSDK essenciais | Não iniciada |
| 5 | Boot visível e vídeo 2D | Primeiro quadro real; VRAM, paletas, BGs, sprites, OAM, janelas e blending | Não iniciada |
| 6 | Vídeo 3D | Geometry Engine, transformações, iluminação, atributos, texturas, transparência e fog | Não iniciada |
| 7 | Áudio | SDAT, SSEQ/SBNK/SWAR/STRM, canais e mixagem | Não iniciada |
| 8 | Entrada e UX | Teclado, gamepad, toque, controles virtuais e configurações | Host SDL3 de teste existe; integração ao runtime não iniciada |
| 9 | Saves e dados | Save/load, cache local versionado e verificação de integridade | Não iniciada |
| 10 | Testes e desempenho | Oracle comportamental externo, testes de frames, sanitizers e profiling | Não iniciada |
| 11 | Empacotamento | Windows, Linux e, após validação, Android ARM64 | Não iniciada |

## Pipeline de recompilação

1. Validar o SHA-1 da ROM selecionada localmente.
2. Ler o header e validar offsets/tamanhos dos segmentos ARM9/ARM7.
3. Extrair os segmentos para diretório local ignorado pelo Git.
4. Construir TOMLs próprios com SHA-1 do binário, load address, entry PC e seeds confirmados.
5. Rodar auditoria do recompilador e revisar instruções/cobertura.
6. Emitir os bancos C/C++ e tabelas de dispatch.
7. Compilar os bancos com o runtime ABI Oracis.
8. Registrar misses, instruções sem suporte e endereços não cobertos; não ocultar falhas em fallback genérico.

## Ordem da implementação do runtime

1. Estado CPU e ABI: registradores, CPSR, modos bancados, ARM/Thumb e helpers.
2. Dispatch: chamadas diretas/indiretas, retorno, interworking e identificação dos bancos por CPU.
3. Memória: regiões, espelhos, acesso alinhado/desalinhado e MMIO.
4. Scheduler: ARM9/ARM7, ciclos, eventos e sincronização IPC.
5. IRQ, timers, DMA, SWI e exceções.
6. OS/NitroSDK: heaps, eventos, espera/sincronização e serviços usados no boot.
7. FS/NitroFS, CARD/save, PAD/TP/RTC e stubs offline explícitos.
8. Vídeo 2D, vídeo 3D e áudio conforme os primeiros caminhos reais exigirem.

## Código dinâmico, overlays e cópias para RAM

O runtime do NDSRecomp usa um interpretador limitado como camada de fallback para código copiado para RAM. Oracis não vai importar essa camada. Precisamos inventariar code-copies, overlays e funções descobertas em tempo de execução, convertendo cada região executável em bancos recompilados e validados. Até isso ser resolvido, uma chamada para código desconhecido deve encerrar o teste com diagnóstico detalhado, não executar instruções guest por interpretação.

## Critério de primeiro boot nativo

- A cópia local é identificada como USA Rev 1.
- ARM9 e ARM7 são extraídos com offsets/tamanhos validados.
- O NDSRecomp gera bancos para ambos e as unidades geradas compilam.
- O runtime próprio faz dispatch inicial sem depender de melonDS ou do runner upstream.
- ARM9/ARM7 e o barramento têm testes unitários de estado e acesso.
- O loop de boot progride até o primeiro ponto de vídeo sem executar código guest por interpretador geral.
- A tela inicial é apresentada pelo host SDL3 com logs e evidências reproduzíveis.

## Critério de qualidade

Não anunciar compatibilidade, desempenho, áudio ou conclusão de subsistemas sem testes reproduzíveis, logs associados e registro das limitações conhecidas.
