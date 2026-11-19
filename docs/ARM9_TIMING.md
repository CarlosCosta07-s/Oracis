# Timing parcial de fetch, dados e refill do Nintendo DS

**Estado:** o timing implementado cobre fetch uncached ARM9, custos-base NDS/DATA em classes estáticas e refill ARM9/ARM7 num subconjunto de regiões documentadas. Cache, contenção e wait states dinâmicos continuam fora do modelo; isto não permite executar o jogo inteiro.

## Contrato da ABI

`runtime_code_cycles(pc)` é o custo de fetch da instrução ARM9 atual. O helper usa CPSR.T e trabalha em ciclos do core ARM9 (aproximadamente 66 MHz), enquanto as tabelas NDS usam ciclos do barramento (aproximadamente 33 MHz). O core ARM9 opera a duas vezes o clock do barramento.

ARM7 retorna zero em `runtime_code_cycles` porque o custo de fetch do ARM7 já está incluído no custo-base das instruções geradas. Isso não significa que fetch ARM7 tenha custo físico zero.

`arm9_refill_cycles(target)` e `arm7_refill_cycles(target)` recebem o destino com o bit 0 indicando Thumb. Ambos contam os fetches sequenciais do pipeline no destino, em vez de retornar um valor fixo independente da região.

## Fetch de instruções ARM9

| Região guest | ARM | Thumb: primeira halfword | Thumb: segunda halfword |
|---|---:|---:|---:|
| ITCM ativa (janela CP15 publicada) | 1 | 1 | 0 |
| Main RAM uncached (`0x02000000–0x02FFFFFF`) | 18 | 18 | 0 |
| WRAM (`0x03000000–0x03FFFFFF`) | 8 | 8 | 0 |
| I/O (`0x04000000–0x04000FFF`) | 8 | 8 | 0 |
| Palette/VRAM (`0x05000000–0x06FFFFFF`) | 10 | 10 | 0 |
| OAM (`0x07000000–0x07003FFF`) | 8 | 8 | 0 |
| BIOS ARM9 (`0xFFFF0000–0xFFFF7FFF`) | 8 | 8 | 0 |

A tabela segue NDS9/CODE: Main RAM N32=9, WRAM/BIOS/I/O/OAM N32=4 e Palette/VRAM N32=5 ciclos de barramento. Esses valores são multiplicados por dois para ciclos core ARM9. ITCM usa 0,5 ciclo de barramento, isto é, 1 ciclo core ARM9.

Em Thumb, duas instruções de 16 bits compartilham um fetch de 32 bits: o custo N32 completo é atribuído à primeira halfword e a segunda recebe zero adicional. Isso segue o contrato `NDS_ARM9_CODE_K(pc, thumb)` do compilador fixado: a halfword com `pc & 2` retorna o sentinel zero.

A janela ITCM publicada pelo controlador CP15 varia de 4 KB a 32 MB. Quando habilitada, o timing reconhece o espelho programado, incluindo a janela de 32 MB que contém `0x01FF8000`; a configuração é atualizada ao escrever Control/c9 e limpa no shutdown da máquina.

## Refill de pipeline ARM9

O refill ARM9 conta os fetches de destino em ciclos core ARM9. Para código, NDS9/CODE força S32 a ser igual a N32; cada palavra usa, portanto, o custo da tabela anterior.

| Região de código | Destino ARM | Thumb alinhado a 4 bytes | Thumb na segunda halfword |
|---|---:|---:|---:|
| ITCM ativa | 2 | 1 | 2 |
| Main RAM uncached | 36 | 18 | 36 |
| WRAM, BIOS, I/O, OAM | 16 | 8 | 16 |
| Palette/VRAM | 20 | 10 | 20 |

Um destino ARM exige duas palavras de 32 bits. Em Thumb, um destino alinhado à palavra coloca as duas primeiras halfwords em um único fetch de 32 bits; um destino com `address & 2` exige dois fetches de 32 bits. O provider soma cada palavra separadamente, inclusive se o refill atravessar uma fronteira de região. Gamecard, regiões cacheadas e outros destinos sem modelo continuam fail-closed.

## Refill de pipeline ARM7

O ARM7TDMI refaz o pipeline por dois fetches sequenciais após a instrução de branch. O provider conta custos S32 em ARM e S16 em Thumb, na escala de clock de 33 MHz do ARM7.

