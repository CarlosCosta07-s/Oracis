<!-- Oracis project page -->
<p align="center">
  <img src="assets/icon/oracis.svg" alt="Ícone do Oracis: estrela de platina e anel celeste" width="160">
</p>

<h1 align="center">Oracis</h1>

<p align="center">Um projeto independente de port nativo de Pokémon Platinum para plataformas modernas.</p>

<p align="center"><strong>Reconstruindo Sinnoh para além do hardware original.</strong></p>

> **Base de CI verificada em 23/11/2026 (commit 1e945e1):** a CI [#45209899276](https://github.com/CarlosCosta07-s/Oracis/actions/runs/45209899276) terminou aprovada nos quatro jobs. Desktop Linux e runtime ARM passaram com **56/56 CTests** cada; tooling Python passou com **53 testes**; o smoke opcional de banco sintético passou **1/1**; Android ARM64 e o compilador estático NDSRecomp também passaram. O CP15 ARM9 agora possui suporte parcial à MPU (atributos c2/c3, permissões c5 e regiões c6), além de Data Abort para violações de acesso a dados e Prefetch Abort para buscas negadas pela MPU em mapeamentos executáveis. A cobertura inclui entradas ARM e Thumb pelos caminhos de dispatch, link-slot e interworking. Isso ainda **não comprova boot do jogo**: os bancos reais permanecem privados, fora do Git/CI, e ainda não foram registrados nem executados; nenhum primeiro quadro foi demonstrado. O host ainda não carrega os handlers reais da BIOS ARM9, e aborts ARM7 e falhas externas de barramento continuam fora do modelo arquitetural. ROM, BIOS, firmware, segmentos, assets e saves continuam fora do repositório, da CI e das releases.




## Experimento Android de entrada nativa

Os commits 3a9bc39, d46f4db, 663c6ba, 4cc9350, a496618 e 0f497d5 adicionam seleção Android de uma imagem .nds, BIOS ARM7 e dump de firmware SPI do usuário por meio do seletor SDL3. URIs content:// são lidas por SDL_IOStream. O dispositivo SPI ARM7 modela READ, RDSR, WREN e WRDI. O carregador KEY1 descriptografa e descomprime IPL 1/2, valida o CRC combinado e pré-valida os destinos antes de publicar os bytes na RAM. Os payloads 3/4/5 usam o decoder P345 com fluxos primário/distância e árvores Huffman estáticas independentes, com validação de CRC em staging; ainda não são instalados na RAM guest nem executam o reset retail das BIOS.

A máquina prepara o estado documentado de entrada do cartucho (R0–R11 zerados, R12/LR/PC no entrypoint, pilhas User/System + IRQ/SVC, metadados determinísticos de boot e POSTFLG bit 0). Quando os dois entrypoints resolvem para bancos estáticos ARM9/ARM7, o host executa fatias nativas limitadas de 1.024 ciclos.

Esse é um experimento de **direct-entry**: ele não executa o reset das BIOS retail nem reproduz toda a inicialização do firmware. Não comprova o primeiro quadro ou compatibilidade jogável. O build Android na CI produz a biblioteca nativa ARM64, não um APK instalável; os bancos reais continuam sendo artefatos privados que precisam estar ligados localmente. Consulte [docs/ANDROID_DIRECT_BOOT.md](docs/ANDROID_DIRECT_BOOT.md).

## O projeto

Oracis busca executar Pokémon Platinum como código nativo em computadores modernos. O projeto utiliza recompilação estática dos códigos ARM7/ARM9 e um runtime independente para os serviços necessários do Nintendo DS. Não é um emulador com uma CPU guest interpretada como caminho principal.

## Direção técnica

- **NDSRecomp estático** para gerar bancos C ARM7/ARM9, em checkout externo e revisão fixada.
- **C99 + C++20** para bancos gerados e runtime independente.
- **CMake + Ninja** para o build.
- **SDL3** para janela, eventos, controles, toque e saída de áudio.
- **Python** para extração local e validação da cópia de jogo.
- **Rust** permanece apenas no gerador isolado do ícone.

Consulte [docs/TECH_STACK.md](docs/TECH_STACK.md), [docs/NDSRECOMP_INTEGRATION.md](docs/NDSRECOMP_INTEGRATION.md), [documentação do scheduler dual-core](docs/DUAL_CORE_SCHEDULER.md), o [mapa da ABI dos bancos](docs/RECOMP_ABI_MAP.md), o [contrato GuestMemory](docs/GUEST_MEMORY.md), a [documentação parcial do ARM9 TCM](docs/ARM9_TCM.md), o [timing parcial de busca ARM9](docs/ARM9_TIMING.md) e a [decisão ADR 0001](docs/adr/0001-ndsrecomp-static-compiler.md).

## Cartucho local no desktop

O host SDL aceita `--game-card caminho/para/jogo.nds` e, quando conhecido, `--game-card-id 0xXXXXXXXX`. O Chip ID é metadado físico do cartucho e não é inferido do dump `.nds`; o valor deve ser fornecido explicitamente para o comando 0x90. O arquivo precisa conter ao menos o cabeçalho NDS de 0x200 bytes e não pode ultrapassar 512 MiB; o desktop confere o tamanho antes da alocação e o bridge copia os bytes para memória própria, liberando-os no desligamento. O dispositivo Game Card implementa os comandos iniciais 9F/00, 90 com Chip ID explicitamente configurado pelo host (`--game-card-id`), e leituras B7 raw ou com comandos/dados KEY2 quando a stream do cartucho foi inicializada; mantém o redirecionamento retail dos primeiros 32 KiB e wrap no limite de 4 KiB. Existem primitivas KEY1 e KEY2 e a escrita dos registradores de seed por CPU com pulso ROMCTRL.bit15 aplica o stream compartilhado. A transformação KEY2 dos dados B7 está integrada ao caminho de leitura, com continuidade da stream entre palavras e comandos. O comando RAW 0x3C ativa o estado KEY1 com comprimento de resposta zero; opcionalmente, o controlador também modela uma palavra HIGH-Z de quatro bytes quando o host solicita explicitamente esse tamanho. Os parâmetros i/j/k são decodificados de seus campos empacotados e preservados para os comandos KEY1 seguintes. O controlador já valida os comandos descriptografados, o avanço de k, os limites de retransmissão dos cartuchos novos e o campo `llll` compartilhado entre os comandos 0x40, 0x10, 0x60 e 0xA0. O 0x20 usa esse campo para o número do bloco Secure Area e valida a faixa 0004h–0007h. O controlador agenda automaticamente o comando 0x40 e seus gaps no barramento para cartuchos antigos e novos. O comando 0x10 também entrega a palavra Chip ID criptografada pela KEY2 através do registrador DATA, incluindo a retransmissão exigida por cartuchos novos. O módulo KEY1 já descriptografa e interpreta os formatos dos comandos 0x10/0x20/0x40/0x60/0xA0 em ordem de fio, validando i/j, o contador k de 20 bits e a faixa do bloco seguro. O controlador também extrai e expõe `header[0x60]`, `header[0x64]`, `header[0x6E]` e `header[0x13]` (perfis ROMCTRL, atraso do Secure Area e índice de seed KEY2), além da variante do protocolo pelo bit 31 do Chip ID. O controlador também deriva `gap1+gap2` e converte o atraso de Secure Area para ciclos na base de 33,51 MHz (`256` ciclos por unidade de 130,912 kHz). A validação de sequência limita comandos comuns a duas transmissões e comandos de bloco Secure Area a nove transmissões nos cartuchos com bit 31 ativo. `advance_bus_cycles()` reduz a contagem regressiva do atraso de Secure Area extraído do cabeçalho após a primeira transmissão validada. `complete_key1_command_gap()` agora valida a quantidade de clocks de gap indicada pelo ROMCTRL ativo. Na variante antiga, inicializa KEY2 ao finalizar o comando 0x40 antes de avançar a stream pelos clocks do gap (incluindo o byte HIGH-Z); nas variantes novas, respeita `ROMCTRL.bit28=0` e não avança a stream durante o gap silencioso. Também inicia o atraso Secure Area depois da conclusão do primeiro gap e aplica a seed KEY2 do comando 0x40 depois da última transmissão exigida (uma nos cartuchos antigos, duas nos novos). O scheduler do barramento consome automaticamente esses planos e agenda os gaps, os payloads KEY2 e o padding pós-bloco. `describe_key1_reply()` descreve o payload da transmissão atual e o total agregado: o `0x10` novo tem primeiro envio vazio e quatro bytes na segunda transmissão; o `0x20` novo tem oito envios de `0x200` com intervalos KEY2 de `0x18`, enquanto a variante antiga usa um payload `0x1000`. O comando `0x20` agora seleciona blocos 4–7 na região 0x4000–0x7FFF e transmite os dados pelo DATA com KEY2: oito partes de 0x200 bytes nos cartuchos novos, cada uma seguida por 0x18 bytes zero criptografados, ou um payload de 0x1000 bytes nos antigos. A primitiva KEY1 implementa a descriptografia em duas fases dos primeiros 0x800 bytes da Secure Area (ID nível 2 e prefixo completo nível 3) e valida `encryObj`. `NdsGameCardControl::decrypt_secure_area_prefix()` e `oracis_host_runtime_decrypt_game_card_secure_area_prefix()` copiam os bytes de 0x4000–0x47FF para um buffer do chamador, descriptografam a cópia e preservam o dump montado; o futuro carregador ARM9 ainda precisa invocar a API no ponto correto do boot. O desktop aceita `--arm7-bios caminho/para/bios7.bin` junto com `--game-card`: o arquivo deve ser a BIOS ARM7 NDS fornecida pelo usuário (16 KiB), da qual o host extrai e copia apenas a tabela KEY1 `0x30..0x1077`. O schedule KEY1 é inicializado em nível 2/modulo 8 usando o Game Code do cabeçalho do cartucho. A BIOS e a tabela não são gravadas no repositório. A ativação RAW 0x3C e os comandos KEY1 0x40, 0x10, 0x20 e 0xA0 estão conectados ao barramento; o `0xA0` transfere o estado para modo de comandos KEY2 após as transmissões exigidas por cada variante. No modo KEY2, os comandos B7/B8 são decifrados pela stream persistente e seus dados/Chip ID são retornados criptografados; B7 respeita o wrap de 4 KiB e o gap2 entre blocos de 0x200. O comando `0x60` permanece pendente. A descriptografia KEY1 adicional dos primeiros 0x800 bytes da Secure Area também precisa ser executada pelo fluxo de boot. Esta opção monta a imagem no barramento para desenvolvimento e testes — **não inicia o jogo nem comprova compatibilidade**. SPI de backup, execução completa do boot e seleção de arquivos no Android continuam pendentes. Não coloque imagens de jogos no repositório.

## Estado atual

A primeira revisão-alvo é **Pokémon Platinum USA Rev 1**, identificada localmente por SHA-1. ROM e arquivos derivados permanecem fora do Git, da CI e das releases.

O bootstrap do NDSRecomp está fixado no commit `14b7e15023e9affc9dc375e4e611ac703f5c76ab` e limita o checkout a `recompiler/`, `cmake/` e ao núcleo ARM MIT. O runtime/oracle/frontend upstream e os componentes derivados de melonDS não são incorporados.

O Oracis já contém:
- estado ARM com registradores bancados e asserts de layout C99;
- GuestMemory: mapa de RAM NDS retail, Main RAM compartilhada/mirrors, WRAM ARM7 privada, quatro modos WRAMCNT e leitura ARM7 de WRAMSTAT; controlador parcial VRAM `VRAMCNT_A-I` (ARM9, 8-bit/write-only) e `VRAMSTAT` (ARM7, 8-bit/read-only), com os nove bancos mapeados em MST=0/LCDC nos endereços `0x06800000`–`0x068A3FFF`; acesso CPU a MST=1/Main BG dos bancos A-D/E/F/G, MST=2/Main OBJ A/B/E/F/G, MST=4/Sub BG/Sub OBJ C/D, MST=1/Sub BG H/I e MST=2/Sub OBJ I; colisões de alias são rejeitadas; stores de 8 bits em VRAM ARM9 são ignorados e escritas de 16/32 bits são aceitas; C/D também suportam MST=2/Work RAM ARM7, onde stores por byte são permitidos nas janelas `0x06000000`/`0x06020000`, OFS=0/1, status independente do OFS e rejeição transacional de colisão no mesmo slot; as sete janelas Shared WRAM são pré-mapeadas e suas máscaras de visibilidade por CPU são atualizadas de forma transacional, com o mapa validado antes da aplicação, sem desalocação nem perda dos bytes físicos;
- wrappers C bus_read_*/bus_write_* que encaminham para RAM guest mapeada e falham com diagnóstico em endereços inválidos;
- controlador MMIO `NdsInterruptControl` com IME/IE/IF independentes por CPU, IRQ requests latcheados, IF W1C e `try_enter_irq` condicionado a `CPSR.I`; `run_dual_core_round` consulta e entrega IRQs nas fronteiras; o callback host fornece o LR ARM/Thumb e a máquina seleciona a base vetorial pelo ARM9 CP15 Control.V;
- `NdsTimerControl` com Timer 0–3 para cada CPU, reload, prescalers, cascata e pedidos de IRQ; `NdsSystemClock` converte os contadores cumulativos ARM9/ARM7 na razão 2:1, preserva meio ciclo e chama `advance_bus_cycles(n)` uma única vez por intervalo de barramento completo;
- carregador local de programas que lê os dois binários extraídos, exige tamanhos e SHA-1 conhecidos de ARM9/ARM7 e só entrega as imagens à máquina depois de validar ambas; módulo SHA-1 próprio coberto por vetores conhecidos;
- `NdsMachine` como contexto proprietário dos dois estados ARM e dos dispositivos base; instalação do mapa retail, vínculo C ABI em ordem de dependência, rollback/desvínculo reverso, início/fim de fatias com relatório automático dos contadores cumulativos, contabilização de ciclos ociosos com precisão de core e rounds dual-core sincronizados em 2:1; HALT/wake básico por CPU conectado ao BIOS SWI `0x06`, SWI ARM7 `0x1F` (parâmetro em R2) e CP15 WFI do ARM9; CP15 ARM9 parcial: ID e tamanho do TCM em c0; V, enable e load-mode ITCM/DTCM em c1; janelas c9 com backing privado de 32 KB ITCM/16 KB DTCM, prioridade sobre a RAM coberta e máscaras independentes de leitura/escrita/busca. Vetores altos/baixos seguem Control.V. O suporte CP15 também inclui os atributos c2/c3, permissões c5, regiões MPU c6 e operações de manutenção c7; sem backing físico de I/D cache ou write buffer, a manutenção c7 é um no-op ordenado. Violações MPU ARM9 geram Data Abort no acesso de dados e Prefetch Abort no dispatch de instrução; falhas de barramento externas e execução dos handlers de BIOS ainda não estão modeladas. Os providers `runtime_code_cycles`, `arm9_refill_cycles` e `arm7_refill_cycles` modelam fetch uncached ARM9 e refill ARM/Thumb nas regiões documentadas, incluindo Palette/VRAM no ARM9 e VRAM C/D no ARM7. A primeira halfword Thumb recebe o fetch 32-bit integral e a segunda custa zero; os refills contam os fetches sequenciais do destino e reclassificam cada fetch ao cruzar regiões. Timing DATA parcial de Main RAM, WRAM/BIOS/I/O/OAM, Palette/VRAM no ARM9, VRAM C/D como Work RAM no ARM7 e TCM normal está implementado; a ABI não distingue loads/stores, então TCM load mode, Gamecard/Wi-Fi e N32 ARM7 VRAM não sequencial continuam fail-closed. Combinação ARM7 cobre classes documentadas de CODE/DATA; cache, contenção e wait states dinâmicos permanecem pendentes;
- tracing ABI limitado por CPU: `runtime_trace_event` e `runtime_live_transfer` gravam em um buffer circular de 512 eventos por core; o host consulta a fila por cursor e recebe a contagem de eventos sobrescritos;
- operações CPSR/SPSR e entrada/retorno de exceções;
- avaliação de condições e atualização de flags NZCV;
- helpers de acesso aos registradores do User bank e instrução `CLZ` na ABI C;
- ponte ABI C para selecionar o estado ARM9 ou ARM7 via `g_cpu`, wrappers de flags/PSR e especialização `NDS_STATIC_CPU`;
- asserts do layout de `NdsStaticValidation`, `NdsDispatchEntry` e `NdsLinkSlot`;
- registro/remoção de bancos estáticos por ARM9/ARM7, lookup ARM/Thumb e `runtime_dispatch` com falha fechada;
- callback de identidade que compara bytes do banco e dependências contra `GuestMemory`, exigindo proveniência para RAM gravável;
- pilha de retorno guest separada por CPU, com chave PC+ARM/Thumb e desenrolamento de retornos não locais;
- contadores de ciclos e controle cooperativo de fatias ARM9/ARM7 por orçamento, yield e unwind automático durante o retorno dos frames C gerados;
- `NdsKeypadControl` com KEYINPUT compartilhado, KEYCNT independente por CPU, condições OR/AND e IRQ de teclado; o host SDL3 mapeia X/Z, Enter/Backspace, setas e Q/W, limpa entradas ao perder foco e encaminha estado pela ponte C;
- entradas `runtime_link_*` encaminhadas ao dispatcher normal; otimização de slots e folhas inline ainda desabilitada.

A validação multiplataforma mais recente está em [CI #45209899276](https://github.com/CarlosCosta07-s/Oracis/actions/runs/45209899276), no commit `1e945e1` (23/11/2026): **4/4 jobs aprovados**, Desktop Linux **56/56 CTests**, runtime ARM **56/56 CTests**, tooling Python **53 testes**, smoke de banco sintético **1/1**, Android ARM64 e NDSRecomp estático aprovados. O teste de máquina cobre Data Abort ARM9, Prefetch Abort ARM/Thumb e retorno cooperativo por unwind. A auditoria sintética não substitui uma auditoria/execução dos bancos comerciais privados.


O extrator e o emissor local validam a identidade dos segmentos, os metadados TOML, os hashes ARM9/ARM7 e a identidade do gerador. A emissão dos dois bancos é transacional. Na revisão USA Rev 1, o gerador produziu localmente 78 funções ARM9 e 957 ARM7; os quatro arquivos C gerados foram compilados em C99 contra os headers ABI Oracis e agrupados em objetos locais. O inventário `nm -u` encontrou 40 referências externas ARM9, 45 ARM7 e 48 nomes únicos combinados. As duas rotinas ausentes (`runtime_live_transfer` e `runtime_trace_event`) agora têm implementações reais. Ainda falta fazer a ligação completa com as bibliotecas Oracis, confirmar todos os símbolos em conjunto e resolver os serviços que deliberadamente falham em timing/BIOS. Continuam pendentes combinação de ciclos ARM7, direção load/store na ABI de timing, TCM load mode, caches/MPU, contenção e wait states dinâmicos de Gamecard/Wi-Fi.
 
**Ainda não foi concluído:** ligar os objetos reais às bibliotecas/runtime, registrar as tabelas ARM9/ARM7, tratar os alvos indiretos desconhecidos e completar serviços BIOS/NitroSDK, DMA, IPC e vídeo. O timing de dados permanece parcial, com as limitações documentadas em ARM9_TIMING.md. O carregador local e a validação de identidade têm testes sintéticos; o Android ainda não possui seleção de arquivos e caminho de dados integrado. Nenhum teste comprova boot ou compatibilidade jogável.


## Princípios

- Recompilação estática e execução nativa; sem interpretador ARM geral como fallback.
- Runtime, scheduler e dispositivos implementados independentemente.
- Não incorporar código ou assets de emuladores terceiros.
- Não incorporar código derivado de melonDS, direta ou indiretamente.
- ROM, BIOS, firmware, extrações, bancos gerados e saves ficam fora do repositório, CI e releases.
- Não presumir autorização a partir da disponibilidade pública de uma descompilação.
- Não declarar compatibilidade, desempenho ou conclusão sem testes reproduzíveis.

## Plataformas planejadas

Linux x86-64 e Android ARM64 já passam pelos builds da CI; isso comprova compilação, não execução jogável. Windows x64 continua planejado e requer validação própria.

## Aviso

Oracis é um projeto independente, sem afiliação ou endosso da Nintendo, The Pokémon Company ou Game Freak. Pokémon e seus elementos relacionados pertencem aos respectivos titulares. O projeto não distribui o jogo nem conteúdo proprietário.
