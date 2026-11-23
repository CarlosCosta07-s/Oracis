# Exceções ARM — entrada e retorno no runtime Oracis

**Estado verificado em 23/11/2026:** as primitivas de entrada/retorno de exceção estão integradas ao runtime. IRQ é consultada em `runtime_tick` na fronteira de instrução. Além disso, a máquina ARM9 entrega Data Abort por violações de permissão de dados da MPU e Prefetch Abort quando um destino de dispatch tem backing executável mas a MPU nega a busca. A CI [#45209899276](https://github.com/CarlosCosta07-s/Oracis/actions/runs/45209899276) aprovou os quatro jobs; runtime ARM e desktop passaram com 56/56 CTests cada.

## 1. Responsabilidade do chamador

`oracis_arm_cpu_enter_exception` recebe explicitamente:
- tipo da exceção;
- `return_address`: endereço de link arquitetural já calculado pelo executor/máquina;
- `vector_base`: base vetorial selecionada para aquela CPU/configuração.

A rotina acrescenta o offset do vetor, salva o CPSR no SPSR do modo de destino, troca os bancos visíveis, escreve o LR bancado, limpa CPSR.T, define o modo e as máscaras de interrupção e publica o endereço vetorial em R15. A primitiva não inventa o endereço de retorno.

## 2. Tabela de exceções

| Exceção | Modo de destino | Offset do vetor | I na entrada | F na entrada |
|---|---|---:|---|---|
| Undefined instruction | Undefined | `0x04` | 1 | Preservado |
| Software interrupt (SWI) | Supervisor | `0x08` | 1 | Preservado |
| Prefetch Abort | Abort | `0x0C` | 1 | Preservado |
| Data Abort | Abort | `0x10` | 1 | Preservado |
| IRQ | IRQ | `0x18` | 1 | Preservado |
| FIQ | FIQ | `0x1C` | 1 | 1 |

## 3. Data Abort ARM9 — MPU de dados

O wrapper C do barramento consulta a MPU ARM9 antes de encaminhar cada acesso de dados. Quando uma leitura ou escrita é negada:

1. a máquina salva o estado ARM9 no primeiro acesso recusado da instrução, junto do endereço, tamanho e direção;
2. leituras recusadas retornam um valor transitório zero e escritas recusadas não alteram o backing;
3. transferências posteriores da mesma instrução são suprimidas;
4. em `runtime_tick`, no limite de instrução, a máquina restaura o snapshot e entra em Abort mode;
5. o link usado é `LR_abt = endereço da instrução que falhou + 8`, conforme o modelo base-restored do ARM946E-S;
6. o scheduler marca unwind para o C gerado retornar ao host sem executar a instrução seguinte.

A regressão de máquina verifica o SPSR_abt, modo Abort, máscara IRQ, vetor selecionado pelo CP15 Control.V, restauração de registradores e ausência de escrita no GuestMemory. O caminho implementado cobre negações da MPU ARM9; não é uma implementação genérica de toda falha de bus.

## 4. Prefetch Abort ARM9 — MPU de instruções

Um miss de dispatch não é, por si só, Prefetch Abort. O dispatcher consulta o callback da máquina somente quando uma entrada estática não foi resolvida. O callback normaliza o bit de estado do destino, escolhe fetch de 2 bytes em Thumb ou 4 bytes em ARM, exige um mapeamento que GuestMemory permita executar e então testa a permissão de busca da MPU ARM9.

- Se a MPU negar a busca, a máquina entra em Prefetch Abort no vetor `base + 0x0C), salva o CPSR anterior e usa `LR_abt = endereço da instrução abortada + 4`.
- Se não existir backing executável, o endereço for não mapeado/inacessível ou a MPU permitir a busca, o callback não cria uma exceção. O miss continua no diagnóstico fail-closed do dispatcher.
- ARM7 retorna “não tratado” neste callback; sua semântica de abort não está implementada.

A entrada está conectada a `runtime_dispatch`, aos link slots `runtime_link_call/branch/fallthrough` e a `runtime_dispatch_miss`. Os testes cobrem dispatch ARM direto, Thumb por link slot e BX/interworking para Thumb, além de verificar que endereço não mapeado e destino dinâmico permitido em modo privilegiado não são promovidos a Prefetch Abort.

## 5. Vetores normais e altos

A primitiva de baixo nível recebe `vector_base` explicitamente. A máquina seleciona a base do ARM9 pelo bit V (bit 13) de CP15 Control (`c1,c0,0`): `0x00000000` quando limpo e `0xFFFF0000` quando definido. ARM7 usa `0x00000000`. Assim, os offsets de Prefetch/Data Abort resultam em `0xFFFF000C/0xFFFF0010` quando V está ativo.

## 6. Limites atuais

- Os vetores da BIOS ARM9 não têm uma imagem/handler retail carregados no host; os testes verificam a entrada arquitetural, não a execução de um handler de BIOS.
- Falhas de bus externas, endereços MMIO não implementados, acessos unmapped e falhas internas do host continuam em caminhos fail-closed de diagnóstico; não são convertidas indiscriminadamente em exceções guest.
- Abort delivery ARM7 ainda não está implementado.
- CP15 ARM9 é parcial: c0 ID/cache type/TCM size; c1 Control, V, MPU enable e controles TCM; c2/c3 retêm atributos de cacheabilidade/bufferabilidade; c5/c6 modelam permissões e regiões MPU; operações de manutenção c7 são no-op ordenado porque não existem caches nem write buffer físicos; c9 modela TCM. O modelo de cache real permanece pendente.
- Reset, FIQ por hardware, prioridades/latências completas de IRQ/FIQ e SWI/Undefined emitidos pelos bancos ainda exigem integração própria.

## 7. Retorno de exceção

`oracis_arm_cpu_return_from_exception` recebe o endereço final para R15; o chamador aplica a correção de cada exceção e da instrução de retorno. A rotina lê o SPSR do modo atual, valida o modo e alinhamento (4 bytes para ARM, 2 bytes para Thumb), restaura CPSR/bancos e publica R15. Retorno em User/System, SPSR inválido ou endereço desalinhado é recusado sem modificar o estado.

## 8. Interfaces e referências

- `oracis_arm_cpu_enter_exception`
- `oracis_arm_cpu_return_from_exception`
- `oracis_arm_cpu_restore_cpsr_from_spsr`
- `oracis_arm_cpu_switch_mode`
- `runtime_tick`, `runtime_check_data_abort` e a ponte de Prefetch Abort

Código: `src/runtime/arm_exception_ops.cpp`, `src/runtime/scheduler_core.cpp`, `src/machine/nds_machine.cpp` e `src/runtime/static_dispatch_abi.cpp`. Testes: `tests/abi/arm_exception_ops_test.cpp` e `tests/machine/nds_machine_test.cpp`.

Referências arquiteturais: [ARM Architecture Reference Manual, ARMv5, DDI 0100F-02](https://kib.kiev.ua/x86docs/ARM/ARMARMv5/DDI0100F-02.pdf) e [ARM946E-S Technical Reference Manual](https://documentation-service.arm.com/static/5e8e3ee588295d1e18d3aa82). O primeiro especifica `LR_abt = PC+4` para Prefetch Abort e `PC+8` para Data Abort.