| Região de código | Destino ARM (2 × S32) | Destino Thumb (2 × S16) |
|---|---:|---:|
| Main RAM | 4 | 2 |
| WRAM, BIOS, I/O, OAM | 2 | 2 |
| Palette/VRAM | 4 | 2 |

Os custos por fetch vêm da tabela NDS7/CODE: Main RAM S32=2/S16=1; WRAM/BIOS/I/O/OAM S32=S16=1; Palette/VRAM S32=2/S16=1. O provider classifica cada fetch isoladamente, inclusive quando cruza a fronteira entre Main RAM e WRAM. Wi-Fi e Gamecard não são aceitos porque seus wait states dependem de configuração externa.

## Timing de acesso a dados

`runtime_mem_cycles(addr,width,sequential)` usa as colunas N32/S32/N16/S16 da tabela NDS/DATA. `width` aceita 1, 2 ou 4 bytes: 8/16-bit usam N16/S16 e 32-bit usam N32/S32. Os custos ARM9 são convertidos do barramento de 33 MHz para ciclos core de 66 MHz; ARM7 já opera na escala de 33 MHz.

| CPU / região | N32 | S32 | N16 | S16 |
|---|---:|---:|---:|---:|
| ARM7 Main RAM | 10 | 2 | 9 | 1 |
| ARM9 Main RAM | 20 | 4 | 18 | 2 |
| ARM7 WRAM/BIOS/I/O/OAM | 1 | 1 | 1 | 1 |
| ARM9 WRAM/BIOS/I/O/OAM | 8 | 2 | 8 | 2 |
| ARM7 Palette/VRAM | não estabelecido | 2 | 1 | 1 |
| ARM9 Palette/VRAM | 10 | 4 | 8 | 2 |
| ARM9 ITCM/DTCM normal | 1 | 1 | 1 | 1 |

A tabela usa os custos-base documentados de NDS7/DATA e NDS9/DATA. Main RAM abrange a janela guest `0x02000000–0x02FFFFFF`; WRAM, BIOS, a janela de I/O suportada (`0x04000000–0x04000FFF`) e OAM usam as classes de barramento de 32-bit; Palette/VRAM usa sua classe de 16-bit. TCM normal custa um ciclo core ARM9 por acesso.

**Limitação da ABI:** o contrato fixado de `runtime_mem_cycles` só recebe endereço, largura e flag sequencial; não informa se a operação é load ou store. A implementação aplica os custos-base DATA uniformemente e não tenta inventar diferenças de write-buffer ou de direção. Quando ITCM/DTCM está em load mode, leituras são encaminhadas à memória externa enquanto stores continuam alimentando o TCM privado; como a ABI não informa direção, qualquer acesso na janela TCM em load mode falha explicitamente. O snapshot de regiões e modos CP15 é publicado pela `NdsMachine` a cada alteração em Control/c9.

Gamecard/GBA slot e Wi-Fi dependem de EXMEMCNT/WIFIWAITCNT e continuam fail-closed. O N32 não sequencial ARM7 de VRAM também permanece bloqueado porque o valor da tabela é incerto. Contenção com PPU, wait states de cache e write-buffer não estão modelados. Isto é timing-base por classe de região, não um modelo de arbitragem global do barramento.

## Limites deliberados

Os seguintes helpers continuam fail-closed:
- `runtime_mul_cycles`: multiplicação e early termination;
- `arm7_cycle_combine`: combinação de fetch/dados/ciclos internos ARM7;
- fetch de código em regiões cacheadas e Gamecard, além de wait states dinâmicos não representados no modelo atual.

O timing parcial não declara precisão temporal global, boot funcional ou compatibilidade jogável.

## Referências técnicas

- ARM946E-S Technical Reference Manual, branch/PC write e fetch pipeline: https://documentation-service.arm.com/static/5e8e3ee588295d1e18d353cf
- ARM7TDMI Technical Reference Manual, branch, branch-and-exchange e ciclos sequenciais: https://documentation-service.arm.com/static/5e8e1323fd977155116a3129
- GBATEK, Nintendo DS Memory Timings, clocks e NDS7/NDS9 CODE/DATA tables: https://problemkaputt.de/gbatek.htm
