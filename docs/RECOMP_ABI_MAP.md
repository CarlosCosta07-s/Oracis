# NDSRecomp — mapa do contrato de bancos gerados

**Estado:** ABI parcial implementada; CP15 ARM9 inclui TCM/load modes testados, tracing usa buffer circular por CPU e `runtime_code_cycles` cobre fetch uncached em um subconjunto de regiões ARM9. Bancos USA Rev 1 foram emitidos localmente e os quatro arquivos C foram compilados em objetos C99; ainda não foram ligados ao runtime nem executados.


**Revisão upstream examinada:** `14b7e15023e9affc9dc375e4e611ac703f5c76ab`.

## 1. Emissão padrão escolhida

O caminho normal de `recompiler/src/main.cpp` emite código C:
- corpos de função em `<bank>.c`;
- protótipos em `<bank>.h`;
- uma tabela `<bank>_dispatch.c` de endereços guest para ponteiros de função host.

Os bancos incluem `runtime_arm.h`. O código gerado usa:
- `g_cpu` para acessar registradores e CPSR da CPU ativa;
- `g_nds_active` para identificar ARM9 ou ARM7;
- `bus_read_*` / `bus_write_*` para acessos guest;
- helpers `arm_*` e `runtime_*` para flags, tempo, dispatch e exceções.

Oracis mantém os arquivos gerados em C e implementa o runtime em C++20 com símbolos C ABI.

## 2. Tipos binários da tabela de dispatch

Os tipos foram comparados campo a campo com o header upstream:

| Tipo | Campos principais | Tratamento |
|---|---|---|
| `NdsStaticValidationRange` | endereço, tamanho, ponteiro para bytes esperados | Definido em `runtime_arm.h` |
| `NdsStaticValidation` | endereço, tamanho, bytes esperados, dependências, quantidade | Definido em `runtime_arm.h` |
| `NdsDispatchEntry` | endereço, modo Thumb, ponteiro de função, validação | Definido em `runtime_arm.h` |
| `NdsLinkSlot` | função resolvida, chave, PC guest, guarda | Definido em `runtime_arm.h` |

O header contém asserts C99 de tamanho e offsets para o layout de 64 bits usado nos alvos planejados (Windows/Linux x64 e Android ARM64). Um alvo de 32 bits exigirá outro layout e testes próprios.

## 3. Estado ARM9/ARM7 ativo

A estrutura `ArmCpuState` contém:
- `R[16]` e `cpsr`;
- `banked_sp[6]`, `banked_lr[6]`, `banked_spsr[6]`;
- `r8_12_user[5]` e `r8_12_fiq[5]`.

O header de compatibilidade Oracis, `include/oracis/recomp/runtime_arm.h`, declara dois estados de CPU e usa:

- `oracis_arm_runtime_bind_cpus(arm9, arm7)`;
- `oracis_arm_runtime_select_cpu(NDS_ARM9/NDS_ARM7)`;
- `oracis_arm_runtime_current_state()`;
- `#define g_cpu (*oracis_active_cpu_state)`;
- `g_nds_active`, ou uma constante via `NDS_STATIC_CPU=0/1` quando o banco é especializado.

Isso evita copiar os registradores para uma shadow state: os bancos acessam diretamente o estado escolhido pelo scheduler. A seleção global é deliberadamente serial/cooperativa nesta fase; o scheduler deverá selecionar a CPU antes de invocar cada banco. Não há ainda suporte a executar bancos ARM9 e ARM7 simultaneamente em threads diferentes.

## 4. Helpers já ligados à ABI

O runtime C++ implementa wrappers C para:

| Símbolo emitido | Implementação Oracis |
|---|---|
| `arm_cond_passes` / `arm_cond_passes_i` | Avaliação dos 16 códigos de condição usando N/Z/C/V |
| `arm_set_nz` | Atualização de N/Z |
| `arm_set_nzc_logic` | Atualização N/Z/C, preservando V |
| `arm_set_nzcv_add/adc/sub/sbc` | Flags aritméticas N/Z/C/V |
| `runtime_mrs_cpsr/spsr` | Leitura de CPSR/SPSR |
| `runtime_msr_cpsr/spsr` | Escrita por máscara de campos e validação de privilégio |
| `runtime_restore_cpsr_from_spsr` | Restauração do PSR salvo |
| `runtime_exception_return` | Restauração de SPSR, bancos e PC |
| `runtime_read_user_reg` / `runtime_write_user_reg` | Acesso à visão User de R0–R15, incluindo bancos FIQ e SP/LR privilegiados |
| `runtime_clz` | Contagem de zeros à esquerda em 32 bits (zero retorna 32) |
| `runtime_unimplemented_op` | Diagnóstico e encerramento fail-closed |

