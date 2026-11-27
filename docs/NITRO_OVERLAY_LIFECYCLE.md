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

A próxima camada necessária para unload é suporte explícito ao contrato MWiDestructorChain:

1. identificar, na USA Rev 1, o símbolo/armazenamento guest de __global_destructor_chain e o entrypoint de __register_global_object a partir de metadados e referências verificadas;
2. preservar o ABI guest de 32 bits: next, dtor, obj, cada um com 4 bytes;
3. validar que cada nó pertence a um overlay carregado e que o destino do destrutor é executável no modo ARM/Thumb correto;
4. ao finalizar um overlay, selecionar pelo mesmo predicado de faixa usado pelo NitroSDK, destacar os nós antes de invocar os destrutores e respeitar a ordem da cadeia;
5. executar cada destrutor como chamada guest limitada, com obj em R0, retorno/estado ARM-Thumb validado e limite de ciclos;
6. somente então liberar o módulo, depois de confirmar que PC, pilhas, endereços de retorno e demais referências guest não apontam para a faixa liberada.

Não presumir que o vetor de construtores possa ser simplesmente percorrido ao contrário: o SDK associa destrutores a objetos por registros explícitos, e um construtor pode registrar múltiplos objetos. Também não inventar o endereço do head global nem o entrypoint de __register_global_object; esses valores ainda precisam de evidência USA Rev 1.

## 5. Estado atual e limite de aceitação

Implementado no Oracis: leitura verificada do catálogo local, carga direta ou adiada de imagem+BSS, parsing da tabela de construtores e execução supervisionada de static-init.

Ainda pendente: interceptar as chamadas NitroSDK guest reais para load/start/end/unload, identificar o ABI/endereço USA Rev 1 do registro de destrutores, executar FS_EndOverlay ou equivalente seguro e comprovar unload sem referências guest pendentes.

Nenhuma ROM, BIOS, imagem, save, banco gerado ou fonte de outro projeto é incluído neste documento ou no Git.
