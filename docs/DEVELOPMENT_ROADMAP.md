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

## Base de CI verificada — 23/11/2026

A base validada pela CI #45209899276 é o commit 1e945e13b751add8e1dc2b2b68f41898c0fba164 (main à época). A CI [#45209899276](https://github.com/CarlosCosta07-s/Oracis/actions/runs/45209899276) passou nos quatro jobs: Desktop Linux e runtime ARM com 56/56 CTests cada, tooling Python com 53 testes, smoke sintético 1/1, Android ARM64 e compilador estático NDSRecomp aprovados.

O suporte de exceções integrado agora inclui Data Abort ARM9 para violações de acesso de dados da MPU e Prefetch Abort ARM9 quando o dispatch atinge um mapeamento executável cuja busca é negada pela MPU. Os caminhos ARM e Thumb incluem dispatch direto, link slots e interworking; o runtime restaura o estado no Data Abort e solicita unwind cooperativo após as entradas de Abort.

Limites que continuam explícitos: o modelo MPU/CP15 é parcial; c7 não opera sobre caches físicos porque ainda não existem; falhas de barramento externas/MMIO e aborts ARM7 ainda não são entregues como exceções guest; os vetores da BIOS ARM9 não têm handlers retail carregados; os bancos reais de Pokémon Platinum permanecem privados e não executados. A CI de bancos usa apenas fixtures sintéticas e não comprova boot nem primeiro quadro.

## Experimento Android de entrada nativa — commits 3a9bc39, d46f4db, 663c6ba, 45f9109, a496618 e 0f497d5

O host Android abre o seletor SDL3 para uma imagem NDS, BIOS ARM7 de 16 KiB e dump SPI de firmware do usuário (1 byte a 16 MiB). URIs content:// são lidas por SDL_IOStream. A ponte instala o dump na flash SPI ARM7; o dispositivo fornece os comandos READ (03h), RDSR (05h), WREN (06h) e WRDI (04h), com demais operações fail-closed. O host só segue para o carregamento dos segmentos depois de a sequência ROM/BIOS/firmware ser concluída.

A máquina prepara o subconjunto documentado do estado de entrada do cartucho: R0–R11 zerados; R12, LR e PC no entrypoint; SP User/System, IRQ e SVC específicos por CPU; metadados determinísticos derivados do cabeçalho/CRC; POSTFLG bit 0 = 1. O host verifica que os dois entrypoints resolvem para bancos estáticos registrados e executa até 128 fatias nativas de 1.024 ciclos por iteração SDL (131.072 ciclos agregados). Erros interrompem a execução e registram ciclo, fatia e status do runtime.

Esse fluxo é direct-entry, não executa o reset pelas BIOS retail nem reproduz toda a inicialização do firmware. A BIOS ARM7 escolhida pelo usuário fornece a tabela KEY1 para descriptografar a Secure Area; o firmware SPI bruto fica acessível ao guest, mas ainda faltam validação de header/CRCs, descriptografia e descompressão das partes IPL 1–5 e cópia dos fragmentos às regiões de RAM. Também faltam Chip ID físico no Android, outras áreas RAM de boot, CP15/MPU/TCM completos e serviços/overlays dependentes do jogo. Os rounds ainda não demonstram primeiro quadro ou compatibilidade jogável.

A CI Android atual compila a biblioteca nativa ARM64; ainda não há projeto Gradle/Manifest no repositório para gerar APK instalável. A compilação dos bancos reais exige artefatos locais privados; a CI não recebe ROM, BIOS, firmware, segmentos extraídos ou bancos derivados. O experimento precisa de validação em aparelho Android. Detalhes em [ANDROID_DIRECT_BOOT.md](ANDROID_DIRECT_BOOT.md).

## Fases

