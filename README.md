<!-- Oracis project page -->
<p align="center">
  <img src="assets/icon/oracis.svg" alt="Ícone do Oracis: estrela de platina e anel celeste" width="160">
</p>

<h1 align="center">Oracis</h1>

<p align="center">
  Um projeto de port nativo de Pokémon Platinum para plataformas modernas.
</p>

<p align="center"><strong>Reconstruindo Sinnoh para além do hardware original.</strong></p>

> **Status: planejamento e fundação.** A identidade visual, a estratégia inicial de linguagens e as regras do projeto estão sendo estabelecidas. Ainda não existe uma versão jogável nem sistemas de jogo implementados.

## O projeto

Oracis tem como objetivo levar Pokémon Platinum a uma execução nativa em computadores e, futuramente, dispositivos móveis. A proposta não é emular um Nintendo DS: é adaptar a lógica do jogo e substituir os serviços específicos do console por implementações próprias para os sistemas modernos.

O desenvolvimento começa pela arquitetura, pela análise técnica da descompilação pública e pela definição de uma camada nativa que possa ser testada separadamente do jogo.

## Direção técnica

- **C99** para a lógica do jogo e estruturas compatíveis com a base de descompilação, somente quando a reutilização do código for autorizada.
- **Rust** para a aplicação, serviços de plataforma, importação e validação de dados, cache, saves, diagnósticos e integração nativa.
- **WGSL** para shaders do renderizador multiplataforma baseado em wgpu.
- **Python**, quando necessário, apenas para ferramentas auxiliares de desenvolvimento.

A decisão completa e suas justificativas estão em [docs/TECH_STACK.md](docs/TECH_STACK.md).

## Princípios

- **Port nativo, não emulador.** Não executar o jogo simulando CPU, memória ou periféricos do Nintendo DS.
- **Separação de responsabilidades.** Lógica do jogo, serviços de plataforma, renderização e interface devem permanecer desacoplados.
- **Código e ferramentas próprios.** Não incorporar código ou recursos de emuladores de terceiros.
- **Preservação responsável.** ROMs, firmware, arquivos extraídos, saves e patches binários não serão armazenados no repositório, na CI ou em releases.
- **Transparência.** Compatibilidade, progresso e testes só serão anunciados quando forem verificáveis.

## Plataformas planejadas

1. Windows x64 e Linux x86-64 como primeiros alvos de desenvolvimento.
2. Android após estabilizar o núcleo e as abstrações de entrada, áudio e renderização.
3. iOS como alvo posterior da mesma arquitetura multiplataforma.

## Referência técnica e licenciamento

A descompilação pública [pret/pokeplatinum](https://github.com/pret/pokeplatinum) é uma referência importante para compreender a estrutura e o comportamento do jogo. Ela é um trabalho em andamento, escrito principalmente em C. Na auditoria inicial de 19 de novembro de 2026, o repositório não declarava uma licença nem continha um arquivo `LICENSE`; portanto, sua visibilidade pública não será tratada como autorização automática para copiar ou redistribuir código. A política de reutilização será definida antes de qualquer incorporação.

## Identidade visual

O símbolo do Oracis é um sigilo celeste original: uma estrela facetada em platina atravessada por um anel orbital dourado. O desenho é vetorial e gerado a partir de código. Consulte [docs/VISUAL_IDENTITY.md](docs/VISUAL_IDENTITY.md).

## Aviso

Oracis é um projeto independente de preservação e engenharia, sem afiliação ou endosso da Nintendo, The Pokémon Company ou Game Freak. Pokémon e seus elementos relacionados pertencem aos respectivos titulares. O projeto não distribui o jogo nem conteúdo proprietário.
