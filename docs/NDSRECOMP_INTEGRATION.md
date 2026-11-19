# Integração do NDSRecomp no Oracis

**Decisão:** utilizar exclusivamente o compilador estático do NDSRecomp como ferramenta externa de desenvolvimento. O runtime do Oracis será independente.

**Revisão upstream fixada:** `14b7e15023e9affc9dc375e4e611ac703f5c76ab`  
**Repositório:** [RetroPortingToolKit/ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp)  
**Referência de build:** `recompiler/CMakeLists.txt`, que declara CMake 3.20+ e C++20.

## 1. Escopo permitido

O Oracis usará o alvo `nds_recompile` para analisar binários ARM e emitir bancos de código nativo. O compilador dá suporte às famílias ARM7TDMI (ARMv4T) e ARM946E-S (ARMv5TE), usadas respectivamente pelos processadores ARM7 e ARM9 do Nintendo DS.

A integração será feita por um checkout de desenvolvimento externo, fora da árvore Git do Oracis. O script `tools/ndsrecomp/build_recompiler.py`:

1. baixa ou verifica o checkout na revisão fixada;
2. inicializa somente o submódulo `external/arm-recomp-core`;
3. configura somente o diretório `recompiler/`;
4. compila `nds_recompile` e alvos de teste independentes do recompilador/núcleo ARM;
5. executa os testes do decodificador ARMv5TE, ciclo do núcleo, relocação, manifesto HLE, emissão, output errors e transferências.

O teste `live_shard_identity_test` fica fora porque depende de `runner/tests/` e `tools/compile_live_shards.py`, não incluídos no sparse checkout.

O bootstrap configura um **sparse checkout** que materializa somente `recompiler/`, `cmake/` e `external/arm-recomp-core/`. Os diretórios `runner/` e `oracle/` não são materializados nesse checkout. Em seguida, o CMake é apontado apenas para `recompiler/` e compila somente o alvo `nds_recompile`.

## 2. Limite absoluto: componentes derivados de melonDS

Os seguintes componentes não serão copiados, incluídos como submódulo, compilados, ligados ou distribuídos pelo Oracis:

- `runner/vendor/melonds/`;
- `runner/src/melonds_compute/`;
- `runner/src/frontend.cpp`;
- interfaces/shims derivados de melonDS;
- `oracle/` e seus patches contra melonDS;
- modelos de GPU, Wi-Fi, rede ou outros dispositivos obtidos de melonDS.

O bootstrap usa sparse checkout e, portanto, não materializa esses diretórios no checkout externo usado pelo Oracis. O script também verifica sua ausência antes de configurar o build. O runtime próprio do Oracis não liga nem depende dessas partes.

Não serão criados submódulos Git do NDSRecomp nem dependências do runner em CMake do Oracis.

## 3. Licenças verificadas na revisão fixada

A auditoria dos arquivos `LICENSE`, `PERMISSIONS.md`, `THIRD_PARTY_ATTRIBUTION.md` e `.gitmodules` indicou:

| Componente | Licença declarada | Tratamento no Oracis |
|---|---|---|
| Código próprio do compilador, finder e ferramentas | PolyForm Noncommercial 1.0.0, com permissões adicionais em `PERMISSIONS.md` | Ferramenta externa; preservar avisos e condições |
| `recompiler/armv4t/` e `recompiler/support/` | MIT, conforme a declaração de escopo em `THIRD_PARTY_ATTRIBUTION.md` | Usados apenas no checkout externo do compilador; não copiados para Oracis |
| `external/arm-recomp-core` | MIT | Dependência de build do compilador; licença e copyright preservados no checkout externo |
| toml++ 3.4.0 (single header) | MIT, conforme a licença upstream da dependência | Dependência de build baixada/verificada pelo CMake upstream; não copiada para Oracis |
| `runner/vendor/melonds/` e código derivado | GPL-3.0-or-later | Excluídos; não materializados no sparse checkout |
| `runner/src/melonds_compute/`, interfaces derivadas e `runner/src/frontend.cpp` | GPL-3.0-or-later, conforme escopo upstream | Excluídos; não materializados no sparse checkout |
| `oracle/` e patches contra melonDS | GPL-3.0-or-later / limites indicados pelo upstream | Excluídos; não materializados no sparse checkout |

As permissões adicionais do NDSRecomp permitem doações, patrocínios voluntários e publicidade quando não compram acesso ao software ou ao conteúdo; o software e os jogos derivados devem continuar acessíveis gratuitamente. Elas não transferem direitos sobre ROMs, BIOS, firmware, assets ou marcas.

A licença do compilador não cobre o conteúdo de Pokémon Platinum. A ROM continua sendo fornecida pelo usuário, validada localmente e nunca incluída em repositório, CI ou release.

## 4. Separação entre compilador e runtime

O compilador emite bancos de código-fonte C e tabelas de dispatch para código ARM identificado. Ele não é, sozinho, um port jogável. O resultado precisa de um runtime que forneça, entre outros:

