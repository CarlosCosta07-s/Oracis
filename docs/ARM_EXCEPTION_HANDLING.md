# Exceções ARM — entrada e retorno no runtime Oracis

**Estado:** entrada/retorno de exceção integrados ao runtime de bancos. `runtime_tick` consulta IRQ na fronteira de instrução; `runtime_irq` usa o PC seguinte fornecido pelo código gerado, deriva LR_irq = PC + 4 e despacha o vetor dentro da fatia ativa. A entrega no limite do round usa o mesmo callback de máquina; executores alternativos ainda podem fornecer um LR explícito.

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

A implementação atual não modela Reset, vectores de interrupção acelerados por periférico (vectored interrupt), prioridades, latência nem a decisão temporal de aceitar IRQ/FIQ. O suporte CP15 ainda é parcial: a máquina modela somente ARM9 CP15 Control (`MRC/MCR p15,0,Rd,c1,c0,0`) e o bit 13 `V`; operações CP15 restantes, incluindo MPU, caches e TCM, continuam sem implementação e falham explicitamente. O estado inicial da máquina segue o reset do Nintendo DS: ARM9 com `V=1` (vetores em `0xFFFF0000`) e ARM7 com vetores em `0x00000000`; depois o guest pode reprogramar ARM9 por CP15. A ARM7 não possui essa seleção por CP15 no modelo e permanece na base baixa. Referências: [ARM946E-S Technical Reference Manual — bit V/VINITHI](https://documentation-service.arm.com/static/5f032835cafe527e86f5b8ad) e [Nintendo DS Architecture — mapeamento dos vetores BIOS](https://www.copetti.org/writings/consoles/nintendo-ds/).

## 3. Vetores normais e altos

A primitiva de baixo nível `oracis_arm_cpu_enter_exception` continua recebendo `vector_base` explicitamente. Já `NdsMachine::run_dual_core_round()` seleciona a base antes da entrada: ARM9 lê o bit `V` (bit 13) de CP15 Control (`c1,c0,0`), usando `0x00000000` quando limpo e `0xFFFF0000` quando definido; ARM7 usa `0x00000000`. O provider externo permanece responsável pelo LR exato da fronteira, não pela seleção do vetor. Referência: [ARM946E-S Technical Reference Manual, CP15 Control Register](https://documentation-service.arm.com/static/5f032835cafe527e86f5b8ad).

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

1. Validar o PC seguinte emitido pelo NDSRecomp em ARM e Thumb contra traces de instrução reais; a ABI `runtime_irq(return_pc)` aplica LR_irq = `return_pc + 4`.
2. Completar prioridade/latência de IRQ e FIQ somente com eventos DS modelados; a linha IRQ atual respeita IME, IE, IF e CPSR.I.
3. Integrar exceções SWI/undefined emitidas pelo código gerado sem inventar um LR genérico.
4. Completar os registradores CP15/MPU/cache/TCM; ARM9 V e vetor baixo fixo do ARM7 já são usados na entrada IRQ.
5. Manter misses de dispatch de vetores fail-closed até que os bancos de BIOS necessários estejam disponíveis.

## Referência arquitetural

- [ARM Architecture Reference Manual, ARMv5, DDI 0100F-02](https://kib.kiev.ua/x86docs/ARM/ARMARMv5/DDI0100F-02.pdf)
- [ARM Architecture Reference Manual, ARMv5TEJ](https://vinnie.work/assets/files/armv5tej-architecture-reference-manual-31c7663537e37b4312bafa57b55c247f.pdf)

Esta implementação é uma camada de estado/transferência de controle. Ela não afirma que interrupções reais ou o boot do Pokémon Platinum já estejam funcionais.
