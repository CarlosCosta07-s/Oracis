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

O script versionado `tools/ndsrecomp/extract_nds_programs.py` foi executado localmente em 19/11/2026 contra a cópia fornecida. A identidade completa conferiu com o SHA-1 esperado, os offsets/tamanhos ficaram dentro do arquivo, ARM9/ARM7 não se sobrepuseram e os arquivos gravados foram reabertos para verificação de SHA-1. A execução foi repetida usando o mesmo Git blob do `main` (`1283f60`); ROM, binários, TOMLs e manifesto ficaram fora do Git e da CI.

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

O extrator aceita somente o SHA-1 USA Rev 1 esperado, valida os limites dos dois segmentos, calcula o SHA-1 de cada binário, confere os dados gravados e gera `header.bin` (os primeiros `0x170` bytes do cabeçalho), `arm9.bin`, `arm7.bin`, os TOMLs e `manifest.json` em `.oracis/local/pokemon-platinum-usa-rev1`. Os testes `tests/tools/test_extract_nds_programs.py` usam imagens sintéticas para cobrir integridade, limites, sobreposição, entry point e overflow. O bootstrap do compilador também executa `tests/tools/test_extractor_recompiler.py`: extrai uma imagem sintética ARM/Thumb e exige que o NDSRecomp fixado emita os dois bancos C e suas tabelas de dispatch; em seguida, compila os arquivos de corpo e dispatch em C99 contra `include/oracis/recomp/runtime_arm.h`. Esses testes não usam nem distribuem ROMs comerciais.

Requer Python 3.10 ou mais recente. Todos os arquivos de saída são locais e ignorados pelo Git.

## 9. Resultados de validação em 19/11/2026

A revisão `90d45fe430b50a4f2ba968cb62fed5bb41d7e2b0` passou na execução completa da CI:

- Desktop Linux x86-64: configuração e build bem-sucedidos; **28/28 CTests** aprovados.
- Integração opcional de bancos: os dois objetos de bancos sintéticos foram ligados e o ciclo de registro/remoção passou (**1/1**).
- Runtime ARM sem SDL3: build e testes aprovados, incluindo os novos testes do carregador local e SHA-1.
- Android ARM64: biblioteca nativa compilada.
- NDSRecomp estático: alvo `nds_recompile` compilado na revisão upstream fixada.
- Tooling Python: **15 testes** aprovados, incluindo identidade de codegen e cenários de publicação transacional/rollback do emissor.

