# Fase 1 — Esqueleto nativo multiplataforma

**Estado:** primeiro código-fonte adicionado; build e execução ainda precisam ser confirmados pela CI e em dispositivos reais.  
**Revisão-alvo:** Pokémon Platinum USA Rev 1.  
**Dependência gráfica:** SDL3 3.4.20, fixada por URL e SHA-256.

## Entrega desta etapa

- `CMakeLists.txt`: projeto C99 e configuração SDL3 via CMake FetchContent.
- `src/main.c`: host SDL3 mínimo, com loop de eventos e duas áreas lógicas de 256 × 192 pixels.
- `.github/workflows/ci.yml`: build desktop Linux e cross-build Android ARM64.
- `.gitignore`: saídas de build e arquivos locais de jogo/dados extraídos fora do controle de versão.

O aplicativo desenha duas áreas de teste com cores diferentes. Isso confirma apenas o esqueleto de apresentação quando executado; não é ainda o renderizador do jogo, o vídeo 2D do Nintendo DS ou a implementação de bgfx.

## Build desktop

Requisitos: CMake 3.25+, Ninja, Clang ou GCC e acesso à internet na primeira configuração (para baixar o SDL3 fixado).

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/oracis
```

Para reutilizar uma instalação SDL3 3.4.20 já disponível no sistema:

```sh
cmake -S . -B build -G Ninja \
  -DORACIS_FETCH_SDL3=OFF \
  -DCMAKE_PREFIX_PATH=/caminho/para/SDL3
cmake --build build
```

## Cross-build Android ARM64

A CI usa o toolchain CMake do Android NDK e compila a biblioteca compartilhada chamada `libmain.so`, que é o nome esperado pelo host SDL Android:

```sh
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android
```

Esse cross-build produz o binário nativo, não um APK instalável. O projeto Gradle, a Activity Java que estende SDLActivity, o empacotamento e a execução em aparelho Android continuam pendentes. A documentação oficial do SDL3 descreve essa integração Java/JNI e o requisito de nomear o alvo nativo como `main`.

## Limites deliberados

- Nenhuma ROM, firmware, arquivo extraído, save ou patch é aberto ou incluído.
- Não há CPU ARM9/ARM7, temporização DS ou interpretação de instruções.
- Nenhum código da descompilação pret/pokeplatinum foi copiado.
- O host usa SDL_Renderer apenas para a prova inicial de janela/apresentação. bgfx permanece candidato para a camada de renderização definitiva.
- Não há ainda carregamento de dados, áudio do jogo, input do jogo, menus ou lógica de Pokémon Platinum.

## Critérios para marcar a Fase 1 como concluída

1. CI desktop compila o executável com CMake + Ninja + Clang.
2. CI compila `libmain.so` para Android `arm64-v8a`.
3. Janela desktop mostra as duas áreas com proporção lógica preservada ao redimensionar.
4. APK de teste inicia em Android ARM64 e mostra as duas áreas.
5. O loop trata eventos de fechamento e tecla Escape sem travar. O ciclo de vida Android (pausa, retomada, baixa memória e encerramento via event watch) será validado quando o projeto APK/Activity for integrado.

Até que os resultados estejam disponíveis, esta etapa permanece **em implementação**.
