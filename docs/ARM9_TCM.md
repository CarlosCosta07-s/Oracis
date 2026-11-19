# Suporte parcial ao ARM9 CP15/TCM

**Estado:** o controlador CP15 do ARM9 cobre os registradores de identificação, tamanho e regiões TCM usados pelo ARM946E-S, além de Control.V e bits de habilitação/carga. MPU, cache, manutenção de cache e o restante do CP15 não estão implementados.

## Registradores implementados

| Registrador | Comportamento |
|---|---|
| c0,c0,0 | Identificação ARM946E-S (0x41059461) |
| c0,c0,2 | Tamanho físico: 32 KB ITCM e 16 KB DTCM |
| c1,c0,0 | Control.V, habilitação e load mode de ITCM/DTCM |
| c9,c1,0 | Região DTCM: base e tamanho visível |
| c9,c1,1 | Região ITCM: tamanho visível; a base ITCM é fixa em zero |

Os registros de região iniciam com base zero e os encodings correspondentes aos tamanhos físicos. O DTCM não é ativado com base zero; a inicialização precisa configurar uma base válida antes de habilitá-lo. A implementação atual aceita janelas entre 4 KB e 32 MB, alinhadas ao tamanho codificado, e rejeita encodings maiores em vez de truncar endereços.

## Armazenamento e sobreposição

O ITCM e o DTCM possuem armazenamentos próprios e privados, separados da RAM retail. As janelas de região apontam para esses backings por espelhos periódicos. O ITCM usa a janela inferior de endereços; o DTCM pode sobrepor uma janela da Main RAM, por exemplo em 0x027C0000.

As regiões TCM têm prioridade sobre o mapa de RAM subjacente. Ao desativar o TCM ou reduzir a janela visível, os bytes originais de RAM voltam a aparecer; eles não são sobrescritos nem descartados. Cada região possui máscaras de acesso independentes para leitura de dados, escrita de dados e busca de instruções.

## Modos de carga

| Modo | Leitura de dados | Escrita de dados | Busca de instruções |
|---|---|---|---|
| ITCM normal | ITCM | ITCM | ITCM |
| ITCM load mode (bit 19) | Memória externa | ITCM | ITCM |
| DTCM normal | DTCM | DTCM | Memória externa |
| DTCM load mode (bit 17) | Memória externa | DTCM | Memória externa |
| TCM desativado | Mapeamento subjacente | Mapeamento subjacente | Mapeamento subjacente |

Esses caminhos permitem inicializar TCM por cargas e stores normais: leituras de origem vão para a memória externa e as escritas de destino alimentam o armazenamento privado. O ITCM continua servindo buscas de instruções enquanto seu caminho de leitura de dados está em load mode. O DTCM não atende busca de instruções.

## Testes e limitações

Os testes cobrem ID/TCM size, valores iniciais das regiões, exigência de base não zero para DTCM, janelas e alinhamento, privacidade do ITCM, preservação da RAM subjacente, habilitação/desabilitação, redimensionamento, load modes e roteamento separado entre leitura, escrita e busca.

Isto não representa suporte integral ao CP15. MPU, caches, efeitos de manutenção, contenção/latência real de TCM e demais registradores CP15 permanecem pendentes e falham de forma explícita. A aprovação dos testes não comprova boot do jogo.

## Referências

- ARM946E-S Technical Reference Manual, registradores CP15 Control/TCM e load modes: https://documentation-service.arm.com/static/5e8e3ee588295d1e18d3aa82
- GBATEK, mapa de memória e TCM do Nintendo DS: https://problemkaputt.de/gbatek.htm
