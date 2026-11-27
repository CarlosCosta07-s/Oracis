# Aceleração com o ecossistema de ports, decomps e recomps

**Auditoria inicial:** 26/11/2026  
**Repositório-alvo:** `CarlosCosta07-s/Oracis`  
**Base examinada:** `main`, commit `4e110576628780b0ec359d700806f10ec2a52496`

Este documento registra como aproveitar o conhecimento e as ferramentas públicas da comunidade para reduzir trabalho repetido no Oracis, sem trazer código incompatível, licenças não auditadas ou componentes proibidos pela arquitetura do projeto.

## 1. Regras de aproveitamento

1. Desenvolver diretamente em `main`; não criar, alternar ou usar branches auxiliares.
2. Não incorporar código ou assets derivados de melonDS, nem o runner/oracle/frontend do NDSRecomp.
3. Não copiar código, recursos, ROM, firmware, BIOS, saves ou dados proprietários de jogos.
4. A árvore `pret/pokeplatinum` continua sendo referência de pesquisa; reutilização de código permanece bloqueada enquanto não houver licença/autorização clara.
5. Toda dependência candidata exige auditoria de licença, proveniência, arquitetura, versão, ABI e testes antes de qualquer integração.
6. Preferir metadados, contratos de hardware, ferramentas independentes e testes diferenciais. Um exemplo de outro jogo não fornece endereços, símbolos ou comportamento específicos de Pokémon Platinum.
7. Artefatos gerados a partir de arquivos do usuário permanecem em `.oracis/` ou outro diretório local ignorado; nunca entram no Git, CI ou releases.
8. Não declarar boot, compatibilidade ou ganho de desempenho com base apenas em compilações e fixtures sintéticas.

## 2. Fontes comunitárias avaliadas

