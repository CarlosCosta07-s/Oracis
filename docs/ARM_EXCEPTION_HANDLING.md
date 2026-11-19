# Exceções ARM — entrada e retorno no runtime Oracis

**Estado:** primitivas implementadas e testadas isoladamente; ainda não ligadas ao scheduler, à linha de interrupções ou à execução dos bancos.

## 1. Responsabilidade do chamador

A entrada de uma exceção depende do ponto arquitetural exato em que a CPU a aceita. O valor de LR para SWI, IRQ/FIQ e abort não deve ser inferido por uma fórmula única a partir de R15.

Por isso, `oracis_arm_cpu_enter_exception` recebe explicitamente:
- o tipo da exceção;
- `return_address`: o valor de link arquitetural já calculado pelo executor/scheduler;
- `vector_base`: a base vetorial selecionada para aquela CPU/configuração.

A rotina acrescenta o offset da exceção à base. O ARMv5 define a gravação do CPSR no SPSR do modo de exceção, o uso do LR bancado, a mudança de modo, o estado ARM e os bits de máscara de interrupção na entrada. O runtime preserva esses limites sem escolher por conta própria o ponto de aceitação.

## 2. Tabela de exceções

| Exceção | Modo de destino | Offset do vetor | I na entrada | F na entrada |
|---|---|---:|---|---|
| Undefined instruction | Undefined | `0x04` | 1 | Preservado |
| Software interrupt (SWI) | Supervisor | `0x08` | 1 | Preservado |
| Prefetch abort | Abort | `0x0C` | 1 | Preservado |
| Data abort | Abort | `0x10` | 1 | Preservado |
| IRQ | IRQ | `0x18` | 1 | Preservado |
| FIQ | FIQ | `0x1C` | 1 | 1 |

A rotina faz:
1. validar o tipo de exceção, modo atual e soma da base vetorial;
2. capturar o CPSR anterior;
3. trocar os bancos visíveis para o modo de exceção;
4. gravar o CPSR anterior no SPSR daquele modo;
5. gravar o link explícito em R14 bancado;
6. limpar CPSR.T, definir o modo de destino e aplicar I/F;
7. colocar em R15 o endereço do vetor.

A implementação atual não modela Reset, vetores de interrupção acelerados por periférico (vectored interrupt), estado de CP15, prioridades, latência nem a decisão temporal de aceitar IRQ/FIQ. Essas responsabilidades ficam para o scheduler e para o controlador de interrupções.

## 3. Vetores normais e altos

O chamador fornece `vector_base`. Isso permite usar a tabela normal `0x00000000` ou uma base alta, por exemplo `0xFFFF0000`, quando a configuração/CPU a suportar. O helper não lê CP15 nem decide se o remapeamento alto está habilitado.

Os testes cobrem:
- todos os seis tipos implementados contra a base alta;
- IRQ contra a base zero;
- overflow de `vector_base + offset`;
- tipo de exceção inválido.

## 4. Retorno de exceção

`oracis_arm_cpu_return_from_exception` também recebe o endereço final para R15. O chamador deve aplicar a correção exigida pelo tipo da exceção e pela instrução de retorno antes da chamada.

A rotina:
1. lê o SPSR do modo atual;
2. valida o modo salvo;
3. valida alinhamento de R15 (4 bytes para ARM, 2 bytes para Thumb);
4. restaura CPSR e os bancos de registradores por meio de `oracis_arm_cpu_restore_cpsr_from_spsr`;
5. publica o PC de retorno.

Ela recusa retorno em User/System (sem SPSR), SPSR com modo inválido e endereço desalinhado, sem modificar o estado nesses casos.

## 5. Interfaces implementadas

- `oracis_arm_cpu_enter_exception`
- `oracis_arm_cpu_return_from_exception`
- `oracis_arm_cpu_restore_cpsr_from_spsr`
- `oracis_arm_cpu_switch_mode`

Código: `src/runtime/arm_exception_ops.cpp`. Teste: `tests/abi/arm_exception_ops_test.cpp`.

## 6. Próxima integração

1. O scheduler deve gerar o return link correto para cada fronteira de instrução.
2. O controlador IRQ/FIQ deve consultar I/F e a prioridade/estado de interrupção.
3. Os caminhos SWI e undefined devem chamar a mesma primitiva, mas com links derivados da instrução atual.
4. CP15 deve selecionar a base vetorial da ARM9; a configuração de vetor da ARM7 deve permanecer independente.
5. O código de exceção recompilado deve ser resolvido pelo dispatch estático; miss continua sendo falha explícita.

## Referência arquitetural

- [ARM Architecture Reference Manual, ARMv5, DDI 0100F-02](https://kib.kiev.ua/x86docs/ARM/ARMARMv5/DDI0100F-02.pdf)
- [ARM Architecture Reference Manual, ARMv5TEJ](https://vinnie.work/assets/files/armv5tej-architecture-reference-manual-31c7663537e37b4312bafa57b55c247f.pdf)

Esta implementação é uma camada de estado/transferência de controle. Ela não afirma que interrupções reais ou o boot do Pokémon Platinum já estejam funcionais.