[Execução da CI](https://github.com/CarlosCosta07-s/Oracis/actions/runs/44611707658).

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

O extrator só aceita a identidade de ROM documentada e grava `header.bin` junto dos segmentos e metadados em `.oracis/local/pokemon-platinum-usa-rev1`. O host verifica o código `CPUE`, os offsets, tamanhos e entry points ARM9/ARM7, o checksum da logo e o CRC-16 do cabeçalho antes de espelhar os `0x170` bytes em `0x027FFE00`; o destino é pré-validado junto com os dois segmentos. O emissor confere novamente tamanho, hash, load address, entry point, modo e TOMLs; consulta `nds_recompile --codegen-identity`; exige `nds-codegen-v3`; gera ARM9 e ARM7 em staging e publica o diretório completo com rollback da versão anterior. Recusa symlinks, destinos que se sobreponham à pasta de extração ou diretórios com arquivos não relacionados; ao substituir uma saída anterior, remove bancos obsoletos do conjunto antigo. Os arquivos gerados permanecem ignorados pelo Git.

Quando `ORACIS_LOCAL_BANK_DIR` aponta para os bancos locais, o CMake compila os corpos e tabelas de dispatch como objetos e liga-os ao runtime. Na inicialização, o host pode registrar as tabelas por meio de `oracis_host_runtime_register_compiled_banks()`. O smoke test da CI valida essa ligação e o ciclo de registro/remoção com fixtures sintéticas.

Para bancos grandes, o emissor também separa cada corpo C em unidades menores
em `<bank>_shards/bank_part_NNNN.c`, por padrão com até 512 funções por
unidade. O arquivo monolítico original e o header permanecem no diretório local
para auditoria; o manifesto registra contagem, limite por unidade e SHA-1 de
cada shard. O validador confere nomes, quantidade, ausência de symlinks,
conteúdo exato do diretório e hashes antes da configuração CMake. O CMake
compila os shards quando existem e usa o corpo monolítico apenas para saídas
antigas sem shards. O limite pode ser ajustado com
`python tools/ndsrecomp/emit_platinum_banks.py --functions-per-shard 256`.
Esse particionamento não altera funções nem endereços do dispatch: cada unidade
inclui o mesmo header público gerado, que declara as funções do banco inteiro.

Antes de criar os alvos CMake, `validate_platinum_bank_set.py` confere o
manifesto `oracis_banks.json`, a identidade USA Rev 1, a identidade exata do
codegen (`nds-codegen-v3`), a identidade do adaptador de memória e o SHA-1 dos
arquivos C/H/dispatch e de cada shard declarado. O campo `codegen_identity` é escrito
pelo emissor depois da consulta ao executável; a validação rejeita bancos
retail emitidos por outra versão ou sem essa identidade. Fixtures sintéticas
usam o marcador separado `synthetic-fixture`, que não pode ser usado para
representar uma emissão retail. O diretório deve conter exatamente os seis arquivos-base, o manifesto
e apenas os diretórios de shards declarados; cada shard deve ser um arquivo C
regular com nome/hash esperado. Symlinks, objetos antigos, arquivos extras,
hashes divergentes ou metadados de outro segmento interrompem a configuração
antes da compilação. Fixtures sintéticas usam
`source_kind=synthetic-fixture` e exigem a opção explícita
`ORACIS_ALLOW_SYNTHETIC_BANKS=ON`; essa opção existe apenas para a prova de
integração e não transforma fixtures em bancos de Pokémon Platinum.

Antes da publicação, `emit_platinum_banks.py` aplica o adaptador de código-fonte `tools/ndsrecomp/patch_memory_access_direction.py` aos corpos C emitidos. O pin NDSRecomp permanece imutável e continua gerando a chamada de três argumentos `runtime_mem_cycles`; o adaptador Oracis associa essa chamada à próxima operação guest `bus_read_*` ou `bus_write_*` e a reescreve para `runtime_mem_cycles_ex(..., ORACIS_MEMORY_ACCESS_READ/WRITE)`. A identidade do adaptador (`oracis-memory-direction-v1`) e as quantidades de chamadas por direção ficam no manifesto local `oracis_banks.json`; os hashes publicados são calculados após a adaptação. O adaptador trata comentários/literais, agrupa chamadas de mesma instrução quando o codegen emite vários timings antes dos acessos (como LDRD/STRD) e falha fechado quando não encontra operação correspondente. `tests/tools/test_extractor_recompiler.py` executa o NDSRecomp fixado sobre instruções sintéticas ARM/Thumb LDR, STR, LDM e STM, aplica o adaptador e compila os C resultantes contra a ABI C99 Oracis. Nenhum código do compilador upstream é alterado ou copiado.

A ponte oferece `oracis_host_runtime_load_local_platinum_segments(directory)`. Quando `directory` é nulo, a função usa `.oracis/local/pokemon-platinum-usa-rev1`; lê `arm9.bin` e `arm7.bin`; exige os tamanhos, endereços de carga, entry points e SHA-1 esperados para USA Rev 1; rejeita arquivo ausente, truncado, maior ou com hash divergente; e só chama o carregamento de segmentos em memória depois da validação integral dos dois arquivos. A falha de validação ocorre antes de qualquer escrita na máquina.

Quando bancos estáticos estão ligados, o host SDL3 desktop chama essa API durante a inicialização. Se os arquivos não existirem, informa que continuará em modo de protótipo; se um arquivo existir mas for inválido, falha de modo explícito. No Android, a seleção de arquivo e a resolução do diretório privado ainda não estão integradas. Em nenhuma das plataformas o host chama `oracis_host_runtime_run_round()` nesta etapa: carregar imagens e registrar bancos não substitui a implementação dos serviços de runtime ou o provider arquitetural de IRQ.

## 11. Próximos marcos técnicos

1. Executar o extrator versionado e o emissor NDSRecomp fixado contra a cópia USA Rev 1 local; preservar localmente logs, hashes e manifesto.
2. Compilar os bancos reais e inventariar símbolos indefinidos, helpers esperados, dependências e overlays/cópias de código.
3. Implementar somente os serviços efetivamente exigidos pelos bancos, com contratos e testes próprios: ciclos, bus, exceções/CP15, IRQ, DMA, IPC, BIOS/NitroSDK e código dinâmico.
4. Implementar e testar o provider arquitetural de IRQ, incluindo LR correto, modo ARM/Thumb e base vetorial CP15.
5. Executar e auditar o caminho de inicialização desktop com a extração real; confirmar que os bancos estáticos compilados e os segmentos carregados pertencem à mesma revisão, e implementar a seleção segura de arquivo/diretório local no Android.
6. Iniciar rounds somente quando ambos os bancos e imagens estiverem validados e os caminhos de dispatch, serviços e IRQ exigidos estiverem prontos.

O critério de primeiro boot nativo continua sendo a execução observável e reproduzível dos dois cores até a apresentação do primeiro quadro, sem interpretador ARM genérico e sem componentes de runtime derivados de melonDS.


## 12. Auditoria local dos símbolos dos bancos

Quando `ORACIS_LOCAL_BANK_DIR` está configurado, o CMake cria o alvo `oracis_audit_platinum_bank_symbols`. Ele usa os objetos exatos dos bancos ARM9/ARM7 e compara seus símbolos indefinidos com os símbolos fornecidos pelos objetos dos próprios bancos e pelos arquivos `liboracis_machine`, `liboracis_memory` e `liboracis_arm_state`.

```sh
cmake -S . -B build \
  -DORACIS_LOCAL_BANK_DIR=.oracis/generated/pokemon-platinum-usa-rev1
cmake --build build --target oracis_audit_platinum_bank_symbols
```

O alvo usa o `CMAKE_NM` selecionado pelo toolchain (GNU `nm` ou LLVM `llvm-nm`). Ele agrupa referências compartilhadas entre CPUs, mostra quais objetos as solicitam e falha quando um símbolo não tem provedor. A CI executa esse alvo com fixtures sintéticas; ela não recebe ROM, segmentos extraídos, bancos comerciais ou objetos derivados do jogo.

Símbolos fornecidos pelo compilador C ou pela plataforma só podem ser excluídos por meio de um arquivo local explícito, passado com `-DORACIS_BANK_SYMBOL_ALLOWLIST=/caminho/absoluto/simbolos.allow`. O arquivo contém um símbolo por linha, com comentários iniciados por `#`. Não coloque nele helpers ARM, funções NitroSDK ou serviços que deveriam pertencer ao runtime Oracis. O allowlist é uma decisão local e revisável; não deve mascarar lacunas de ABI.

A execução contra os bancos reais ainda precisa ser feita localmente e seu relatório deve ser revisado antes de habilitar execução. A existência do alvo e o sucesso das fixtures não significam que os 48 símbolos reais já estejam resolvidos.

A auditoria também grava um relatório estruturado em `build/oracis_platinum_bank_symbol_audit.json` (ou no diretório de build usado pelo CMake). O schema `oracis-bank-symbol-audit-v1` registra, para cada referência indefinida, os bancos que a solicitam, o status (`resolved`, `allowlisted` ou `unresolved`) e todos os objetos/arquivos de runtime que fornecem a definição. O relatório é escrito de forma atômica e permanece no diretório local de build; não é enviado como artifact pela CI. Para executar a ferramenta diretamente, use `--report-json /caminho/local/relatorio.json`.


## 13. Proteção de entrada antes da execução

A ponte `oracis_host_runtime_run_round()` executa automaticamente o preflight
dos PCs atuais de ARM9 e ARM7 antes de iniciar qualquer fatia. Os dois PCs
precisam resolver em suas tabelas e passar pela validação de identidade dos
bytes locais no modo determinado por CPSR.T. Caso contrário, retorna
`ORACIS_HOST_RUNTIME_ENTRYPOINT_UNRESOLVED` sem executar funções nem avançar o
clock. O host ainda pode chamar `oracis_host_runtime_validate_entrypoints()`
antes para obter relatórios detalhados; a checagem automática torna essa
chamada diagnóstica, não um pré-requisito de segurança.


## 14. Inventário e extração local de overlays ARM9/ARM7

A etapa de extração dos programas principais agora tem um complemento para os
módulos executáveis armazenados no NitroFS. O script
**tools/ndsrecomp/extract_nds_overlays.py** lê as tabelas ARM9 e ARM7 do cabeçalho
NDS, valida cada registro de 32 bytes e resolve **file_id** pela FAT. Ele valida
os limites do arquivo, IDs repetidos, faixas de RAM/BSS, intervalo de
inicialização estática e flags conhecidas antes de publicar qualquer saída.

Overlays sem compressão são copiados como imagens de RAM. Quando o bit BLZ da
tabela está ativo, o extrator descomprime o fluxo backwards-LZ e exige que o
tamanho resultante coincida exatamente com **ram_size**. O decodificador valida
o rodapé, o comprimento da região comprimida, o padding **0xFF**, cada literal e
cada referência retroativa; também exige consumo exato dos bytes comprimidos.
O manifesto local registra CPU, ID, endereço de carga, tamanho, BSS, intervalo
de inicialização, ID FAT, flags, hash SHA-1 da origem e SHA-1/SHA-256 da imagem
extraída. O registro estático exige também o diretório local dessa extração
(`ORACIS_LOCAL_OVERLAY_IMAGE_DIR`, por padrão o diretório indicado pelo
extrator), valida os bytes reais de todos os arquivos e compara o SHA-1
registrado pelo emissor com a identidade da imagem extraída. Assim, uma tabela
de dispatch não é aceita apenas por nome/ID: ela precisa corresponder à mesma
imagem binária que será carregada na RAM guest.

Uso, sempre com uma cópia da ROM fornecida pelo usuário:

    python tools/ndsrecomp/extract_nds_overlays.py \
      "/caminho/Pokemon - Platinum Version (USA) (Rev 1).nds"

Por padrão, os módulos e **manifest.json** são gravados em
**.oracis/local/pokemon-platinum-usa-rev1/overlays/**. A pasta **.oracis/** já é
ignorada pelo Git. Ao atualizar uma extração anterior, o script substitui
somente diretórios que tenham um manifesto Oracis válido e contenham exatamente
os arquivos declarados nele; não apaga arquivos extras do usuário. A CI usa
apenas fixtures sintéticas e nunca recebe ROM nem overlays comerciais.

A descoberta de candidatos de entrada agora é reproduzível por
`tools/ndsrecomp/discover_platinum_overlay_configs.py`. Ela recebe o diretório
de imagens extraídas e um ZIP/diretório local da árvore Poképlatinum:

    python tools/ndsrecomp/discover_platinum_overlay_configs.py \
      --overlay-image-dir .oracis/local/pokemon-platinum-usa-rev1/overlays \
      --source /caminho/pokeplatinum-main.zip

A ferramenta inspeciona somente nomes de arquivos no padrão
`src/overlayNNN/ovNNN_ADDRESS.c`; não lê, copia nem compila o conteúdo desses
arquivos. Cada endereço é conferido contra a faixa de RAM e a identidade/hash
do overlay extraído. Os TOMLs resultantes permanecem no diretório local
ignorado pelo Git, e `anchor_discovery_report.json` lista também IDs sem
âncoras no código-fonte fornecido. O campo de modo dos candidatos usa Thumb
como hipótese provisória compatível com a auditoria anterior; os entry points
permanecem marcados como não autoritativos e exigem revisão por bytes/
desassemblagem antes de habilitar execução.

Na cópia de Poképlatinum fornecida para a auditoria de 23/11/2026, foram
encontrados arquivos-fonte com nomes de endereço para 29 dos 122 overlays
ARM9. A execução padrão gerou/conferiu 29 TOMLs e identificou 93 overlays sem
âncoras correspondentes nessa árvore. Esses 93 módulos continuam pendentes:
não são inicializados com endereços inventados nem inferidos apenas de
`load_address` ou `static_init_start`.

A árvore também possui sete headers com nomes
`include/overlayNNN/funcptr_ovNNN_ADDRESS.h`. Eles descrevem tipos de callback,
não implementação C; por isso são candidatos opcionais, não entry points
autoritativos. Para incluí-los sem sobrescrever os TOMLs anteriores, use outro
diretório de saída:

    python tools/ndsrecomp/discover_platinum_overlay_configs.py \
      --overlay-image-dir .oracis/local/pokemon-platinum-usa-rev1/overlays \
      --source /caminho/pokeplatinum-main.zip \
      --output-dir .oracis/local/pokemon-platinum-usa-rev1/overlays-with-funcptr \
      --include-function-pointer-headers

A auditoria NDSRecomp local desses sete candidatos terminou sem falhas de
decodificação nos seis overlays afetados. A descoberta aumentou o total
agregado de funções dos 29 bancos de **10.415 para 10.636** (+221):
overlay 0005 +202, 0006 +1, 0022 +0, 0061 +1, 0062 +11 e 0070 +6. Os corpos C
e tabelas de dispatch atualizados desses seis overlays foram compilados como
objetos C99 com Clang. A auditoria conjunta dos objetos dos dois bancos
principais e dos 29 overlays resolveu **11.719 referências**, sem símbolos
não resolvidos nem allowlist. Isso valida compilação e resolução de símbolos,
não a execução funcional dos módulos em jogo. A hipótese Thumb continua
provisória e requer conferência byte-level antes de ativar execução.

**Limite desta etapa:** extrair e validar um overlay não significa recompilá-lo.
O manifesto não inventa um entry point para o módulo. Ainda é necessário
descobrir referências e alvos de função nos bancos principais, gerar a
configuração NDSRecomp por overlay, compilar os módulos, auditar símbolos e
conectar a resolução de overlays/código dinâmico ao runtime. O host continua
sem executar rounds do jogo até que essas dependências e os serviços/IRQ
pendentes estejam validados.

## 15. Registro dos bancos estáticos de overlays

A opção local ORACIS_LOCAL_OVERLAY_BANK_DIR liga os bancos ARM9 de overlays emitidos separadamente ao lado dos bancos principais. Ela exige ORACIS_LOCAL_BANK_DIR, porque os dois bancos principais continuam sendo a base do programa. Exemplo:

    cmake -S . -B build \
      -DORACIS_LOCAL_BANK_DIR=.oracis/generated/pokemon-platinum-usa-rev1 \
      -DORACIS_LOCAL_OVERLAY_BANK_DIR=.oracis/generated/pokemon-platinum-usa-rev1-overlays \
      -DORACIS_LOCAL_OVERLAY_IMAGE_DIR=.oracis/local/pokemon-platinum-usa-rev1/overlays

O diretório de bancos deve conter emission_report.json e uma pasta overlay_NNNN/ para cada resultado emitido, com o corpo C, header e tabela _dispatch.c. O diretório de imagens deve ser a saída local de extract_nds_overlays.py. O gerador tools/ndsrecomp/generate_overlay_bank_registry.py valida ambos os manifestos, o pin nds-codegen-v3, o adaptador de direção de memória e cada imagem extraída; exige que o image_sha1 do relatório de emissão corresponda à imagem do mesmo overlay_id; calcula SHA-256 dos arquivos C efetivamente ligados e gera um manifesto de cobertura e um registrador C++ no diretório de build. Os bancos são compilados como objetos C99 e ligados ao oracis_machine.

Na inicialização, oracis_host_runtime_register_compiled_banks() registra primeiro ARM9/ARM7 principais e depois os bancos de overlay. A operação é idempotente para tabelas já registradas e o registrador gerado desfaz apenas os overlays adicionados pela chamada que falhou. Todos os overlays desta primeira revisão são ARM9. Endereços iguais entre bancos são permitidos pelo registry; cada entrada mantém sua validação de bytes, então uma função de um overlay só pode ser selecionada quando a memória guest contém os bytes esperados por aquela entrada.

**Limite importante:** registrar uma tabela não carrega o conteúdo do overlay na memória guest. O NdsOverlayLoader já copia uma imagem extraída e verificada para a RAM guest e inicializa o BSS; a API exige que o host forneça a imagem e seus metadados do manifesto local. Ainda falta ligar essa API ao gerenciador de overlays/NitroSDK para automatizar carga, substituição, inicializadores estáticos e unload seguro. Bancos ligados e registrados não tornam os overlays executáveis por si só. O preflight dos entry points principais continua obrigatório e nenhum round do jogo deve ser habilitado apenas por causa do registro dos bancos.

## 16. Carregamento transacional dos overlays extraídos

A classe `oracis::machine::NdsOverlayLoader` consome os campos de uma linha
de `overlays/manifest.json` junto com o arquivo de imagem correspondente.
O chamador deve fornecer uma imagem **já descomprimida**, com exatamente
`ram_size` bytes e o `image_sha1` publicado pelo extrator. O loader valida
CPU, ID, limites de 32 bits, BSS, intervalo de inicialização estática, SHA-1,
permissões de escrita e execução e colisões com outros overlays ativos da
mesma CPU.

A escrita da imagem e a inicialização de BSS são preparadas em um buffer único
e aplicadas por uma única chamada a `GuestMemory::write_bytes`. O registro do
overlay só é publicado depois da confirmação de identidade dos bytes escritos.
A geração de mutação do mapa de memória muda durante essa escrita e, portanto,
invalida os links estáticos cacheados que dependem dos bytes guest. Uma imagem
com hash incorreto, destino não mapeado ou sobreposição é rejeitada antes de
ser considerada carregada.

A API de máquina é `NdsMachine::load_overlay()` /
`NdsMachine::unload_overlay()`. A ponte de host expõe as mesmas operações por
`oracis_host_runtime_load_overlay()` e
`oracis_host_runtime_unload_overlay()`; os dados podem ser obtidos
diretamente do manifesto local e do arquivo `arm9_overlay_NNNN.bin` ou
`arm7_overlay_NNNN.bin`. O runtime não lê a ROM nem faz parsing de JSON nessa
API: a camada host deve selecionar os campos do manifesto e fornecer os bytes.

O unload só é aceito fora de fatias ativas, quando o PC atual não está dentro
do módulo e quando o host passa `no_live_guest_references=1`. Essa confirmação
é necessária porque o runtime ainda não rastreia todos os endereços de retorno
guardados em pilhas guest. O módulo é zerado e removido do inventário somente
depois que a faixa inteira continua gravável. Não chame unload até que o
gerenciador de overlays possa provar que não há retorno pendente para o módulo.

Durante `NdsMachine::shutdown()`, depois que as fatias dos dois CPUs
são encerradas e antes de a memória ser desvinculada do barramento, o loader
zera todas as faixas RAM+BSS que ainda constam no inventário e descarta seus
registros. Assim, um ciclo posterior de initialize/load não herda bytes de
overlays da sessão anterior. Se a limpeza falhar, o shutdown falha
explicitamente em vez de fingir que o inventário foi resetado.

O intervalo `static_init_start..static_init_end` é validado e preservado
como metadado, mas **não é executado** pelo loader. A execução dos
construtores/initializers, o protocolo específico de substituição usado pelo
jogo e a resolução de dependências de overlays ainda precisam ser integrados.
O carregador também não faz relocations nem converte funções compiladas
ausentes em código executável: os bancos estáticos por overlay continuam
sendo obrigatórios para dispatch nativo.

O manifesto do registry informa total de overlays extraídos, quantidade
recompilada e lista explícita de IDs ainda sem banco. Na extração local
auditada em 23/11/2026, há 122 overlays ARM9 no manifesto e 29 bancos
recompilados; os 93 IDs restantes ainda precisam de emissão e auditoria.
Os hashes dos 29 bancos emitidos coincidem com as imagens correspondentes do
manifesto local.

A regressão `oracis_nds_overlay_loader_test` cobre cópia de imagem, BSS zerado,
rejeição de hash sem mutação, idempotência, colisões, validação do intervalo
de inicialização e unload condicionado à quiescência. Esses testes não
representam ainda o primeiro boot de Pokémon Platinum.

## 17. Emissão incremental dos bancos estáticos de overlays

O pipeline local completo está disponível em
`tools/ndsrecomp/emit_platinum_overlay_banks.py`. Ele exige o executável
NDSRecomp fixado em `nds-codegen-v3`, o manifesto/imagens produzidos por
`extract_nds_overlays.py` e o relatório/TOMLs da descoberta de âncoras.
Para cada overlay selecionado, confere os campos `[program]` e `[identity]`
do TOML com a imagem extraída, executa `nds_recompile --audit`, recusa
qualquer relatório que omita os resumos obrigatórios ou indique operações
sem codegen, emite o banco com `--validate-live-bytes`, aplica o adaptador
de direção de memória e divide o corpo em unidades C limitadas.

Uso padrão, com todos os overlays que possuem configuração local:

    python tools/ndsrecomp/emit_platinum_overlay_banks.py

A emissão é feita em diretório de staging e publicada somente quando todos
os overlays selecionados tiverem terminado. O relatório
`emission_report.json` vincula os bancos à identidade SHA-1 das imagens,
registra os hashes SHA-256 dos C/H/dispatch, os hashes SHA-1 das unidades
shard, contagens de funções, resultados de audit e cobertura compilada/
pendente. Nenhum conteúdo derivado da ROM é gravado em Git.

Para atualizar apenas uma seleção sem repetir o trabalho dos demais bancos:

    python tools/ndsrecomp/emit_platinum_overlay_banks.py \
      --overlay-ids 5,6,22,61,62,70 \
      --reuse-existing-bank-dir .oracis/generated/pokemon-platinum-usa-rev1-overlays

O diretório anterior só é reutilizado quando a identidade ROM/codegen,
SHA-1 de cada imagem, SHA-256 de cada C/H/dispatch e, quando existentes,
hashes/conjunto completo dos shards conferem. Arquivos divergentes fazem a
operação falhar; não são silenciosamente copiados. O resultado novo continua
sendo um diretório autônomo com todos os bancos reutilizados e recém-emitidos.

O CMake agora prefere os arquivos
`overlay_NNNN/<bank>_shards/bank_part_NNNN.c` quando o relatório contém
shards validados, compilando o corpo monolítico apenas para bancos antigos
sem shards. Cada pasta de overlay entra no include path para que os shards
encontrem o header gerado do banco. O gerador do registry rejeita shards
ausentes, extras, com nome inesperado ou SHA-1 divergente.

**Validação local em 23/11/2026:** a emissão incremental re-auditou e
re-emitiu os overlays 0005, 0006, 0022, 0061, 0062 e 0070; os outros 23
bancos foram reutilizados depois da verificação de identidade e hashes.
O conjunto final contém 29/122 bancos e 10.636 funções. Os seis overlays
re-auditados não reportaram gaps em `codegen NOT-IMPLEMENTED ops`.
Foram compiladas 17 novas unidades C99 (shards e dispatch) e a auditoria
combinada de 67 objetos (bancos principais e overlays) resolveu 11.719
referências, com zero símbolos não resolvidos e zero allowlisted.

Esses resultados confirmam a emissão/verificação estática e a ligação de
símbolos; não confirmam execução guest, inicializadores estáticos,
relocations, retorno seguro entre overlays nem boot jogável. Os 93 IDs sem
âncoras seguem pendentes. Os entry points criados a partir dos nomes de
arquivos permanecem candidatos provisórios, não pontos autoritativos.
