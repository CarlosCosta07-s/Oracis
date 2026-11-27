# Contrato de ciclo de vida de overlays NitroSDK (Nintendo DS)

**Auditoria de referência:** 27/11/2026  
**Alvo Oracis:** Pokémon Platinum USA Rev 1 (0862ec35b24de5c7e2dcb88c9eea0873110d755c)

Este documento registra o contrato observável do NitroSDK para overlays e como o runtime do Oracis deve se conectar a ele. As referências comunitárias abaixo são documentação/fonte pública para pesquisa de interface e comportamento; nenhum código ou asset desses projetos foi copiado.

## 1. Formato do registro de overlay

O registro NDS tem oito palavras de 32 bits (32 bytes), na ordem:

| Offset | Campo | Significado |
|---:|---|---|
| 0x00 | id | ID do overlay |
| 0x04 | ram_address | endereço de carga em Guest RAM |
| 0x08 | ram_size | tamanho da imagem descomprimida |
| 0x0C | bss_size | bytes adicionais que devem começar zerados |
| 0x10 | sinit_init | endereço inicial da tabela de ponteiros de construtores |
| 0x14 | sinit_init_end | endereço final exclusivo da tabela |
| 0x18 | file_id | ID do arquivo no FAT da ROM |
| 0x1C | compressed:24 / flag:8 | tamanho comprimido e flags |

O extractor Oracis já lê esses oito campos como oito inteiros little-endian de 32 bits, valida os intervalos e os flags conhecidos, descomprime BLZ localmente quando necessário e publica uma imagem de exatamente ram_size bytes com hashes SHA-1/SHA-256. O arquivo extraído e o manifesto continuam locais.