| Fase | Área | Entrega principal | Estado |
|---:|---|---|---|
| 0 | Baseline | Revisão/região e direitos avaliados | USA Rev 1 identificada; licença geral de pret/pokeplatinum não confirmada |
| 1 | NDSRecomp | Checkout fixado e compilação isolada | Bootstrap e alvo nds_recompile validados na revisão fixada; CI produz artefato temporário com compilador + scripts + headers ABI Oracis e checksum; identidade nds-codegen-v3 exigida pelo emissor; compilador usado localmente em 19/11 para emitir os bancos USA Rev 1 |
| 2 | Extração e bancos | Extrair ARM9/ARM7 e emitir bancos C | USA Rev 1 identificada e extraída localmente; última emissão privada documentada: 20.951 funções ARM9 (3.629 ARM e 17.322 Thumb) e 957 ARM7, divididas em 41 shards ARM9 + 2 ARM7; cobertura parcial (ARM9: 12 leituras não mapeadas/10 indiretos desconhecidos; ARM7: 388 leituras não mapeadas/30 indiretos desconhecidos); objetos e bancos reais permanecem fora do Git/CI e ainda não foram registrados/executados na máquina |
| 3 | ABI e estado ARM | Estado ARM, CPSR/SPSR, exceções, flags e ponte C ABI | Estado/flags, User-bank helpers e CLZ implementados; tracing e transferência dinâmica registram em buffer circular de 512 eventos por CPU; CI #45209899276 aprovada: runtime ARM 56/56 CTests e desktop 56/56 CTests; tooling Python 53 testes |
| 4 | Memória e serviços base | Barramento, MMIO, IRQ, timers, DMA, IPC, CP15 e NitroSDK | Mapa NDS retail e WRAMCNT/WRAMSTAT; IME/IE/IF, IPCSYNC e IPC FIFO bidirecional de 16 palavras por sentido, status/clear/erro e IRQs de sincronização/send-empty/receive-not-empty por borda; DMA0–3 independente por CPU, transferência imediata e disparos VBlank/HBlank/display-sync por linha visível integrados ao clock de vídeo, RAM mapeada de 16/32 bits, registradores SAD/DAD programados preservados, ponteiros internos recarregados na borda de enable e reload de DAD em repetição, incremento/decremento/fixo, DMAxFILL ARM9 para preenchimento 16/32 bits, preflight de intervalos e IRQ opcional; ARM9 main-memory-display (modo 4) com FIFO DISP_MMEM_FIFO de quatro palavras, bursts sob demanda e pixels RGB555 consumidos em ordem até VBlank; timers 0–3 por CPU, clock dual-core 2:1, HALT/wake básico; BIOS `IntrWait` (SWI 0x04) com KEEP/discard, shadow flags por CPU, suspensão não bloqueante e reentrada após IRQ não relacionada; `VBlankIntrWait` (SWI 0x05) aguardando o edge VBlank da linha 192; CustomHalt ARM7/CustomPost ARM9 (SWI 0x1F, HALTCNT/POSTFLG); SWI BIOS NDS7/NDS9 Div (0x09), CpuSet (0x0B), CpuFastSet (0x0C), Sqrt (0x0D), GetCRC16 (0x0E), IsDebugger (0x0F, perfil retail 4MB; debugger 8MB ainda não suportado), BitUnPack (0x10, expansão LSB-first em palavras de 32 bits, offset condicionado pelo bit 31 e preflight de memória) e LZ77UnCompWram/Vram (0x11/0x12, LZ10 (`0x10`, sem extensão LZ11), saída staged em byte/halfword) e HuffUnComp (0x13, árvores para símbolos 4/8 bits e palavras de 32 bits; callback guest fail-closed) e RLUnCompWram/Vram (0x14/0x15, runs literais/repetidos staged em byte/halfword), Diff8bitUnFilterWram (0x16, NDS9) e Diff16bitUnFilter (0x18, NDS9, acumulação modular de deltas); CRC-16 refletido e retorno do último halfword; BIOSPROT não modelado, então leituras da BIOS falham;  transferências prevalidam origem/destino, tratam 16/32 bits, fill e proteção BIOS do ARM7;  ARM9 CP15 parcial: ID/cache type/TCM size c0; V, MPU enable, controles de cache e ITCM/DTCM enable/load mode c1; atributos de cacheabilidade c2, bufferabilidade c3, permissões padrão/estendidas c5, oito regiões MPU c6, manutenção c7 como no-op ordenado sem caches físicos e regiões TCM c9 com backings 32 KB ITCM/16 KB DTCM e máscaras independentes; violações MPU ARM9 geram Data Abort no limite da instrução e Prefetch Abort para busca em mapeamento executável negada pela MPU, com LR arquitetural +8/+4 respectivamente; fetch uncached ARM9 e refill ARM9/ARM7 nas classes documentadas; DATA parcial NDS7/DATA e NDS9/DATA para Main RAM, WRAM/BIOS/I/O/OAM, Palette/VRAM no ARM9, VRAM C/D como Work RAM no ARM7 e TCM normal; ABI `runtime_mem_cycles_ex` com direção READ/WRITE e adaptador Oracis aplicado ao C gerado pelo NDSRecomp fixado; TCM load mode parcial: WRITE continua no TCM, READ do DTCM sobre Main RAM usa o custo externo, READ do ITCM em load mode ainda falha fechado por falta de região externa inferior temporizada; Gamecard/Wi-Fi, NDS7/DATA VRAM N32 provisório (1? ciclo) implementado e testado; confirmação física ainda pendente, backing físico de I/D cache e write buffer, contenção, latência/prioridade IRQ e integração de outros gatilhos IRQ, extended palettes, textura, expansão dos modos DMA por evento, temporização de transferências IPC e demais MMIO pendentes; `NdsDisplayControl` já gera DISPSTAT/VCOUNT sincronizados, com HBlank/VBlank e IRQs `NdsVramControl` implementa parcialmente VRAMCNT_A-I para MST=0/LCDC, A-D/E/F/G em MST=1/Main BG, A/B/E/F/G em MST=2/Main OBJ, C/D em MST=4/Sub BG/OBJ, H/I em MST=1/Sub BG, I em MST=2/Sub OBJ e C/D também em MST=2/Work RAM ARM7 (OFS=0/1), com aliases por CPU transacionais e rejeição de colisões; `NdsVideoMemory` instala 2 KiB de paletas padrão e 2 KiB de OAM como regiões ARM9-only, suprime STRB e permite STRH/STR; `NdsDisplayControl` mantém `DISPCNT` e `BG0CNT–BG3CNT` dos motores A/B; DISPSTAT/VCOUNT, scroll/affine, janelas, blending, PPU, avaliação OAM e arbitragem de scanline ainda pendentes |
| 5 | Dispatch e scheduler | Tabelas por CPU, validação de conteúdo, interworking e scheduler dual-core cooperativo | Registry, validação de bytes, pilha de retorno e orçamento/yield por CPU implementados; `run_dual_core_round()` sincroniza ARM9/ARM7 em 2:1 e contabiliza HALT/yield; a ponte C registra/remover tabelas locais opcionais e verifica presença dos bancos; CMake compila bancos locais; smoke test com fixtures sintéticas valida ligação e ciclo registro/remoção; API de tracing/live-transfer com buffer circular por CPU; API de carga em memória e carregador local verificado (tamanho/SHA-1 de ambos os segmentos antes de escrever) implementados; testes cobrem falta, truncamento, hash divergente e preservação do estado; host SDL desktop tenta carregar os segmentos no início quando bancos estáticos estão ligados; Android ainda não possui seleção de arquivos/diretório local; nenhuma plataforma executa código real dos bancos de Platinum; rounds sintéticos e scheduler cooperativo são testados; scheduler geral DS, latência/prioridade IRQ e cache de links pendentes |
| 6 | Boot visível e vídeo 2D | Primeiro quadro, VRAM, BGs, sprites, OAM e blending | Base de memória/configuração/temporização iniciada: VRAM A-I mapeada nos destinos CPU LCDC/BG/OBJ cobertos por MST/OFS; `NdsVideoMemory` oferece palette RAM padrão (2 KiB) e OAM (2 KiB) para ARM9, com STRB ignorados e STRH/STR aceitos; `NdsDisplayControl` mantém DISPCNT, BG0CNT–BG3CNT, scroll/affine, DISPSTAT/VCOUNT, janelas e registradores de blend. `Nds2dEffects` calcula máscaras de janela, mosaic e efeitos RGB555; `Nds2dTextBgFetcher` gera scanlines de BG texto 4bpp/8bpp; `Nds2dAffineBgFetcher` rasteriza BG2/BG3 affine clássicos nos modos 1, 2 e 4, além dos backgrounds affine estendidos por tiles nos modos 3/4 (BG3) e 5 (BG2/BG3). Os mapas clássicos usam entradas de 8 bits; os estendidos usam entradas de 16 bits com índice de tile de 10 bits e flips horizontal/vertical. Ambos os formatos de tile usam tiles 8bpp, matriz signed 8.8, referências signed 19.8, acumuladores PB/PD por scanline, overflow e mosaic. Também estão implementados BGs bitmap estendidos indexados 8bpp (paleta BG padrão, índice zero transparente) e direct-color 16bpp RGB555 (bit 15 como alpha), com base de VRAM em blocos de 16 KiB, dimensões BGxCNT e overflow; bitmaps maiores que 128 KiB são suportados somente na Engine A. Com DISPCNT.30, BG2/BG3 também usam paletas affine estendidas: os bits 12–15 da entrada de mapa selecionam uma das 16 paletas de 256 cores no slot de 8 KiB do BG; o índice zero é transparente. A Engine A consome VRAM E (slots 0–3) ou F/G (OFS=0 para slots 0/1 e OFS=1 para 2/3); a Engine B usa VRAM H MST=2. A leitura é feita pelo display controller a partir do backing físico e não torna os bancos acessíveis à CPU; falta de alocação ou conflito retorna UnsupportedMode. BGs de texto 8bpp também usam paletas estendidas com DISPCNT.30: BG0 escolhe Slot 0/2 e BG1 Slot 1/3 via BGxCNT.13; BG2/BG3 usam slots fixos 2/3. BGxCNT.7 ainda determina a profundidade, portanto tiles 4bpp permanecem no caminho de paleta padrão mesmo com DISPCNT.30 ativo. Engine A lê VRAM E/F/G e Engine B VRAM H MST=2 por acesso interno do display controller, sem expor os bancos aos CPUs. O compositor seleciona o fetcher de acordo com DISPCNT. A acumulação PB/PD e escritas visíveis de BGxX/BGxY estão integradas a VCOUNT, mas o ponto exato de cópia das referências em VBlank ainda é uma aproximação de frame boundary. `Nds2dObjFetcher` rasteriza OBJ tiled 4bpp/8bpp não affine e affine nas Engines A/B, com OAM, mapeamento 1D/2D, formas/tamanhos, clipping, flips não affine, matrizes PA/PB/PC/PD signed 8.8 selecionadas por Attr1[9:13], transformação inversa por pixel e bounds double-size, além de mosaic, transparência, semitransparência e OBJ-window. Também suporta bitmap OBJ direct-color RGB555: 2D com largura-fonte 128/256, 1D com fronteira de 128 bytes (256 na Engine A com extensão; Engine B fixo em 128 KiB), alphaBMP no bit 15 e alphaOAM em Attr2[12:15]. `Nds2dBgCompositor` ordena os dois pixels OBJ superiores junto aos BGs, por prioridade e desempate OAM; OBJ semitransparente e bitmap OBJ usam um BG/backdrop elegível como segundo alvo de alpha e nunca outro OBJ. Para bitmap OBJ, o alpha efetivo substitui os fatores de BLDALPHA. O compositor também aplica backdrop, janelas e brightness. O host também interpreta DISPCNT display mode 0 como tela branca e Engine A mode 2 como bitmap RGB555 direto do banco VRAM A-D selecionado, exigindo alocação LCDC e preservando a linha de saída em caso de falha. Engine A mode 3 continua usando DISP_MMEM_FIFO. `MASTER_BRIGHT`/`DB_MASTER_BRIGHT` possuem registradores independentes por engine e são aplicados depois da seleção do modo de saída, inclusive gráficos, display-off, VRAM direto e Main Memory Display. O `Nds2dObjFetcher` também aplica os limites de OBJ por scanline do DS, contabilizando 2124 ciclos com processamento durante H-Blank ou 1530 sem ele (DISPCNT.23), cobrando um ciclo por pixel normal, dois por pixel affine, dez ciclos de setup affine, dois ciclos para cada OBJ habilitado fora da scanline atual e mais dois ciclos quando um OBJ na linha está parcialmente cortado pelos limites da tela; a avaliação interrompe em ordem OAM quando o orçamento termina. O compositor agora recebe uma scanline BG0 3D explícita para Engine A quando DISPCNT.3=1, respeita BG0HOFS no domínio horizontal de 512 pixels, transparência alpha=0, prioridade, janelas e registro BG0 como camada 3D na saída. A ausência da fonte 3D falha fechada. A referência técnica do DS documenta que BG0 3D pode exigir blending com o segundo alvo selecionado imediatamente atrás mesmo quando BLDCNT está em None ou em brightness; como a conversão dos 5 bits de alpha 3D em coeficientes EVA/EVB não está suficientemente especificada, o compositor falha fechada nesses casos em vez de aproximar a cor. Isso define e testa a fronteira 2D/3D, mas não implementa Geometry Engine, rasterização de polígonos ou cache real de 48 linhas. Arbitragem/timing de fetch PPU e apresentação final ainda faltam; nenhum primeiro quadro ou boot do jogo foi comprovado |
| 7 | Vídeo 3D | Geometry Engine, transformações, iluminação, texturas e composição | Engine Geometry com transformação/clipping, iluminação e texgen; sampling de texturas 2D/4×4; rasterização por scanline com depth-test, alpha-test/blending, rear-plane uniforme/bitmap, fog e Toon/Highlight via TOON_TABLE. Engine A BG0 usa staging RGB6/A5 e FIFO de 48 linhas acionado pelo relógio sincronizado. Shadow mode em duas passagens, wireframe opaco e Edge Marking com pós-passe de vizinhança implementados; a cobertura de wireframe/contornos ainda é aproximada e precisa de comparação com hardware. Auto-sort aceita buffers somente sólidos; alpha 1–30 continua fail-closed até Y-sort. Ainda faltam timing sub-scanline/ciclo-a-ciclo, apresentação Android validada e primeiro boot/frame real de Platinum. Detalhes em [NDS_3D_TEXTURE_PIPELINE.md](NDS_3D_TEXTURE_PIPELINE.md) |
| 8 | Áudio | SDAT, SSEQ/SBNK/SWAR/STRM, canais e mixagem | Não iniciada |
| 9 | Input e UX | Teclado, gamepad, toque e configurações | MMIO KEYINPUT/KEYCNT testados; ponte C retém estado até o vínculo do dispositivo e host SDL3 envia teclado desktop (X/Z, Enter/Backspace, setas, Q/W), com limpeza ao perder foco; o host inicializa `NdsMachine` pela ponte C ABI e conecta o teclado ao dispositivo vinculado; gamepad, EXTKEYIN e toque pendentes |
| 10 | Saves e dados | Save/load e cache local versionado | Não iniciada |
| 11 | Testes e desempenho | Comparação comportamental, frames, sanitizers e profiling | Testes unitários do estado/ABI iniciados |
| 12 | Empacotamento | Windows, Linux e posteriormente Android ARM64 | Não iniciada |


