# NDSRecomp — mapa do contrato de bancos gerados

**Estado:** análise estática do gerador concluída em 19/11/2026; contrato Oracis ainda não implementado nem validado contra bancos emitidos.

**Revisão examinada:** `14b7e15023e9affc9dc375e4e611ac703f5c76ab`.

## 1. Emissão padrão escolhida

O caminho de emissão padrão de `recompiler/src/main.cpp` produz:

- um arquivo de corpos C para o banco, por exemplo `<bank>.c`;
- um header com os protótipos das funções geradas;
- um arquivo `<bank>_dispatch.c` com a tabela de endereços guest para ponteiros de função host.

Os bancos incluem `runtime_arm.h`. Os corpos gerados usam o estado guest `g_cpu`, as operações de memória `bus_read_*` / `bus_write_*` e helpers `runtime_*` / `arm_*`.

O comentário de emissão no upstream descreve esse formato como funções C sobre uma ABI de recompilação. O Oracis manterá o C gerado como entrada de compilação e implementará o runtime em C++20, exportando símbolos C compatíveis.

## 2. Interface de dispatch

O gerador emite entradas equivalentes a:

- endereço virtual guest;
- indicação ARM/Thumb;
- ponteiro para a função C recompilada;
- ponteiro opcional para metadados de validação estática.

A tabela é ordenada por endereço e o runtime faz lookup por endereço e estado ARM/Thumb. Os metadados opcionais descrevem o intervalo de bytes que pertence à função/banco e podem incluir intervalos dependentes. O Oracis precisa validar a identidade do banco antes de registrar suas entradas.

Para o primeiro protótipo, vamos manter o dispatch conservador:
1. registrar separadamente ARM9 e ARM7;
2. resolver o alvo pela CPU ativa e pelo bit ARM/Thumb;
3. validar a faixa de endereço e a identidade do banco;
4. falhar com CPU, PC, modo e banco quando não houver alvo;
5. não redirecionar para interpretador ARM/JIT.

## 3. Estado de CPU exigido

O contrato de tipos compartilhados do núcleo ARM declara um estado POD com os seguintes campos arquiteturais:

- `R[16]` — registradores visíveis, incluindo PC;
- `cpsr` — estado de flags, modo e ARM/Thumb;
- `banked_sp[6]`, `banked_lr[6]`, `banked_spsr[6]`;
- `r8_12_user[5]` e `r8_12_fiq[5]`.

As seis posições bancadas correspondem a User, FIQ, IRQ, Supervisor, Abort e Undefined. As constantes CPSR incluem N/Z/C/V/Q, I/F e T.

O Oracis implementará o layout necessário em um header próprio sob `runtime/abi/`, com tamanhos fixos e asserts de tamanho/offset. O header terá os nomes de tipo/campos que o código gerado precisa, mas não conterá a implementação do runtime upstream. A definição deverá preservar os avisos MIT do contrato de tipos caso o conteúdo do header seja reutilizado diretamente; preferimos reescrever a declaração a partir dos requisitos de ABI e documentar a compatibilidade.

## 4. Famílias de helper

A inspeção de `recompiler/armv4t/runtime_arm.h` identificou as seguintes famílias de contrato:

| Família | Exemplos | Responsabilidade Oracis |
|---|---|---|
| Memória | `bus_read_u8/u16/u32`, `bus_write_u8/u16/u32` | Barramento guest e MMIO |
| Ciclos | `runtime_tick`, `runtime_mem_cycles`, `runtime_mul_cycles` | Retirada de instrução, custos e limite de evento |
| Condições/flags | `arm_cond_passes`, `arm_shift_*`, `arm_set_nzcv_*` | Semântica ARM/Thumb |
| Transferência de controle | `runtime_dispatch`, `runtime_dispatch_with_exchange`, `runtime_call_*` | Chamadas, retornos, interworking e dispatch |
| Exceções | `runtime_swi`, `runtime_irq`, `runtime_msr_*`, `runtime_coproc_*` | SWI, IRQ, CPSR/SPSR e CP15 |
| Falhas | `runtime_unimplemented_op`, dispatch miss/bad entry | Diagnóstico fail-closed |
| Observabilidade | `runtime_trace_event`, `runtime_insn_*` | Logs, contadores e traces de depuração |

Essa tabela é um inventário inicial do cabeçalho, não uma promessa de que todos os helpers serão necessários ou implementados de uma vez. O conjunto definitivo será extraído do primeiro código C emitido, compilado e inspecionado por símbolos indefinidos.

## 5. Estratégia de compatibilidade sem importar o runtime

O caminho padrão do compilador inclui `runtime_arm.h` nos arquivos gerados. O Oracis criará seu próprio `runtime_arm.h` compatível com a superfície mínima exigida pelos bancos, disponibilizado pelo include path do build local.

As implementações serão código novo do Oracis:
- estado e bancos de registradores;
- funções C ABI com implementação em C++20;
- barramento, scheduler e dispatch próprios;
- diagnóstico fail-closed.

Não serão copiados `recompiler/armv4t/runtime_arm.cpp`, `runner/src/runtime_arm.cpp`, `runner/src/bus.cpp`, `runner/src/scheduler.cpp` ou shims do upstream.

O contrato de símbolo poderá manter nomes como `runtime_tick` e `runtime_dispatch`, pois são nomes emitidos pelo gerador; reutilizar nomes de ABI não significa reutilizar as implementações.

## 6. Opção de geração excluída

O parâmetro opcional `--flat-execution` altera os corpos gerados: eles passam a incluir `execution_region.h`, usam `NdsExecRegion` e chamam `nds_region_begin`, `nds_region_end`, `nds_region_needs_exit` e `runtime_region_single_step`.

Esse caminho depende de um contrato diferente e de serviços do runtime upstream. **O Oracis não usará `--flat-execution`** no primeiro pipeline. Usaremos a emissão padrão por função, com dispatch tradicional por banco.

Também não habilitaremos inicialmente:
- HLE profiling manifests;
- live overlay/DLL compilation;
- dynamic JIT ou interpretador tier de código RAM;
- runtime/oracle/frontend upstream.

## 7. Prova de integração exigida

A ABI não será considerada concluída com base apenas na leitura do header. Ela precisa passar por esta sequência:

1. compilar o executável `nds_recompile`;
2. emitir ARM9 e ARM7 da revisão USA Rev 1 pelo caminho padrão, sem `--flat-execution`;
3. compilar cada arquivo de banco C usando somente o header/runtime ABI Oracis;
4. listar símbolos indefinidos e resolver cada referência com uma implementação ou declaração documentada;
5. executar testes unitários de estado/flags, memória, dispatch e transferências ARM/Thumb;
6. provar que um endereço sem banco gera falha explícita, sem execução guest interpretada.

Até essa prova existir, esta página é um mapa técnico preliminar e a ABI Oracis permanece em implementação.