- estado de registradores ARM e flags CPSR;
- transições ARM/Thumb e chamadas/retornos;
- acesso à memória por endereço guest;
- dispatch entre bancos ARM9 e ARM7;
- interrupções, eventos e sincronização entre os dois processadores;
- acesso aos serviços de sistema e dispositivos necessários ao jogo.

O NDSRecomp publica uma interface C ABI em `recompiler/armv4t/runtime_arm.h`. Essa interface será estudada como referência de contrato, mas o Oracis escreverá sua própria ABI e suas próprias implementações. Não será copiado o runtime upstream nem o código de `runner/src/`.

O projeto adotará C99 onde a saída gerada exigir esse padrão e C++20 para o runtime, scheduler e serviços nativos. A linguagem exata de cada unidade gerada será confirmada com um banco de teste antes de congelar o pipeline.

## 5. Código dinâmico, overlays e limites de execução

O NDSRecomp upstream possui mecanismos adicionais para código copiado para RAM, incluindo caminhos interpretados/JIT no runtime. Esses mecanismos não serão importados automaticamente.

O Oracis manterá o requisito de port nativo: código ARM executável deverá ter um banco recompilado e validado. Overlays e regiões de código materializadas em RAM precisam ser identificados, extraídos localmente, recompilados e associados à identidade/hash correta. Uma região não reconhecida deverá falhar de forma explícita, com endereço, CPU e contexto de execução; não será executada por um interpretador geral ARM como fallback.

A estratégia para code-copies, overlays, dispatch indireto e invalidation será especificada em um ADR separado após o primeiro banco real ser emitido. Não declarar compatibilidade até demonstrar que todos os alvos executáveis necessários estão cobertos.

## 6. Identidade da primeira revisão

A primeira revisão-alvo é **Pokémon Platinum USA Rev 1**, identificada localmente pelo SHA-1:

`0862ec35b24de5c7e2dcb88c9eea0873110d755c`

O identificador pode ser usado na validação local. O conteúdo da ROM, os binários ARM extraídos, os bancos recompilados e os arquivos de configuração gerados a partir dela devem permanecer em diretórios ignorados pelo Git.

### Segmentos verificados localmente

A cópia local USA Rev 1 foi inspecionada com uma rotina de extração de trabalho. O SHA-1 completo conferiu com o identificador esperado, os offsets/tamanhos dos segmentos estavam dentro do arquivo e ARM9/ARM7 não se sobrepunham. Os valores abaixo vieram dessa verificação local; o script versionado `tools/ndsrecomp/extract_nds_programs.py` ainda aguarda sua primeira execução integral após publicação.

| Segmento | Offset na ROM | Load address | Entry PC | Tamanho | SHA-1 do segmento |
|---|---:|---:|---:|---:|---|
| ARM9 | `0x00004000` | `0x02000000` | `0x02000800` (ARM) | `0x001023F8` | `7ec963e737f0f61efcbbf5c7a3d33cbf378143f8` |
| ARM7 | `0x00409800` | `0x02380000` | `0x02380000` (ARM) | `0x000277FC` | `082bb349ae7d057edbbd91a07316b1402b18b803` |

Os hashes são dos segmentos extraídos, não de bancos recompilados. Os binários e TOMLs temporários foram gerados apenas no ambiente local e não foram adicionados ao Git.

## 7. Como preparar o compilador

Na raiz do repositório Oracis, com Python 3.10+, Git, CMake 3.20+, Ninja e um compilador C++20 instalados:

```sh
python tools/ndsrecomp/build_recompiler.py
```

Por padrão, o checkout externo será criado em `../ndsrecomp` e o build em `.oracis/ndsrecomp-build`.

Para usar outro local:

```sh
python tools/ndsrecomp/build_recompiler.py \
  --source /caminho/externo/ndsrecomp \
  --build-dir .oracis/ndsrecomp-build
```

O script não altera checkouts existentes com revisão ou conteúdo diferente. Uma pasta já existente que não seja um checkout limpo, fixado e configurado com os mesmos sparse paths será recusada; nesse caso, indique um novo diretório vazio com `--source`. Ele inicializa somente o submódulo `external/arm-recomp-core`; não inicializa FreeBIOS.

## 8. Extrair os segmentos da cópia local

Com a ROM fornecida pelo próprio usuário:

```sh
python tools/ndsrecomp/extract_nds_programs.py "/caminho/Pokemon - Platinum Version (USA) (Rev 1).nds"
```

O extrator aceita somente o SHA-1 USA Rev 1 esperado, valida os limites dos dois segmentos, calcula o SHA-1 de cada binário, confere os dados gravados e gera `arm9.toml`, `arm7.toml` e `manifest.json` em `.oracis/local/pokemon-platinum-usa-rev1`. Os testes `tests/tools/test_extract_nds_programs.py` usam imagens sintéticas para cobrir integridade, limites, sobreposição, entry point e overflow. O bootstrap do compilador também executa `tests/tools/test_extractor_recompiler.py`: extrai uma imagem sintética ARM/Thumb e exige que o NDSRecomp fixado emita os dois bancos C e suas tabelas de dispatch; em seguida, compila os arquivos de corpo e dispatch em C99 contra `include/oracis/recomp/runtime_arm.h`. Esses testes não usam nem distribuem ROMs comerciais.

