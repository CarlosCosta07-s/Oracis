# Base técnica do Oracis

> Decisões iniciais revisadas em 19/11/2026 a partir do guia de desenvolvimento e da auditoria da referência. O protótipo poderá alterar decisões de backend, mas não deve introduzir complexidade sem evidência.

## Decisão resumida

| Camada | Tecnologia | Responsabilidade |
|---|---|---|
| Lógica do jogo | **C99** | Compatibilidade com a base decompilada, somente quando a reutilização for autorizada |
| Camada nativa / Nitro compatibility | **C99** | Serviços do host que substituem dependências do Nintendo DS |
| Build | **CMake + Ninja** | Build desktop e cross-build Android |
| Compilador | **Clang** | x86-64 desktop e Clang do Android NDK |
| Janela, eventos e input | **SDL3** | Janela, ciclo de eventos, gamepad, teclado e toque |
| Áudio | **SDL3** | Saída de áudio; sequenciador e mixer são implementados no projeto |
| Renderização | **bgfx (API C99)** | Abstração gráfica multiplataforma, sujeita ao protótipo |
| Shaders | **bgfx shaderc** | Compilação dos shaders para os backends ativos |
| Ferramentas auxiliares | **Python** | Inspeção, extração local, validação e testes de dados |
| Android host | **Java/JNI mínimo + C nativo** | Activity e integração exigida pela plataforma |
| Ícone | **Rust** | Ferramenta isolada de geração do SVG do Oracis; não é linguagem do runtime |

## 1. C99 — jogo e camada nativa

A descompilação pública `pret/pokeplatinum` é descrita como um trabalho em andamento. Seu build usa C99, mas também declara C++, NASM e um toolchain voltado ao Nintendo DS. C99 é a escolha inicial para manter a lógica próxima à base de referência e para implementar serviços nativos com ABI simples.

Não vamos traduzir todo o jogo para outra linguagem. As rotinas ainda em assembly devem ser avaliadas individualmente: primeiro procurar um equivalente C validável; usar ferramentas de lifter apenas como recurso pontual e sempre conferir semântica.

**Bloqueio de reutilização:** na auditoria de 19/11/2026, o repositório `pret/pokeplatinum` não declarou uma licença geral nem apresentou arquivo `LICENSE` na raiz. Não copiar, incorporar ou redistribuir código ou recursos dessa descompilação até esclarecer os direitos e condições aplicáveis. A referência pode ser estudada sem tratá-la como autorização.

## 2. CMake, Ninja e Clang

O build de referência usa Make, Meson, Ninja e Metrowerks para produzir a ROM DS. Isso não é o build do port nativo. Oracis terá um build host independente com CMake + Ninja e Clang, usando o toolchain oficial do Android NDK no alvo Android.

Essa separação evita confundir “reconstruir a ROM ARM do console” com “compilar uma aplicação nativa de PC ou Android”.

Referências:
- [CMake — Android NDK](https://developer.android.com/ndk/guides/cmake)
- [Clang — suporte a C99](https://clang.llvm.org/c_status.html)

## 3. SDL3 — plataforma

SDL3 é a camada proposta para janela, ciclo de eventos, gamepad, teclado, toque e dispositivo de áudio. Sua documentação lista Windows, Linux, macOS, Android e iOS como plataformas onde SDL3 já funcionou; a equipe não testa todos os dispositivos, então CI e testes em hardware real continuam necessários.

No Android, a integração documentada usa um projeto Java com uma pequena camada de Activity/JNI para iniciar o código C. Isso não altera a escolha de C99 para o núcleo.

Referências:
- [SDL3 — plataformas](https://wiki.libsdl.org/SDL3/README-platforms)
- [SDL3 — integração Android](https://wiki.libsdl.org/SDL3/README-android)

## 4. bgfx — renderização candidata

A escolha inicial preferida é bgfx, usando a API C99. Sua documentação lista Direct3D 11/12, Metal, OpenGL, OpenGL ES 3.0+ e Vulkan, além de Windows, Linux, macOS, Android e iOS. O projeto inclui um exemplo C99.

Isso permite manter o runtime em C, sem obrigar a aplicação a adotar uma API gráfica diferente em cada plataforma. O backend final deverá ser selecionado com base nos recursos disponíveis no dispositivo e nas necessidades do jogo.

**Prova obrigatória antes de congelar a escolha:** abrir uma janela SDL3, inicializar bgfx, limpar e apresentar um quadro no desktop e em Android ARM64. Até passar nesse teste, bgfx é uma decisão preferida, não uma compatibilidade já demonstrada.

Referências:
- [bgfx — plataformas e backends](https://bkaradzic.github.io/bgfx/overview.html)
- [Exemplo de API C99 do bgfx](https://github.com/bkaradzic/bgfx/blob/master/examples/25-c99/helloworld.c)

### Por que não adotar SDL_GPU como backend universal agora

A documentação atual do SDL informa que SDL_GPU utiliza backends DX12, Metal e Vulkan; não é uma API de backend OpenGL. No Android, o próprio FAQ recomenda desativar recursos Vulkan para obter maior cobertura de dispositivos. É uma opção válida, mas bgfx parece oferecer uma cobertura mais ampla de backends e uma API C99. O protótipo decidirá.

Referência: [SDL3 FAQ — GPU backends e Android](https://wiki.libsdl.org/SDL3/FAQDevelopment).

## 5. Python e Rust

Python será usado apenas para ferramentas auxiliares que não sejam parte obrigatória da execução do jogo. O gerador do ícone Oracis permanece em Rust como uma ferramenta isolada já criada; não há decisão de usar Rust no runtime principal.

## 6. Política de plataforma e dados

- O produto é um **port nativo**, não um emulador do Nintendo DS.
- Não implementar CPU ARM9/ARM7 nem temporização/periféricos DS para executar o jogo em uma máquina virtual.
- ROMs, firmware, recursos extraídos, saves e patches binários não entram no repositório, CI ou releases.
- O app deverá validar localmente a cópia de jogo do usuário e manter quaisquer dados extraídos em cache local, versionado e verificável.
- Não copiar código nem assets de emuladores de terceiros.
- As comparações com melonDS/DeSmuME são testes externos de comportamento; não são fontes de código para o Oracis.

## 7. Plataformas e prioridade

1. Windows x64 e Linux x86-64 para desenvolver e depurar o host.
2. Android ARM64 integrado desde o esqueleto inicial e na CI.
3. iOS depois de estabilizar a camada multiplataforma.

A arquitetura deve nascer compartilhada, mesmo que a primeira validação ocorra em um conjunto menor de dispositivos.
