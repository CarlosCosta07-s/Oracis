# Orquestrador dual-core

## Escopo

`NdsMachine::run_dual_core_round()` é o primeiro driver cooperativo que coordena as linhas de tempo ARM9 e ARM7. Ele não é ainda o scheduler completo de eventos do Nintendo DS: DMA, IPC, contenção do barramento, wait states, vídeo, áudio e prioridades de eventos ainda precisam ser integrados.

O round usa um alvo comum em ciclos de barramento. Para um avanço de `N` ciclos:

- ARM9 precisa alcançar `2 * N` ciclos de core;
- ARM7 precisa alcançar `N` ciclos de core.

A ordem inicial alterna a cada round concluído. Se um core já estiver à frente do watermark (por overshoot de instrução), ele não é executado novamente até que o outro o alcance.

## Execução de uma fatia

O host fornece um `NdsMachineSliceExecutor`. O callback recebe CPU, estado ARM e orçamento restante em ciclos de core. A máquina já selecionou o core e abriu uma fatia cooperativa.

O callback deve executar código nativo e retornar apenas em uma fronteira segura de instrução/dispatch, respeitando `runtime_should_yield()`. Quando um poll retorna verdadeiro, a ABI marca `runtime_unwinding()` até a pilha C gerada retornar ao host; `end_cpu_slice()` encerra essa janela antes da próxima fatia. Há um adaptador pronto para os bancos estáticos:

```cpp
NdsMachine::execute_registered_bank_slice
```

Esse adaptador encaminha `state->R[15]` para `runtime_dispatch()`. Um banco ausente ou inválido continua sendo um erro fatal e diagnosticado; não existe fallback para interpretador ARM.

Se o core estiver em HALT, ou se a execução devolver a fatia antes de alcançar o watermark, o restante é contabilizado por `report_idle_core_cycles()`. Essa operação preserva unidades de ciclos de core sem arredondar o meio ciclo do ARM9.

## Entrada IRQ

A máquina verifica interrupções no início e no fim de cada round. Quando a linha IRQ está ativa e `CPSR.I` está limpo, ela consulta `NdsMachineIrqContextProvider`.

O provider deve calcular, com base no estado real do executor, o LR arquitetural correto para o ponto exato em que a execução foi interrompida. A seleção da base vetorial pertence à máquina: ARM9 consulta o bit `V` de CP15 Control (`c1,c0,0`), e ARM7 usa a base baixa `0x00000000`. O parâmetro `vector_base` legado do callback é ignorado pela integração de rounds.

A máquina não calcula LR a partir de `R[15]`. Se um IRQ elegível existir sem provider, o round retorna `IrqContextUnavailable`; se o provider não conseguir fornecer o LR, retorna `IrqContextFailed`.

O IRQ é entregue somente fora de uma fatia ativa. O bit IF continua pendente até o software guest reconhecê-lo por write-one-to-clear.

## Estados e falhas

- `InvalidCycleBudget`: quantum zero ou overflow no alvo de tempo.
- `SliceExecutionFailed`: callback de execução falhou ou lançou exceção.
- `IrqContextUnavailable` / `IrqContextFailed`: não há contexto arquitetural seguro para a entrega IRQ.
- `ClockReportFailed`: o watermark não pôde ser atualizado de forma monotônica.

O round só alterna sua CPU inicial depois de terminar com sucesso.

## Limites atuais

O SDL host agora inicializa e encerra `NdsMachine` por meio da ponte C em `oracis/host/runtime_bridge.h`, e o input do teclado é encaminhado ao dispositivo vinculado. A ponte oferece `oracis_host_runtime_run_round()`, que só executa quando ambos os bancos estão registrados e aceita um provider C para o LR arquitetural; a base vetorial é selecionada pela máquina a partir do CP15 ARM9. O loop SDL ainda não chama essa função porque os bancos reais e o provider arquitetural não estão prontos. Os bancos reais ARM9/ARM7 ainda não foram emitidos pelo pipeline NDSRecomp. A implementação também ainda não ordena eventos DS concorrentes nem substitui a necessidade de integrar DMA, IPC, vídeo, áudio e tempos de acesso ao barramento.

Os testes de `nds_machine_test` exercitam a razão 2:1, yield antecipado, avanço ocioso durante HALT, despertar/entrega IRQ ARM7 e o adaptador `runtime_dispatch()` usando bancos estáticos sintéticos. Esses testes precisam passar na CI antes de este marco ser considerado validado.
