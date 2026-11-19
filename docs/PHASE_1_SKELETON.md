# Fase 1 — Integração do compilador estático

**Estado:** bootstrap e extrator publicados; extração ARM9/ARM7 da cópia local USA Rev 1 validada. Build do recompilador e emissão de bancos ainda pendentes.  
**Revisão-alvo:** Pokémon Platinum USA Rev 1.  
**Compilador:** NDSRecomp, commit `14b7e15023e9affc9dc375e4e611ac703f5c76ab`.

## Entrega desta etapa

- `tools/ndsrecomp/build_recompiler.py`: sparse checkout externo fixado contendo somente `recompiler/`, `cmake/` e `external/arm-recomp-core/`; inicializa o submódulo MIT necessário e compila apenas `nds_recompile`.
- `CMakeLists.txt`: linguagens C e C++ habilitadas, com C99 e C++20.
- `.gitignore`: checkout, extrações e bancos ficam em `.oracis/` ou `generated/`, fora do Git.
- `tools/ndsrecomp/extract_nds_programs.py`: valida a revisão USA Rev 1, extrai ARM9/ARM7, calcula hashes e gera TOMLs locais.
- `docs/NDSRECOMP_INTEGRATION.md`: separação de licenças, componentes permitidos, hashes dos segmentos e runtime próprio.
- `include/oracis/recomp/arm_cpu_state.h`: layout POD de registradores/flags compartilhável entre C99 e C++20.
- `tests/abi/`: testes C e C++ do layout do estado ARM, registrados no CTest.

O checkout externo não materializa `runner/` nem `oracle/`: o sparse checkout limita a árvore aos diretórios necessários para o compilador e seu núcleo ARM MIT. O checkout não é submódulo e não é distribuído junto do Oracis.

## Preparar o recompilador

Requisitos do bootstrap: Python 3.10+, Git, CMake 3.20+, Ninja e compilador C++20. Para extração local, Python 3.10+.

```sh
python tools/ndsrecomp/build_recompiler.py
```

Por padrão, o checkout esparso ficará em `../ndsrecomp` e a compilação em `.oracis/ndsrecomp-build`. O script não redefine checkouts existentes; se o diretório já existir e não estiver limpo, fixado e com os sparse paths aprovados, use `--source` para indicar outro diretório vazio.

## Extrair ARM9 e ARM7

```sh
python tools/ndsrecomp/extract_nds_programs.py "/caminho/Pokemon - Platinum Version (USA) (Rev 1).nds"
```

A execução local conferiu o SHA-1 da ROM e gerou configurações TOML válidas. Os metadados e hashes dos segmentos estão em [NDSRECOMP_INTEGRATION.md](NDSRECOMP_INTEGRATION.md). Os binários extraídos permanecem em `.oracis/local/`, fora do Git.

## Host SDL3

O host SDL3 e as duas áreas lógicas de 256 × 192 permanecem como frontend experimental. O layout de armazenamento ARM passou nos testes C99/C++20, mas ainda não há ABI de funções, dispatch, scheduler ou runtime ARM/DS integrado. O host não deve ser tratado como evidência de que o jogo já está sendo recompilado ou executado.

## Exclusões

- Não incorporar o runtime upstream.
- Não incorporar runner, oracle, frontend ou componentes derivados de melonDS.
- Não importar interpretador ARM ou JIT como fallback de execução.
- Não versionar ROM, BIOS, firmware, segmentos ARM, TOMLs específicos da cópia, bancos gerados ou saves.
- Não reutilizar código de pret/pokeplatinum enquanto a licença/autorização não estiver esclarecida.

## Critérios para concluir a Fase 1

1. Bootstrap executa em ambiente limpo e confirma a revisão fixada.
2. O alvo `nds_recompile` compila; nenhum alvo runner/oracle é construído.
3. Testes do decodificador ARMv5TE e do ciclo do núcleo ARM passam.
4. Os segmentos ARM9/ARM7 foram extraídos localmente e passaram na validação de offset, tamanho e SHA-1 para a cópia USA Rev 1.
5. O recompilador emite pelo menos um banco de teste por CPU.
6. O código emitido compila com o contrato C ABI ainda em desenvolvimento.

A compilação do próprio recompilador, isoladamente, não conclui a Fase 1. É necessário emitir e compilar bancos reais.
