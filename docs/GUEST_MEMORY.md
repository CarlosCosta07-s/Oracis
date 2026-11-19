# GuestMemory e mapa de RAM do Nintendo DS

**Estado:** armazenamento guest configurável, visibilidade por CPU, aliases, mirrors periódicos e mapa de RAM NDS retail implementados. `NdsWramControl` liga WRAMCNT ao barramento para ARM9 e implementa WRAMSTAT read-only de 8 bits no ARM7. `NdsKeypadControl` implementa KEYINPUT compartilhado e KEYCNT independente por CPU, incluindo IRQ de teclado OR/AND. O endereço compartilhado `0x04000241` não é capturado pelo dispositivo no ARM9, onde corresponde a VRAMCNT_B. Os demais handlers MMIO continuam pendentes. O bloco IME/IE/IF mantém estado independente por CPU e IF W1C; a entrega automática de IRQ ainda depende do scheduler.

## 1. Responsabilidades

`GuestMemory` é o backing store byte-addressed do barramento Oracis. Ele oferece:
- regiões guest de 32 bits;
- permissões Read/Write/Execute;
- máscara de visibilidade ARM9/ARM7;
- aliases lineares para o mesmo backing;
- mirrors periódicos de uma região física menor;
- transferências little-endian de 8, 16 e 32 bits;
- proveniência por byte para identidade de código.

Não é um modelo completo de hardware. MMIO, wait states, DMA, IPC, comportamento de barramento e efeitos colaterais de dispositivos pertencem a camadas separadas.

## 2. Visibilidade por CPU

Cada região possui uma máscara de acesso:
- `CpuAccessArm9`;
- `CpuAccessArm7`;
- `CpuAccessBoth`;
- `CpuAccessNone`, reservado para backing interno usado por aliases.

Duas regiões podem ocupar o mesmo intervalo guest quando suas máscaras de CPU são disjuntas. Isso é necessário no Shared WRAM: em alguns modos, ARM9 e ARM7 acessam bancos físicos diferentes no mesmo endereço guest `0x03000000`.

Quando uma CPU tenta ler ou escrever uma região que só está mapeada para a outra, o resultado é `CpuAccessDenied`, não um zero inventado.

## 3. Mapa instalado para NDS retail

O builder está em `include/oracis/memory/nds_retail_map.hpp` e `src/memory/nds_retail_map.cpp`.

`NdsRetailRamMap::install(memory, shared_wram_mode)` instala o seguinte mapa inicial:

| Região guest | Tamanho físico | CPUs | Comportamento |
|---|---:|---|---|
| `0x02000000` | 4 MB | ARM9 + ARM7 | Main RAM compartilhada |
| `0x02400000` | Espelho de 4 MB | ARM9 + ARM7 | Mesmo backing da Main RAM |
| `0x02800000` | Espelho de 4 MB | ARM9 + ARM7 | Mesmo backing da Main RAM |
| `0x02C00000` | Espelho de 4 MB | ARM9 + ARM7 | Mesmo backing da Main RAM |
| `0x03000000` | 0/16/32 KB | Conforme WRAMCNT | Shared WRAM, espelhada na janela de cada CPU |
| `0x03800000` | 64 KB | ARM7 | WRAM privada ARM7 |

