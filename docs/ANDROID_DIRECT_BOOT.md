# Experimento de entrada nativa no Android

## Objetivo

O host Android pode selecionar uma imagem NDS e uma BIOS ARM7 fornecidas pelo usuário, validar os segmentos do cartucho e tentar iniciar os entrypoints recompilados em código nativo.

Este caminho é um experimento de direct-entry: ele começa nos endereços ARM9/ARM7 do cabeçalho do cartucho e não executa a sequência de power-on das BIOS retail. Portanto, não deve ser descrito como boot completo de hardware/BIOS.

## Seleção de arquivos

O host usa o seletor de arquivos SDL3 no Android para pedir, nesta ordem:

1. Uma imagem .nds escolhida pelo usuário.
2. Uma BIOS ARM7 de exatamente 16 KiB, também escolhida pelo usuário.
3. Um dump da flash SPI de firmware do próprio usuário, entre 1 byte e 16 MiB.

No Android, o seletor pode retornar URIs content://. A leitura dos três arquivos usa SDL_IOStream, sem presumir que a URI seja um caminho POSIX ou passar por fopen. O host limita os tamanhos antes da alocação e copia os dados para os dispositivos próprios.

A BIOS ARM7 é usada atualmente para inicializar a tabela KEY1 e descriptografar o prefixo da Secure Area. A imagem de firmware é conectada ao dispositivo SPI ARM7 para que leituras da flash pelo guest possam retornar bytes reais. O dispositivo SPI atualmente modela leituras (03h), status (05h), WREN (06h) e WRDI (04h); gravação, apagamento, JEDEC ID e outros comandos ainda não são implementados.

Importante: conectar a flash SPI não executa o IPL do firmware. Os blocos de código do firmware permanecem criptografados/comprimidos; a descriptografia, descompressão, validação das partes e cópia para as regiões de boot ainda precisam ser implementadas. O host não carrega nem executa a BIOS ARM7 retail como vetor de reset. A BIOS ARM9 retail também não é carregada ou executada neste experimento.

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

O carregador ainda não reproduz todas as regiões de RAM preenchidas durante o boot, fragmentos de código copiados pela BIOS, campos de identificação/CRC do cartucho, user settings do firmware, configuração completa CP15/MPU/TCM ou a inicialização de todos os periféricos. O cabeçalho NDS é espelhado em 0x027FFE00 quando fornecido ao loader. A ausência dessas outras condições continua sendo um possível bloqueio para Pokémon Platinum.

## Condições para executar código guest

O scheduler nativo só é habilitado quando:

- A imagem NDS foi montada e os segmentos ARM9/ARM7 foram validados antes de serem escritos em GuestMemory.
- A BIOS ARM7 do usuário foi aceita pelo loader de KEY1.
- A imagem SPI de firmware do usuário foi carregada no dispositivo ARM7 SPI.
- Existem bancos estáticos registrados para ARM9 e ARM7.
- O PC inicial de cada CPU resolve para uma entrada compilada no modo ARM/Thumb esperado.

Depois do preflight, o host Android chama oracis_host_runtime_run_round em 512 fatias sequenciais de 256 ciclos de barramento por iteração do loop principal (131.072 ciclos agregados). A granularidade de 256 ciclos preserva os estados transitórios do handshake IPCSYNC entre ARM9 e ARM7, que podem ser perdidos quando um CPU executa um quantum longo antes do outro. Fatias curtas também limitam o crescimento da pilha nativa, pois os dispatches entre funções guest recompiladas são encadeados por chamadas C. Se o scheduler devolver erro, a execução é desabilitada e o host registra o ciclo global, o número da fatia, o código da ponte e o estado da máquina. Não há fallback para interpretador ARM.

A seleção de ROM/BIOS e o preflight não comprovam que o jogo iniciou. Uma sequência de rounds concluída sem erro comprova apenas que o código guest avançou durante aquele orçamento. O primeiro quadro real, a inicialização completa dos overlays e a compatibilidade jogável precisam de evidência separada.

## Dependências ainda abertas

- Os bancos ARM9/ARM7 e os bancos de overlays continuam sendo artefatos locais privados e precisam estar ligados ao build Android. A CI não recebe ROMs, BIOS, segmentos extraídos ou bancos derivados.
- O workflow Android atual compila a biblioteca nativa ARM64; o repositório ainda não contém um projeto Gradle/Manifest que gere um APK instalável.
- O experimento reproduz apenas o subconjunto documentado de registradores/pilhas e POSTFLG; não executa as BIOS retail e não reproduz todas as áreas de RAM/firmware inicializadas pelo boot, o Chip ID, nem a configuração completa de CP15/MPU/TCM e periféricos.
- O host ainda precisa de uma forma Android para fornecer o Chip ID físico do cartucho quando um comando Game Card depender dele. Esse valor não é inferido do dump .nds.
- O firmware SPI bruto já pode ser selecionado e lido pelo dispositivo ARM7, mas o IPL ainda não descriptografa/descompacta as partes 1–5 nem copia os fragmentos para as regiões de RAM de boot.
- A cobertura do protocolo Game Card KEY1/KEY2, dos serviços BIOS/NitroSDK, dos overlays e de MMIO/timing continua parcial. O primeiro dispatch miss ou acesso não implementado deve ser tratado como bloqueio real, não mascarado.

## Política de dados

ROM, BIOS, firmware, segmentos extraídos, overlays extraídos, bancos gerados e saves são fornecidos pelo usuário e permanecem em armazenamento local. Nada disso deve ser adicionado ao Git, à CI ou às releases.