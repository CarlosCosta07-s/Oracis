# GuestMemory — contrato de regiões de memória

**Estado:** RAM guest configurável, wrappers C bus_read_*/bus_write_* e validação de identidade implementados. Ainda não é o mapa físico de memória do Nintendo DS e não possui MMIO. A execução dos testes atuais ainda não foi confirmada.

## 1. Objetivo

GuestMemory fornece uma camada de armazenamento de endereços guest para o futuro barramento Oracis. Ela não reproduz sozinha a semântica completa do barramento DS.

Responsabilidades atuais:
- mapear regiões em um espaço guest de 32 bits;
- rejeitar mapeamentos vazios, sobrepostos ou que ultrapassem o espaço de endereços;
- mapear aliases explícitos para o mesmo backing;
- impor permissões de leitura, escrita e execução;
- executar acessos little-endian de 8, 16 e 32 bits;
- retornar erros para endereços não mapeados, overflow e acesso que cruza o limite de uma região.

## 2. API principal

Header: include/oracis/memory/guest_memory.hpp  
Implementação: src/memory/guest_memory.cpp

ABI C: include/oracis/memory/guest_memory_abi.h e src/memory/bus_abi.cpp

- map_ram(base, size, permissions, name): aloca armazenamento próprio, inicialmente vazio, e mapeia uma região.
- map_image(base, contents, size, permissions, name): copia bytes fornecidos pelo carregador e marca a faixa como materializada.
- map_alias(alias_base, canonical_base, size, name): cria outra faixa guest que compartilha exatamente os bytes de uma região já mapeada.
- read(cpu, address, width): leitura de 1, 2 ou 4 bytes.
- write(cpu, address, width, value): escrita de 1, 2 ou 4 bytes.
- validate_execute(cpu, address, size): verifica mapeamento contínuo e permissão de execução.
- validate_identity(cpu, address, size, expected): exige bytes materializados e compara a faixa com os bytes esperados.

As regiões são half-open: [base, base + size). O limite superior pode alcançar 0x1_0000_0000, mas nunca ultrapassá-lo. Aliases herdam as permissões do intervalo canônico.

## 3. Ponte C ABI usada pelos bancos

O runtime oferece os símbolos:
- bus_read_u8 / bus_read_u16 / bus_read_u32;
- bus_write_u8 / bus_write_u16 / bus_write_u32.

O host cria e mantém vivo um objeto GuestMemory, depois o registra com oracis_bus_bind_memory(&memory). O objeto não é transferido nem destruído pela biblioteca; o host deve mantê-lo vivo até chamar oracis_bus_unbind_memory(&memory).

Cada wrapper usa g_nds_active para encaminhar o acesso como ARM9 ou ARM7. O armazenamento é compartilhado quando ambos os processadores acessam a mesma região mapeada. Erros de região, permissão ou largura encerram a execução com log de CPU, endereço, largura, PC e código de erro; nenhum valor zero é inventado como fallback.

O wrapper executa apenas a transferência little-endian da largura solicitada. A inspeção do codegen ARMv5TE mostrou que instruções LDR/STR de palavra/halfword alinham o endereço antes de chamar bus_* e aplicam a rotação/sign-extension na função gerada. Por isso o wrapper não aplica uma segunda rotação ou correção ARM.

A ligação global é serial/cooperativa nesta etapa. O scheduler deverá selecionar a CPU correta antes de chamar um banco e não pode executar os wrappers em paralelo sem uma futura proteção por contexto.

## 4. Códigos de erro

- InvalidCpu: identificador diferente de ARM9/ARM7.
- InvalidWidth: largura diferente de 1, 2 ou 4 bytes.
- InvalidRegion: tamanho zero, nome vazio, permissões inválidas ou intervalo fora do espaço guest.
- AddressOverflow: o acesso extrapola 32 bits.
- Overlap: novo mapeamento cruza outro mapeamento existente.
- Unmapped: início do acesso ou alvo canônico não está mapeado.
- CrossesRegion: o acesso começa em uma região, mas termina fora dela.
- PermissionDenied: leitura, escrita ou execução não permitida.

