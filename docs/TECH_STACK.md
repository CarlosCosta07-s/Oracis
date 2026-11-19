# Base técnica do Oracis

> Documento de decisão inicial. As escolhas abaixo orientam os protótipos; podem ser revistas após testes de compilação, fidelidade e desempenho em cada plataforma.

## Decisão resumida

| Camada | Tecnologia | Responsabilidade |
|---|---|---|
| Código do jogo | **C99** | Preservar a lógica e os formatos de dados do código decompilado, quando a reutilização for juridicamente autorizada |
| Aplicação e camada nativa | **Rust estável** | Inicialização, serviços de plataforma, sistema de arquivos, importação e validação de dados, cache, saves, diagnósticos e integração |
| Shaders gráficos | **WGSL** | Shaders do renderizador multiplataforma baseado em wgpu |
| Ferramentas auxiliares | **Python, quando necessário** | Scripts de inspeção, validação e conversão durante o desenvolvimento; nunca obrigatório para executar o jogo |

**Não vamos traduzir todo o código C para Rust por princípio.** A prioridade é preservar o comportamento original quando houver uma base de código utilizável e licenciada, isolando as dependências específicas do Nintendo DS atrás de interfaces nativas.

## 1. C99 — lógica do jogo

A descompilação pública `pret/pokeplatinum` identifica o projeto como uma descompilação de Pokémon Platinum e usa C como linguagem principal. Seu sistema de build também declara C99, além de incluir etapas e dependências específicas do Nintendo DS.

Isso torna C99 a escolha de menor atrito para a camada de lógica do jogo: estruturas, rotinas e dados podem ser mantidos próximos da base de referência, reduzindo o risco de mudanças semânticas causadas por uma tradução extensa.

**Limite importante:** o projeto `pret/pokeplatinum` é marcado como trabalho em andamento e, na auditoria inicial de 19/11/2026, o repositório não declarava uma licença nem continha um arquivo `LICENSE`. Portanto, ele será tratado como referência técnica até que os direitos de reutilização sejam esclarecidos. Não vamos copiar ou incorporar seu código automaticamente só porque o repositório é público.

Referências:
- [pret/pokeplatinum — repositório e estado da descompilação](https://github.com/pret/pokeplatinum)
- [README do pret/pokeplatinum](https://github.com/pret/pokeplatinum#readme)
- [Build Meson do pret/pokeplatinum](https://github.com/pret/pokeplatinum/blob/main/meson.build)

## 2. Rust — aplicação e serviços nativos

Rust será a camada que conecta o jogo ao sistema operacional e implementa os serviços modernos do port:

- ciclo de vida da aplicação e abstração de plataforma;
- seleção, importação e validação da cópia de jogo fornecida pelo usuário;
- extração local e cache versionado de dados, com verificações de integridade;
- saves, configurações, logs e relatórios de falha;
- integração com áudio, entrada, janela e renderização;
- empacotamento e ferramentas de desenvolvimento.

A fronteira entre C e Rust será pequena e explícita, usando uma ABI C estável (`extern "C"`) e estruturas versionadas. A posse de recursos e a lógica de plataforma ficarão preferencialmente do lado Rust; o código C não deverá depender de detalhes internos do Rust.

Referência: [Rust Embedded Book — interoperabilidade entre Rust e C](https://doc.rust-lang.org/stable/embedded-book/interoperability/rust-with-c.html).

## 3. WGSL e renderização

A escolha inicial para o renderizador é **wgpu**, com shaders escritos em **WGSL**. A matriz atual do wgpu documenta suporte de primeira classe a Vulkan em Windows/Linux/Android, Direct3D 12 em Windows e Metal em macOS/iOS, além de caminhos de compatibilidade OpenGL.

Essa combinação dá uma única API de renderização para desktop e dispositivos móveis. A decisão só será considerada consolidada depois de protótipos medirem compatibilidade, latência e desempenho em GPUs integradas e aparelhos Android reais.

Referência: [wgpu — plataformas e backends suportados](https://docs.rs/wgpu/latest/wgpu/#supported-platforms).

## 4. Camada de janela, entrada e áudio

SDL3 é a candidata inicial para janela, eventos, controles e áudio, pois declara suporte a Windows, macOS, Linux, Android e iOS. A integração exata com wgpu será validada em um protótipo mínimo antes de se tornar dependência definitiva.

Referências:
- [SDL3 — plataformas suportadas](https://wiki.libsdl.org/SDL3/README-platforms)
- [SDL3 — visão geral](https://wiki.libsdl.org/SDL3/FrontPage)

## 5. Build e integração

- **Cargo**: aplicação Rust, bibliotecas internas, testes e ferramentas Rust.
- **Meson + Ninja**: camada C nativa, com um alvo host separado do alvo original de reconstrução de ROM.
- **CI**: compilar e testar componentes próprios; nunca armazenar ROMs, firmware, arquivos extraídos, saves ou patches binários.

O build de referência do pret gera uma ROM de Nintendo DS e utiliza compilação ARM e bibliotecas NitroSDK/NitroSystem/NitroWiFi/NitroDWC. Isso não é, por si só, um executável nativo para Windows, Linux ou Android. O Oracis precisará de uma camada host e de substitutos para as chamadas específicas do hardware e do SDK do DS.

## 6. Plataformas e ordem de trabalho

1. **Windows x64 e Linux x86-64**: primeiros alvos para desenvolver e depurar o núcleo nativo.
2. **Android**: depois que entrada, áudio, renderização e ciclo de vida estiverem desacoplados.
3. **iOS**: após estabilizar o mesmo núcleo multiplataforma e validar o pipeline de distribuição.

## Regras técnicas

- Oracis é um **port nativo**, não um emulador de Nintendo DS.
- Não implementar CPU ARM9/ARM7, temporização de hardware ou periféricos DS para executar o jogo como se estivesse dentro do console.
- Separar lógica do jogo, serviços de plataforma, renderizador e interface.
- Não afirmar compatibilidade ou desempenho sem testes reproduzíveis.
- Não introduzir código de emuladores de terceiros.
