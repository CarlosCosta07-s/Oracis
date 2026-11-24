# Experimento de entrada nativa no Android

## Objetivo

O host Android pode selecionar uma imagem NDS e uma BIOS ARM7 fornecidas pelo usuário, validar os segmentos do cartucho e tentar iniciar os entrypoints recompilados em código nativo.

Este caminho é um experimento de direct-entry: ele começa nos endereços ARM9/ARM7 do cabeçalho do cartucho e não executa a sequência de power-on das BIOS retail. Portanto, não deve ser descrito como boot completo de hardware/BIOS.

## Seleção de arquivos

O host usa o seletor de arquivos SDL3 no Android para pedir, nesta ordem:

1. Uma imagem .nds escolhida pelo usuário.
2. Uma BIOS ARM7 de exatamente 16 KiB, também escolhida pelo usuário.
3. Opcionalmente, uma BIOS ARM9 de exatamente 4 KiB.
4. Um dump da flash SPI de firmware do próprio usuário, entre 1 byte e 16 MiB.
5. Opcionalmente, um arquivo de texto com o ID físico de 32 bits do Game Card,
   em decimal ou hexadecimal com prefixo `0x`. O arquivo deve conter somente
   esse valor, podendo ter espaços ou quebras de linha ao redor. O ID não é
   inferido do dump `.nds`; cancelar essa seleção mantém o experimento de
   entrada direta, mas transações KEY1/KEY2 que dependam do ID podem falhar.

No Android, o seletor pode retornar URIs `content://`. A leitura dos arquivos selecionados usa SDL_IOStream, sem presumir que a URI seja um caminho POSIX ou passar por `fopen`. O host limita os tamanhos antes da alocação e copia os dados para os dispositivos próprios.

**Estado da camada de BIOS:** o dispositivo `NdsBiosControl` agora aceita imagens fornecidas pelo usuário, mapeia a BIOS ARM7 de 16 KiB em `0x00000000` somente para ARM7 e a BIOS ARM9 de 4 KiB em `0xFFFF0000` somente para ARM9. As regiões são somente leitura e executáveis. A ponte C ABI já oferece `oracis_host_runtime_load_arm9_bios()`; porém, o seletor SDL/Android agora solicita esse arquivo como etapa opcional após a BIOS ARM7, e o desktop aceita `--arm9-bios caminho/bios9.bin`. O arquivo deve ter exatamente 4096 bytes.

A proteção ARM7 `BIOSPROT` em `0x04000308` está modelada: começa em zero, aceita uma única escrita de 32 bits, ignora o bit baixo, e uma escrita de `0x1205` fixa o limite em `0x1204`. Depois disso, bytes abaixo do limite só são legíveis quando o PC permanece abaixo do limite; bytes a partir do limite exigem que o PC continue dentro da BIOS ARM7 (`0x00000000–0x00003FFF`). Leituras bloqueadas retornam `0xFF` por byte. Essas regras e as janelas por CPU são cobertas por fixtures sintéticas. O mapeamento ARM9 atualmente cobre os 4 KiB da imagem fornecida; comportamento de qualquer extensão/espelhamento do restante da janela arquitetural não é presumido.

CLI desktop: `--game-card jogo.nds --arm7-bios bios7.bin --arm9-bios bios9.bin --firmware firmware.bin`. A opção `--arm9-bios` também é opcional para o fluxo direct-entry atual.

**Isso ainda não executa as BIOSes.** O ARM7 carregado também fornece a tabela KEY1 no offset `0x30`, usada para a Secure Area. O firmware SPI continua sendo carregado pelo dispositivo ARM7. O reset retail, execução das duas BIOSes, sincronização ARM9/ARM7 e handoff ao IPL continuam pendentes.