## 5. Proveniência e identidade de código

Cada backing mantém um marcador de proveniência por byte.
- \`map_ram\` cria bytes vazios com proveniência zero.
- \`write\` (incluindo os wrappers C \`bus_write_*\`) marca os bytes atingidos como materializados.
- \`map_image\` copia os bytes fonte e marca toda a faixa como materializada.
- \`validate_identity\` recusa qualquer byte sem proveniência e só então compara o conteúdo.

Isso evita que uma região recém-alocada, preenchida inicialmente com zero, seja confundida com um banco compilado que também espera zeros. Para código, o callback do dispatcher ainda exige \`Executable\` na faixa principal; depois valida conteúdo e dependências. O callback é instalado pelo bind do objeto GuestMemory na ABI do barramento e removido no unbind.

O marcador de proveniência ainda é simples: não representa gerações de páginas, DMA, escrita por dispositivo, mapeamentos por CPU ou invalidação de cache de instruções. Esses casos precisarão de um modelo de proveniência/timing mais completo no barramento DS.

## 6. Semântica deliberadamente fora desta camada

O barramento de hardware deve permanecer responsável por:
- MMIO e efeitos colaterais de registradores;
- DMA, IPC e periféricos;
- latência e custo de ciclos por CPU/região;
- mirrors reais do Nintendo DS, definidos por região e dispositivo;
- regras de alinhamento/rotação específicas de cada instrução ARM;
- comportamento de regiões não mapeadas que dependa da CPU ou do dispositivo.

GuestMemory não aplica automaticamente rotação ARM a leituras desalinhadas. A camada CPU deverá decidir a transformação arquitetural e então solicitar as transferências de bytes apropriadas.

O parâmetro CPU valida que a chamada pertence a ARM9 ou ARM7, mas as regiões mapeadas são atualmente compartilhadas. Mapas separados por processador, privilégios e overlays executáveis ainda não existem.

## 7. Testes

O teste tests/memory/guest_memory_test.cpp verifica:
1. leitura/escrita little-endian de 8/16/32 bits;
2. backing compartilhado entre ARM9 e ARM7;
3. alias de região e visibilidade bidirecional;
4. permissões de leitura/escrita/execução;
5. rejeição de overlaps e intervalos inválidos;
6. leituras não mapeadas e acessos que cruzam a fronteira;
7. larguras e CPUs inválidas;
8. overflow de endereço guest.

O teste tests/memory/guest_memory_bus_abi_test.cpp verifica o bind/unbind do backing, chamadas C bus_* e compartilhamento de RAM quando o seletor muda entre ARM9 e ARM7.

A biblioteca é construída como oracis_memory mesmo com BUILD_TESTING=OFF; ela depende de oracis_arm_state e o host SDL3 a linka. O CMake registra testes da memória, barramento e validação de identidade, mas não há confirmação de execução para os commits mais recentes.

## 8. Próxima integração

1. Definir o mapa de regiões DS usado pela revisão USA Rev 1, com fontes verificáveis para cada mirror/alias.
2. Adicionar regiões backing compartilhadas para main RAM e WRAM e regiões independentes para ITCM/DTCM/VRAM conforme o modelo escolhido.
3. Implementar uma camada MMIO por intervalo com handlers específicos, sem tratar registradores como RAM comum.
4. Completar proveniência para DMA/dispositivos e gerações de página.
5. Adicionar MMIO e handlers de dispositivo por faixa, mantendo RAM e I/O separados.
6. Modelar latência/ciclos e efeitos colaterais dos acessos por CPU.
7. Adicionar testes de mirrors reais, alinhamento, timing e I/O antes de ligar os bancos reais.
