# NDSRecomp — mapa do contrato de bancos gerados

**Estado:** contrato parcial implementado; testes C99/C++20 estão registrados, mas a execução da revisão atual não está confirmada. Bancos reais ainda não foram emitidos nem ligados.

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

Essa ponte cobre RAM/aliases explicitamente configurados. `NdsRetailRamMap` agora instala Main RAM retail 4MB compartilhada com espelhos em 0x02400000/0x02800000/0x02C00000, WRAM privada ARM7 64KB em 0x03800000 e Shared WRAM segundo os quatro modos de WRAMCNT. As regiões sobrepostas de Shared WRAM usam máscaras de CPU disjuntas e espelhos periódicos; no modo 0, a janela ARM7 espelha a WRAM privada de 64KB. `set_shared_wram_mode()` altera transacionalmente, após validar o mapa resultante, a visibilidade de sete janelas Shared WRAM pré-mapeadas. O dispositivo MMIO `NdsWramControl` já implementa leitura/escrita de 8 bits do `WRAMCNT` (`0x04000247`) pelo ARM9 e aciona essa troca; `WRAMSTAT` (`0x04000241`, ARM7, somente leitura) devolve os bits 0–1 do modo WRAMCNT; no ARM9 o mesmo endereço corresponde a VRAMCNT_B e não é capturado pelo dispositivo WRAM. `NdsInterruptControl` implementa IME/IE/IF de 32 bits (`0x04000208`, `0x04000210`, `0x04000214`) com bancos independentes por CPU, requests latcheados e IF write-one-to-clear. `try_enter_irq()` já testa IME/IE/IF e `CPSR.I`, depois entra na exceção ARM IRQ com LR e base vetorial fornecidos pelo chamador; a chamada automática nos limites de bloco ainda depende do scheduler. `NdsTimerControl` implementa os quatro timers por CPU, prescalers, cascata, reload/overflow e requests IF. `advance_bus_cycles(n)` avança as duas banks na mesma quantidade de ciclos do clock periférico. A integração desse tempo ao scheduler global continua pendente; não há conversão direta a partir de `runtime_tick`, pois os contadores independentes dos dois CPUs não podem ser somados sem sincronização. DMA, IPC, wait states e efeitos colaterais dos demais registradores ainda não existem. O codegen ARMv5TE alinha LDR/STR e aplica rotação/sign-extension fora do wrapper.

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
- `runtime_code_cycles()`, `runtime_mem_cycles()`, `runtime_mul_cycles()`, `arm7_refill_cycles()`, `arm9_refill_cycles()` e `arm7_cycle_combine()` têm símbolos definidos fail-closed: abortam com diagnóstico até que o modelo de timing de hardware correspondente seja implementado. Nenhum custo inventado é retornado.

## 8. Declarações e serviços ainda sem comportamento funcional

O header fornece o contrato C99 necessário para compilar os bancos emitidos, e os helpers de timing acima permitem a ligação fail-closed. Os seguintes grupos ainda precisam de implementações funcionais:

- custos de hardware por região, cache/TCM e acesso: os entry points existem, mas interrompem a execução;
- otimização de transferências: resolução/cache de link slots e admissão de folhas inline (os símbolos atuais fazem dispatch conservador e não usam cache);
- observabilidade de transferências: `runtime_live_transfer`;
- exceções geradas: `runtime_swi`, `runtime_irq`;
- coprocessador: `runtime_coproc_*` e serviços CP15;
- observabilidade: `runtime_trace_event` e consumidor do hook lento.

A compilação C99 de um banco não significa que ele possa executar. A primeira emissão sintética testa o formato ARM/Thumb e a compilação; os bancos reais ainda precisam ser extraídos localmente, ligados ao carregador e validados contra GuestMemory.
## 9. Opção de geração excluída

O parâmetro `--flat-execution` altera os corpos gerados para usar `NdsExecRegion`, `execution_region.h` e serviços do runtime upstream. **Oracis não usa essa opção.** Usaremos emissão padrão por função.

Também ficam fora da integração:
- runner, oracle e frontend upstream;
- código derivado de melonDS;
- JIT ou interpretador ARM como fallback;
- HLE manifests e live-overlay compilation até que a necessidade seja mapeada.

## 10. Testes registrados

O CMake registra vinte e dois testes C99/C++20 para o runtime e ABI, incluindo o mapa NDS retail, WRAMCNT/WRAMSTAT, IME/IE/IF e timers. A CI da revisão `a435c0f` passou nos quatro jobs e em 22/22 testes de runtime; os novos casos de WRAMSTAT da revisão atual aguardam execução. Os alvos registrados são: RAM guest; mapa NDS retail; WRAMCNT MMIO; controlador IME/IE/IF; TM0–TM3, prescalers, cascata e rota bus_*; ponte C bus_* e validação por GuestMemory; layouts ABI C99/C++20; bancos ARM e modos; CPSR/SPSR; exceções; flags; acesso ao User bank e CLZ; pilha de retorno guest; controle de fatias; registry de dispatch; runtime dispatch C++ e ABI dispatch C99; ponte ARM9/ARM7 C/C++; especialização estática ARM9 e ARM7.

A execução completa da CI na revisão `a435c0f` passou nos quatro jobs e em 22/22 testes de runtime. A revisão atual acrescenta WRAMSTAT ARM7 read-only e aguarda a nova execução dos testes. O CMake também inclui as bibliotecas de produto independentemente de `BUILD_TESTING`, mas o build completo com SDL3 e a ligação de bancos NDSRecomp continuam pendentes.

## 11. Prova de integração ainda exigida

1. Compilar o executável `nds_recompile` na revisão fixada.
2. Emitir os bancos C ARM9 e ARM7 da cópia USA Rev 1, sem `--flat-execution`.
3. Compilar os bancos usando somente o header e a biblioteca ABI Oracis.
4. Inspecionar símbolos indefinidos e implementar as funções necessárias.
5. Conectar `try_enter_irq` ao scheduler, sincronizar ARM9/ARM7 para alimentar `advance_bus_cycles` a partir do relógio global e completar os demais handlers MMIO (incluindo VRAMCNT_B).
6. Completar CP15/TCM, BIOS, VRAM/OAM, scheduler de dispositivos e timing por região.
7. Demonstrar que um endereço guest sem banco gera falha detalhada, sem execução interpretada.

Até a emissão, compilação e execução de bancos reais existir, o Oracis ainda não inicializa Pokémon Platinum.
