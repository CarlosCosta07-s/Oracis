# Fase 0 — Baseline e auditoria da descompilação

**Data da auditoria:** 19 de novembro de 2026  
**Projeto:** Oracis — port nativo independente de Pokémon Platinum  
**Referência técnica examinada:** [pret/pokeplatinum](https://github.com/pret/pokeplatinum)  
**Commit examinado:** `62bdaacdca6c56a6a396f3e9911e2f922b79bba8` (`main`, 7 de novembro de 2026)

## 1. Resultado executivo

A referência `pret/pokeplatinum` é uma descompilação em andamento de Pokémon Platinum. Seu build atual produz uma ROM Nintendo DS correspondente às revisões norte-americanas documentadas. Ele **não** produz um executável nativo para Windows, Linux ou Android.

O repositório Oracis ainda não incorpora código, dados de jogo ou submódulos do projeto de referência. A incorporação de código está bloqueada até que a situação de licenciamento e as condições de reutilização sejam esclarecidas.

## 2. Estado observado na referência

| Item | Evidência observada |
|---|---|
| Branch padrão | `main` |
| Commit HEAD examinado | `62bdaacdca6c56a6a396f3e9911e2f922b79bba8` |
| Commit mais recente observado | 7/11/2026 — `tools: Make scripts and events easier to work with by hand (#1332)` |
| Descrição oficial | WIP decompilation of Pokémon Platinum |
| Linguagens declaradas pelo sistema de build | C, C++ e NASM |
| Padrão do código C | C99 |
| Build | Make como ponto de entrada, Meson e Ninja |
| Toolchain de destino original | ARM9/ARM7 e ferramentas de assembly ARM |
| Saída principal | `pokeplatinum.us.nds` |
| Plataformas nativas desktop/mobile | Não há alvo host nativo documentado |

A árvore consultada continha aproximadamente 30.654 entradas. A presença de diretórios como `res/`, `platinum.us/`, `asm/` e `generated/` reforça que não se trata de uma biblioteca de lógica de jogo isolada: ela inclui recursos, dados e componentes necessários à reconstrução da ROM. Oracis não copiará essa árvore para seu próprio repositório.

## 3. Como a referência compila

O `meson.build` observado declara os projetos C, C++ e NASM e exige Meson `>=1.12.0`. O código C recebe `-lang c99`, mas as flags de compilação e ligação incluem opções específicas do processador ARM e do toolchain Metrowerks.

O build também declara dependências NitroSDK, NitroSystem, NitroWiFi, NitroDWC, libvct e libsyscall. O alvo principal é configurado como `native: false`; em seguida, o `nitrorom` empacota o executável e o sistema de arquivos na ROM `.nds`.

O Makefile disponibiliza, entre outros, os alvos:

- `make`: build de release e verificação;
- `make rom`: constrói a ROM sem executar a verificação final;
- `make check`: depende de `rom` e executa os testes/checksums;
- `make debug`: build de depuração e auxiliares GDB.

As instruções de instalação listam dependências que incluem GCC ARM bare-metal, Bison, Flex, G++, Make, Ninja, Python, NASM e libpng. O sistema baixa uma versão fixada do Meson e prepara o wrapper de toolchain Metrowerks.

**Conclusão:** reutilizar o código C em um host x86-64/ARM64 exige um build novo para Oracis, remoção ou substituição das dependências específicas do DS, conversão das rotinas que ainda dependem de assembly e implementação dos serviços nativos. O build upstream não resolve esses problemas automaticamente.

## 4. Revisões e identificadores documentados

O README da referência documenta duas saídas norte-americanas:

| Revisão | SHA-1 documentado |
|---|---|
| US Rev 1 | `0862ec35b24de5c7e2dcb88c9eea0873110d755c` |
| US Rev 0 | `ce81046eda7d232513069519cb2085349896dec7` |

### Cópia local de referência identificada

Em 19/11/2026, o SHA-1 da cópia local fornecida para identificação foi calculado localmente e resultou em:

`0862ec35b24de5c7e2dcb88c9eea0873110d755c`

O valor corresponde exatamente ao identificador documentado para **Pokémon Platinum USA Rev 1** na referência. O arquivo tinha 134.217.728 bytes (128 MiB). Portanto, a primeira revisão-alvo de desenvolvimento do Oracis fica definida como **USA Rev 1**.

Somente o identificador criptográfico e os metadados mínimos de verificação são registrados aqui. A ROM não foi adicionada ao repositório, não será enviada à CI e não será incluída em releases.

A cobertura de idiomas/regiões da descompilação não deve ser presumida além das revisões explicitamente documentadas.

## 5. Licença, dados e regras da referência

Na consulta de 19/11/2026, a API do GitHub retornou `license: null`, e a árvore raiz examinada não continha um arquivo geral `LICENSE`. Há arquivos de licença de ferramentas individuais, mas eles não definem automaticamente os direitos sobre todo o código e os recursos do jogo.

O arquivo `AGENTS.md` também estabelece regras de participação para agentes automatizados: não automatizar issues, PRs ou comentários; não publicar textos escritos por IA em interações com mantenedores; e exigir que o colaborador humano compreenda o código e as alterações antes de publicar contribuições.

Oracis não fará commits, issues, comentários ou pull requests no repositório `pret/pokeplatinum`. A referência será estudada sem copiar código ou assets. Qualquer reutilização futura depende de uma base de autorização/licença válida e de análise individual das dependências.

## 6. Decisão de arquitetura para Oracis

O guia de desenvolvimento adotado pelo projeto recomenda CMake + Ninja + Clang para o build nativo, SDL3 para janela/entrada/áudio e SDL_GPU ou uma camada gráfica multiplataforma separada.

A direção inicial adotada após esta auditoria é:

| Camada | Decisão inicial | Situação |
|---|---|---|
| Lógica original do jogo | C99 | Condicionada à autorização de reutilização |
| Serviços nativos / Nitro compatibility | C99 | Implementação independente |
| Build host | CMake + Ninja + Clang | Esqueleto inicial em preparação |
| Desktop e Android: janela, eventos, controles e áudio | SDL3 | Dependência fixada para o protótipo |
| Renderização | bgfx com sua API C99 | Candidata preferida; exige protótipo próprio |
| Shaders | pipeline `shaderc` do bgfx | A validar com o renderizador |
| Ferramentas de análise | Python, quando necessário | Auxiliar; não obrigatório em runtime |
| Ícone | Gerador independente em Rust | Ferramenta isolada; não define a linguagem do jogo |

### Por que bgfx é a candidata gráfica preferida

A documentação pública do bgfx lista backends Direct3D 11/12, Metal, OpenGL, OpenGL ES 3.0+ e Vulkan, e plataformas Windows, Linux, macOS, Android e iOS. O projeto também oferece uma API C99 e um exemplo `examples/25-c99/helloworld.c`, permitindo manter a integração do jogo em C sem exigir que o núcleo seja convertido para C++.

A escolha ainda não está definitivamente fechada: antes disso, um protótipo deve abrir uma janela SDL3, inicializar bgfx, limpar a tela e apresentar um quadro no desktop e em Android ARM64.

### Integração Android

A documentação do Android NDK oferece um toolchain CMake próprio. A documentação SDL3 explica que aplicações Android usam um pequeno host Java/JNI para iniciar a aplicação nativa. Portanto, uma camada Java mínima para Activity/integração Android é aceitável; a lógica do jogo e os serviços principais continuam em C.

## 7. O que foi e não foi validado

### Concluído nesta auditoria remota

- [x] Confirmado o commit e a branch examinados.
- [x] Lidos README, INSTALL, CONTRIBUTING, AGENTS, Makefile, meson.build e meson.options.
- [x] Identificado que a saída upstream é uma ROM DS e não um executável nativo.
- [x] Identificados os toolchains e dependências do build original.
- [x] Verificada a ausência de licença geral declarada na referência.
- [x] Registrada a regra de não contribuir automaticamente no repositório upstream.
- [x] Definida uma arquitetura candidata para o host nativo.
- [x] Identificada localmente a cópia-alvo como Pokémon Platinum USA Rev 1 por correspondência exata do SHA-1 documentado.

### Ainda pendente para fechar a Fase 0

- [ ] Executar o build/checksum matching da referência em ambiente local apropriado, se houver base legal e técnica para isso.
- [ ] Resolver a autorização de reutilização antes de integrar qualquer código da descompilação.
- [ ] Medir o protótipo de SDL3 + renderizador antes de congelar a decisão gráfica.

**Importante:** o assistente não executou `make`, `make rom` ou `make check` nesta auditoria. Os resultados do build foram inspecionados estaticamente nos arquivos de configuração e instruções; não se deve interpretar isso como uma compilação bem-sucedida.

## 8. Próximo passo verificável

A identificação da revisão de referência está concluída. A Fase 1 começa com um executável independente que:

1. compile para desktop com CMake + Ninja + Clang;
2. inicialize SDL3 e abra uma janela;
3. apresente duas áreas de teste de 256 × 192;
4. tenha o alvo Android ARM64 compilado na CI;
5. não carregue ROM, dados extraídos ou recursos proprietários.

O esqueleto deverá compilar e rodar antes de receber código do jogo.