| Fonte | O que vale aprender | Decisão para o Oracis |
|---|---|---|
| [RetroPortingToolKit/ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp) | Compilador estático ARM, bancos nativos e validação de bytes guest | Manter a revisão fixada atual; acompanhar apenas mudanças relevantes ao alvo `nds_recompile` |
| [mstan/supermario64dsrecomp](https://github.com/mstan/supermario64dsrecomp) | Preparação local da ROM, geração de TOMLs a partir de metadados de funções, módulos, overlays, validação de conteúdo e boot de cartucho | Referência de pipeline; não importar o runner ou assumir compatibilidade com o framework/fork usado pelo projeto |
| [Pokemon-PC/pokeplatinum-pc](https://github.com/Pokemon-PC/pokeplatinum-pc) | Port nativo recente de Platinum, camada Nitro para PC, overlay static-init e testes de jogo scriptados | Referência funcional prioritária; licença do repositório não declarada, sem cópia de código |
| [tangosdev/sm64ds-decomp](https://github.com/tangosdev/sm64ds-decomp) | Exemplo de metadados de função e módulo fornecidos separadamente do código do jogo | Só estudar a metodologia. Os símbolos são de outro jogo/região e não servem para Platinum |
| [darthmdev/pmd-sky-recomp](https://github.com/darthmdev/pmd-sky-recomp) | Organização de muitos bancos AOT, overlays, ARM9/ARM7, memória e serviços de plataforma | Referência arquitetural apenas; repositório declara GPL-3.0 |
| [cybervisi0n/pokeplatinum](https://github.com/cybervisi0n/pokeplatinum/tree/pc_port) | Port nativo de Platinum baseado em decompilação e substituição de serviços NitroSDK por bibliotecas host | Estudar o problema dos serviços e o fluxo de port; não copiar código nem enviar contribuições/trechos gerados por IA |
| [cybervisi0n/libntr](https://github.com/cybervisi0n/libntr) | Separação de serviços NitroSDK em bibliotecas | A licença MIT do repositório libntr foi identificada; qualquer módulo específico ainda exige revisão de proveniência, dependências e encaixe na arquitetura |
| [AKCore/pokeplatinum-port](https://github.com/AKCore/pokeplatinum-port) | Casos de teste, determinismo, diagnósticos e comparação diferencial | Não importar hardware: a documentação do próprio projeto identifica modelos derivados de melonDS e licença GPL-3.0-or-later |
| [pret/pokeplatinum](https://github.com/pret/pokeplatinum) | Referência comportamental e organização da descompilação | Não incorporar código/recursos; o repositório não apresenta uma licença geral identificada nesta auditoria |
| [GBATEK](https://problemkaputt.de/gbatek.htm) e [Nintendo DS Architecture](https://classic.copetti.org/writings/consoles/nintendo-ds/) | Contratos públicos dos dispositivos, boot, memória e registradores | Fontes técnicas para implementar e testar o runtime próprio |

### Observação prioritária: port nativo de Platinum publicado em 25/11/2026

O repositório [Pokemon-PC/pokeplatinum-pc](https://github.com/Pokemon-PC/pokeplatinum-pc) é uma referência funcional direta do mesmo jogo, mas usa outra estratégia: compila os 1.012 arquivos C da descompilação com uma camada de plataforma PC, em vez de executar bancos gerados por recompilação estática. O [PORTING_ROADMAP.md](https://github.com/Pokemon-PC/pokeplatinum-pc/blob/main/PORTING_ROADMAP.md) e a documentação [ARCHITECTURE](https://github.com/Pokemon-PC/pokeplatinum-pc/blob/main/docs/pc_port/ARCHITECTURE.md) descrevem uma camada Nitro para OS/FS/cartucho/RTC/entrada/áudio/gráficos, adaptação de estruturas com ponteiros de 32 bits, geração de cabeçalhos e tratamento de inicializadores estáticos de overlays.

As árvores foram examinadas apenas pelos Git trees, sem abrir ou copiar conteúdos C:
- `Pokemon-PC/pokeplatinum-pc`, commit `7c16dea6b7ada730912b0d3b05d0ca2d216ad9f2`: **214 arquivos** no padrão `src/overlayNNN/ovNNN_ADDRESS.c`, distribuídos pelos mesmos **29 IDs de overlay** do Oracis.
- `cybervisi0n/pokeplatinum`, branch `pc_port`: **216 arquivos** no mesmo padrão, também nos mesmos **29 IDs**.
- As duas árvores contêm os mesmos **7 headers** `include/overlayNNN/funcptr_ovNNN_ADDRESS.h`, em 6 IDs (`005, 006, 022, 061, 062, 070`); esses já correspondem aos headers opcionais documentados no fluxo de descoberta do Oracis.

A consulta não identificou novos IDs de overlay por nomes de fonte ou por esses headers. A contagem não lê o conteúdo dos arquivos e não mede todos os callbacks ou todas as funções. Aumentar a cobertura dos 60 overlays restantes exige novas evidências — por exemplo, outras tabelas de ponteiros validadas, referências internas verificadas ou rastros de execução — não apenas trocar a árvore de fontes por esses ports.

A [documentação de testes](https://github.com/Pokemon-PC/pokeplatinum-pc/blob/main/docs/pc_port/TESTING.md) desse projeto relata 783 verificações automatizadas, testes de jogo scriptados com gravação de saves e capturas, uma sequência pelos oito ginásios, Elite Four, Cynthia e Hall da Fama, e uma inspeção de dezenas de locais. O projeto também documenta dumps de frame, registradores, paletas, OAM, VRAM e texturas, além de análise automática de áudio. Os commits mais recentes encontrados em 26/11/2026 relatam correções na sequência final de save e execução de história em francês e inglês. **Esses resultados são declarações e artefatos do próprio projeto; não foram executados nem verificados pelo Oracis.**

O GitHub informa `license: null` para esse repositório e não foi localizado um arquivo `LICENSE` na raiz. Além disso, o port declara depender da árvore decompilada e de subprojetos Nitro. Portanto, não copiar seus fontes, camada Nitro, ferramentas, testes ou dados. Usar somente a documentação pública como mapa de serviços e referência de casos de teste até que a licença/proveniência seja esclarecida. A documentação desse port também usa ROM francesa/revisão 0; endereços, hashes e resultados binários não são automaticamente aplicáveis à USA Rev 1 do Oracis.

**Aplicação prática ao Oracis:** transformar os cenários funcionais descritos por esse port em casos independentes de aceitação (título, nova partida, salvamento/recarga, exploração e batalha); usar a lista de serviços OS/FS/RTC/cartucho/entrada/áudio/GX como checklist de lacunas NitroSDK; e tratar a execução de `NitroStaticInit` durante `FS_LoadOverlay`/`FS_StartOverlay` como hipótese de comportamento a validar contra os metadados da ROM USA Rev 1. Não copiar ou assumir os endereços, código ou layout da revisão francesa.

### Observações importantes sobre licenças e diferenças

- O repositório `mstan/supermario64dsrecomp` possui uma licença MIT para seu próprio código, mas também mantém um submódulo separado chamado `ndsrecomp` apontando para `mstan/ndsrecomp`. Esse framework não é o mesmo identificador de repositório do pin `RetroPortingToolKit/ndsrecomp` usado pelo Oracis. Não misturar revisões nem presumir compatibilidade. O próprio projeto também documenta componentes de runner sob GPL devido a código derivado de melonDS.
- O `pmd-sky-recomp` declara GPL-3.0. Seu desenho pode ser estudado, mas não é uma fonte de código para integração no Oracis.
- A licença MIT de `libntr` não concede automaticamente licença sobre o port `cybervisi0n/pokeplatinum`, `libntrsystem`, `libntrwifi`, `libntrdwc` ou outros subprojetos. O README do port também declara que contribuições ou sugestões de código feitas com IA serão rejeitadas; não enviaremos sugestões geradas por IA aos mantenedores.
- A auditoria do build de `libntr` encontrou uma superfície ampla de substituição NitroSDK (OS, FS, CARD/CTRDG, GX, SND, SPI, RTC, PAD e outras), compilada por Meson para DS ou targets host. Os targets host definem `SDK_PORT`, `SDK_X86` e `SDK_64BIT`; isso não torna a biblioteca um drop-in direto para o runtime Oracis. Nosso modelo de bancos estáticos C99 + ABI guest + máquina C++20 é diferente do port que compila os fontes C do jogo e instala uma camada Nitro/host. **Decisão:** usar `libntr` como inventário de APIs e requisitos de serviços; não ligá-la diretamente até mapear layouts, ponteiros guest, ownership de memória, callbacks, MMIO e dependências de cada módulo.
- O `AKCore/pokeplatinum-port` documenta que seus modelos de hardware são derivados de melonDS. Isso conflita diretamente com a regra do Oracis e exclui esses componentes de qualquer reutilização.
- A presença de código público, nomes de funções ou arquivos em uma descompilação não equivale, por si só, a uma licença para incorporá-los.

## 3. Decisão sobre o NDSRecomp atual

O Oracis fixa `RetroPortingToolKit/ndsrecomp` em:

```text
14b7e15023e9affc9dc375e4e611ac703f5c76ab
```

O bootstrap local usa checkout esparso e permite apenas:

- `recompiler/`
- `cmake/`
- `external/arm-recomp-core/`

A comparação entre esse pin e o `main` de `RetroPortingToolKit/ndsrecomp` encontrou 12 commits posteriores. Os arquivos alterados estavam no runner, frontend, gráficos, serviços e testes do runner; nenhum dos caminhos permitidos do compilador estático apareceu no diff. Portanto, **não há benefício demonstrado para atualizar o pin agora**. Uma revisão futura só deve ser considerada quando uma mudança atingir o compilador ou seu núcleo ARM permitido, após nova auditoria de licença e execução da suíte de integração do Oracis.

## 4. O que o Oracis já tem para aproveitar

A base examinada já contém ferramentas próprias para:

- extrair e validar os segmentos principais ARM9/ARM7 de uma cópia local;
- gerar bancos C e shards com identidade de codegen e validação dos bytes guest;
- auditar símbolos externos e classificar referências ausentes;
- extrair imagens de overlays, validar hashes e produzir inventários;
- descobrir candidatos de entrada de overlays a partir de metadados locais;
- gerar registradores e manifestos de cobertura;
- carregar e descarregar overlays de forma transacional na camada de máquina;
- recompilar BIOSes fornecidas pelo usuário em bancos locais separados;
- rastrear eventos e transferências por CPU;
- testar ABI, memória, dispositivos, máquina, loaders e bancos sintéticos na CI.

A auditoria documentada da revisão USA Rev 1 registra 122 overlays ARM9: 29 com âncoras encontradas por nomes de arquivos de fonte, 33 adicionais com sementes provenientes das tabelas de inicialização estática e 60 ainda sem essas sementes. A cobertura de sementes não equivale a cobertura de todas as funções dos módulos.

**O mecanismo de baixo nível já existe e não deve ser refeito:** `NdsOverlayLoader` valida o SHA-1, interpreta a tabela de ponteiros static-init preservando ordem e modo ARM/Thumb, copia a imagem descomprimida e zera BSS. `NdsMachine` expõe execução supervisionada de construtores com orçamento, restauração do contexto da CPU e solicitação cooperativa a partir de uma fatia guest; o C bridge já fornece APIs de load/run/request/unload.

**Primeira parte da orquestração agora implementada:** `NdsOverlayCatalog` lê `overlays/manifest.json`, exige a identidade USA Rev 1, valida schema, contagem, CPU/ID únicos, faixas de RAM/BSS/static-init e nomes de arquivos derivados do par CPU/ID. Ao pedir um overlay, lê somente o arquivo correspondente, exige arquivo regular sem symlink, tamanho exato e SHA-1 correto, então chama `NdsMachine::load_overlay`. A ponte C expõe `oracis_host_runtime_load_local_overlay(directory, cpu, overlay_id)`; o diretório padrão é `.oracis/local/pokemon-platinum-usa-rev1/overlays`. Foram adicionados testes sintéticos para identidade incorreta, contagem divergente, IDs duplicados, path traversal, faixa static-init inválida, JSON malformado e imagem adulterada.

**A lacuna restante é a integração do serviço NitroSDK:** ainda não existe um callback de jogo que traduza `FS_LoadOverlayImage`/`FS_StartOverlay` e a seleção de módulos para as novas APIs. A API de carga não registra automaticamente os bancos nem executa construtores; o chamador deve pedir a execução supervisionada somente quando o código guest atingir o ponto correto. O unload exige certificado de ausência de referências guest; após construtores terem sido executados, o runtime recusa o descarregamento com `FinalizersUnavailable`, pois ainda não há tabela/execução de finalizadores validada.

## 5. Pipeline de aceleração adotado

### Etapa A — medir antes de implementar

Executar os caminhos de teste local com os arquivos do usuário e guardar relatórios reproduzíveis. Para cada falha, registrar:

- CPU, PC, estado ARM/Thumb e banco selecionado;
- identidade/hash do módulo ou overlay em RAM;
- endereço MMIO ou região de memória envolvida;
- instrução/transferência e orçamento de ciclos;
- evento de IRQ, DMA, IPC ou troca de contexto relacionado;
- resultado esperado e resultado observado.

Não misturar resultados de ROMs, revisões ou BIOSes diferentes.

### Etapa B — transformar cada falha em um caso pequeno

Cada primeiro dispatch miss, acesso MMIO sem suporte, falha de timing ou dependência BIOS/NitroSDK deve virar um teste sintético mínimo. Corrigir uma classe por vez e preservar o comportamento anterior. O teste sintético deve isolar a semântica; a validação posterior com a imagem real do usuário deve confirmar que a hipótese se aplica à revisão alvo.

### Etapa C — ampliar a cobertura de código

Usar primeiro as sementes já verificadas e os manifestos existentes. Para novos alvos indiretos:

1. distinguir destino resolvido, inalcançável com evidência e desconhecido;
2. buscar evidência em referências/ponteiros extraídos, tabelas de inicialização e rastros de execução;
3. confirmar modo ARM/Thumb, limites, bytes e identidade do módulo;
4. gerar novo banco local com o compilador fixado;
5. auditar símbolos, compilar e validar os bytes antes de registrar;
6. manter desconhecidos em falha fechada, sem interpretar ARM como fallback.

Não inventar entrypoints a partir do endereço de carga ou do início do BSS. Um endereço de inicialização estática é apenas uma semente para aquele callback, não uma prova de cobertura completa do overlay.

### Etapa D — ligar serviços sem duplicar trabalho

Usar as referências comunitárias para inventariar serviços NitroSDK e contratos de hardware, não para copiar implementações incompatíveis. Priorizar o serviço somente quando um rastro real ou um teste de boot mostrar que ele é necessário. Os contratos de IPC, DMA, timers, Game Card, SPI, vídeo e BIOS devem permanecer testáveis de forma independente.

### Etapa E — validar o primeiro quadro

A sequência de aceitação é: entrypoints reais resolvidos; execução nativa de BIOS/boot ou direct-entry explicitamente identificado; dependências e overlays requeridos cobertos; primeiro quadro obtido pelo pipeline de vídeo do Oracis; repetição determinística com logs e identidade dos arquivos. Compilar para Android ARM64 não substitui teste em aparelho nem gera por si só um APK instalável.

## 6. Próximas prioridades técnicas

1. Executar localmente o direct-entry e o caminho BIOS com bancos/arquivos reais fornecidos pelo usuário; capturar o primeiro dispatch e os primeiros eventos que interrompem o avanço.
2. Resolver a primeira falha real observada, em vez de implementar serviços especulativos.
3. Fechar a descoberta dos destinos indiretos e ampliar a cobertura dos 60 overlays sem sementes.
4. Integrar um adaptador NitroSDK para carga/inicialização de overlays aos APIs do catálogo e de `NdsMachine`; manter unload bloqueado até haver finalização e rastreamento de referências guest demonstrados.
5. Demonstrar o primeiro quadro real e só então avançar os subsistemas de áudio, saves e acabamento de plataforma.

## 7. Critério para aceitar uma contribuição comunitária

Uma contribuição só passa para integração quando houver: URL e revisão exatas; licença do arquivo/componente e de suas dependências; proveniência clara; compatibilidade com C99/C++20, ABI, arquitetura e plataformas; ausência de código/asset proibido; teste reproduzível; e registro da decisão neste documento. Até lá, ela é referência técnica, não dependência.

Nenhuma alteração nesta auditoria importa código de outros ports, altera o pin do NDSRecomp ou inclui conteúdo derivado do jogo.