**Validação mais recente (22/11/2026):** commit de correção `062ece4` e CI [#1120](https://github.com/CarlosCosta07-s/Oracis/actions/runs/45070941504) concluída com sucesso nos quatro jobs. Desktop Linux: **54/54 CTests** e smoke de ligação static-bank **1/1**; ARM runtime: **54/54 CTests**; Android ARM64 e compilador estático NDSRecomp aprovados. O teste novo de fog valida registradores MMIO, tabela de densidade, interpolação, densidade máxima, alpha-only e a flag por pixel. Isso valida builds/testes sintéticos, não o boot nem a apresentação de Pokémon Platinum. ROM, BIOS, firmware e dados extraídos permanecem fora do Git/CI.

## Próxima ordem de implementação

1. Validar no aparelho Android a seleção SAF da ROM e da BIOS ARM7 e confirmar a leitura dos URIs content:// por SDL_IOStream.
2. Preparar o empacotamento Android instalável; a CI atual compila somente a biblioteca nativa ARM64.
3. Construir localmente com os bancos privados reais, confirmar a resolução dos entrypoints ARM9/ARM7 e capturar o primeiro dispatch guest do experimento direct-entry.
4. Resolver o primeiro dispatch miss, acesso MMIO não suportado, falha de timing ou dependência BIOS/NitroSDK observada pelo scheduler, sem fallback de interpretador.
5. Completar o fornecimento Android do Chip ID físico; implementar o IPL do firmware SPI (validação, CRC, KEY1, descompressão e cópia dos módulos 1–5), mantendo os dumps e os módulos derivados privados do usuário.
6. Auditar os 60 overlays ainda sem banco e os destinos de chamadas indiretas desconhecidos; integrar os gerenciadores NitroSDK/HLE necessários.
7. Completar a sincronização de vídeo, apresentar e verificar o primeiro quadro real; depois avançar áudio, saves e UX.


## IPC ARM9/ARM7

O dispositivo `NdsIpcControl` modela IPCSYNC e as duas filas FIFO independentes. Cada direção comporta 16 palavras de 32 bits; FIFO enable, send-empty/receive-not-empty IRQs por borda, clear, overflow/underflow error, leituras sem pop quando desabilitada e envio de IRQ por IPCSYNC são exercitados por testes. O acesso é cooperativo no barramento central; contenção, wait states e operação concorrente em threads não são simulados.

## DMA

O `NdsDmaControl` mantém quatro canais independentes por CPU e os registradores SAD/DAD/COUNT+CONTROL na janela `0x040000B0–0x040000DF`. Os valores programados de SAD/DAD permanecem inalterados após cada cópia; os ponteiros internos são inicializados quando ENABLE sobe de 0 para 1. Cada disparo repetido recarrega a contagem, enquanto a origem interna continua avançando entre eventos; o destino interno é restaurado ao DAD programado somente no modo incremento/reload. A implementação também expõe os quatro registradores DMAxFILL exclusivos do ARM9 em `0x040000E0–0x040000EC`, usados como origem fixa para preenchimento repetido de 16 ou 32 bits. O runtime executa as transferências em memória guest mapeada, com incremento/decremento/fixo, preflight das faixas completas de destino e IRQ de conclusão opcional. Os contadores zero respeitam os limites por canal e CPU. Modos VBlank/HBlank e ARM9 display-sync por início de cada linha visível disparam no clock do display. O controlador Game Card inicial expõe AUXSPICNT com habilitação de slot/modo paralelo e IRQ, ROMCTRL, comandos 9F (dummy de 8 KiB), 00 (cabeçalho raw de 0x200 bytes) e 90 (Chip ID raw configurável pelo host), além do B7 e da porta de dados de 32 bits em 0x04100010. B7 redireciona endereços abaixo de 0x8000 para 0x8000+(addr&0x1FF), aceita endereços não alinhados e faz wrap dentro da janela de 4 KiB ativa; blocos de 4 a 16384 bytes são pré-validados byte a byte contra a imagem montada. O slot exige ROM paralelo, DRQ por palavra, clock serial e IRQ de conclusão. Os registradores de seed são escrita-only e independentes por CPU; ROMCTRL.bit15 agora aplica os dois valores de 39 bits ao stream KEY2 compartilhado, com reversão de bits documentada. Os bits de cifragem de comando/dados são integrados após a ativação KEY1 3C/4; o scheduler trata gaps e o fluxo KEY2, inclusive comandos B7/B8 com stream persistente. O DMA Game Card está integrado por DRQ: ARM9 timing 5 e ARM7 timing 2, fonte fixa 32-bit, uma palavra por pedido, repetição por canal e destino em memória mapeada. Testes do dispositivo cobrem um bloco ARM9 de 128 palavras e um pedido ARM7. A integração de máquina percorre um B7 de 0x200 bytes como 128 pedidos DRQ, copia via DMA0 para RAM ARM9, confere cada palavra e valida IRQs do DMA e do cartucho; no ARM7, um B7 de uma palavra percorre DMA1 timing 2 e valida os dois pedidos e a entrada IRQ nativa no vetor 0x00000018. Ambos usam imagem sintética fornecida pelo teste. O núcleo criptográfico KEY1 implementa carregamento externo da tabela 0x1048 do BIOS ARM7, derivação por Game Code nos níveis 1–3 e cifragem/decifragem de blocos de 64 bits, com vetores sintéticos fixos; nenhum BIOS ou chave foi incluído. A primitiva KEY2 agora deriva os dois estados de 39 bits a partir do valor aleatório de 24 bits já decifrado do comando 4 e do índice de seed em Header[0x13], incluindo a reversão de bits dos registradores, e aplica o fluxo XOR em buffers contínuos. O Chip ID do cartucho deve ser fornecido explicitamente pelo host porque não pode ser recuperado apenas dos bytes da imagem .nds. O valor aleatório mmmnnn ainda deve ser obtido pela etapa KEY1/RTC do chamador. Os comandos KEY1 0x40/0x10/0x20/0xA0, os gaps, o padding KEY2 e os comandos KEY2 B7/B8 estão integrados ao controlador e cobertos por testes antigos/novos. A ABI `runtime_irq(return_pc)` agora está ligada à máquina: `runtime_tick` faz polling na fronteira de instrução, deriva LR_irq = PC seguinte + 4, respeita IME/IE/IF e CPSR.I e despacha a entrada de IRQ no vetor ARM9 alto ou ARM7 baixo dentro da fatia ativa. O scheduler usa o mesmo handler em limites de round e mantém o provider explícito apenas para executores externos. O teste C do host aciona KEYCNT nos dois processadores, valida SPSR/LR/vetor e retorna por `runtime_exception_return`; uma rota usa chamada direta e a outra a consulta automática de `runtime_tick`. O teste de máquina também cobre a entrada nativa em fronteira segura sem provider: IRQs simultâneas ARM9/ARM7, LR, SPSR, vetores alto/baixo e retorno de exceção. Outro caso de integração arma Timer 0 em `0xFFFF` nos dois CPUs, avança um round pelo relógio comum e confirma overflow, IF, LR, SPSR e entrega de IRQ nativa nos vetores correspondentes. Os testes de máquina também provocam IRQ de conclusão do DMA0 após cópia real em RAM e IRQ IPCSYNC remota do ARM9 para ARM7; ambos confirmam entrada pelo handler nativo, sem provider externo. A primitiva de descriptografia da Secure Area valida encryObj. `oracis_host_runtime_load_game_card_program_segments()` confere os campos ARM9/ARM7 do cabeçalho NDS, exige correspondência byte a byte com a imagem montada e aplica o prefixo descriptografado de 0x800 bytes à cópia ARM9; `oracis_host_runtime_load_mounted_game_card_program()` deriva os segmentos diretamente do cabeçalho com limites verificados. O host SDL chama esse caminho quando recebe `--game-card` e `--arm7-bios`. Testes sintéticos cobrem ausência da tabela BIOS, metadados divergentes, bytes divergentes, identificador inválido, intervalo fora da imagem e carga válida. Isso apenas materializa os segmentos e prepara PC/CPSR.T: não substitui o setup do BIOS nem comprova o boot completo. O comando 0x60, escrita NAND/FLASH, SPI backup e o boot completo continuam pendentes. O modo ARM9 main-memory-display (modo 4) abastece DISP_MMEM_FIFO em bursts de quatro palavras de 32 bits sob demanda; o clock do display consome os pixels RGB555 low/high a cada seis ciclos de barramento (1536 ciclos para 256 pixels), mantém o limiar HBlank separado em 1606 ciclos, registra o framebuffer 256x192 e encerra o canal no VBlank. O bridge de vídeo do host lê as linhas produzidas pelo clock sem consumir FIFO em ritmo de renderização SDL. A sincronização com emulação real e a apresentação de um jogo ainda não foram comprovadas. DMA timer-overflow não pertence ao NDS clássico (é recurso de NDMA do DSi). GBA Slot, geometry FIFO e outros gatilhos, bloqueio do CPU e arbitragem/timing ainda não foram integrados.

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


## Atualização 2026-11-22 — amostragem inicial de texturas 3D

A camada de textura agora possui uma interface de amostragem independente (`Nds3dTextureSampler`) e leituras internas validadas na `NdsVramControl`. O Geometry Engine decodifica o comando `PLTT_BASE` (0x2B) e preserva seus 13 bits por polígono junto de `TEXIMAGE_PARAM`. Os slots de imagem 3D (A-D, MST=3, OFS 0-3) e os slots de paleta (E para 0-3; F/G para 0, 1, 4 ou 5) são resolvidos pelo mapa de VRAM; ausência ou colisão retorna diagnóstico fail-closed. O sampler implementa lookup nearest-texel para A3I5, 2bpp, 4bpp, 8bpp, A5I3 e Direct RGB555, com alpha, unidades de PLTT_BASE, clamp/repeat/repeat+flip. Naquele marco, o formato comprimido 4x4 ainda estava pendente e o sampler ainda não estava ligado a um rasterizador; essas limitações foram parcialmente superadas pelas atualizações posteriores abaixo. Este trecho é histórico e não descreve o estado atual do código. Os testes usam apenas dados sintéticos e não comprovam boot ou compatibilidade jogável. Consulte [NDS_3D_TEXTURE_PIPELINE.md](NDS_3D_TEXTURE_PIPELINE.md).


## Atualização 2026-11-22 — decodificação de texturas comprimidas 4×4

O sampler inicial agora decodifica blocos comprimidos 4×4: 32 bits/16 texels por bloco nos slots 0 ou 2, metadados de 16 bits por bloco no slot 1 (metade inferior associada ao slot 0; metade superior ao slot 2), offset de paleta em unidades de quatro bytes, além dos modos 0–3 de transparência/interpolação. Os testes sintéticos exercitam transparência, média 1:1, seleção das quatro cores e ponderações 5:3/3:5. O caminho de slot 2 tem um vetor sintético para a metade superior dos metadados; limites extremos ainda precisam de vetores adicionais. A CI #1036 passou nos quatro jobs (Desktop Linux, ARM runtime, Android ARM64 e NDSRecomp), com 50/50 testes no job de runtime. Em seguida foi adicionado `Nds3dRasterizer`, que percorre o buffer renderizado após SWAP_BUFFERS e gera fragmentos por scanline com cobertura top-left, profundidade normalizada, cor por vértice, S/T com correção de perspectiva e modos de textura 0/1. O teste de integração constrói um triângulo pelo Geometry Engine, realiza o swap no VBlank e verifica um fragmento texturizado; sua última revisão usa W distintos para separar interpolação perspectiva da afim. A CI #1048 concluiu com sucesso nos quatro jobs: Desktop Linux x86-64, ARM runtime, Android ARM64 e NDSRecomp estático. O job de runtime e o Desktop executaram 51/51 testes, incluindo `oracis_nds_3d_rasterizer_test`. A regressão constrói dois polígonos sobrepostos com W variável e comprova seleção de texel por interpolação perspectiva (distinta da afim). A interpretação inicial de alpha zero como fragmento transparente de `POLYGON_ATTR` foi corrigida na atualização abaixo: esse valor seleciona wireframe; transparência total do fragmento é determinada pelo alpha efetivo do texel e da cor combinada. Ainda não há teste concluído que comprove profundidade/composição de polígonos, fog blending, modos toon/shadow, cache de 48 linhas, integração BG0 ou apresentação de um frame completo. Consulte [NDS_3D_TEXTURE_PIPELINE.md](NDS_3D_TEXTURE_PIPELINE.md).


## Atualização 2026-11-22 — descarte de transparência e teste de profundidade 3D

A revisão do rasterizador alinhou `POLYGON_ATTR` à definição do hardware: alpha de polígono zero seleciona wireframe e permanece explicitamente não suportado nesta etapa; não é convertido em um fragmento interior transparente. Separadamente, o descarte é aplicado ao alpha final após o modo de textura: Modulation com texel alpha zero gera fragmento transparente e é descartado; Decal com texel alpha zero preserva a cor e o alpha do vértice. A regressão de integração cobre as duas rotas, além de wireframe e da interpolação perspectiva.

Foi acrescentado `Nds3dDepthBuffer`, com armazenamento de profundidade 24-bit para 256×192 pixels, teste Less, Depth Equal com tolerância inclusiva ±0x200, gravação para pixels opacos e gravação de pixels translúcidos somente quando `POLYGON_ATTR.Bit11` permite. Fragmentos totalmente transparentes não passam nem atualizam profundidade. O novo teste cobre comparações, limite da tolerância, controle de depth-write e linhas inválidas. A etapa consome os fragmentos na ordem recebida e não implementa ordenação automática/manual nem composição de cor/alpha.

A CI #1051 validou esta atualização nos quatro jobs: Desktop Linux, ARM runtime, Android ARM64 e compilador NDSRecomp estático. Desktop e ARM runtime executaram 52/52 CTests, incluindo as regressões de rasterização e profundidade 3D. Isso valida esta etapa, não o pipeline 3D completo nem o boot de jogo. Consulte [NDS_3D_TEXTURE_PIPELINE.md](NDS_3D_TEXTURE_PIPELINE.md).


## Atualização 2026-11-22 — opções Z/W por buffer e teste de profundidade no rasterizador

A Geometry RAM agora preserva os dois bits de `SWAP_BUFFERS` associados ao conjunto que acabou de ser entregue ao Rendering Engine. O parâmetro do comando atual continua configurando somente o próximo conjunto de geometria; a opção anterior fica vinculada ao render buffer já publicado. Isso evita que o modo Z/W de uma construção seguinte altere a profundidade da imagem em renderização.

O rasterizador consulta o bit 1 do próprio render buffer. Em Z-buffer, continua usando Z/W normalizado; em W-buffer, reconstrói W de forma perspectiva-correta a partir dos W de clip-space. Foi acrescentada a entrada `render_scanline_depth_tested`, que filtra os fragmentos pelo depth buffer antes de encaminhá-los ao próximo consumidor. O teste de integração percorre duas trocas consecutivas para provar a latência de um swap: o primeiro buffer permanece em Z, enquanto o segundo usa W mesmo quando o segundo comando já seleciona Z para a terceira construção.

O Depth Equal usa ±0x200 em Z-buffer (valor documentado por GBATEK) e ±0xFF em W-buffer (observação de engenharia reversa da comunidade, ainda pendente de conformance test em hardware). O depth buffer continua pertencendo ao consumidor por frame; clear/rear-plane reais, ordenação de polígonos translúcidos, composição RGBA, fog, modos toon/shadow e cache 48-line continuam pendentes. CI ainda precisa validar esta atualização.

Referência: [GBATEK — SWAP_BUFFERS e profundidade](https://mgba-emu.github.io/gbatek/#ds-3d-video). Observação de tolerância W: [discussão técnica de depth buffering](https://melonds.kuribo64.net/board/thread.php?id=13&page=1).


## Atualização 2026-11-22 — composição de cor 3D em ordem manual

Foi acrescentado `Nds3dColorBuffer`, um buffer de staging de software 256×192 com RGB6, alpha5, polygon ID e fog-enable. A rotina de composição aplica a equação DS por canal `(source × (alpha + 1) + destination × (31 − alpha)) / 32`, usa `max(sourceAlpha, destinationAlpha)` e respeita os três desvios documentados: blending desabilitado, fragmento opaco e destino com alpha zero. Fragmentos com alpha final zero não alteram a cor.

A nova API `render_scanline_composited` encadeia o alpha test (alpha final maior que a referência), teste de profundidade e composição. A rejeição no alpha test ocorre antes da atualização de profundidade. Os parâmetros de `DISP3DCNT` e `ALPHA_TEST_REF` ainda são fornecidos à API; sua integração MMIO não foi feita. Com `SWAP_BUFFERS.Bit0=0`, o consumidor permite renderizar buffers compostos somente por polígonos sólidos (alpha 31); polígonos com alpha 1–30 continuam retornando `TranslucentSortingNotSupported` até a implementação da ordenação Y automática. O buffer 256×192 é uma superfície provisória do software, não o cache hardware de 48 linhas.

O teste `oracis_nds_3d_color_buffer_test` cobre a fórmula, alpha máximo, blending desativado, rear-plane transparente e descarte alpha-zero. A integração do rasterizador cobre auto-sort fail-closed, alpha-test antes do depth write e composição de scanline W-buffer em modo manual. A CI #1056 passou nos quatro jobs: Desktop Linux, ARM runtime, Android ARM64 e NDSRecomp estático. Desktop e ARM runtime executaram 53/53 CTests. Isso ainda não comprova a saída 3D completa, integração BG0, apresentação de quadro ou boot de jogo.

Referência de composição: [GBATEK — alpha blending, alpha test e DISP3DCNT](https://mgba-emu.github.io/gbatek/#ds-3d-toon-edge-fog-alpha-blending-anti-aliasing).


## Atualização 2026-11-22 — rear-plane uniforme e validação multiplataforma

`NdsDisplayControl` agora modela `CLEAR_COLOR` (32 bits), `CLEAR_DEPTH` (15 bits úteis) e `CLRIMAGE_OFFSET` (16 bits: dois offsets de 8 bits) no MMIO ARM9, todos write-only. A inicialização uniforme usa RGB555 expandido a RGB6, alpha5, fog-enable e polygon ID6 de `CLEAR_COLOR`, além de depth15 expandido a depth24 por `raw × 0x200 + ((raw + 1) / 0x8000) × 0x1FF`. O teste cobre também o endpoint de profundidade máxima.

`Nds3dRasterizer::initialize_rear_plane` prepara os buffers de staging a partir desses valores. O modo bitmap de rear-plane (`DISP3DCNT.Bit14`) é fail-closed e não modifica os buffers, porque a leitura dos mapas de cor/profundidade nas Texture Slots 2/3 e o offset de imagem ainda não foram implementados. O armazenamento de saída continua sendo um staging buffer 256×192 do software, sem o cache de hardware de 48 linhas.

A CI #1061 passou nos quatro jobs: Desktop Linux, ARM runtime, Android ARM64 e NDSRecomp estático. Desktop e ARM runtime executaram 53/53 CTests; o build/auditoria opcional local também passou. Isso valida os registradores e o rear-plane uniforme, mas não o rear-plane bitmap, o cache 48-line, composição BG0 ou a apresentação de um frame completo. Consulte [NDS_3D_TEXTURE_PIPELINE.md](NDS_3D_TEXTURE_PIPELINE.md).


## Atualização 2026-11-22 — rear-plane bitmap nos slots 2/3

O modo `DISP3DCNT.Bit14=1` deixou de ser recusado. A leitura halfword de um slot de textura foi exposta pelo `NdsVramControl` e encaminhada pelo `Nds3dTextureSampler`, preservando a validação de slot, alinhamento, limite de 128 KiB e alocação física única. O slot 2 fornece a imagem RGB555 com alpha binário em bit15; o slot 3 fornece depth15 com fog-enable em bit15. O polygon ID permanece extraído de `CLEAR_COLOR`.

`initialize_rear_plane` agora amostra o bitmap 256×256 para preencher as 256×192 posições visíveis. `CLRIMAGE_OFFSET` preserva os 16 bits: byte baixo = deslocamento horizontal/coluna; byte alto = deslocamento vertical/linha. Os dois eixos envolvem em 256 e os pixels transparentes preservam RGB e alpha zero no buffer, conforme o formato de bitmap. Antes de alterar os buffers, o renderer verifica todos os halfwords visíveis dos dois slots; um slot ausente/ambíguo falha com `TextureFailure` sem limpar parcialmente a imagem.

A regressão cria imagens artificiais nos slots 2/3 e testa alpha sólido/transparente, RGB555→RGB6, fog independente, valores de profundidade (inclusive 0x7FFF→0xFFFFFF), deslocamento horizontal e vertical, wrap da última linha visível e slot 3 temporariamente ausente. O teste do sampler cobre o acesso halfword aos slots 2/3 e limites. A CI #1067 passou nos quatro jobs: Desktop Linux, ARM runtime, Android ARM64 e NDSRecomp estático; Desktop e ARM executaram 53/53 CTests, e a verificação local opcional executou 1/1.

Permanece pendente integrar a superfície 256×192 ao cache hardware de 48 linhas e ao caminho Engine A BG0; esta etapa não comprova quadro 3D completo nem boot de jogo. Consulte [NDS_3D_TEXTURE_PIPELINE.md](NDS_3D_TEXTURE_PIPELINE.md).


## Atualização 2026-11-22 — FIFO 3D de 48 linhas e entrada BG0

Foi criado `Nds3dScanlineCache`, um FIFO circular limitado a 48 scanlines BG0. A fila controla cursores de produtor/consumidor, exige sequência estrita de linhas, rejeita RGB555/alpha fora do domínio e precisa ser reiniciada a cada frame. A numeração não faz wrap implícito de 191 para 0.

`Nds3dColorBuffer::copy_bg0_scanline` converte a superfície RGB6/A5 para RGB555/alpha5 da entrada BG0. `Nds3dRasterizer::render_scanline_to_cache` verifica capacidade e ordem antes de rasterizar, compõe a linha com os registradores MMIO e enfileira a saída convertida. O compositor 2D ganhou uma sobrecarga com `Nds3dScanlineCache`: quando Engine A seleciona 3D para BG0, consulta exatamente a linha solicitada e só a remove depois que a composição 2D inteira termina com sucesso. FIFO vazio, linha fora de ordem ou falha na composição preservam o buffer de saída; uma falha de composição também mantém a linha pendente na fila.

Os testes verificam capacidade 48, full/empty, wrap do anel, ordem do produtor/consumidor, reset, alpha inválido, conversão RGB6→RGB555, consumo BG0, underflow e retenção da linha diante de falha de composição. A CI #1070 passou nos quatro jobs: Desktop Linux e ARM runtime com 54/54 CTests, Android ARM64 e NDSRecomp estático; a checagem opcional de static-bank executou 1/1.

A integração já está conectada à `NdsMachine` e à apresentação desktop. O relógio sincronizado agenda o primeiro lote de 48 linhas em scanline 214, durante VBlank, antes do início da saída no frame seguinte; cada limite de scanline visível repõe os slots liberados. A temporização ainda é por evento de scanline, não ciclo-a-ciclo, e não implementa ainda o comportamento completo de underflow/overflow, `RDLINES_COUNT` nem a apresentação Android. Não há comprovação de quadro de jogo ou boot. Consulte [NDS_3D_TEXTURE_PIPELINE.md](NDS_3D_TEXTURE_PIPELINE.md) e [NDS_2D_PIPELINE.md](NDS_2D_PIPELINE.md).


## Atualização 2026-11-22 — integração do BG0 3D à máquina e ao host

A `NdsMachine` agora possui o sampler de texturas, rasterizador, buffers de cor/profundidade e FIFO BG0 de 48 linhas. Quando Engine A está no modo gráfico e BG0 está habilitado como 3D, o host desktop prepara uma janela progressiva de até 48 scanlines e usa a sobrecarga cache-aware do compositor 2D. A janela avança conforme o host solicita cada linha; saltos ou ativação tardia do BG0 3D reinicializam a fila e o rear-plane. A regressão de máquina cobre preenchimento inicial, avanço da janela e recuperação após linha perdida.

A produção e o consumo de BG0 3D estão integrados à máquina e ao host desktop. O produtor prepara 48 linhas na scanline 214 durante VBlank; o compositor consome as linhas em ordem e o relógio repõe o espaço disponível a cada limite visível. O RDLINES_COUNT expõe uma aproximação do menor nível da FIFO no quadro anterior, fechada em VBlank. Permanecem pendentes a temporização sub-scanline/ciclo-a-ciclo e a apresentação Android; este marco ainda não demonstra um quadro real do Pokémon Platinum.


## 2026-11-22 follow-up — 3D scanline scheduling

The 3D BG0 lookahead producer is now driven by the synchronized display clock.
The display device reports completed scanline boundaries to its owning machine;
the system clock splits elapsed bus time at those boundaries and advances the
geometry engine before the display callback. This ensures the line-192 VBlank
notification is delivered before the scanline-214 producer start. The first 48
visible lines are queued during VBlank, before the 262-to-0 frame wrap begins
output at line zero. Each later visible boundary refills free slots without
writing into a full FIFO. The machine test exercises VBlank pre-render and one
rolling refill using synthetic bus time.

Still outstanding: sub-scanline geometry/render timing, measured FIFO
underflow behavior, cycle-exact RDLINES_COUNT sampling, Android BG0 presentation,
translucent polygon sorting, and an actual Platinum boot/frame.


The `DISP3DCNT` bits 12/13 retain sticky status and clear on a
write-one acknowledgement. A missing BG0 3D scanline source sets bit 12;
actual Geometry Engine polygon/vertex RAM submission overflow sets bit 13.
The machine integration test submits 2,049 valid triangles against the
2,048-polygon capacity and verifies that the status bit is latched.


The DISP3DCNT polygon/vertex RAM overflow bit is now connected to actual
Geometry Engine submission failures. The machine integration test fills the
2,048-polygon buffer with valid triangles, submits one additional triangle,
and checks that bit 13 is latched.


RDLINES_COUNT (0x04000320) is now readable by ARM9 as a read-only byte. The machine measures the minimum BG0 FIFO occupancy after consumption during one displayed frame and publishes the clamped, minus-two value in the 0..46 range at VBlank entry (scanline 192), before the next pre-render at scanline 214. This is a scanline-level low-water approximation, not a cycle-accurate implementation of the hardware's internal FIFO sampling latency.


The display-boundary observer now treats only consecutive VCOUNT transitions
as scanline scheduling edges. A software jump directly to line 192 cannot
finalize the FIFO low-water measurement, and a jump directly to line 214 cannot
restart pre-render. Natural 262-to-0 frame wrap remains the separate frame
boundary. The machine regression covers both cases; CI run #1112 passed all
four jobs (Desktop Linux, ARM runtime, Android ARM64 and pinned NDSRecomp).


## Atualização 2026-11-22 — composição de polígonos sólidos em auto-sort

O rasterizador não bloqueia mais todo o modo automático de `SWAP_BUFFERS`: quando o buffer submetido contém apenas polígonos sólidos (alpha 31), o teste de profundidade é suficiente e a composição segue normalmente. Polígonos com alpha 1–30 continuam falhando fechados porque a ordenação Y de translúcidos ainda não foi implementada. Alpha zero continua identificado como wireframe não suportado. A regressão cobre os dois lados dessa fronteira. A CI da revisão está em execução; não há alegação de conformidade visual ou boot de jogo.


## Atualização 2026-11-22 — fog 3D no MMIO e na composição

`NdsDisplayControl` agora mantém `FOG_COLOR`, `FOG_OFFSET` e as 32 entradas
de `FOG_TABLE`, todos ARM9 write-only. O compositor usa as fronteiras de fog
derivadas de `FOG_SHIFT`, interpola a densidade entre entradas, converte o
depth24 de staging para o domínio 15-bit e aplica a mistura no fim da linha
somente aos pixels com fog-enable. O modo alpha-only preserva RGB e atualiza
alpha. A flag da framebuffer é substituída por polígonos sólidos e combinada
por AND em pixels translúcidos.

A regressão cobre permissões/larguras e máscaras MMIO, escritas empacotadas,
amostras em diferentes profundidades, densidade 127 como total, alpha-only e
semântica da flag. A CI #1120 passou: ARM e Desktop 54/54, smoke static-bank
1/1, Android ARM64 e NDSRecomp aprovados. Ainda faltam comparação com capturas
de hardware, correção do glitch de alpha próximo à primeira fronteira e testes
separados para escalas Z/W. Não há boot nem frame real de Platinum comprovado.


## Atualização 2026-11-22 — Toon e Highlight Shading

O MMIO ARM9 agora modela o `TOON_TABLE` write-only de 32 entradas RGB555
(0x04000380–0x040003BF). A rasterização de polígonos no modo 2 usa o vermelho
de cinco bits da cor interpolada do vértice como índice da tabela e combina a
cor selecionada com a textura na escala RGB6. `DISP3DCNT.Bit1` escolhe Toon
(0) ou Highlight (1); Highlight soma a cor da tabela ao resultado modulado,
saturando cada canal em 63. Sem textura, a cor da tabela é a cor do fragmento.
O alpha segue a combinação de alpha da textura e do vértice. O modo 3 (shadow)
continua explicitamente não suportado; a sobrecarga de rasterização sem estado
de display também continua fail-closed para o modo 2.

A regressão cobre escritas/alinhamento/read-protection do TOON_TABLE, lookup
pelo índice vermelho, resultado Toon modulado e resultado Highlight com
saturação. CI #1123 passou nos quatro jobs: Desktop Linux 54/54, smoke
static-bank 1/1, ARM runtime 54/54, Android ARM64 e NDSRecomp aprovados. São
vetores sintéticos; ainda não houve comparação visual contra hardware físico
nem boot/frame real de Pokémon Platinum.


## Atualização 2026-11-22 — estado MMIO de Edge Marking

`NdsDisplayControl` agora modela os oito registradores write-only `EDGE_COLOR` em `0x04000330..0x0400033F`, com cores RGB555 e seleção por grupo de oito IDs (`polygon_id >> 3`). `DISP3DCNT.Bit5` também tem constante e accessor próprios. A regressão verifica armazenamento/máscara, alinhamento, larguras inválidas, leitura bloqueada e acesso ARM9.

Esta etapa implementa somente o estado de hardware: o contorno ainda não é aplicado aos pixels. O Edge Marking exige preservar candidatos de borda e concluir a comparação de ID/profundidade com os vizinhos ortogonais após a composição dos polígonos; o fluxo atual envia scanlines para uma FIFO de 48 linhas. O pós-passe será integrado sem alterar prematuramente as linhas já enfileiradas. Não há alegação de conformidade visual ou boot de jogo.


## Atualização 2026-11-22 — Edge Marking no passe de composição

Os polígonos opacos agora sinalizam pixels candidatos junto ao perímetro externo (sem marcar as diagonais internas da triangulação em leque). Após rasterizar a linha seguinte, o pós-passe avalia os vizinhos ortogonais: a cor `EDGE_COLOR[polygon_id >> 3]` só substitui o RGB do candidato quando existe ao menos um vizinho com ID diferente e profundidade estritamente maior. Nos limites da tela, o vizinho externo usa ID de `CLEAR_COLOR` e profundidade de `CLEAR_DEPTH`.

O caminho da FIFO BG0 segura uma linha até a próxima ficar pronta; scanline 192 é o flush final para a linha 191. A aplicação de fog ocorre depois da substituição da cor de borda. A cobertura geométrica usa uma aproximação determinística baseada na interseção da borda com o footprint do pixel; ainda precisa de comparação com capturas físicas, sobretudo para arestas inferior/direita omitidas pelo hardware, wireframe, antialias e polígonos translúcidos. A CI desta revisão ainda precisa confirmar as quatro plataformas. Não há boot ou quadro real de Pokémon Platinum comprovado.


## Atualização 2026-11-22 — Shadow Polygons em duas passagens

Foi integrado o modo 3 do `POLYGON_ATTR`. O primeiro passe (ID 0, alpha 1–30, Back=Render/Front=Hide) executa o teste de profundidade e marca o buffer de stencil, sem alterar cor nem ID do receptor. O segundo passe (ID 1–63, alpha 1–30, Back=Render/Front=Render) só mistura a cor do vértice quando o stencil está limpo e o ID atual do Attribute Buffer difere do ID da sombra; a verificação consome o stencil e o desenho grava o ID da sombra, impedindo que volumes subsequentes com o mesmo ID escureçam novamente aquele pixel. A ordem dos grupos de máscara e desenho é validada e o recurso requer `SWAP_BUFFERS` com ordenação automática desativada.

O stencil é mantido por pixel durante o quadro e reinicializado no início de uma nova sequência de renderização 3D da máquina. O teste sintético cobre a máscara, consumo, ID receptor, mistura e rejeição do caminho sem stencil. Ainda faltam vetores de conformidade em hardware físico, volumes fechados/clipping near-far, interação completa com transparência e validação visual em jogos; não há boot ou quadro real de Pokémon Platinum comprovado.


## Atualização 2026-11-22 — Ordenação manual opaco/translúcido

Quando SWAP_BUFFERS.Bit0=1, o rasterizador mantém a ordem relativa original dos polígonos opacos e os processa primeiro; depois processa os translúcidos na ordem relativa em que foram submetidos. A classificação inclui POLYGON_ATTR alpha 1–30 e formatos de textura A3I5/A5I3. Isso mantém a sequência manual dos volumes Shadow (máscara antes do passe de desenho) e garante que os alvos opacos já estejam no framebuffer antes dos polígonos translúcidos. A regressão envia uma sequência intencionalmente intercalada (translúcido vermelho, opaco azul, translúcido verde) e verifica a mistura final na ordem manual.

O modo automático (SWAP_BUFFERS.Bit0=0) continua fail-closed quando há polígonos translúcidos; a regra exata de Y-sort ainda não está implementada e não foi substituída por ordenação por profundidade ou ordem de submissão. A comparação com hardware físico continua pendente. Não há boot/frame real de Pokémon Platinum comprovado.


## Atualização 2026-11-22 — wireframe de polígonos

O modo `POLYGON_ATTR.alpha = 0` agora rasteriza a faixa de perímetro dos
polígonos em vez de rejeitá-los. O interior não produz fragmentos; as arestas
externas emitem a cor interpolada do polígono com alpha fixo 31. A classificação
de ordenação trata wireframes como opacos, inclusive no modo automático, e o
teste alpha continua sendo aplicado antes do depth write. A triangulação em
leque não desenha suas diagonais internas. O Edge Marking pode receber as
arestas do wireframe como candidatos opacos.

A cobertura atual é uma aproximação determinística sobre centros de pixels já
cobertos pelo rasterizador de triângulos. Ela não é uma declaração de
conformidade visual com o hardware físico. Primitivas separadas marcadas como
`line_segment` continuam sem suporte e falham explicitamente. A regressão
sintética cobre o interior transparente, pixels de borda alpha 31, ordenação
automática e descarte por `ALPHA_TEST_REF = 31`.


## Atualização 2026-11-22 — alpha final de A5I3 e Direct Color

A regressão integrada do rasterizador agora cobre os três caminhos de alpha
derivado da textura: A5I3 com alpha parcial continua translúcido mesmo quando
POLYGON_ATTR.alpha é 31 e não gera candidatos de Edge Marking; Direct Color
com o bit 15 definido continua opaco e pode gerar candidatos no perímetro;
com o bit 15 limpo, o fragmento totalmente transparente é descartado antes
da entrega ao compositor. Os casos usam triângulos sintéticos submetidos pelo
Geometry Engine e SWAP_BUFFERS manual, sem ROM ou dados extraídos de jogo.


## Atualização 2026-11-23 — base de extração dos overlays

Foi adicionado **tools/ndsrecomp/extract_nds_overlays.py** para inventariar as
tabelas de overlays ARM9/ARM7 da ROM USA Rev 1, resolver os **file_id** pela FAT
e extrair localmente as imagens executáveis. O parser valida limites, IDs,
faixas de RAM/BSS, metadados de inicialização e flags. Overlays marcados como
BLZ passam por um decodificador backwards-LZ com validação estrita do rodapé,
do tamanho final e do consumo do fluxo. O manifesto local registra os hashes
da origem e da imagem de RAM.

A regressão nova usa ROMs sintéticas sem conteúdo comercial e cobre imagens
comprimidas e não comprimidas, prefixo não comprimido, padding BLZ, erro de
tamanho, referência inválida, identidade de ROM divergente, referência
corrompida e proteção contra sobrescrita de arquivos que não pertencem ao
extrator. A ferramenta foi adicionada à etapa de py_compile e seus testes
entram na descoberta normal de tests/tools.

Isso conclui somente a **extração e o inventário verificáveis** dos overlays.
Nenhum overlay real de Platinum foi extraído nesta sessão; ainda não há
configuração de recompilação por overlay, análise de entry points/targets,
bancos overlay ligados ao runtime ou dispatch dinâmico. A próxima etapa é
executar o extrator sobre a cópia USA Rev 1 local, conferir os hashes e então
construir os bancos estáticos por overlay sem habilitar a execução do jogo
antes de resolver as dependências reais.

## Atualização 2026-11-23 — ligação dos bancos de overlays

Foi criado o gerador local tools/ndsrecomp/generate_overlay_bank_registry.py. Ele valida o relatório de emissão (schema e nds-codegen-v3), exige que o adaptador de direção de memória tenha passado, confere os arquivos C/H/dispatch de cada overlay emitido e calcula hashes SHA-256 dos arquivos efetivamente ligados. O gerador produz um registrador C++ e um manifesto no diretório de build; nenhum banco derivado da ROM é enviado ao Git.

O CMake agora aceita ORACIS_LOCAL_OVERLAY_BANK_DIR, em conjunto obrigatório com ORACIS_LOCAL_BANK_DIR. Os corpos C e tabelas de dispatch dos overlays são compilados como objetos e ligados a oracis_machine. A ponte de runtime registra os overlays ARM9 depois dos bancos principais, com rollback dos bancos adicionados pela chamada quando ocorre erro. A descoberta de endereços duplicados permanece responsabilidade do registry, que valida cada candidato contra os bytes guest esperados.

Validação local desta rodada: o relatório disponível contém 29 overlays ARM9 emitidos e 10.415 funções. O gerador local produziu um registrador para os 29 bancos e um manifesto com hashes SHA-256. Isso é uma validação do inventário/geração do registrador, não uma confirmação de que os objetos retail foram recompilados por esta nova configuração CMake ou executados em jogo. A integração ainda não carrega overlays na RAM guest; sem cópia do módulo, inicialização de BSS e unload sincronizado, as funções não podem ser consideradas executáveis. Nenhum round do jogo deve ser habilitado nesta etapa.

## Atualização 2026-11-23 — primeiro carregador de overlays integrado

Foi implementado `NdsOverlayLoader` e conectado à `NdsMachine`. A nova API
recebe a imagem extraída já descomprimida e o SHA-1 do campo
`image_sha1` do manifesto local. Antes de escrever, valida CPU/ID, tamanho,
overflow de endereço, faixa de inicialização estática, hash, permissões de
execução e escrita e ausência de colisão com overlays ativos da mesma CPU.
A imagem e o BSS são preparados em um único buffer e enviados a
`GuestMemory::write_bytes`; o inventário só é atualizado após a validação
dos bytes guest.

A ponte C do host agora oferece carga e descarga de módulos individuais.
O unload exige fatias encerradas, PC fora da faixa e a declaração explícita
de que não existem endereços de retorno/referências guest pendentes. O runtime
não consegue provar essa última condição: o rastreamento de referências
guardadas nas pilhas guest ainda não está implementado.

Foi adicionado o teste `oracis_nds_overlay_loader_test` para imagem, BSS,
hash divergente, duplicidade, colisões, faixa de inicialização e unload.
O shutdown da máquina também foi conectado ao loader: com as duas fatias
encerradas e o barramento de memória ainda vinculado, todas as faixas de
overlays registradas são zeradas e os registros são descartados. Se a
limpeza não puder ser concluída, o shutdown retorna falha.

O carregador **não** executa construtores estáticos, não faz relocations,
não carrega automaticamente o manifesto do disco e não implementa a política
de troca de overlays do NitroSDK. A camada host ainda precisa ler o manifesto
local, selecionar o arquivo correto e chamar a API. Portanto, esta etapa
conclui o mecanismo de cópia verificada para RAM guest, não a execução
completa dos overlays nem um boot funcional do jogo.

## Atualização 2026-11-23 — identidade e cobertura dos overlays recompilados

O gerador do registry agora exige o diretório da extração local e o seu
`manifest.json`. Ele valida o schema e a identidade da ROM, confere tamanho,
SHA-1 e SHA-256 dos arquivos de imagem extraídos e compara o SHA-1 do overlay
registrado no relatório do NDSRecomp com a imagem de origem daquele ID. Um
banco emitido para outro conteúdo, mesmo com nome/ID compatíveis, é recusado.
O CMake passa `ORACIS_LOCAL_OVERLAY_IMAGE_DIR` ao gerador; seu padrão é a
pasta produzida por `extract_nds_overlays.py`.

O manifesto de registry informa quantidade de overlays de origem, bancos
compilados e IDs ainda sem recompilação. A auditoria dos arquivos locais
disponíveis em 23/11/2026 encontrou 122 imagens ARM9 no manifesto e 29 bancos
emitidos; os 93 IDs restantes ainda não têm bancos estáticos. Os hashes SHA-1
dos 29 bancos emitidos coincidem com os hashes das imagens de origem. A
validação local também confirmou os tamanhos e SHA-256 dos 122 arquivos e
não encontrou inconsistência de metadados no manifesto.

A verificação acima foi feita sobre os artefatos locais fornecidos, não sobre
a compilação do novo gerador nem sobre a execução dos overlays no runtime.
Os novos testes Python cobrem hash divergente entre emissão e extração,
imagem extraída adulterada e relatório de cobertura; a CI deste commit ainda
precisa concluir para confirmar os testes/build.

## Atualização 2026-11-23 — descoberta reproduzível das âncoras de overlays

Foi adicionada a ferramenta
`tools/ndsrecomp/discover_platinum_overlay_configs.py`. Ela aceita o ZIP ou
diretório local da árvore Poképlatinum e examina somente os nomes dos arquivos
C no padrão `src/overlayNNN/ovNNN_ADDRESS.c`. O conteúdo dos arquivos-fonte
não é lido, copiado ou compilado. Cada candidato é validado contra o intervalo
de RAM e os hashes da imagem extraída; a ferramenta produz TOMLs locais e um
relatório com os overlays sem âncoras. Se um TOML preexistente tiver conteúdo
diferente, a ferramenta recusa sobrescrevê-lo.

Execução com os artefatos locais desta sessão: **29/122** overlays ARM9
possuem candidatos de entrada na árvore Poképlatinum fornecida; **93/122**
não possuem nomes de função correspondentes naquela árvore. Os 29 TOMLs
gerados coincidem com os arquivos locais já existentes. A hipótese Thumb é
explicitamente provisória e os entry points continuam não autoritativos.
Não foram criadas âncoras artificiais para os 93 módulos restantes.

Os cinco testes da ferramenta passaram localmente: descoberta ZIP/diretório,
relatório de ausência de âncoras, endereço fora da faixa, recusa de
sobrescrita e integridade dos binários extraídos. A CI agora executa a
compilação sintática do script e a descoberta automática desses testes.

## Atualização 2026-11-23 — candidatos de callbacks e recompilação expandida

A descoberta de âncoras recebeu a opção
`--include-function-pointer-headers`, que considera os nomes
`funcptr_ovNNN_ADDRESS.h` da árvore de Poképlatinum como candidatos separados.
Os arquivos header não são lidos nem copiados. Eles permanecem opcionais porque
tipos de função indicam callbacks, mas não provam isoladamente todos os
detalhes de estado ARM/Thumb.

Na extração local, sete candidatos adicionais afetaram seis overlays
(0005, 0006, 0022, 0061, 0062 e 0070). A auditoria do NDSRecomp para esses
candidatos aumentou a descoberta total de 10.415 para 10.636 funções (+221).
Os corpos C e dispatch atualizados foram compilados como objetos C99 com
Clang. A auditoria combinada dos objetos dos bancos ARM9/ARM7 principais,
dos 29 overlays e das bibliotecas locais do runtime resolveu 11.719
referências; unresolved=0, allowlisted=0 e providers duplicados=0.

Essas validações continuam locais: imagens e bancos derivados da ROM não
foram enviados ao Git. A recompilação melhorada ainda não é prova de
execução em jogo. A política de carregamento automático, inicializadores
estáticos, dependências de overlays, referências de retorno e os 93 overlays
sem âncoras continuam em aberto.

## Atualização 2026-11-23 — emissor incremental versionado e shards no CMake

Foi adicionada a ferramenta local
`tools/ndsrecomp/emit_platinum_overlay_banks.py`. Ela valida os hashes das
imagens extraídas e a identidade dos TOMLs, confirma `nds-codegen-v3`,
executa a auditoria do NDSRecomp e recusa emitir bancos quando o audit relata
operações sem codegen. Em seguida executa a emissão com validação de bytes
ao vivo, aplica o adaptador de direção de memória e divide o corpo em shards
C. A publicação usa diretório de staging, com inventário de hashes e
cobertura de bancos compilados/pendentes.

O pipeline aceita `--overlay-ids` e `--reuse-existing-bank-dir` para
atualização incremental. Cada banco não selecionado só é reutilizado depois
de verificar identidade da ROM/codegen, SHA-1 da imagem, SHA-256 dos três
arquivos C/H/dispatch e, quando presentes, o conjunto exato e SHA-1 dos
shards. O CMake e o gerador do registry agora validam/compilam shards quando
existem, mantendo compatibilidade com bancos antigos monolíticos.

A execução local re-auditou/re-emitiu seis overlays (0005, 0006, 0022, 0061,
0062 e 0070) e reutilizou 23 bancos com hashes conferidos. O conjunto
permanece em 29/122 overlays, com 10.636 funções. Os seis novos audits
reportaram zero operações sem codegen; 17 unidades C99 novas foram
compiladas. A auditoria combinada de 67 objetos dos bancos principais e
overlays resolveu 11.719 referências: unresolved=0, allowlisted=0.

Os artefatos C/OBJ/manifesto permanecem apenas em `.oracis` local. Os
93 overlays sem âncoras não receberam entry points inventados. O sucesso de
emissão, compilação C99 e resolução de símbolos não significa que o jogo
já execute: faltam inicializadores estáticos, relocations, integração com
o gerenciador NitroSDK, rastreamento de retornos guest e validação de boot.
\n\n## Atualização 2026-11-23 — métricas de alvos indiretos no relatório

O emissor passou a preservar, por overlay, os resultados de
`[finder] indirect outcomes`: destinos resolvidos, sabidamente
inalcançáveis, desconhecidos e seeds de ponteiros armazenados. Também
registra as regiões guest de 64 KiB e a quantidade de sites marcados pelo
NDSRecomp como possíveis aliases não declarados. O manifesto agrega somente
overlays que realmente possuem as métricas e lista explicitamente os IDs
reutilizados de relatórios antigos que ainda não as contêm.

Na leitura agregada dos 29 logs locais: 8.966 chamadas indiretas analisadas,
34 destinos resolvidos, 39 sabidamente inalcançáveis, 570 desconhecidos,
62 seeds de ponteiro armazenados e 39 sites em 24 regiões candidatas; o
finder também reportou 59 alvos undefined. Esses números são distintos da
auditoria de símbolos do linker: os 67 objetos nativos continuaram com
11.719 referências resolvidas e zero símbolos unresolved/allowlisted. A
lacuna que resta aqui é a resolução de destinos guest indiretos, não a
provisão de símbolos C do runtime. Não habilitar execução apenas porque a
auditoria do linker passou.\n\n\n## Atualização 2026-11-23 — função-callback a partir da tabela static-init

A ferramenta de descoberta passou a aceitar
`--include-static-init-pointers`. Ela lê as tabelas de ponteiros da região
`static_init_start..static_init_end` das imagens extraídas, valida os limites
e alinhamento e deriva o modo ARM/Thumb do bit 0 do ponteiro. Cada seed recebe
um comentário de origem no TOML, e os entry points continuam explicitamente
não autoritativos. A inspeção local encontrou 34 overlays ARM9 com ponteiros
não nulos nessa faixa: 33 IDs sem nomes de função na árvore Poképlatinum e
um overlay (0017) que já tinha fonte nomeada.

Os 34 audits isolados desses ponteiros pelo NDSRecomp encontraram 41 funções,
zero finder-undefined, zero destinos indiretos desconhecidos e zero operações
sem codegen. Somados aos 29 overlays com âncoras de fonte, agora há TOMLs com
candidatos para 62/122 overlays. Os 60 restantes não possuem nem nomes de
função correspondentes nem ponteiros static-init não nulos. Esta etapa
descobre somente callbacks de inicialização: não afirma que os 62 módulos
estejam totalmente cobertos, nem executa os construtores automaticamente.\n

## Atualização 2026-11-23 — 33 novos bancos a partir dos ponteiros static-init

Foram emitidos 33 bancos estáticos adicionais usando os ponteiros ARM/Thumb
lidos das faixas `static_init_start..static_init_end` dos overlays. O overlay
0017 tinha ponteiro static-init, mas já estava no conjunto de bancos
existente; ele não foi contado novamente. A cobertura local subiu de
29 para **62/122 overlays**, totalizando **10.673 funções**. Permanecem
**60 overlays sem banco**.

Os 33 novos bancos contêm 37 funções. Os 66 corpos/dispatch C passaram em
66 verificações sintáticas C99 e o adaptador registrou 99 leituras e 5
escritas. Os objetos equivalentes à seleção de fontes do CMake foram
auditados em conjunto: 133 objetos (bancos principais, shards dos seis
overlays antigos particionados, 23 bancos antigos monolíticos e 33 bancos
novos). Resultado: **11.756 símbolos/referências resolvidos**, zero
unresolved, zero allowlisted e zero provedores múltiplos.

Os relatórios e os objetos continuam locais, fora do Git. A auditoria
conjunta dos logs de 62 overlays ainda registra 570 alvos indiretos
desconhecidos e 59 funções undefined pelo finder. São lacunas de descoberta
de alvos guest, não símbolos de linker ausentes. As raízes static-init são
construtores/callbacks, não entradas principais autoritativas; o runtime
ainda não executa esses inicializadores nem integra o protocolo de carga/
substituição do NitroSDK. Assim, a cobertura de 62 bancos é uma ampliação
de recompilação estática, não um boot funcional de Pokémon Platinum.

A CI `45234103186` concluiu com sucesso os quatro jobs. Essa execução
testou a versão do código no Git, CMake e ferramentas com fixtures
sintéticas; não compilou nem executou os bancos locais derivados da ROM.

## Atualização 2026-11-23 — static-init decodificado pelo runtime, ainda não executado

O loader agora lê a tabela static-init apenas depois que a imagem do overlay
passa pela validação SHA-1. Ele preserva a ordem dos ponteiros, interpreta
o bit 0 como estado Thumb/ARM, valida alinhamento e exige que a instrução
inicial esteja integralmente na imagem do próprio overlay. Um ponteiro
inválido faz a carga falhar antes de alterar a RAM. A primeira palavra zero
encerra a sequência.

Os callbacks validados ficam associados ao registro do overlay e podem ser
consultados por `NdsOverlayLoader::static_initializers(cpu, overlay_id)`.
O unload e o shutdown eliminam os descritores junto com o módulo. O loader
continua sem invocar essas funções: falta uma camada de chamada guest segura,
com contexto ARM/Thumb, retorno, tratamento de falhas e dependências entre
módulos. Os testes cobrem ordem, dois modos, ponteiro fora da imagem e
terminador zero.

## Atualização 2026-11-23 — execução supervisionada de static-init

A máquina agora oferece
`run_overlay_static_initializers(cpu, overlay_id, cycle_budget)`, e o host
bridge expõe `oracis_host_runtime_run_overlay_static_initializers`. A
operação é explícita; carregar um overlay não executa seus construtores.
Antes da primeira chamada, todos os entry points são resolvidos no dispatch
registry. O host exige SP alinhado a 8 bytes e uma janela gravável de 256 bytes
abaixo dele, sem inventar a configuração que deveria vir do BIOS.

Cada função recebe uma slice com orçamento limitado, um LR sentinela e um
frame na pilha de retornos guest. O retorno é reconhecido pela mesma convenção
`BX LR` do codegen. Um tratador temporário captura dispatch misses aninhados;
o scheduler solicita unwind em checkpoints de instrução quando o orçamento
vence. Ao terminar, os registradores e a pilha de retornos do chamador são
restaurados, mas efeitos em GuestMemory/MMIO são mantidos. O loader registra
progresso e impede segunda execução. Falhas depois de começar deixam o
overlay marcado como failed, sem retry automático nem rollback das mutações
guest.

Foi adicionado um teste dedicado para ARM/Thumb, ordem, retorno sintético,
restauração de CPU, persistência de memória, orçamento e dispatch miss.
A execução real dos 62 bancos continua condicionada ao estágio de boot que
estabeleça a pilha guest, às dependências de overlays e à validação dos
serviços NitroSDK. O executor supervisionado é infraestrutura de inicialização,
não é evidência de boot jogável.

auditoria do linker passou.\n

## Atualização 2026-11-23 — RTC e SPI do ARM7

Foram acrescentados dois dispositivos seriais exclusivos do ARM7, ligados ao
barramento e à `NdsMachine`: `NdsRtc` (`0x04000138`) e `NdsSpiControl`
(`0x040001C0/C2`). Ambos recusam acesso do ARM9 (`CpuAccessDenied`) e usam o
novo `Status::Unsupported` para comandos que o hardware aceita mas o Oracis
não modela; nada é inventado em silêncio.

- **RTC:** protocolo bit-bang (CS/CLK/DATA, LSB primeiro, comando `0110b`),
  Status 1/2, Date/Time, Time, Alarm 1/2, Clock Adjust e registrador livre, em
  BCD. O relógio não consulta o horário do host: `set_date_time()` e
  `advance_seconds()` mantêm testes e replays determinísticos (anos bissextos,
  virada 99→00). Alarme/IRQ de frequência e a linha INT não são modelados. O
  tratamento do bit PM em modo 24 horas segue a descrição pública e ainda não
  foi conferido em hardware.
- **SPI:** firmware flash (somente leitura: READ 03h, RDSR 05h, WREN 06h,
  WRDI 04h), power management (registradores 0–4, bit de desligamento
  travado, bateria) e touchscreen (conversões de 12/8 bits, canais AUX/temp).
  A duração da transferência (8/16 bits × divisor de baud) é consumida pelo
  relógio do sistema e a IRQ de conclusão (bit 23) chega ao ARM7. Escrita em
  SPIDATA com o barramento ocupado, JEDEC ID, programação/erase e conversões
  sobrepostas falham fechado.
- **Firmware:** nenhum conteúdo é embutido. O host precisa fornecer a imagem do
  próprio usuário via `set_firmware_image()`; sem ela todo acesso ao flash é
  recusado. Os valores de pen-up e dos canais Z/temperatura são suposições
  pendentes de capturas de hardware.

Testes novos: `oracis_nds_rtc_test`, `oracis_nds_spi_control_test` e
`oracis_nds_machine_serial_test` (RTC e firmware pelo bus C ABI, temporização
via relógio do sistema, IRQ somente no ARM7 e ciclo bind/unbind). O CTest local
executou 60/60. Isso valida dispositivos sintéticos, não o boot de Pokémon
Platinum. Ainda faltam: carregamento do firmware pelo host SDL/Android,
entrada de toque (mapeamento mouse/toque → `set_touch`) e a seleção de
arquivos no Android.


## Atualização 2026-11-24 — cobertura ampliada do decoder P345

A fixture sintética da parte 3 agora usa uma árvore Huffman primária com três símbolos (dois literais e um match de comprimento 4) e uma árvore independente de distâncias que seleciona distância 2. A saída esperada alterna dois bytes (`ABABAB`), exercitando decodificação por ramos internos e cópia LZ sobreposta com distância maior que 1. As partes 4 e 5 mantêm o caso simples de distância 1. Essa etapa aumenta a cobertura estrutural do decoder e não altera a política de manter P3/P4/P5 em staging: a documentação disponível identifica a função dos componentes, mas não define destinos de RAM suficientes para instalação segura.
