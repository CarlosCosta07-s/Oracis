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
| `runtime_unimplemented_op` | Diagnóstico e encerramento fail-closed |

O código gerado pelo perfil ARMv5TE atual expande os shifts de operand2 inline, cobrindo as formas imediatas e por registrador e os casos especiais de LSR/ASR #0 e ROR #0 (RRX). Por isso, não adicionamos uma segunda implementação de shifter ao runtime. A compatibilidade com bancos antigos que chamem símbolos `arm_shift_*` ainda não é uma meta do primeiro pipeline.

## 5. Declarações ainda sem implementação

O header também declara os serviços que o código gerado precisa, mas não fornece stubs vazios. Os seguintes grupos ainda precisam de implementações reais:

- memória e MMIO: `bus_read_*`, `bus_write_*`;
- ciclos: `runtime_tick`, `runtime_should_yield`, `runtime_code_cycles`, `runtime_mem_cycles`, `runtime_mul_cycles`;
- dispatch: `runtime_dispatch*`, `runtime_link_*`, misses e validação de entradas;
- transferências/retornos: `runtime_call_*`, `runtime_live_transfer`;
- exceções geradas: `runtime_swi`, `runtime_irq`;
- coprocessador: `runtime_coproc_*` e serviços CP15;
- observabilidade: `runtime_trace_event`, `runtime_insn_slow`.

Um banco pode incluir o header e compilar sintaticamente, mas continuará sem poder ligar enquanto todos os símbolos efetivamente referenciados não tiverem uma implementação. Não fornecemos no-op stubs para simular progresso.

## 6. Opção de geração excluída

O parâmetro `--flat-execution` altera os corpos gerados para usar `NdsExecRegion`, `execution_region.h` e serviços do runtime upstream. **Oracis não usa essa opção.** Usaremos emissão padrão por função.

Também ficam fora da integração:
- runner, oracle e frontend upstream;
- código derivado de melonDS;
- JIT ou interpretador ARM como fallback;
- HLE manifests e live-overlay compilation até que a necessidade seja mapeada.

## 7. Memória guest versus barramento

A biblioteca independente `oracis_memory` fornece armazenamento guest configurável, aliases compartilhados, permissões e transferências little-endian. Ela foi separada dos wrappers C da ABI: não define ainda o mapa DS da revisão, MMIO, DMA, custos de ciclos, rotação ARM nem os símbolos `bus_read_*`/`bus_write_*`.

Consulte [GUEST_MEMORY.md](GUEST_MEMORY.md) para o contrato e os limites atuais.

## 8. Testes feitos

O harness runtime-only local, reconstruído a partir dos arquivos versionados, compilou com Clang/CMake/Ninja e passou em onze testes:
1. RAM guest: little-endian, aliases, permissões, limites e overflow;
2. layout ABI C99;
3. layout ABI C++20;
4. bancos e troca de modos ARM;
5. CPSR/SPSR e privilégios;
6. entrada e retorno de exceções;
7. condições e flags NZCV;
8. ponte C++ para o estado ativo;
9. ponte C99 para o estado ativo;
10. especialização estática ARM9 (`NDS_STATIC_CPU=0`);
11. especialização estática ARM7 (`NDS_STATIC_CPU=1`).

Uma configuração separada com `BUILD_TESTING=OFF` também compilou `liboracis_arm_state.a` e `liboracis_memory.a`. Isso valida as duas bibliotecas no harness local, não o build completo com SDL3 nem os bancos NDSRecomp.

## 9. Prova de integração ainda exigida

1. Compilar o executável `nds_recompile` na revisão fixada.
2. Emitir os bancos C ARM9 e ARM7 da cópia USA Rev 1, sem `--flat-execution`.
3. Compilar os bancos usando somente o header e a biblioteca ABI Oracis.
4. Inspecionar símbolos indefinidos e implementar as funções necessárias.
5. Implementar e testar barramento, dispatch, scheduler e serviços de exceção/timing.
6. Demonstrar que um endereço guest sem banco gera falha detalhada, sem execução interpretada.

Até a emissão, compilação e execução de bancos reais existir, o Oracis ainda não inicializa Pokémon Platinum.
