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
- `include/oracis/recomp/runtime_arm.h` e `src/runtime/recomp_abi.cpp`: ponte parcial para o C gerado, estado ativo ARM9/ARM7, wrappers de flags/PSR/exceção e falhas fail-closed.
- `include/oracis/memory/guest_memory.hpp` e `src/memory/guest_memory.cpp`: regiões guest configuráveis, aliases e acessos little-endian com permissões e limites.
- `src/memory/bus_abi.cpp`: wrappers C bus_read_*/bus_write_* ligados ao estado ARM9/ARM7 ativo e ao backing GuestMemory.
- `tests/abi/` e `tests/memory/`: doze testes C99/C++20, incluindo ABI, especialização estática, mapa de memória e wrappers do barramento.

O runtime usa um ponteiro global para o estado ativo. A futura execução será serial/cooperativa nesta primeira versão: o scheduler deve selecionar ARM9 ou ARM7 antes de chamar um banco. Execução concorrente dos dois bancos em threads ainda não é suportada. GuestMemory ainda não é o mapa físico do DS e não implementa MMIO, DMA, timing ou alinhamento ARM. Os wrappers bus_* operam apenas sobre regiões RAM explicitamente mapeadas e falham quando o endereço não existe.

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

- GuestMemory e os wrappers C bus_* oferecem RAM/aliases genéricos com identidade de bytes; ainda não são o mapa DS e não implementam MMIO.
- O registry/runtime_dispatch e a pilha de retorno guest estão implementados para tabelas registradas. Os link slots permanecem sem cache/resolução direta; não há carregador de bancos reais nem scheduler.
- Ciclos, timers, DMA, IPC, IRQ/SWI/CP15 e serviços NitroSDK continuam pendentes.
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
