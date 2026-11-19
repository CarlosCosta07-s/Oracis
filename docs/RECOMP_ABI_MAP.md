# NDSRecomp — mapa do contrato de bancos gerados

**Estado:** contrato parcial implementado e testado em C99/C++20. Bancos reais ainda não foram emitidos nem ligados.

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

Essa ponte cobre RAM/aliases explicitamente configurados. `NdsRetailRamMap` agora instala Main RAM retail 4MB compartilhada com espelhos em 0x02400000/0x02800000/0x02C00000, WRAM privada ARM7 64KB em 0x03800000 e Shared WRAM segundo os quatro modos de WRAMCNT. As regiões sobrepostas de Shared WRAM usam máscaras de CPU disjuntas e espelhos periódicos; no modo 0, a janela ARM7 espelha a WRAM privada de 64KB. MMIO, DMA, IPC, wait states e efeitos colaterais dos registradores ainda não existem. O codegen ARMv5TE alinha LDR/STR e aplica rotação/sign-extension fora do wrapper.

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
- \`runtime_should_yield()\` e \`runtime_slice_yield()\` verificam pedido de yield ou vencimento do orçamento da fatia.
- \`runtime_unwinding()\` consulta o estado de unwind explícito da CPU ativa.
- \`oracis_scheduler_begin_slice/end_slice\`, \`request_yield\`, \`set_unwinding\`, \`cycle_count\` e \`reset\` permitem ao host controlar cada CPU.

Os contadores e deadlines ARM9/ARM7 são independentes. O runtime não executa automaticamente a outra CPU: o host ainda precisa selecionar a CPU e iniciar a próxima fatia.

**Limite desta etapa:** isto não é o scheduler Nintendo DS completo. Não processa timers, DMA, IRQ, IPC, PPU, wait states, custos por região de memória nem eventos periféricos. \`runtime_code_cycles\`, \`runtime_mem_cycles\` e \`runtime_mul_cycles\` ainda não possuem modelos de hardware e continuam pendentes.

## 8. Declarações ainda sem implementação

O header também declara os serviços que o código gerado precisa, mas não fornece stubs vazios. Os seguintes grupos ainda precisam de implementações reais:

- custos de hardware: `runtime_code_cycles`, `runtime_mem_cycles`, `runtime_mul_cycles`;
- otimização de transferências: resolução/cache de link slots e admissão de folhas inline (os símbolos atuais fazem dispatch conservador e não usam cache);
- observabilidade de transferências: `runtime_live_transfer`;
- exceções geradas: `runtime_swi`, `runtime_irq`;
- coprocessador: `runtime_coproc_*` e serviços CP15;
- observabilidade: `runtime_trace_event`, `runtime_insn_slow`.

Um banco pode incluir o header e compilar sintaticamente, mas continuará sem poder ligar enquanto todos os símbolos efetivamente referenciados não tiverem uma implementação. Não fornecemos no-op stubs para simular progresso.

## 9. Opção de geração excluída

O parâmetro `--flat-execution` altera os corpos gerados para usar `NdsExecRegion`, `execution_region.h` e serviços do runtime upstream. **Oracis não usa essa opção.** Usaremos emissão padrão por função.

Também ficam fora da integração:
- runner, oracle e frontend upstream;
- código derivado de melonDS;
- JIT ou interpretador ARM como fallback;
- HLE manifests e live-overlay compilation até que a necessidade seja mapeada.

## 10. Testes registrados

O CMake registra dezenove testes C99/C++20 para o runtime e ABI, incluindo o mapa NDS retail e os quatro modos de Shared WRAM. A execução da versão atual ainda não foi confirmada. Os alvos registrados são: RAM guest; mapa NDS retail (Main RAM/mirrors, WRAM privada e Shared WRAM modes); ponte C bus_* e validação por GuestMemory; layouts ABI C99/C++20; bancos ARM e modos; CPSR/SPSR; exceções; flags; acesso ao User bank e CLZ; pilha de retorno guest; controle de fatias; registry de dispatch; runtime dispatch C++ e ABI dispatch C99; ponte ARM9/ARM7 C/C++; especialização estática ARM9 e ARM7.

Não há status check atual confirmado para o último commit; portanto, não declarar os testes como aprovados. O CMake também inclui as bibliotecas de produto independentemente de `BUILD_TESTING`, mas o build completo com SDL3 e a ligação de bancos NDSRecomp continuam pendentes.

## 11. Prova de integração ainda exigida

1. Compilar o executável `nds_recompile` na revisão fixada.
2. Emitir os bancos C ARM9 e ARM7 da cópia USA Rev 1, sem `--flat-execution`.
3. Compilar os bancos usando somente o header e a biblioteca ABI Oracis.
4. Inspecionar símbolos indefinidos e implementar as funções necessárias.
5. Implementar MMIO para WRAMCNT/WRAMSTAT e reconfiguração dinâmica das janelas Shared WRAM.
6. Completar MMIO/CP15/TCM, BIOS, VRAM/OAM, scheduler de dispositivos e timing por região.
6. Demonstrar que um endereço guest sem banco gera falha detalhada, sem execução interpretada.

Até a emissão, compilação e execução de bancos reais existir, o Oracis ainda não inicializa Pokémon Platinum.