O mapa é intencionalmente o **NDS retail**, não o DSi nem o hardware debugger. BlocksDS documenta 4 MB de Main RAM no DS retail, com os três espelhos indicados, e 64 KB de WRAM privada para ARM7. GBATEK documenta a área de Shared WRAM e os registros de controle. Consulte [BlocksDS — Memory map](https://blocksds.skylyrac.net/docs/internal/memory_map/) e [GBATEK — DS Memory Control](https://mgba-emu.github.io/gbatek/).

### Main RAM

O backing principal começa em `0x02000000`, tem 4 MB e é compartilhado pelos dois processadores. Os endereços `0x02400000`, `0x02800000` e `0x02C00000` são aliases do mesmo armazenamento, não cópias separadas.

### ARM7 private WRAM

O backing físico `0x03800000–0x0380FFFF` pertence ao ARM7. Como os barramentos dos dois processadores têm visões distintas, no modo 0 o ARM9 pode enxergar Shared WRAM através de sua janela de 16 MB sobre o mesmo intervalo numérico; essa visão ARM9 aponta para os bancos compartilhados, nunca para os bytes privados do ARM7. Os testes verificam que escritas de cada CPU nesse endereço permanecem isoladas.

### Shared WRAM e WRAMCNT

A API recebe explicitamente um valor de `SharedWramMode`, correspondente aos bits 0–1 de `WRAMCNT` (registrador NDS9 em `0x04000247`). Não se presume o valor deixado pelo firmware.

| Bits | Modo Oracis | ARM9 | ARM7 |
|---:|---|---|---|
| 0 | `Arm9All32K` | Bancos 0 + 1 (32 KB) | A janela ARM7 espelha sua WRAM privada de 64 KB |
| 1 | `Arm9Bank1Arm7Bank0` | Banco físico 1 (16 KB) | Banco físico 0 (16 KB) |
| 2 | `Arm9Bank0Arm7Bank1` | Banco físico 0 (16 KB) | Banco físico 1 (16 KB) |
| 3 | `Arm7All32K` | Sem Shared WRAM | Bancos 0 + 1 (32 KB) |

Os bancos de 16 KB são espelhados pelas janelas de endereço documentadas: janela ARM9 de 16 MB e janela ARM7 de 8 MB, com a faixa privada ARM7 começando em `0x03800000`. Os modos 1 e 2 são modelados com regiões sobrepostas no mesmo endereço virtual e máscaras de CPU disjuntas.

No modo 0, o mapa não deixa o espaço ARM7 vazio: conforme o comportamento documentado de desalocação, a janela ARM7 espelha a WRAM privada de 64 KB em `0x03800000`. A instalação inicial recebe o modo explicitamente. `NdsRetailRamMap::install()` pré-mapeia as sete visões possíveis. `set_shared_wram_mode()` substitui em uma única operação validada as máscaras de CPU dessas regiões preexistentes; nenhuma janela é desalocada ou recriada, e os backings dos dois bancos de 16 KB permanecem intactos. `NdsWramControl` conecta essa API ao endereço `0x04000247`: o ARM9 pode ler e escrever apenas 8 bits; a escrita usa os bits 0–1 e ignora os bits reservados 2–7. A escrita ARM7 em WRAMCNT é rejeitada. `WRAMSTAT` em `0x04000241` é somente leitura no ARM7 e devolve os bits 0–1 do modo atual; bits 2–7 são zero. O mesmo endereço não é capturado pelo dispositivo WRAM no ARM9, onde representa VRAMCNT_B (ainda sem implementação própria).

Os bytes físicos das duas banks são armazenados em um backing interno com `CpuAccessNone`; os processadores acessam somente os mirrors que o modo WRAMCNT lhes atribui.

### Controlador de interrupções

`NdsInterruptControl` mantém registradores separados para ARM9 e ARM7:

| Endereço | Registrador | Semântica |
|---|---|---|
| `0x04000208` | `IME` | Bit 0 habilita interrupções globalmente para aquela CPU |
| `0x04000210` | `IE` | Máscara de fontes habilitadas |
| `0x04000214` | `IF` | Flags de solicitação; escrever 1 limpa somente os bits correspondentes |

O dispositivo aceita transferências alinhadas de 32 bits nesses endereços e falha de forma explícita para larguras não suportadas. Os dispositivos de hardware chamam `request_irq(cpu, mask)` para latchear uma fonte em IF. `irq_asserted(cpu)` calcula `IME.bit0 && (IE & IF) != 0`. A classe oferece `try_enter_irq(cpu, state, return_address, vector_base)`: se IME/IE/IF mantiver a linha ativa e `CPSR.I` estiver limpo, a função entra em IRQ usando o retorno e o vetor fornecidos pelo chamador. Ela não apaga IF; o guest reconhece a solicitação. O scheduler ainda precisa invocar essa função no limite seguro após um bloco recompilado. Timer requests chegam a IF pelo controlador associado.

### Teclado ARM9/ARM7

`NdsKeypadControl` modela os dez botões físicos A, B, Select, Start, direcional e L/R nos registradores de 16 bits:

| Endereço | Registrador | Semântica |
|---|---|---|
| `0x04000130` | `KEYINPUT` | Leitura compartilhada pelos dois processadores; bits 0–9 ativos em nível baixo, bits reservados 10–15 lidos como zero |
| `0x04000132` | `KEYCNT` | Leitura/escrita independente por CPU; bits 0–9 selecionam botões, bit 14 habilita IRQ e bit 15 escolhe OR (0) ou AND (1) |

O host fornece o estado físico por `set_pressed_keys(mask)`, usando bits ativos em nível alto para botões pressionados. O dispositivo converte para o formato ativo-baixo de KEYINPUT e limita a máscara aos dez botões. A ponte C `oracis_host_set_keypad_state(mask)` mantém a última máscara mesmo antes de o dispositivo ser conectado ao barramento; `oracis_bus_bind_keypad_control()` aplica esse estado no vínculo. O host SDL3 envia pressionamentos/liberações pela ponte, limpa as teclas ao perder foco e usa o mapa X/Z (A/B), Enter/Backspace (Start/Select), setas (direcional) e Q/W (L/R). X/Y, toque e tampa não fazem parte deste bloco: pertencem ao registrador ARM7 `EXTKEYIN`, ainda não implementado.

Quando a condição selecionada se torna ativa com o bit 14 habilitado, o dispositivo solicita o IF bit 12 da CPU correspondente. OR significa pelo menos uma tecla selecionada pressionada; AND exige todas as teclas selecionadas. A condição é latcheada na transição para ativa para não gerar solicitações repetidas enquanto o mesmo conjunto permanece pressionado. Uma seleção vazia não gera uma condição de tecla. A entrega da exceção IRQ continua sob responsabilidade futura do scheduler.

Os acessos ao KEYINPUT/KEYCNT são de 16 bits; larguras não suportadas e escritas em KEYINPUT são rejeitadas explicitamente. O barramento C pode conectar o dispositivo após o GuestMemory e o controlador de interrupções.

### Relógio global ARM9/ARM7

`NdsSystemClock` converte os ciclos cumulativos dos dois núcleos para um domínio comum de meio ciclo de barramento. O ARM9 opera a 2 ciclos internos por ciclo de barramento; o ARM7 opera a 1:1. O relógio calcula um watermark sincronizado usando o menor tempo reportado pelos dois processadores e só então converte meios-ciclos completos em ciclos globais do barramento. Isso evita somar os ciclos locais como se os processadores executassem em série e evita avançar periféricos além do núcleo mais atrasado.

A API `report_cpu_cycles(cpu, total)` recebe contadores cumulativos e monotônicos, não deltas. A cada atualização, `NdsSystemClock` chama `NdsTimerControl::advance_bus_cycles(delta)` uma única vez para o intervalo global recém-concluído. Relatórios repetidos não avançam o tempo; regressão, CPU inválida e overflow são rejeitados. A fração de meio ciclo é preservada entre relatórios.

A ponte C oferece `oracis_bus_bind_system_clock()`, `oracis_bus_report_cpu_cycles(cpu, total)`, `oracis_bus_report_idle_bus_cycles(cpu, bus_ticks)` e `oracis_bus_global_bus_cycles()`. O relógio deve ser conectado depois do controlador de timers. `NdsMachine::end_cpu_slice(cpu)` já lê o contador cumulativo do scheduler e o reporta ao relógio; `begin_cpu_slice(cpu, budget)` seleciona o estado ARM correto e inicia o orçamento cooperativo. Quando uma CPU não está executando, mas seu domínio de clock continua ativo, o driver pode usar `NdsMachine::report_idle_bus_cycles(cpu, ticks)`. A seleção dos modos HALT/STOP ainda não está integrada ao loop host. A contabilização atual usa ciclos core fornecidos pelo scheduler e não inclui ainda todas as esperas de memória/barramento.

### Contexto proprietário e fronteiras de fatia: NdsMachine

`NdsMachine` centraliza os objetos que formam a base atual da máquina: `GuestMemory`, os estados ARM9/ARM7, `NdsWramControl`, `NdsInterruptControl`, `NdsTimerControl`, `NdsKeypadControl` e `NdsSystemClock`. A inicialização instala o mapa retail e conecta o barramento C na ordem exigida pelas dependências. Se um vínculo falhar, os vínculos já realizados são desfeitos em ordem reversa. Apenas uma máquina pode possuir o runtime global por vez; a ABI ARM agora rejeita a substituição de estados ainda vinculados e oferece desvinculação explícita.

`begin_cpu_slice(cpu, budget)` seleciona o estado ARM e abre uma única fatia cooperativa global; a API rejeita fatias sobrepostas para não permitir que os estados e o relógio global sejam avançados simultaneamente sem um scheduler de eventos. `end_cpu_slice(cpu)` fecha a fatia e reporta automaticamente o contador cumulativo de ciclos ao relógio sincronizado. Isso elimina a necessidade de o chamador copiar manualmente o contador do scheduler, mas ainda exige que o driver externo delimite corretamente cada fatia e alterne as duas CPUs. `report_idle_bus_cycles(cpu, ticks)` é uma chamada separada, rejeitada enquanto qualquer fatia estiver ativa; o driver só deve usá-la quando o domínio de clock permanece ativo. O modo inicial WRAMCNT é obrigatório no construtor: a classe não presume o estado deixado por firmware.

A classe administra propriedade, ordem de vínculos e contabilização nas fronteiras de execução. O estado HALT agora é separado por CPU: ARM7 desperta quando `IE & IF` fica diferente de zero (mesmo com IME ou CPSR.I desabilitados), enquanto ARM9 usa a linha IRQ controlada por IME; CPSR.I continua pertencendo à decisão posterior de entrada da exceção. Uma interrupção já ativa na entrada impede o HALT. A API `write_arm7_halt_control()` reconhece o HALT ARM7 (valor `0x80`); GBA mode (`0x40`) e SLEEP (`0xC0`) são rejeitados explicitamente, pois SLEEP exige suspender vídeo/áudio e outros domínios ainda não modelados. O acesso MMIO direto a `HALTCNT` em `0x04000301` ainda não está ligado a esse método: por enquanto, o caminho funcional é o BIOS `CustomHalt`. O HALT do ARM9 pode ser solicitado diretamente via `enter_cpu_halt(NDS_ARM9)` e já está conectado ao CP15 MCR Wait-For-Interrupt (`c7,c0,4` e `c7,c8,2`). O runtime conecta o SWI NDS 06 (Halt) em ambos os cores e o SWI ARM7 1F (CustomHalt, parâmetro de 8 bits em R2) às APIs da máquina. Um pedido feito dentro de um bloco gerado marca yield no scheduler para encerrar a fatia antes da próxima instrução. SWI Sleep (07), outras chamadas BIOS e operações CP15 não implementadas continuam falhando com diagnóstico explícito. A distinção entre os critérios de wake e os modos ARM7 está documentada em [GBATEK — DS System and Built-in Peripherals](https://fabiensanglard.net/another_world_polygons_GBA/gbatech.html) e na API ARM7 de [devkitPro/libnds — bios.h](https://github.com/devkitPro/libnds/blob/master/include/nds/bios.h); o estado Sleep não é reduzido incorretamente a um HALT simples.

A classe administra propriedade, ordem de vínculos, contabilização e estado HALT nas fronteiras de execução. Ela **não** é ainda o loop dual-core: não escolhe intervalos de interleaving, não resolve eventos concorrentes nem entrega IRQ automaticamente e não implementa DMA/IPC ou wait states. O host SDL3 ainda não cria uma instância desta classe.

### Temporizadores ARM9/ARM7

`NdsTimerControl` modela quatro timers de 16 bits para cada processador:

| Timer | Contador/recarga | Controle |
|---:|---:|---:|
| 0 | `0x04000100` | `0x04000102` |
| 1 | `0x04000104` | `0x04000106` |
| 2 | `0x04000108` | `0x0400010A` |
| 3 | `0x0400010C` | `0x0400010E` |

O registro baixo lê o contador atual e recebe a recarga em escritas. A recarga é carregada no contador ao iniciar (borda 0→1) e após cada overflow. O controle implementa divisores 1/64/256/1024, cascata por overflow do timer anterior, IRQ de overflow e start/stop. Timer 0 não aceita cascata. Os overflows dos quatro canais solicitam IF bits 3–6 da CPU correspondente.

Escrita de 32 bits iniciada no registro baixo atualiza recarga e controle na mesma operação, preservando o caso em que reload e start são escritos juntos. As operações de 16 bits também são suportadas; outras larguras falham explicitamente.

`advance_bus_cycles(n)` recebe ciclos do barramento NDS e avança, numa única chamada, as quatro timers ARM9 e as quatro ARM7. As duas banks compartilham o clock periférico de 33,513982 MHz, mas mantêm contadores, recargas, prescalers e flags IRQ independentes. `NdsSystemClock` agora sincroniza o tempo cumulativo reportado pelos dois núcleos e chama esta API uma única vez por intervalo global completo. A ligação ao loop de execução ainda depende de o driver reportar os ciclos de ambas as CPUs nas fronteiras das fatias; não há chamada automática em `runtime_tick`.


## 4. API principal do GuestMemory

Header: `include/oracis/memory/guest_memory.hpp`  
Implementação: `src/memory/guest_memory.cpp`

- `map_ram(base, size, permissions, name, cpu_access)`: aloca bytes zerados, inicialmente sem proveniência.
- `map_image(base, contents, size, permissions, name, cpu_access)`: copia bytes fornecidos pelo carregador e marca a faixa como materializada.
- `map_alias(alias_base, canonical_base, size, name, cpu_access)`: cria uma janela linear para o mesmo backing.
- `map_mirror(alias_base, canonical_base, period, window_size, name, cpu_access)`: cria uma janela maior que repete um backing físico de período menor.
- `unmap_region(name)`: remove uma janela guest pelo nome único, mantendo vivos backings ainda referenciados por outros aliases.
- `update_cpu_access(updates, count)`: valida o mapa resultante inteiro antes de alterar as máscaras de várias regiões existentes; a etapa de aplicação não aloca e não pode falhar. GuestMemory não fornece sincronização entre threads.
- `NdsRetailRamMap::set_shared_wram_mode(memory, mode)`: altera transacionalmente a visibilidade das sete janelas Shared WRAM pré-mapeadas e preserva os bancos físicos.
- `NdsWramControl::install()`: instala o mapa inicial e mantém o modo atual do dispositivo.
- `NdsWramControl::read/write(cpu, address, width, value)`: implementa WRAMCNT ARM9 e WRAMSTAT ARM7; WRAMSTAT é 8-bit, read-only e devolve o modo nos bits 0–1. O endereço ARM9 correspondente a VRAMCNT_B é deixado para o futuro dispositivo de vídeo.
- `NdsInterruptControl::read/write(cpu, address, width, value)`: implementa IME/IE/IF para ambas as CPUs, em 32 bits, com IF write-one-to-clear.
- `NdsInterruptControl::request_irq(cpu, mask)` / `irq_asserted(cpu)`: latcheia fontes IF e calcula a linha habilitada por IME/IE.
- `NdsInterruptControl::try_enter_irq(cpu, state, return_address, vector_base)`: entra em IRQ apenas quando a linha está ativa e `CPSR.I` está limpo; o chamador fornece LR e vetor; IF não é apagado.
- `NdsTimerControl::read/write(cpu, address, width, value)`: implementa Timer 0–3 de cada CPU, com operações de 16 bits e acesso combinado de 32 bits ao par contador/controle.
- `NdsTimerControl::advance_bus_cycles(n)`: avança as duas banks ARM9/ARM7 pelo mesmo número de ticks do clock global do barramento.
- `NdsSystemClock::report_cpu_cycles(cpu, total)`: sincroniza watermarks cumulativos ARM9/ARM7 na razão de clock 2:1 e avança temporizadores apenas pelos ciclos globais completos.
- `NdsSystemClock::global_bus_cycles()`: consulta o tempo periférico global já comprometido.
- `oracis_bus_bind_system_clock()` / `oracis_bus_unbind_system_clock()`: conecta o relógio global após o controlador de timers.
- `oracis_bus_report_cpu_cycles(cpu, total)`: reporta o contador cumulativo de uma CPU ao relógio conectado; rejeita regressões/overflow e não soma tempos locais.
- `NdsSystemClock::report_idle_bus_cycles(cpu, ticks)` / `oracis_bus_report_idle_bus_cycles(cpu, ticks)`: adiciona tempo de barramento explicitamente transcorrido para uma CPU que não está executando instruções mas cujo clock permanece ativo; converte pela razão 2:1 e preserva o watermark comum.
- `oracis_bus_global_bus_cycles()`: consulta os ciclos de barramento globais já avançados.
- `NdsKeypadControl::read/write(cpu, address, width, value)`: implementa KEYINPUT e KEYCNT, com KEYINPUT físico compartilhado e KEYCNT/condição de IRQ separados por CPU.
- `NdsKeypadControl::set_pressed_keys(mask)`: atualiza os dez botões físicos e solicita IRQ bit 12 quando a condição KEYCNT habilitada se torna ativa.
- `oracis_bus_bind_keypad_control()` / `oracis_bus_unbind_keypad_control()`: conecta/desconecta o teclado depois do GuestMemory e de NdsInterruptControl; o estado de entrada armazenado pela ponte host é aplicado no vínculo.
- `oracis_host_set_keypad_state(mask)`: recebe os dez botões em formato ativo-alto e encaminha ao dispositivo conectado, retendo também o estado quando ele está temporariamente desvinculado.
- `oracis_bus_bind_interrupt_control()` / `oracis_bus_unbind_interrupt_control()`: conecta/desconecta o controlador de interrupções ao barramento C.
- `oracis_bus_bind_wram_control()` / `oracis_bus_unbind_wram_control()`: conecta e desconecta o dispositivo ao barramento C.
- `read(cpu, address, width)`: leitura little-endian de 1, 2 ou 4 bytes.
- `write(cpu, address, width, value)`: escrita little-endian de 1, 2 ou 4 bytes.
- `validate_execute(cpu, address, size)`: verifica mapeamento e permissão de execução.
- `validate_identity(cpu, address, size, expected)`: exige bytes materializados e compara o conteúdo.

As regiões são intervalos half-open `[base, base + size)`. O limite superior pode alcançar `0x1_0000_0000`, mas não ultrapassá-lo.

## 5. Ponte C ABI usada pelos bancos

A biblioteca também exporta:
- `bus_read_u8/u16/u32`;
- `bus_write_u8/u16/u32`.

O host cria e mantém vivo um objeto `GuestMemory`, depois registra-o por `oracis_bus_bind_memory(&memory)`. O objeto não é transferido nem destruído pela biblioteca; o host deve mantê-lo vivo até `oracis_bus_unbind_memory(&memory)`.

Cada wrapper usa `g_nds_active` para encaminhar o acesso como ARM9 ou ARM7. Erros de região, CPU, permissão ou largura encerram a execução com log de CPU, endereço, largura, PC e código de erro.

O wrapper executa apenas a transferência little-endian da largura solicitada. O codegen ARMv5TE alinha LDR/STR e aplica rotação/sign-extension na função gerada; o wrapper não duplica essas transformações.

## 6. Identidade e proveniência de código

Cada backing mantém um marcador de proveniência por byte:
- `map_ram` cria bytes vazios com proveniência zero;
- `write` (incluindo os wrappers C `bus_write_*`) marca os bytes atingidos;
- `map_image` copia os bytes fonte e marca toda a faixa;
- `validate_identity` recusa bytes sem proveniência e depois compara o conteúdo.

O callback `oracis_bus_validate_dispatch` usa essa checagem para validar a faixa executável principal e cada dependência de um banco antes que o dispatch o selecione.

O modelo ainda não inclui gerações de página, invalidação de cache de instruções, escrita por DMA/dispositivo ou proveniência independente por CPU.

## 7. O que não está mapeado

Ainda não são regiões GuestMemory funcionais:
- I/O em `0x04000000` — deve usar handlers MMIO, nunca RAM simples;
- ITCM/DTCM ARM9 — dependem de configuração CP15, cache e MPU;
- BIOS ARM9/ARM7 — precisam de carregamento e regras de proteção apropriadas;
- VRAM, paletas, OAM e VRAM usada como WRAM;
- Wi-Fi RAM/registradores;
- GBA slot e Game Card;
- ligação do driver de fatias ao `NdsSystemClock` e seleção correta dos modos HALT/STOP para reportar progresso ocioso; accounting de wait states, DMA, IPC, PPU e ordenação geral de eventos;
- wait states, prioridades de barramento e penalidades de contenção Main RAM.

O runtime mantém contadores cumulativos locais via `runtime_tick`; `NdsSystemClock` converte esses contadores quando o driver os reporta, usando o watermark comum ARM9/ARM7. O driver ainda não faz esses relatórios automaticamente, e os modelos de `runtime_mem_cycles`/`runtime_code_cycles` não representam todo o hardware NDS.

## 8. Testes registrados

O CMake registra testes separados para:
- regiões/aliases/proveniência genéricas;
- wrappers C do barramento;
- Main RAM compartilhada e seus três espelhos;
- WRAM ARM7 privada;
- todos os quatro modos de Shared WRAM, incluindo regiões sobrepostas com visibilidade diferente por CPU e o espelho da WRAM privada ARM7 no modo 0;
- transições dinâmicas entre modos, preservação de conteúdo dos dois bancos e rejeição de modo inválido sem alterar a janela corrente;
- leitura/escrita de WRAMCNT pelo ARM9 e leitura WRAMSTAT pelo ARM7 via barramento C; WRAMSTAT read-only, modos 0–3, larguras inválidas e distinção do alias ARM9 VRAMCNT_B;
- KEYINPUT ativo-baixo e compartilhado, KEYCNT independente por CPU, mascaramento de bits reservados, acesso somente leitura de KEYINPUT, condições OR/AND, IRQ bit 12, não repetição enquanto a condição permanece ativa e roteamento/teardown do barramento C;
- sincronização 2:1 dos contadores cumulativos ARM9/ARM7, watermark de tempo comum, preservação de meio ciclo, avanço único dos dois bancos de timer, rejeição de regressões/overflow e bind/unbind C ABI do relógio global;
- `IME`/`IE`/`IF` independentes por CPU, IME master gate, máscara IE, IF write-one-to-clear, requests latcheados e roteamento por `bus_read_u32`/`bus_write_u32`.

**Na revisão `a435c0f`, a CI passou nos quatro jobs**, incluindo desktop Linux, Android ARM64, NDSRecomp e 22/22 testes de runtime. A revisão atual acrescenta WRAMSTAT ARM7 e regressões de acesso; a CI está em execução.
