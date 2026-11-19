# Fase 1 — Integração do compilador estático

**Estado:** script de bootstrap publicado; compilação e primeiro banco ainda não validados.  
**Revisão-alvo:** Pokémon Platinum USA Rev 1.  
**Compilador:** NDSRecomp, commit `14b7e15023e9affc9dc375e4e611ac703f5c76ab`.

## Entrega desta etapa

- `tools/ndsrecomp/build_recompiler.py`: checkout externo fixado, inicialização exclusiva do submódulo MIT necessário ao compilador e build somente de `nds_recompile`.
- `CMakeLists.txt`: linguagens C e C++ habilitadas, com C99 e C++20.
- `.gitignore`: checkout, extrações e bancos ficam em `.oracis/` ou `generated/`, fora do Git.
- `docs/NDSRECOMP_INTEGRATION.md`: separação de licenças, componentes permitidos e runtime próprio.

O checkout externo contém mais pastas do upstream, mas Oracis não configura nem compila `runner`, `oracle`, frontend ou componentes derivados de melonDS. O checkout não é submódulo e não é distribuído junto do Oracis.

## Preparar o recompilador

Requisitos: Python 3, Git, CMake 3.20+, Ninja e compilador C++20.

```sh
python tools/ndsrecomp/build_recompiler.py
```

Por padrão, o código upstream ficará em `../ndsrecomp` e a compilação em `.oracis/ndsrecomp-build`. O script não redefine checkouts existentes em revisões diferentes; use `--source` para escolher outro diretório vazio.

## Host SDL3

O host SDL3 e as duas áreas lógicas de 256 × 192 permanecem como frontend experimental. Ainda não há runtime ARM/DS integrado. O host não deve ser tratado como evidência de que o jogo já está sendo recompilado ou executado.

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
4. Os segmentos ARM9/ARM7 são extraídos localmente com validação de offset, tamanho e SHA-1.
5. O recompilador emite pelo menos um banco de teste por CPU.
6. O código emitido compila com o contrato C ABI ainda em desenvolvimento.

A compilação do próprio recompilador, isoladamente, não conclui a Fase 1. É necessário emitir e compilar bancos reais.
