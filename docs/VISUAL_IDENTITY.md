# Identidade visual do Oracis

## Conceito

O ícone usa um **sigilo celeste de origem**: uma estrela facetada em platina atravessada por um anel orbital dourado. A estrela representa criação e equilíbrio; o anel remete ao espaço e à mitologia cósmica de Sinnoh.

A inspiração é temática e abstrata. O desenho é original e não reproduz a silhueta, o modelo, a arte oficial ou o logotipo de Arceus.

## Relação visual com Aceris

Oracis acompanha a mesma linguagem de ícone criada para Aceris:

- composição quadrada com cantos arredondados;
- fundo escuro com gradientes profundos;
- símbolo central com volume, camadas e sombreamento;
- reflexos e pontos de luz discretos;
- formas legíveis mesmo em tamanhos reduzidos.

A identidade é própria: Oracis troca a paleta de fruta vermelha e verde por azul-noite, platina, ouro e um pequeno brilho ciano.

## Paleta

| Papel | Cor |
|---|---|
| Fundo principal | `#3B315F` |
| Fundo profundo | `#11172E` |
| Platina clara | `#FFFFFF` |
| Platina média | `#AFC3D8` |
| Ouro claro | `#FFF0B8` |
| Ouro escuro | `#A87536` |
| Brilho ciano | `#80C8E8` |

## Arquivos e geração

- Fonte de verdade: `tools/oracis-icongen/src/main.rs`.
- Ícone vetorial gerado: `assets/icon/oracis.svg`.
- Regenerar a imagem a partir do código:

```sh
cargo run --manifest-path tools/oracis-icongen/Cargo.toml
```

Por enquanto o projeto publica somente o SVG. PNG, ICO e variantes para Android serão acrescentados quando a pipeline de renderização e empacotamento estiver pronta.

## Regras

- Escrever o nome como **Oracis**.
- Não usar capturas de tela, sprites, modelos 3D ou logotipos oficiais de Pokémon como elementos do ícone.
- Não alterar proporções ou cores do símbolo sem atualizar este documento e a fonte de geração.