A BIOS ARM7 é usada para copiar a tabela KEY1 (offset 0x30, tamanho 0x1048) e inicializar a descriptografia da Secure Area do cartucho. A imagem de firmware é instalada no dispositivo SPI ARM7; o dispositivo modela READ (03h), RDSR (05h), WREN (06h) e WRDI (04h). Programação, apagamento, JEDEC ID e outros comandos SPI ainda não são implementados.

Além de expor a flash ao guest, o host chama `NdsMachine::load_firmware_boot_code()` ao carregar a imagem fornecida. Esse estágio valida o identificador MACP/MACh e o CRC do cabeçalho, descriptografa o bloco ROM-info e as partes IPL 1 (ARM9) e 2 (ARM7) com KEY1, descomprime os dois fluxos LZ77 e confere o CRC16 combinado. Antes de publicar qualquer byte, valida escrita e execução nos dois destinos; se um dos destinos falhar, nenhuma das partes é gravada.

O decodificador também reconhece os payloads secundários 3 (ARM9), 4 (ARM7) e 5 (dados de idioma/interface). Eles usam o formato P345: cada componente declara o tamanho do bloco, o comprimento descomprimido e o offset do fluxo de distâncias; os dados são separados em duas árvores Huffman estáticas (símbolos primários de 9 bits e distâncias de 11 bits), com tokens LZ de janela de 2 KiB. O decoder valida a árvore, limites de bits, alinhamento e referências de cópia antes de aceitar a saída. Os três payloads ficam retidos em buffers de staging pertencentes à máquina e seus CRCs são verificados (CRC combinado de 3/4 e CRC separado de 5). Depois de copiar IPL1/2 para a RAM guest e validar a identidade dos bytes, a máquina descarta os vetores temporários de P1/P2 e conserva apenas seus destinos, tamanhos, metadados ROM-info e CRCs; assim, não duplica em memória host os bytes que já estão na RAM guest. O bridge expõe esses metadados e os tamanhos/resultados de CRCs de P3/P4/P5 para diagnóstico. A partir do CRC16 de IPL1/2 no offset 0x06, o relatório também expõe uma pista de revisão (v1, v2, família de checksum v3, v4, v6/DS Lite ou iQue); valores desconhecidos permanecem como Unknown. Essa pista não autentica o firmware nem distingue builds modificados que compartilhem o checksum. Os bytes P3/P4/P5 permanecem em staging porque serão consumidos por etapas posteriores; ainda não são gravados na RAM guest, pois seus destinos e a sequência retail que os consome não estão implementados. Os testes usam firmware P345 sintético criado durante a execução; nenhum dump real é incluído no repositório.

A publicação de IPL 1/2 e o staging validado de 3/4/5 não executam o IPL, não transferem o PC para os blocos recém-carregados e não executam o reset das BIOS retail. A sequência completa de inicialização do firmware permanece em aberto. A BIOS ARM7 retail não é executada como vetor de reset e a BIOS ARM9 retail não é carregada nem executada neste experimento.

## Estado de entrada reproduzido

Depois de validar e carregar os dois segmentos, `NdsMachine::load_program_segments()` prepara o subconjunto documentado do contexto de entrada do cartucho:

| Estado | ARM9 | ARM7 |
|---|---:|---:|
| R12, LR e PC | entrypoint do cabeçalho | entrypoint do cabeçalho |
| SP User/System | 0x03002F7C | 0x0380FD80 |
| SP IRQ | 0x03003F80 | 0x0380FF80 |
| SP Supervisor | 0x03003FC0 | 0x0380FFC0 |
| R0–R11 | zero | zero |
| LR/SPSR IRQ e SVC | zero | zero |

O CPSR permanece em modo System e respeita o bit ARM/Thumb do entrypoint. O loader também define POSTFLG bit 0 como 1. Esses valores correspondem ao estado de registradores documentado por GBATEK para a entrada do cartucho; isso reproduz o handoff de registradores/pilhas, não a execução das BIOS ou do firmware.