Requer Python 3.10 ou mais recente. Todos os arquivos de saída são locais e ignorados pelo Git.

## 9. Resultados de validação em 19/11/2026

A revisão `a3032f13ca8a11130ccc17fe8c6f871ee253d24e` passou na execução completa da CI:

- Desktop Linux x86-64: configuração e build bem-sucedidos; **28/28 CTests** aprovados.
- Integração opcional de bancos: os dois objetos de bancos sintéticos foram ligados e o ciclo de registro/remoção passou (**1/1**).
- Runtime ARM sem SDL3: build e testes aprovados, incluindo os novos testes do carregador local e SHA-1.
- Android ARM64: biblioteca nativa compilada.
- NDSRecomp estático: alvo `nds_recompile` compilado na revisão upstream fixada.
- Tooling Python: **11 testes** aprovados, incluindo a rejeição de uma identidade de codegen diferente de `nds-codegen-v3`.

[Execução da CI](https://github.com/CarlosCosta07-s/Oracis/actions/runs/44604378710).

Esses resultados validam o pipeline sintético, a ligação/remoção de tabelas e a integridade do carregador com casos de erro. **Não** validam a emissão do código real de Pokémon Platinum nem o boot do jogo. A extração e a emissão comercial continuam dependendo da ROM fornecida pelo usuário e ainda não foram executadas pelo pipeline versionado.

## 10. Pipeline local completo

Com a cópia USA Rev 1 fornecida pelo usuário, o fluxo esperado é:

```sh
python tools/ndsrecomp/extract_nds_programs.py "/caminho/Pokemon - Platinum Version (USA) (Rev 1).nds"
python tools/ndsrecomp/build_recompiler.py
python tools/ndsrecomp/emit_platinum_banks.py
cmake -S . -B build \
  -DORACIS_LOCAL_BANK_DIR=.oracis/generated/pokemon-platinum-usa-rev1
cmake --build build --parallel
```

O extrator só aceita a identidade de ROM documentada e grava os segmentos e metadados em `.oracis/local/pokemon-platinum-usa-rev1`. O emissor confere novamente tamanho, hash, load address, entry point, modo e TOMLs; consulta `nds_recompile --codegen-identity`; exige `nds-codegen-v3`; e só publica o conjunto quando ARM9 e ARM7 foram emitidos e validados. Os arquivos gerados permanecem ignorados pelo Git.

Quando `ORACIS_LOCAL_BANK_DIR` aponta para os bancos locais, o CMake compila os corpos e tabelas de dispatch como objetos e liga-os ao runtime. Na inicialização, o host pode registrar as tabelas por meio de `oracis_host_runtime_register_compiled_banks()`. O smoke test da CI valida essa ligação e o ciclo de registro/remoção com fixtures sintéticas.

A ponte agora oferece `oracis_host_runtime_load_local_platinum_segments(directory)`. Quando `directory` é nulo, a função usa `.oracis/local/pokemon-platinum-usa-rev1`; lê `arm9.bin` e `arm7.bin`; exige os tamanhos, endereços de carga, entry points e SHA-1 esperados para USA Rev 1; rejeita arquivo ausente, truncado, maior ou com hash divergente; e só chama o carregamento de segmentos em memória depois da validação integral dos dois arquivos. A falha de validação ocorre antes de qualquer escrita na máquina.

O carregador é uma API disponível para o host, mas o host SDL3 **ainda não a chama automaticamente**. Ele também não chama `oracis_host_runtime_run_round()`; carregar imagens e registrar bancos não substitui a implementação dos serviços de runtime ou o provider arquitetural de IRQ.

## 11. Próximos marcos técnicos

1. Executar o extrator versionado e o emissor NDSRecomp fixado contra a cópia USA Rev 1 local; preservar localmente logs, hashes e manifesto.
2. Compilar os bancos reais e inventariar símbolos indefinidos, helpers esperados, dependências e overlays/cópias de código.
3. Implementar somente os serviços efetivamente exigidos pelos bancos, com contratos e testes próprios: ciclos, bus, exceções/CP15, IRQ, DMA, IPC, BIOS/NitroSDK e código dinâmico.
4. Implementar e testar o provider arquitetural de IRQ, incluindo LR correto, modo ARM/Thumb e base vetorial CP15.
5. Ligar no host SDL3 o registro dos bancos e a chamada ao carregador local somente depois da validação da extração real; manter a execução bloqueada até os serviços e o provider IRQ estarem completos.
6. Iniciar rounds somente quando ambos os bancos e imagens estiverem validados e os caminhos de dispatch/IRQ exigidos estiverem prontos.

O critério de primeiro boot nativo continua sendo a execução observável e reproduzível dos dois cores até a apresentação do primeiro quadro, sem interpretador ARM genérico e sem componentes de runtime derivados de melonDS.