O código gerado pelo perfil ARMv5TE atual expande os shifts de operand2 inline, cobrindo as formas imediatas e por registrador e os casos especiais de LSR/ASR #0 e ROR #0 (RRX). Por isso, não adicionamos uma segunda implementação de shifter ao runtime. A compatibilidade com bancos antigos que chamem símbolos `arm_shift_*` ainda não é uma meta do primeiro pipeline.

## 5. Memória e barramento C ABI

A camada `oracis_memory` já implementa os seis símbolos C `bus_read_u8/u16/u32` e `bus_write_u8/u16/u32`. Eles usam o seletor `g_nds_active`, encaminham a transferência para `GuestMemory` e falham com diagnóstico quando a região não está mapeada ou a permissão não permite o acesso.

Essa ponte cobre RAM/aliases explicitamente configurados. `NdsRetailRamMap` agora instala Main RAM retail 4MB compartilhada com espelhos em 0x02400000/0x02800000/0x02C00000, WRAM privada ARM7 64KB em 0x03800000 e Shared WRAM segundo os quatro modos de WRAMCNT. As regiões sobrepostas de Shared WRAM usam máscaras de CPU disjuntas e espelhos periódicos; no modo 0, a janela ARM7 espelha a WRAM privada de 64KB. `set_shared_wram_mode()` altera transacionalmente, após validar o mapa resultante, a visibilidade de sete janelas Shared WRAM pré-mapeadas. O dispositivo MMIO `NdsWramControl` já implementa leitura/escrita de 8 bits do `WRAMCNT` (`0x04000247`) pelo ARM9 e aciona essa troca; `WRAMSTAT` (`0x04000241`, ARM7, somente leitura) devolve os bits 0–1 do modo WRAMCNT; no ARM9 o mesmo endereço corresponde a VRAMCNT_B e não é capturado pelo dispositivo WRAM. `NdsVramControl` implementa `VRAMCNT_A-I` (`0x04000240–0x04000246` e `0x04000248–0x04000249`, ARM9, escrita de 8 bits e somente escrita) e `VRAMSTAT` (`0x04000240`, ARM7, leitura de 8 bits e somente leitura); `0x04000247` permanece WRAMCNT. No modo enable+MST=0/LCDC, os nove bancos são acessíveis pelo ARM9: A-D possuem 128 KiB em `0x06800000`, `0x06820000`, `0x06840000`, `0x06860000`; E possui 64 KiB em `0x06880000`; F/G possuem 16 KiB em `0x06890000`/`0x06894000`; H possui 32 KiB em `0x06898000`; I possui 16 KiB em `0x068A0000`. OFS não participa desse destino. C/D também podem ser atribuídos com enable+MST=2 ao ARM7 nas janelas `0x06000000`/`0x06020000`, selecionadas por OFS.0; OFS.1 não é mapeado. `VRAMSTAT` informa enable+MST para C/D sem refletir OFS. Backings físicos separados são expostos por aliases com permissões por CPU atualizadas transacionalmente. O barramento ignora stores de 8 bits nos aliases LCDC do ARM9; stores de 16/32 bits são gravados normalmente. A exceção é C/D em Work RAM do ARM7, que aceita stores de 8 bits. Se C e D selecionarem o mesmo slot ARM7, a alteração é recusada com `Overlap` e os controles/mapeamento anteriores permanecem. Os aliases CPU também cobrem A-D/E/F/G em Main BG (MST=1), A/B/E/F/G em Main OBJ (MST=2), C/D em Sub BG/OBJ (MST=4), H/I em Sub BG (MST=1) e I em Sub OBJ (MST=2). F/G usam a fórmula de OFS.0/OFS.1 para posicionar suas janelas de 16 KiB; E/H/I e C/D em MST=4 ignoram OFS; A/B/H/I também ignoram o bit 2 do campo MST. Os aliases permanecem separados por backing físico e as colisões de janelas CPU continuam sendo recusadas transacionalmente. Extended palettes e textura não são mapeadas no espaço de endereços da CPU; exigem consumidores do controlador gráfico e acesso da PPU. Os próprios consumidores gráficos/PPU ainda não estão implementados. `NdsVideoMemory` instala os 2 KiB de palette RAM padrão em `0x05000000–0x050007FF` (Engine A BG/OBJ e Engine B BG/OBJ, quatro regiões de 512 B) e os 2 KiB de OAM em `0x07000000–0x070007FF` (1 KiB para cada engine), somente para ARM9. `write_bus()` encaminha essas escritas pelo dispositivo: STRB é ignorado e STRH/STR chegam ao backing; ARM7 não possui acesso a essas regiões. O dispositivo não implementa consumidor PPU, avaliação de sprites, temporização de scanline ou composição 2D. `NdsDisplayControl` mantém `DISPCNT` (32 bits) em `0x04000000` e `0x04001000`, além de `BG0CNT–BG3CNT` (16 bits por registrador, com acesso 32-bit a pares alinhados) nos offsets `+0x08–+0x0E` dos motores A/B. Os registradores são acessíveis apenas pelo ARM9. O motor B mascara os campos DISPCNT reservados/exclusivos do motor A (bits 3, 18–19, 22 e 24–29). `DISPSTAT` (`0x04000004`) e `VCOUNT` (`0x04000006`) são compartilhados pelo ARM9/ARM7 e avançam pelo watermark de barramento do `NdsSystemClock`. O modelo usa 2130 ciclos por linha, 263 linhas, HBlank em 1606 ciclos ARM9/1613 ARM7, VBlank nas linhas 192–261, compare LYC de 9 bits e IRQs VBlank/HBlank/VCOUNT roteadas ao controlador IF. VCOUNT aceita uma escrita de 16 bits e aplica o novo valor na próxima fronteira de linha; 202–212 é a janela recomendada de sincronização, não uma restrição de acesso. O IRQ de VBlank é emitido na linha 192, não por salto de software para outra linha dentro da faixa VBlank. Offsets de scroll/affine, janelas, blending, fetch PPU e saída gráfica permanecem pendentes. `NdsInterruptControl` implementa IME/IE/IF de 32 bits (`0x04000208`, `0x04000210`, `0x04000214`) com bancos independentes por CPU, requests latcheados e IF write-one-to-clear. `try_enter_irq()` testa IME/IE/IF e `CPSR.I`, depois entra na exceção ARM IRQ com LR e base vetorial fornecidos pelo chamador; `NdsMachine::run_dual_core_round()` executa a verificação nos limites seguros, obtendo o LR do provider e selecionando o vetor ARM9 por CP15 Control.V (ARM7 usa base zero). `NdsTimerControl` implementa os quatro timers por CPU, prescalers, cascata, reload/overflow e requests IF. `advance_bus_cycles(n)` avança as duas banks na mesma quantidade de ciclos do clock periférico. A integração desse tempo ao scheduler global continua pendente; não há conversão direta a partir de `runtime_tick`, pois os contadores independentes dos dois CPUs não podem ser somados sem sincronização. DMA, IPC, wait states e efeitos colaterais dos demais registradores ainda não existem. O codegen ARMv5TE alinha LDR/STR e aplica rotação/sign-extension fora do wrapper.