O modelo de POSTFLG também aceita a leitura halfword observada nos helpers de inicialização ARM9 e ARM7: o `LDRH` em 0x04000300 lê o byte POSTFLG na lane baixa e o byte HALTCNT adjacente na lane alta. Como esses caminhos testam somente o bit 0 de POSTFLG, o Oracis retorna o byte POSTFLG zero-extendido; o estado/readback próprio de HALTCNT continua fora desse dispositivo.

O Slot-2 GBA vazio é modelado separadamente do Game Card DS serial: EXMEMCNT bit 7 seleciona ARM9 ou ARM7; a CPU sem posse vê zero; a janela ROM ausente usa o padrão de open bus dependente do primeiro tempo de acesso; a janela SRAM ausente lê `0xFF`. O DMA pode consumir esse mesmo caminho sem criar uma região sintética em GuestMemory.

O probe de entrada direta também observou uma transferência ARM9 para 0x020C2ECC (uma instrução `B` para 0x020C2F18) e, em seguida, uma entrada indireta no início da função ARM9 em 0x020C2F18. As duas entradas adicionais são vinculadas ao SHA-1 do segmento ARM9 USA Rev 1 e permanecem apenas como metadados de recompilação; não há código ou dados de jogo embutidos no repositório.

Depois da leitura DMA do Slot-2 vazio, o probe também observou a chamada ARM9 em 0x020C1978: a rotina lê EXMEMCNT, ativa o bit 7 e devolve a posse do Slot-2 ao ARM7. A entrada estática é registrada separadamente e validada pela mesma identidade SHA-1 do ARM9 USA Rev 1.

O carregador ainda não reproduz todas as regiões de RAM preenchidas durante o boot, fragmentos de código copiados pela BIOS, campos de identificação/CRC do cartucho, user settings do firmware, configuração completa CP15/MPU/TCM ou a inicialização de todos os periféricos. O cabeçalho NDS é espelhado em 0x027FFE00 quando fornecido ao loader. A ausência dessas outras condições continua sendo um possível bloqueio para Pokémon Platinum.

## Condições para executar código guest

O scheduler nativo só é habilitado quando:

- A imagem NDS foi montada e os segmentos ARM9/ARM7 foram validados antes de serem escritos em GuestMemory.
- A BIOS ARM7 do usuário foi aceita pelo loader de KEY1.
- O dump SPI de firmware do usuário foi instalado; IPL 1/2 passaram por KEY1/LZ77/CRC e pré-validação dos dois destinos; quando presentes, os payloads P345 3/4/5 passaram pela descompressão Huffman/LZ e validação de CRC, ainda em staging.
- Existem bancos estáticos registrados para ARM9 e ARM7.
- O PC inicial de cada CPU resolve para uma entrada compilada no modo ARM/Thumb esperado.

Depois do preflight, o host Android chama oracis_host_runtime_run_round em 512 fatias sequenciais de 256 ciclos de barramento por iteração do loop principal (131.072 ciclos agregados). A granularidade de 256 ciclos preserva os estados transitórios do handshake IPCSYNC entre ARM9 e ARM7, que podem ser perdidos quando um CPU executa um quantum longo antes do outro. Fatias curtas também limitam o crescimento da pilha nativa, pois os dispatches entre funções guest recompiladas são encadeados por chamadas C. Se o scheduler devolver erro, a execução é desabilitada e o host registra o ciclo global, o número da fatia, o código da ponte e o estado da máquina. Não há fallback para interpretador ARM.

A seleção dos arquivos e o preflight não comprovam que o jogo iniciou. Uma sequência de rounds concluída sem erro comprova apenas que o código guest avançou durante aquele orçamento. O primeiro quadro real, a inicialização completa dos overlays e a compatibilidade jogável precisam de evidência separada.

## Dependências ainda abertas

