# Experimento de entrada nativa no Android

## Objetivo

O host Android pode selecionar uma imagem NDS e uma BIOS ARM7 fornecidas pelo usuário, validar os segmentos do cartucho e tentar iniciar os entrypoints recompilados em código nativo.

Este caminho é um experimento de direct-entry: ele começa nos endereços ARM9/ARM7 do cabeçalho do cartucho e não executa a sequência de power-on das BIOS retail. Portanto, não deve ser descrito como boot completo de hardware/BIOS.

## Seleção de arquivos

O host usa o seletor de arquivos SDL3 no Android para pedir:

1. Uma imagem .nds escolhida pelo usuário.
2. Uma BIOS ARM7 de exatamente 16 KiB, também escolhida pelo usuário.

No Android, o seletor pode retornar URIs content://. A leitura usa SDL_IOFromFile, SDL_GetIOSize, SDL_SeekIO e SDL_ReadIO, em vez de presumir que a URI é um caminho POSIX ou passar por fopen.

A BIOS ARM7 é usada atualmente para inicializar a tabela KEY1 e descriptografar o prefixo da Secure Area. O host não carrega nem executa a BIOS ARM7 retail como vetor de reset. A BIOS ARM9 retail também não é carregada ou executada neste experimento.

## Condições para executar código guest

O scheduler nativo só é habilitado quando:

- A imagem NDS foi montada e os segmentos ARM9/ARM7 foram validados antes de serem escritos em GuestMemory.
- A BIOS ARM7 do usuário foi aceita pelo loader de KEY1.
- Existem bancos estáticos registrados para ARM9 e ARM7.
- O PC inicial de cada CPU resolve para uma entrada compilada no modo ARM/Thumb esperado.

Depois do preflight, o host Android chama oracis_host_runtime_run_round com orçamento limitado de 131.072 ciclos de barramento por iteração do loop principal. Se o scheduler devolver erro, a execução é desabilitada e o host registra o ciclo global, o código da ponte e o estado da máquina. Não há fallback para interpretador ARM.

A seleção de ROM/BIOS e o preflight não comprovam que o jogo iniciou. Uma sequência de rounds concluída sem erro comprova apenas que o código guest avançou durante aquele orçamento. O primeiro quadro real, a inicialização completa dos overlays e a compatibilidade jogável precisam de evidência separada.

## Dependências ainda abertas

- Os bancos ARM9/ARM7 e os bancos de overlays continuam sendo artefatos locais privados e precisam estar ligados ao build Android. A CI não recebe ROMs, BIOS, segmentos extraídos ou bancos derivados.
- O workflow Android atual compila a biblioteca nativa ARM64; o repositório ainda não contém um projeto Gradle/Manifest que gere um APK instalável.
- O experimento não reproduz o handoff completo das BIOS retail, o estado integral de firmware nem todas as configurações de hardware esperadas no boot.
- O host ainda precisa de uma forma Android para fornecer o Chip ID físico do cartucho quando um comando Game Card depender dele. Esse valor não é inferido do dump .nds.
- A cobertura do protocolo Game Card KEY1/KEY2, dos serviços BIOS/NitroSDK, dos overlays e de MMIO/timing continua parcial. O primeiro dispatch miss ou acesso não implementado deve ser tratado como bloqueio real, não mascarado.

## Política de dados

ROM, BIOS, firmware, segmentos extraídos, overlays extraídos, bancos gerados e saves são fornecidos pelo usuário e permanecem em armazenamento local. Nada disso deve ser adicionado ao Git, à CI ou às releases.