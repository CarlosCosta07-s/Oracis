# Fase 1 — Integração do compilador estático

**Estado:** bootstrap e extrator publicados; extração local USA Rev 1 verificada; runtime ARM e uma ponte ABI parcial implementados/testados. Build do NDSRecomp e emissão de bancos reais ainda pendentes.  
**Revisão-alvo:** Pokémon Platinum USA Rev 1.  
**Compilador:** NDSRecomp, commit `14b7e15023e9affc9dc375e4e611ac703f5c76ab`.

## Entrega desta etapa

- `tools/ndsrecomp/build_recompiler.py`: sparse checkout externo fixado contendo somente `recompiler/`, `cmake/` e `external/arm-recomp-core/`; inicializa o submódulo MIT necessário e compila `nds_recompile` e os testes standalone do núcleo.
- `CMakeLists.txt`: C99 e C++20; `oracis_arm_state` e `oracis_memory` fazem parte do build de produto mesmo com `BUILD_TESTING=OFF` e são ligados ao host SDL3.
- `include/oracis/recomp/arm_cpu_state.h`: layout POD de estado ARM, com asserts de tamanho/offset.
- `src/runtime/arm_cpu_state_ops.cpp`: mapeamento dos modos ARM e troca dos registradores bancados.
- `src/runtime/arm_psr_ops.cpp`: operações CPSR/SPSR, máscaras de campos e privilégio User.
- `src/runtime/arm_exception_ops.cpp`: entrada/retorno de Undefined, SWI, prefetch/data abort, IRQ e FIQ; o chamador fornece o link e a base vetorial.
- `src/runtime/arm_flags.cpp`: condições ARM e atualização NZ/NZC/NZCV.
- `src/runtime/recomp_abi.cpp`: acesso aos registradores User e `runtime_clz` para o codegen.
- `include/oracis/recomp/runtime_scheduler.h` e `src/runtime/scheduler_core.cpp`: contadores de ciclos e controle de fatias/yield/unwind cooperativo por CPU.
- `include/oracis/recomp/runtime_arm.h` e `src/runtime/recomp_abi.cpp`: ponte parcial para o C gerado, estado ativo ARM9/ARM7, wrappers de flags/PSR/exceção e falhas fail-closed.
- `include/oracis/memory/guest_memory.hpp` e `src/memory/guest_memory.cpp`: regiões guest configuráveis, aliases, mirrors periódicos e visibilidade por CPU.
- `include/oracis/memory/nds_retail_map.hpp` e `src/memory/nds_retail_map.cpp`: Main RAM 4MB/mirrors, WRAM ARM7 privada e modos WRAMCNT 0–3.
- `src/memory/bus_abi.cpp`: wrappers C bus_read_*/bus_write_* ligados ao estado ARM9/ARM7 ativo, ao GuestMemory e aos dispositivos MMIO WRAMCNT e IME/IE/IF.
- `include/oracis/memory/nds_wram_control.hpp` e `src/memory/nds_wram_control.cpp`: WRAMCNT ARM9 de 8 bits, remapeamento Shared WRAM no barramento; WRAMSTAT permanece pendente.
- `include/oracis/memory/nds_interrupt_control.hpp` e `src/memory/nds_interrupt_control.cpp`: IME/IE/IF independentes por CPU, IF W1C e `try_enter_irq()` com máscara CPSR.I e LR/vetor explícitos; chamada automática pelo scheduler pendente.
- `include/oracis/memory/nds_timer_control.hpp` e `src/memory/nds_timer_control.cpp`: Timer 0–3 de 16 bits por CPU, reload/start, prescalers, cascata e IRQ; o observador de `runtime_tick` alimenta os timers via conversão ARM9/ARM7.
- `tests/abi/` e `tests/memory/`: vinte e dois alvos CTest registrados, incluindo ABI, dispatch, User bank, GuestMemory, WRAMCNT, IME/IE/IF, entrada IRQ e timers. A CI da revisão `480651a` passou nos quatro jobs e em 21/21 testes; o novo teste de temporizadores aguarda execução.

O runtime usa um ponteiro global para o estado ativo. A futura execução será serial/cooperativa nesta primeira versão: o scheduler deve selecionar ARM9 ou ARM7 antes de chamar um banco. Execução concorrente dos dois bancos em threads ainda não é suportada. GuestMemory instala o mapa de RAM do NDS retail; WRAMCNT=0 também espelha a WRAM privada ARM7 na janela Shared WRAM ARM7. `set_shared_wram_mode()` altera transacionalmente as máscaras de CPU das sete janelas Shared WRAM pré-mapeadas, depois de validar o mapa completo, sem alocação/desalocação nem perda dos bancos físicos; o MMIO WRAMCNT ARM9 de 8 bits já chama essa API. IME/IE/IF possuem controlador independente por CPU; `try_enter_irq()` faz entrada condicionada a CPSR.I com LR/vetor explícitos. Timer 0–3 por CPU implementa prescalers, cascata e requests IF. WRAMSTAT, demais MMIO, DMA, CP15/TCM, BIOS, VRAM/OAM e timing por região continuam pendentes. Os wrappers bus_* operam apenas sobre regiões RAM explicitamente mapeadas e falham quando o endereço não existe.

## Preparar o recompilador

Requisitos do bootstrap: Python 3.10+, Git, CMake 3.20+, Ninja e compilador C++20.

```sh
python tools/ndsrecomp/build_recompiler.py
```

Por padrão, o checkout esparso ficará em `../ndsrecomp` e a compilação em `.oracis/ndsrecomp-build`. O script recusa checkouts existentes que não estejam limpos, fixados e com os sparse paths aprovados.

## Extrair ARM9 e ARM7

```sh
python tools/ndsrecomp/extract_nds_programs.py "/caminho/Pokemon - Platinum Version (USA) (Rev 1).nds"
```

A validação local da cópia conferiu SHA-1, offsets, limites e hashes dos dois segmentos. O script versionado ainda aguarda sua primeira execução integral após publicação. Os binários extraídos ficam fora do Git.

## Limites atuais

- O mapa NDS retail cobre Main RAM e WRAM. O barramento reconhece WRAMCNT, IME/IE/IF e TM0–TM3; timers vinculados avançam por `runtime_tick`. Ainda não há WRAMSTAT, ordenação geral de eventos, entrega automática de IRQ, demais MMIO, CP15/TCM, BIOS, VRAM/OAM ou timing por região.
- O registry/runtime_dispatch e a pilha de retorno guest estão implementados para tabelas registradas. Os link slots permanecem sem cache/resolução direta; o controle de fatias existe, mas o scheduler DS com eventos, timers e sincronização ainda não existe.
- A ordenação geral de eventos e a entrega IRQ ao scheduler, DMA, IPC, SWI/CP15 e serviços NitroSDK continuam pendentes.
- O código gerado ainda não foi emitido nem ligado.
- O host SDL3 continua um frontend experimental sem runtime de jogo conectado.
- Nenhuma ROM ou dado derivado entra no Git, CI ou releases.

## Critérios para concluir a Fase 1

1. Bootstrap executa em ambiente limpo e confirma a revisão fixada.
2. `nds_recompile` e os testes standalone upstream passam; nenhum runner/oracle é construído.
3. Os segmentos ARM9/ARM7 são validados e configurados.
4. O recompilador emite os bancos padrão para ARM9 e ARM7.
5. Os bancos compilam e ligam usando somente a ABI/runtime Oracis.
6. Os misses de cobertura são conhecidos e fail-closed.

A compilação do próprio recompilador isoladamente não conclui a Fase 1.
