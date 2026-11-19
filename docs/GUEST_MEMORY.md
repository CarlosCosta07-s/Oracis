# GuestMemory e mapa de RAM do Nintendo DS

**Estado:** armazenamento guest configurável, visibilidade por CPU, aliases, mirrors periódicos e mapa de RAM do NDS retail implementados. `set_shared_wram_mode()` reconfigura os quatro modos preservando os bytes físicos, e `NdsWramControl` liga a escrita/leitura de 8 bits em `WRAMCNT` ao barramento C para ARM9. `WRAMSTAT` e os demais handlers MMIO ainda não estão implementados. O bloco de interrupções `IME`/`IE`/`IF` já possui estado independente ARM9/ARM7, MMIO de 32 bits e semântica W1C para IF; a entrada real de IRQ ainda depende do scheduler de eventos.

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

No modo 0, o mapa não deixa o espaço ARM7 vazio: conforme o comportamento documentado de desalocação, a janela ARM7 espelha a WRAM privada de 64 KB em `0x03800000`. A instalação inicial recebe o modo explicitamente. `NdsRetailRamMap::install()` pré-mapeia as sete visões possíveis. `set_shared_wram_mode()` substitui em uma única operação validada as máscaras de CPU dessas regiões preexistentes; nenhuma janela é desalocada ou recriada, e os backings dos dois bancos de 16 KB permanecem intactos. `NdsWramControl` conecta essa API ao endereço `0x04000247`: o ARM9 pode ler e escrever apenas 8 bits; a escrita usa os bits 0–1 e ignora os bits reservados 2–7. A escrita ARM7 é rejeitada. `WRAMSTAT` em `0x04000241` é um registrador distinto, somente leitura no ARM7; sua semântica de valor ainda não está implementada. No mapa ARM9, `0x04000241` pertence a `VRAMCNT_B`, não a `WRAMSTAT`.

Os bytes físicos das duas banks são armazenados em um backing interno com `CpuAccessNone`; os processadores acessam somente os mirrors que o modo WRAMCNT lhes atribui.

### Controlador de interrupções

`NdsInterruptControl` mantém registradores separados para ARM9 e ARM7:

| Endereço | Registrador | Semântica |
|---|---|---|
| `0x04000208` | `IME` | Bit 0 habilita interrupções globalmente para aquela CPU |
| `0x04000210` | `IE` | Máscara de fontes habilitadas |
| `0x04000214` | `IF` | Flags de solicitação; escrever 1 limpa somente os bits correspondentes |

O dispositivo aceita transferências alinhadas de 32 bits nesses endereços e falha de forma explícita para larguras não suportadas. Os dispositivos de hardware chamam `request_irq(cpu, mask)` para latchear uma fonte em IF. `irq_asserted(cpu)` calcula `IME.bit0 && (IE & IF) != 0`. A classe oferece `try_enter_irq(cpu, state, return_address, vector_base)`: se IME/IE/IF mantiver a linha ativa e `CPSR.I` estiver limpo, a função entra em IRQ usando o retorno e o vetor fornecidos pelo chamador. Ela não apaga IF; o guest reconhece a solicitação. O scheduler ainda precisa invocar essa função no limite seguro após um bloco recompilado. Timer requests chegam a IF pelo controlador associado.

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

`advance_bus_cycles(cpu, n)` recebe ciclos brutos do barramento NDS. Os periféricos usam 33,513982 MHz; `advance_core_cycles(cpu, n)` converte os custos enviados por `runtime_tick`: ARM9 usa dois ciclos de core por tick de barramento e acumula a meia-fase entre instruções; ARM7 usa um para um. Quando vinculado ao barramento, o timer registra o observador e avança em cada limite de instrução. Isso alimenta os contadores, mas não implementa a ordenação geral de eventos nem entrega automaticamente a exceção IRQ.


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
- `NdsWramControl::read/write(cpu, address, width, value)`: implementa WRAMCNT para o ARM9; outros endereços não são consumidos por esse dispositivo.
- `NdsInterruptControl::read/write(cpu, address, width, value)`: implementa IME/IE/IF para ambas as CPUs, em 32 bits, com IF write-one-to-clear.
- `NdsInterruptControl::request_irq(cpu, mask)` / `irq_asserted(cpu)`: latcheia fontes IF e calcula a linha habilitada por IME/IE.
- `NdsInterruptControl::try_enter_irq(cpu, state, return_address, vector_base)`: entra em IRQ apenas quando a linha está ativa e `CPSR.I` está limpo; o chamador fornece LR e vetor; IF não é apagado.
- `NdsTimerControl::read/write(cpu, address, width, value)`: implementa Timer 0–3 de cada CPU, com operações de 16 bits e acesso combinado de 32 bits ao par contador/controle.
- `NdsTimerControl::advance_bus_cycles(cpu, n)`: avança timers em ciclos brutos do barramento.
- `NdsTimerControl::advance_core_cycles(cpu, n)`: converte ciclos ARM9/ARM7 em ticks de barramento, preservando a meia-fase ARM9.
- Ao vincular o timer, ele se registra no observador de `runtime_tick`; o unbind remove esse observador.
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
- DMA, IPC, PPU, wait states e ordenação geral de eventos; timers vinculados já recebem o clock de `runtime_tick`;
- wait states, prioridades de barramento e penalidades de contenção Main RAM.

O runtime soma os custos enviados por `runtime_tick` e oferece um observador síncrono para dispositivos vinculados. Os modelos de `runtime_mem_cycles`/`runtime_code_cycles` ainda não representam todo o hardware NDS.

## 8. Testes registrados

O CMake registra testes separados para:
- regiões/aliases/proveniência genéricas;
- wrappers C do barramento;
- Main RAM compartilhada e seus três espelhos;
- WRAM ARM7 privada;
- todos os quatro modos de Shared WRAM, incluindo regiões sobrepostas com visibilidade diferente por CPU e o espelho da WRAM privada ARM7 no modo 0;
- transições dinâmicas entre modos, preservação de conteúdo dos dois bancos e rejeição de modo inválido sem alterar a janela corrente;
- leitura/escrita de `WRAMCNT` pelo ARM9 via `bus_read_u8`/`bus_write_u8`, rejeição de acesso pelo ARM7 e ligação/desligamento do dispositivo;
- `IME`/`IE`/`IF` independentes por CPU, IME master gate, máscara IE, IF write-one-to-clear, requests latcheados e roteamento por `bus_read_u32`/`bus_write_u32`.

**Na revisão d7e9e23, a CI confirmou os 20 testes existentes e todos passaram.** O teste do controlador de interrupções é o 21º alvo; sua primeira execução está na CI da revisão atual.