Referências de API/layout: [Nitro FS overlay.h](https://github.com/cybervisi0n/libntr/blob/main/include/nitro/fs/overlay.h) e [Oracis extractor](../tools/ndsrecomp/extract_nds_overlays.py).

## 2. Sequência do NitroSDK

A implementação pública de referência de FS_LoadOverlay no Nitro FS SDK 4 mostra a sequência de alto nível:

1. FS_LoadOverlayInfo(target, id): seleciona a tabela ARM9 ou ARM7, calcula id * sizeof(FSOverlayInfoHeader), lê os 32 bytes do cabeçalho e resolve o arquivo FAT.
2. FS_LoadOverlayImage(info): abre o arquivo, limpa a região BSS e lê o payload para ram_address. O comprimento lido é compressed quando o flag de compressão está ativo; caso contrário é ram_size.
3. FS_StartOverlay(info): descomprime para o tamanho de RAM quando aplicável, efetua a manutenção de cache necessária e percorre sinit_init .. sinit_init_end em ordem. Cada ponteiro não nulo é chamado como função void(void).
4. FS_EndOverlay(info): executa os destrutores globais que pertencem à faixa de memória do overlay e remove seus nós da cadeia global.
5. FS_UnloadOverlayImage(info): no código de referência, chama FS_EndOverlay. FS_UnloadOverlay(target, id) resolve novamente o cabeçalho e solicita essa finalização.

A versão exata do SDK e os caminhos de boot/ROM podem variar; o Oracis deve reproduzir o comportamento requerido pela revisão USA Rev 1, e não assumir que todos os detalhes de uma variante SDK5/TWL se aplicam.

Fonte examinada: [libntr — fs_overlay.c](https://github.com/cybervisi0n/libntr/blob/main/libraries/fs/src/fs_overlay.c). A leitura da implementação foi usada somente para documentar sequência e contratos; não houve cópia de código.

## 3. Construtores e cadeia de destrutores

### Construtores

A tabela sinit_init .. sinit_init_end é uma sequência de ponteiros de função no próprio overlay. O runtime Oracis já:

- valida os limites da tabela dentro da imagem;
- preserva a ordem original;
- usa o bit 0 de cada ponteiro para distinguir Thumb de ARM;
- resolve todos os destinos no dispatch registry antes de executar o primeiro;
- executa os callbacks em slices supervisionadas e limitadas por ciclos;
- restaura o contexto arquitetural do chamador e mantém os efeitos de memória guest.

O início do overlay deve solicitar esses construtores somente depois que a imagem e o BSS foram publicados. Carregar bytes não equivale a iniciar o overlay.

### Destrutores

Não existe uma segunda tabela de finalizadores no cabeçalho NDS de 32 bytes. O SDK usa a cadeia global Metrowerks:

    typedef void (*MWI_DESTRUCTOR_FUNC)(void *);

    typedef struct MWiDestructorChain {
        struct MWiDestructorChain *next;
        MWI_DESTRUCTOR_FUNC dtor;
        void *obj;
    } MWiDestructorChain;

No ABI ARM de 32 bits, esse nó ocupa 12 bytes. A função declarada pelo runtime SDK é:

    void __register_global_object(
        void *obj,
        MWI_DESTRUCTOR_FUNC dtor,
        MWiDestructorChain *chain);

O compilador reserva nós dessa estrutura em dados/BSS do módulo; uma função em .init pode construir mais de um objeto global e registrar mais de um destrutor. A cadeia é global, mas cada nó de registro pode residir na faixa de RAM do overlay que o criou.

Quando FS_EndOverlay executa, a implementação Nitro percorre a cadeia global e seleciona cada nó para o qual:

- obj está dentro de [ram_address, ram_address + ram_size + bss_size); ou
- obj == 0 e o endereço do destrutor dtor está dentro dessa faixa.

Os nós selecionados são destacados da cadeia antes da invocação. O SDK então chama cada dtor(obj). O processo repete a varredura até não restarem nós pertencentes à faixa do overlay; assim, registros criados durante a destruição também podem ser encontrados.

Referências examinadas:
- [mw_dtor.h — ABI do nó e assinatura](https://github.com/cybervisi0n/libntr/blob/main/libraries/fs/include/mw_dtor.h)
- [fs_overlay.c — filtro e execução em FS_EndOverlay](https://github.com/cybervisi0n/libntr/blob/main/libraries/fs/src/fs_overlay.c)
- [ZeldaRET Spirit Tracks — notas sobre .init e DestructorChain](https://github.com/zeldaret/st/blob/main/docs/decompiling.md)

A documentação de Spirit Tracks é usada apenas como confirmação independente do ABI Metrowerks (nó de 12 bytes e possibilidade de vários objetos por inicializador); seus endereços, símbolos e comportamento específico de jogo não são importados.

## 4. Consequências para o Oracis

O carregador Oracis já opera sobre imagens descomprimidas. Portanto, o adaptador HLE não deve executar BLZ novamente. A API de carga adiada publica a imagem e o BSS após a fatia guest terminar; só depois deve solicitar os construtores pelo caminho supervisionado existente.

**Primeira parte de suporte à cadeia já implementada:** a classe `NdsOverlayDestructorChain` recebe o endereço guest do head global e a faixa de RAM+BSS do overlay, percorre os nós de 12 bytes, detecta ciclos e ponteiros/memória inválidos, aplica o predicado de seleção do NitroSDK, destaca os nós escolhidos preservando a ordem e mantém os demais nós ligados. As escritas têm rollback em caso de falha. Ela não invoca os destrutores e ainda não está ligada ao ciclo de unload da máquina.

**Registro Metrowerks e gate de fatia agora implementados:** `NdsMachine::register_overlay_global_object` encaminha o registro à cadeia guest somente quando o CPU indicado é o dono da fatia ativa, não existe a fatia do outro CPU e o estado global selecionado corresponde àquele CPU. A ponte C expõe:

- `oracis_host_runtime_register_global_destructor(cpu, head, obj, dtor, chain)`: forma explícita dos três argumentos guest;
- `oracis_host_runtime_register_active_global_destructor(head)`: lê `obj`, `dtor` e `chain` de R0, R1 e R2 do CPU ativo.

As duas formas usam o mesmo gate da máquina. Uma chamada na fronteira host ou durante a fatia do outro CPU retorna `OVERLAY_BUSY`; nenhuma API converte endereços guest em ponteiros host. O helper insere o nó de 12 bytes na cabeça da cadeia, validando a cadeia existente e publicando o head somente após os três campos do nó estarem escritos. O código guest não é invocado por esse caminho.

O teste `oracis_nds_overlay_initializer_test` realiza o registro dentro de uma fatia ARM9 e confere `head`, `next`, `dtor` e `obj` na GuestMemory. O teste host `oracis_nds_overlay_catalog_test` confirma que ambas as APIs C recusam registros chamados fora da fatia com `OVERLAY_BUSY`.

A integração com o jogo ainda não resolve automaticamente o endereço USA Rev 1 de `__global_destructor_chain` nem substitui o entrypoint guest de `__register_global_object` por um HLE thunk. O endereço do head deve vir de símbolos/metadados locais verificados; não é fixado no runtime.

**Validação:** CI [#45923102952](https://github.com/CarlosCosta07-s/Oracis/actions/runs/45923102952), commit `8135dda`, passou nos quatro jobs. Desktop Linux e runtime ARM: **68/68 CTests** cada; Python: **99 testes**; NDSRecomp: **6/6**; Android ARM64 aprovado; smokes opcionais de bancos principal e BIOS: **1/1** cada. Os testes exercitam registro sintético e o gate de fatia, não a chamada de ROM retail.

A integração completa necessária para unload continua sendo:

1. identificar, na USA Rev 1, o símbolo/armazenamento guest de `__global_destructor_chain` e o entrypoint de `__register_global_object` a partir de metadados e referências verificadas;
2. ligar o HLE thunk de `__register_global_object` ao endereço/ABI guest verdadeiro, passando o head correto;
3. obter a faixa do overlay carregado e chamar `detach_for_overlay` somente numa fronteira segura do scheduler;
4. executar cada função `dtor(obj)` como chamada guest limitada, com `obj` em R0, retorno/estado ARM-Thumb validado e limite de ciclos;
5. repetir a varredura até não restarem novos nós pertencentes ao overlay, pois um destrutor pode registrar outros objetos;
6. somente então liberar o módulo, depois de confirmar que PC, pilhas, endereços de retorno e demais referências guest não apontam para a faixa liberada.

Não presumir que o vetor de construtores possa ser simplesmente percorrido ao contrário: o SDK associa destrutores a objetos por registros explícitos, e um construtor pode registrar múltiplos objetos. Também não inventar o endereço do head global nem o entrypoint de `__register_global_object`; esses valores ainda precisam de evidência USA Rev 1.

## 5. Estado atual e limite de aceitação

Implementado no Oracis: leitura verificada do catálogo local, carga direta ou adiada de imagem+BSS, parsing da tabela de construtores e execução supervisionada de static-init.

Ainda pendente: interceptar as chamadas NitroSDK guest reais para load/start/end/unload, identificar o ABI/endereço USA Rev 1 do registro de destrutores, executar FS_EndOverlay ou equivalente seguro e comprovar unload sem referências guest pendentes.

Nenhuma ROM, BIOS, imagem, save, banco gerado ou fonte de outro projeto é incluído neste documento ou no Git.
