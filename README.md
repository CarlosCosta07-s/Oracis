<!-- Oracis project page -->
<p align="center">
  <img src="assets/icon/oracis.svg" alt="Ícone do Oracis: estrela de platina e anel celeste" width="160">
</p>

<h1 align="center">Oracis</h1>

<p align="center">Um projeto independente de port nativo de Pokémon Platinum para plataformas modernas.</p>

<p align="center"><strong>Reconstruindo Sinnoh para além do hardware original.</strong></p>

> **Status: Fase 1 — Esqueleto multiplataforma em implementação.** A revisão-alvo USA Rev 1 foi identificada. O primeiro host SDL3 e a CI para desktop/Android estão sendo preparados; ainda não há build validado nem versão jogável.

## O projeto

Oracis busca levar Pokémon Platinum a uma execução nativa em computadores e dispositivos móveis. Não é um emulador de Nintendo DS: o objetivo é compilar a lógica do jogo para plataformas modernas e substituir os serviços específicos do console por implementações nativas.

## Direção técnica

- **C99** para a lógica do jogo e os serviços nativos, condicionados à autorização de reutilização da descompilação.
- **CMake + Ninja + Clang** para o build host e cross-build Android.
- **SDL3** para janela, eventos, controles e saída de áudio.
- **bgfx (API C99)** como renderizador multiplataforma preferido, ainda sujeito a um protótipo desktop + Android.
- **Python** para ferramentas auxiliares; o gerador do ícone permanece uma ferramenta Rust isolada.

Veja [docs/TECH_STACK.md](docs/TECH_STACK.md) para decisões e referências.

## Estado atual

A primeira revisão-alvo é **Pokémon Platinum USA Rev 1**. A cópia fornecida para identificação teve SHA-1 calculado localmente e correspondente ao hash Rev 1 documentado; a ROM não foi incorporada ao repositório ou à CI.

A auditoria de [pret/pokeplatinum](https://github.com/pret/pokeplatinum) identificou uma descompilação em andamento cujo build atual produz uma ROM de Nintendo DS, não um executável host. Na auditoria de 19/11/2026, a referência não declarava uma licença geral; por isso nenhum código ou recurso foi incorporado ao Oracis. Consulte o [relatório de baseline](docs/PHASE_0_BASELINE.md).

A Fase 1 está começando com um host SDL3 vazio, duas áreas de apresentação de 256 × 192 e compilação Android ARM64 na CI. Consulte o [registro do esqueleto](docs/PHASE_1_SKELETON.md) e o [roteiro completo](docs/DEVELOPMENT_ROADMAP.md).

## Princípios

- Port nativo, não emulação de hardware Nintendo DS.
- Código e serviços independentes, com fronteiras explícitas.
- ROMs, firmware, dados extraídos, saves e patches binários ficam fora do repositório, CI e releases.
- Não incorporar código ou recursos de emuladores de terceiros.
- Não presumir autorização a partir da disponibilidade pública de um repositório.
- Não declarar compatibilidade, desempenho ou conclusão sem testes reproduzíveis.

## Plataformas planejadas

Windows x64 e Linux x86-64 são os primeiros ambientes de desenvolvimento. Android ARM64 está no escopo do esqueleto inicial; iOS vem depois da estabilização da camada multiplataforma.

## Identidade visual

O símbolo Oracis é um sigilo celeste original: uma estrela facetada em platina atravessada por um anel orbital dourado. O desenho é vetorial e gerado por código. Consulte [docs/VISUAL_IDENTITY.md](docs/VISUAL_IDENTITY.md).

## Aviso

Oracis é um projeto independente, sem afiliação ou endosso da Nintendo, The Pokémon Company ou Game Freak. Pokémon e seus elementos relacionados pertencem aos respectivos titulares. O projeto não distribui o jogo nem conteúdo proprietário.