- Os bancos ARM9/ARM7 e os bancos de overlays continuam sendo artefatos locais privados e precisam estar ligados ao build Android. A CI não recebe ROMs, BIOS, segmentos extraídos ou bancos derivados.
- O workflow Android atual compila a biblioteca nativa ARM64; o repositório ainda não contém um projeto Gradle/Manifest que gere um APK instalável.
- O experimento reproduz apenas o subconjunto documentado de registradores/pilhas e POSTFLG; não executa as BIOS retail e não reproduz todas as áreas de RAM/firmware inicializadas pelo boot, o Chip ID, nem a configuração completa de CP15/MPU/TCM e periféricos.
- O host Android aceita o Chip ID físico por arquivo de texto opcional. Se a
  seleção for cancelada, o direct-entry continua sem esse dado e comandos do
  Game Card que precisem dele podem falhar. O valor não é inferido do dump .nds.
- As partes 1/2 já são descriptografadas, descomprimidas e publicadas; os payloads 3/4/5 já usam o decoder P345 de duas árvores Huffman e são validados em staging, mas ainda não são instalados na RAM guest. A execução do IPL e a sequência completa de boot continuam pendentes.
- A cobertura do protocolo Game Card KEY1/KEY2, dos serviços BIOS/NitroSDK, dos overlays e de MMIO/timing continua parcial. O primeiro dispatch miss ou acesso não implementado deve ser tratado como bloqueio real, não mascarado.

## Política de dados

ROM, BIOS, firmware, segmentos extraídos, overlays extraídos, bancos gerados e saves são fornecidos pelo usuário e permanecem em armazenamento local. Nada disso deve ser adicionado ao Git, à CI ou às releases.

## Estado novo do reset retail (infraestrutura de máquina)

A máquina agora possui uma transição explícita separada do modo direct-entry:
`start_retail_bios_boot()` exige que as imagens ARM9 (4 KiB) e ARM7 (16 KiB)
do usuário estejam carregadas e que nenhum ciclo de CPU/barramento tenha sido
executado. Ela inicializa o ARM9 em modo Supervisor/ARM, com IRQ/FIQ mascarados,
no vetor alto `0xFFFF0000`, e mantém o ARM7 no estado `ResetHeld`, distinto
de HALT. Interrupções pendentes não acordam um processador em reset.

`release_arm7_from_reset()` é uma operação explícita para o próximo estágio
do host, que deve chamá-la somente após validar o hand-off arquitetural correto;
a operação posiciona o ARM7 em `0x00000000`. Ela ainda **não está ligada ao
IPCSYNC** porque o valor/transição exatos do handshake retail não foram
confirmados. O frontend SDL/Android ainda não inicia esse caminho automaticamente.

Essa etapa é apenas o modelo de estado inicial e a sincronização do scheduler:
não compila nem registra bancos nativos de BIOS, não executa instruções BIOS e
não constitui boot retail. O caminho direct-entry existente permanece separado.


## Ponte C ABI para o caminho BIOS nativo

O host agora dispõe de três operações distintas: `start_retail_bios_boot()`
(preflight dos dois vetores e entrada no estado reset), `run_retail_bios_round()`
(execução nativa sincronizada sem exigir que os segmentos do jogo já estejam
carregados) e `release_arm7_from_reset()` (liberação explícita). O início
não consome ciclos e não altera o estado da máquina quando falta banco, ou
quando um dos vetores não resolve. As linhas de dispatch validadas são
checadas contra os bytes das BIOS mapeadas pelo dispositivo; entradas
sintéticas sem metadados são usadas apenas no teste da ABI.

O teste da ponte C usa BIOS e tabelas de dispatch inteiramente sintéticas:
confirma que a máquina recusa iniciar sem bancos, que ARM9 executa enquanto
ARM7 não recebe fatias, que o relógio comum avança e que ARM7 só entra depois
da liberação explícita. Ele não executa instruções reais das BIOSes, não
confirma hand-off pelo IPCSYNC e não constitui evidência de boot retail. A
interface SDL/Android ainda não seleciona nem registra bancos BIOS do usuário.