Consulte [GUEST_MEMORY.md](GUEST_MEMORY.md) para o contrato completo.

## 6. Registro e dispatch estático

O Oracis agora possui uma implementação própria de registro/lookup em \`static_dispatch.hpp/.cpp\` e uma fachada C em \`static_dispatch_abi.h/.cpp\`.

- \`oracis_static_dispatch_register_bank\` registra uma tabela imutável para ARM9 ou ARM7.
- \`oracis_static_dispatch_unregister_bank\` remove a tabela exata por ponteiro e tamanho.
- \`oracis_static_dispatch_lookup\` consulta endereço e estado ARM/Thumb (CPSR.T).
- \`runtime_dispatch\`, variantes literais e \`runtime_dispatch_with_exchange\` chegam ao dispatcher e invocam a função escolhida.
- Miss, CPU sem banco, estado desalinhado e entrada inválida encerram com diagnóstico; não existe fallback interpretado.
- As tabelas precisam estar ordenadas por \`(addr, thumb)\`; duplicatas do mesmo par dentro da mesma tabela são rejeitadas.
- Se bancos diferentes oferecem a mesma chave, o maior intervalo validado vence; empate mantém a primeira tabela registrada.

O contrato de seleção foi comparado ao algoritmo de lookup do NDSRecomp. O registro não copia nem depende do runtime/runner upstream.

### Identidade de código

O callback \`oracis_bus_validate_dispatch\` é instalado quando \`oracis_bus_bind_memory\` liga o \`GuestMemory\`. Ele verifica a faixa executável da entrada, compara os bytes esperados e confere cada dependência de validação.

\`GuestMemory::map_ram\` cria armazenamento vazio e não prova identidade. Escritas C \`bus_write_*\` marcam a proveniência byte a byte; \`GuestMemory::map_image\` copia bytes fornecidos pelo carregador e marca a faixa inicializada. Um banco que não tenha bytes materializados, ou cujos bytes divergirem, permanece inativo. O unbind da memória remove o callback.

Essa camada ainda não implementa o carregador de bancos do Pokémon Platinum, cache por revisão, ciclo de vida de overlays ou proteção contra descarga enquanto uma função do banco ainda está em execução. O chamador precisa manter tabelas, metadados e funções válidos enquanto registrados.

### Pilha de retorno e link slots

O runtime também implementa \`runtime_call_push_return\`, \`runtime_call_should_return\` e \`runtime_call_cancel_return\` em \`src/runtime/call_return_stack.cpp\`.

- A pilha tem capacidade de 1024 frames por CPU; ARM9 e ARM7 mantêm estados independentes.
- A chave de retorno combina o endereço (bit 0 removido) com o estado atual ARM/Thumb.
- \`runtime_call_should_return\` procura do topo para baixo; ao encontrar o frame, remove esse frame e todos os mais novos. Assim um retorno não local não deixa frames guest órfãos.
- \`runtime_call_cancel_return\` remove apenas o frame do topo quando o endereço corresponde.
- \`runtime_call_stack_depth/data/restore\` expõem a pilha ativa para futura serialização de estado.

Os símbolos \`runtime_link_call/branch/fallthrough\` já existem, mas não resolvem nem armazenam links: encaminham o alvo ao dispatcher estático normal. \`runtime_inline_leaf_admit\` retorna falso. Sem cache com epoch e validação de conteúdo, nenhuma cópia inline é admitida.

## 7. Controle cooperativo de fatias

\`src/runtime/scheduler_core.cpp\` fornece o primeiro núcleo de contabilização de ciclos e yield por CPU:

- \`runtime_tick(cycles)\` incrementa o contador de ciclos da CPU ativa.
- \`runtime_should_yield()\` e \`runtime_slice_yield()\` verificam pedido de yield ou vencimento do orçamento; ao retornar verdadeiro, marcam automaticamente o estado de unwind da CPU ativa.
- \`runtime_unwinding()\` consulta o estado de unwind enquanto os frames C gerados retornam ao host, permitindo que os caminhos BL/BLX preservem frames guest pendentes durante a preempção.
- \`oracis_scheduler_begin_slice/end_slice\`, \`request_yield\`, \`set_unwinding\`, \`cycle_count\` e \`reset\` permitem ao host controlar cada CPU. O fim da fatia limpa o unwind somente depois que o callback já retornou ao host.

Os contadores e deadlines ARM9/ARM7 são independentes, mas `NdsMachine::run_dual_core_round()` já coordena fatias até um watermark comum na razão 2:1, contabiliza o restante de HALT/yield como tempo ocioso e solicita contexto arquitetural ao host para entrega IRQ. A ponte C do host também oferece `oracis_host_runtime_run_round()`, protegida pela presença de bancos para ambas as CPUs.

**Limite desta etapa:** isto ainda não é o scheduler Nintendo DS completo. O round sincroniza os dois cores e alimenta o relógio/timers, mas não ordena todos os eventos DS nem modela DMA, IPC, PPU, wait states ou contenção do barramento.

Os corpos C gerados usam um contrato adicional de timing:
- `g_insn_count[2]` mantém contadores independentes de instruções aposentadas; `oracis_scheduler_reset()` limpa apenas o contador da CPU selecionada.
- `g_insn_hook_armed` inicia desarmado. `runtime_insn_slow()` falha explicitamente até haver um consumidor de tracing registrado.
- `NDS_ARM9_CODE_K` emite o sentinel que desativa o fast path packed; `nds_code_numc()` encaminha ao provedor `runtime_code_cycles()`.
- `arm9_cycle_combine()` implementa a combinação aritmética entre fetch, acesso de dados e ciclos internos.
- `runtime_code_cycles()` implementa fetch uncached ARM9 em ITCM (1 ciclo core), Main RAM (18), WRAM/BIOS/I/O/OAM (8); Thumb atribui o fetch N32 completo à primeira halfword e zero à segunda. ARM7 retorna 0 nesse helper porque seu fetch já está incorporado ao custo-base gerado. Os valores são em ciclos core ARM9 (66 MHz), derivados do clock de barramento de 33 MHz e das tabelas N32. Palette/VRAM também tem custo N32 suportado (10 ciclos core ARM9); Gamecard, cache e outras regiões permanecem fail-closed.
- `runtime_mem_cycles()` implementa custos-base NDS7/DATA e NDS9/DATA para Main RAM, WRAM/BIOS/I/O/OAM, Palette/VRAM no ARM9 e VRAM C/D atribuída como Work RAM no ARM7, além de TCM normal; resultados ARM9 são convertidos para 66 MHz. A Palette RAM não está no mapa ARM7. A ABI não distingue load/store, então TCM load mode, Wi-Fi/Gamecard e N32 não sequencial ARM7 em VRAM falham de forma explícita. `runtime_mul_cycles()` implementa early termination ARM7TDMI (m=1–4 por extensão signed/unsigned, mais extra=0/1/2); ARM9 retorna zero porque o ciclo interno é estático. `arm7_cycle_combine()` reconstrói AddCycles_CD/CDI com PC/estado Thumb e último acesso publicados pelos helpers anteriores; usa NDS7/CODE para numC, NDS7/DATA para numD e trata Main RAM/Main RAM e o overlap entre Main RAM e regiões rápidas. Fetch/refill não suportados (cache, regiões dinâmicas) continuam fail-closed. Veja [ARM9_TIMING.md](ARM9_TIMING.md).

## 8. Declarações e serviços ainda sem comportamento funcional

O header fornece o contrato C99 necessário para compilar os bancos emitidos, e os helpers de timing acima permitem a ligação fail-closed. Os seguintes grupos ainda precisam de implementações funcionais:

- timing parcial de instruções: `runtime_code_cycles` implementa fetch uncached ARM9 em ITCM (1), Main RAM (18), WRAM/BIOS/I/O/OAM (8) e Palette/VRAM (10) ciclos core ARM9; a janela de I/O coberta pelo provider é `0x04000000–0x04000FFF`. `arm9_refill_cycles` soma os fetches N32 do destino: duas palavras em ARM, uma palavra compartilhada para Thumb alinhado a 4 bytes ou duas palavras se o destino começa na segunda halfword. `arm7_refill_cycles` soma dois fetches sequenciais S32 (ARM) ou S16 (Thumb), usando a tabela NDS7/CODE. No ARM7, Palette RAM não é mapeada; o timing de VRAM aplica-se somente à faixa de VRAM C/D atribuída como Work RAM (`0x06000000–0x0603FFFF`). A visibilidade dos aliases é imposta pelo `NdsVramControl` para enable+MST=2 e OFS=0/1; um endereço da faixa potencial sem banco atribuído continua inacessível. Ambos reclassificam cada fetch quando o destino atravessa uma fronteira de região e falham fora das classes suportadas. Veja [ARM9_TIMING.md](ARM9_TIMING.md).
- timing de dados parcial: `runtime_mem_cycles` implementa N/S cycles para Main RAM, WRAM, BIOS, I/O (primeiros 4 KB), OAM e Palette/VRAM no ARM9; no ARM7, inclui VRAM C/D quando atribuída como Work RAM. O helper não recebe direção read/write; load modes TCM são recusados, assim como Gamecard/GBA slot, Wi-Fi e N32 ARM7 VRAM não sequencial. Cache/MPU, contenção de PPU e write-buffer continuam ausentes.
- multiply ARM7TDMI: `runtime_mul_cycles` implementa early termination por bytes significativos para multiplicadores signed/unsigned e soma o parâmetro adicional emitido pelo codegen; ARM9 usa o acumulador estático. `arm7_cycle_combine` implementa AddCycles_CD/CDI para as regiões documentadas. Veja [ARM9_TIMING.md](ARM9_TIMING.md).
- otimização de transferências: resolução/cache de link slots e admissão de folhas inline (os símbolos atuais fazem dispatch conservador e não usam cache);
- exceções geradas: `runtime_swi` atende HALT (0x06) e CustomHalt ARM7 (0x1F); os demais SWIs ainda falham de forma explícita. O helper `runtime_irq` permanece sem provider arquitetural completo;
- coprocessador: runtime_coproc_read/write fazem dispatch fail-closed por registrador; ARM9 CP15 implementa c0 ID/TCM size, c1 Control.V + enable/load-mode de ITCM/DTCM e c9 regiões dos TCM. Os backings de 32 KB ITCM e 16 KB DTCM usam janelas overlay com máscaras independentes de leitura/escrita/busca; MPU, cache e demais operações CP15 continuam sem implementação. WFI via c7,c0,4/c7,c8,2 segue separado; veja docs/ARM9_TCM.md;
- observabilidade: `runtime_trace_event` e `runtime_live_transfer` gravam eventos em uma fila circular de 512 entradas por CPU; a API C permite consultar, ler por cursor, limpar e obter a contagem de eventos descartados. O hook `runtime_insn_slow` segue fail-closed se for armado sem consumidor dedicado.

A emissão real USA Rev 1 produziu 78 funções ARM9 e 957 ARM7 (8 Thumb) em dois conjuntos C/dispatch locais. O relatório do compiler encerrou a varredura ARM9 por limite de passos, com 12 leituras não mapeadas e 10 alvos indiretos desconhecidos; ARM7 encerrou ao sair do módulo, com 388 leituras não mapeadas e 30 alvos indiretos desconhecidos. Isso é cobertura parcial, não cobertura integral do jogo. Os quatro arquivos C gerados foram compilados localmente em C99 contra a ABI Oracis e agrupados em objetos. O inventário encontrou 40 referências externas ARM9, 45 ARM7 e 48 nomes únicos no conjunto; `runtime_live_transfer` e `runtime_trace_event`, que não tinham definições, agora são implementados pelo runtime próprio. A ligação completa às bibliotecas de runtime ainda não foi executada, e os provedores de timing de regiões dinâmicas/cache continuam fail-closed. Nenhum segmento ou banco derivado da ROM foi adicionado ao Git/CI.

## 9. Opção de geração excluída

O parâmetro `--flat-execution` altera os corpos gerados para usar `NdsExecRegion`, `execution_region.h` e serviços do runtime upstream. **Oracis não usa essa opção.** Usaremos emissão padrão por função.

Também ficam fora da integração:
- runner, oracle e frontend upstream;
- código derivado de melonDS;
- JIT ou interpretador ARM como fallback;
- HLE manifests e live-overlay compilation até que a necessidade seja mapeada.

## 10. Testes registrados

O CMake registra **34 testes C99/C++20**. A CI do commit `3c9ff13` passou nos quatro jobs: Desktop Linux com 34/34 CTests, smoke de bancos sintéticos 1/1 e tooling Python com 15 testes; runtime ARM independente com 34/34 CTests; Android ARM64 e compilador estático NDSRecomp também passaram. Os testes de timing cobrem fetch, DATA, TCM, refill ARM9/ARM7, combinação ARM7 e early termination de multiply em ARM/Thumb. A CI não recebe ROM, segmentos nem bancos reais. [Execução da CI](https://github.com/CarlosCosta07-s/Oracis/actions/runs/44641462588).


## 11. Prova de integração ainda exigida

1. Ligar os quatro objetos corpo/dispatch às bibliotecas Oracis e confirmar a resolução dos 48 símbolos externos inventariados.
2. Completar a distinção load/store e timing de regiões dinâmicas/cache depois de ampliar a ABI e a cobertura de testes.
3. Auditar alvos indiretos e overlays desconhecidos; manter dispatch fechado para qualquer endereço sem banco válido.
4. Registrar os bancos no runtime, validar bytes/dependências contra GuestMemory e exercitar entradas sem iniciar rounds completos.
5. Completar SWIs e serviços BIOS/NitroSDK requeridos pelo boot, além de DMA/IPC/MMIO.
6. Expandir fetch/refill ARM9 para cache/MPU e regiões dinâmicas somente quando houver modelo e testes.
7. Implementar o provider arquitetural de LR IRQ ARM/Thumb e testar entrada/saída de exceção nas duas CPUs.
8. Só então habilitar rounds reais e avançar para primeiro quadro, áudio e saves.

Até a ligação dos bancos reais ao runtime e a execução verificável da sequência de inicialização, o Oracis ainda não inicializa Pokémon Platinum.
