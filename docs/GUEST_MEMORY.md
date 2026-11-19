# GuestMemory — contrato de regiões de memória

**Estado:** abstração de RAM configurável implementada e testada. Ainda não é o mapa físico de memória do Nintendo DS e não exporta os símbolos bus_read_* / bus_write_*.

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

- map_ram(base, size, permissions, name): aloca armazenamento próprio e mapeia uma região.
- map_alias(alias_base, canonical_base, size, name): cria outra faixa guest que compartilha exatamente os bytes de uma região já mapeada.
- read(cpu, address, width): leitura de 1, 2 ou 4 bytes.
- write(cpu, address, width, value): escrita de 1, 2 ou 4 bytes.
- validate_execute(cpu, address, size): verifica mapeamento contínuo e permissão de execução.

As regiões são half-open: [base, base + size). O limite superior pode alcançar 0x1_0000_0000, mas nunca ultrapassá-lo. Aliases herdam as permissões do intervalo canônico.

## 3. Códigos de erro

- InvalidCpu: identificador diferente de ARM9/ARM7.
- InvalidWidth: largura diferente de 1, 2 ou 4 bytes.
- InvalidRegion: tamanho zero, nome vazio, permissões inválidas ou intervalo fora do espaço guest.
- AddressOverflow: o acesso extrapola 32 bits.
- Overlap: novo mapeamento cruza outro mapeamento existente.
- Unmapped: início do acesso ou alvo canônico não está mapeado.
- CrossesRegion: o acesso começa em uma região, mas termina fora dela.
- PermissionDenied: leitura, escrita ou execução não permitida.

## 4. Semântica deliberadamente fora desta camada

O barramento de hardware deve permanecer responsável por:
- MMIO e efeitos colaterais de registradores;
- DMA, IPC e periféricos;
- latência e custo de ciclos por CPU/região;
- mirrors reais do Nintendo DS, definidos por região e dispositivo;
- regras de alinhamento/rotação específicas de cada instrução ARM;
- comportamento de regiões não mapeadas que dependa da CPU ou do dispositivo.

GuestMemory não aplica automaticamente rotação ARM a leituras desalinhadas. A camada CPU deverá decidir a transformação arquitetural e então solicitar as transferências de bytes apropriadas.

O parâmetro CPU valida que a chamada pertence a ARM9 ou ARM7, mas as regiões mapeadas são atualmente compartilhadas. Mapas separados por processador, privilégios e overlays executáveis ainda não existem.

## 5. Testes

O teste tests/memory/guest_memory_test.cpp verifica:
1. leitura/escrita little-endian de 8/16/32 bits;
2. backing compartilhado entre ARM9 e ARM7;
3. alias de região e visibilidade bidirecional;
4. permissões de leitura/escrita/execução;
5. rejeição de overlaps e intervalos inválidos;
6. leituras não mapeadas e acessos que cruzam a fronteira;
7. larguras e CPUs inválidas;
8. overflow de endereço guest.

A biblioteca é construída como oracis_memory mesmo com BUILD_TESTING=OFF; o host SDL3 também a linka. A validação local foi feita em harness CMake/Ninja com Clang, não no build completo com SDL3.

## 6. Próxima integração

1. Definir o mapa de regiões DS usado pela revisão USA Rev 1, com fontes verificáveis para cada mirror/alias.
2. Adicionar regiões backing compartilhadas para main RAM e WRAM e regiões independentes para ITCM/DTCM/VRAM conforme o modelo escolhido.
3. Implementar uma camada MMIO por intervalo com handlers específicos, sem tratar registradores como RAM comum.
4. Exportar bus_read_u8/u16/u32 e bus_write_u8/u16/u32 na ABI C dos bancos.
5. Adicionar testes de acesso por CPU, alinhamento, mirrors e efeitos de I/O antes de ligar o dispatch.
