# Timing parcial de fetch, dados e refill do Nintendo DS

**Estado:** o timing implementado cobre fetch uncached ARM9, custos-base NDS/DATA em classes estáticas e refill ARM9/ARM7 num subconjunto de regiões documentadas. Cache, contenção e wait states dinâmicos continuam fora do modelo; isto não permite executar o jogo inteiro.

## Contrato da ABI

`runtime_code_cycles(pc)` retorna o custo de fetch ARM9 em ciclos de core (aproximadamente 66 MHz). Para ARM7, a chamada retorna zero — o custo é reconstruído no fechamento da instrução — mas registra PC e estado Thumb para `arm7_cycle_combine`. As tabelas NDS usam ciclos do barramento (aproximadamente 33 MHz); o ARM9 opera a duas vezes esse clock.

ARM7 retorna zero em `runtime_code_cycles` porque o custo de fetch é corrigido uma única vez por `arm7_cycle_combine`, substituindo o fetch sequencial 1S embutido no custo-base pelo custo regional correto. O retorno zero não significa custo físico zero.

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
| VRAM C/D atribuída como Work RAM ARM7 | 4 | 2 |

Os custos por fetch vêm da tabela NDS7/CODE: Main RAM S32=2/S16=1; WRAM/BIOS/I/O/OAM S32=S16=1; VRAM S32=2/S16=1. A Palette RAM (`0x05000000`) não faz parte do mapa ARM7. Somente VRAM C/D podem ser atribuídas ao ARM7 como Work RAM, cada banco com 128 KB no intervalo `0x06000000–0x0603FFFF`. `NdsVramControl` aplica a visibilidade real: `VRAMCNT_C/D` precisam ter enable=1 e MST=2; OFS.0 seleciona `0x06000000` ou `0x06020000`, enquanto OFS.1 não é mapeado. `VRAMSTAT` informa enable+MST e não reflete OFS. Em MST=0, os nove bancos A-I têm aliases LCDC fixos acessíveis pelo ARM9: A-D usam `0x06800000`, `0x06820000`, `0x06840000`, `0x06860000` (128 KiB cada); E usa `0x06880000` (64 KiB); F/G usam `0x06890000`/`0x06894000` (16 KiB cada); H usa `0x06898000` (32 KiB); I usa `0x068A0000` (16 KiB). OFS não participa do destino LCDC. Os aliases têm máscaras CPU atualizadas pelo controlador; se C e D disputarem o mesmo slot ARM7, a escrita é rejeitada e o mapeamento anterior é preservado. O helper de timing classifica a faixa potencial, mas a leitura/escrita só é possível quando o controlador habilita o alias. Cada fetch é classificado isoladamente, inclusive ao cruzar fronteiras, e endereços fora dessa janela falham fechado. Wi-Fi e Gamecard continuam sem suporte por dependerem de wait states configuráveis.

## Timing de acesso a dados

`runtime_mem_cycles(addr,width,sequential)` usa as colunas N32/S32/N16/S16 da tabela NDS/DATA. `width` aceita 1, 2 ou 4 bytes: 8/16-bit usam N16/S16 e 32-bit usam N32/S32. Os custos ARM9 são convertidos do barramento de 33 MHz para ciclos core de 66 MHz; ARM7 já opera na escala de 33 MHz.

| CPU / região | N32 | S32 | N16 | S16 |
|---|---:|---:|---:|---:|
| ARM7 Main RAM | 10 | 2 | 9 | 1 |
| ARM9 Main RAM | 20 | 4 | 18 | 2 |
| ARM7 WRAM/BIOS/I/O/OAM | 1 | 1 | 1 | 1 |
| ARM9 WRAM/BIOS/I/O/OAM | 8 | 2 | 8 | 2 |
| ARM7 VRAM C/D como Work RAM | não estabelecido | 2 | 1 | 1 |
| ARM9 Palette/VRAM | 10 | 4 | 8 | 2 |
| ARM9 ITCM/DTCM normal | 1 | 1 | 1 | 1 |

A tabela usa os custos-base documentados de NDS7/DATA e NDS9/DATA. Main RAM abrange `0x02000000–0x02FFFFFF`; WRAM, BIOS, a janela de I/O suportada (`0x04000000–0x04000FFF`) e OAM usam as classes de barramento de 32-bit. No ARM9, Palette/VRAM usa sua classe de 16-bit; no ARM7, somente VRAM C/D atribuída como Work RAM usa a classe VRAM, na janela potencial `0x06000000–0x0603FFFF`. TCM normal custa um ciclo core ARM9 por acesso.

**Limitação da ABI:** o contrato fixado de `runtime_mem_cycles` só recebe endereço, largura e flag sequencial; não informa se a operação é load ou store. A implementação aplica os custos-base DATA uniformemente e não tenta inventar diferenças de write-buffer ou de direção. Quando ITCM/DTCM está em load mode, leituras são encaminhadas à memória externa enquanto stores continuam alimentando o TCM privado; como a ABI não informa direção, qualquer acesso na janela TCM em load mode falha explicitamente. O snapshot de regiões e modos CP15 é publicado pela `NdsMachine` a cada alteração em Control/c9.

Gamecard/GBA slot e Wi-Fi dependem de EXMEMCNT/WIFIWAITCNT e continuam fail-closed. O N32 não sequencial ARM7 para VRAM C/D também permanece bloqueado porque o valor da tabela é incerto. Palette RAM não é mapeada no ARM7. Contenção com PPU, wait states de cache e write-buffer não estão modelados. Isto é timing-base por classe de região, não um modelo de arbitragem global do barramento.

## Combinação de ciclos ARM7

`arm7_cycle_combine(flat_cycles,num_data,is_load,has_internal)` reconstrói a combinação AddCycles_CD/CDI do ARM7TDMI. O prologue gerado chama `runtime_code_cycles(pc)` em cada instrução; essa chamada guarda PC e CPSR.T. Cada `runtime_mem_cycles` acumula seu custo bruto em `num_data` e registra o endereço do último acesso. A combinação é aplicada uma única vez por instrução, inclusive para LDM/STM.

As tabelas de código e dados são diferentes: em Main RAM, NDS7/CODE usa N32=9/N16=8/S32=2/S16=1; NDS7/DATA usa N32=10/N16=9/S32=2/S16=1. O ARM7 usa NDS7/CODE para `num_code` e o valor acumulado dos acessos NDS7/DATA para `num_data`.

Sem acesso a dados, o helper substitui o 1S embutido no custo-base pelo fetch S da região em instruções C, ou pelo fetch N quando `has_internal` indica uma instrução CI. Com acesso a dados:

- CODE Main RAM + DATA Main RAM: retorna `num_code + num_data`.
- Somente DATA Main RAM: aplica o ajuste de leitura em `num_code` e calcula o maior entre os dois lados e a soma com três ciclos de sobreposição removidos.
- Somente CODE Main RAM: aplica o ajuste CDI em `num_data` e usa a mesma regra de sobreposição.
- Nenhum lado em Main RAM: soma CODE + DATA e acrescenta um ciclo para instruções de leitura.

As classes de CODE aceitas são Main RAM, BIOS ARM7, WRAM, os primeiros 4 KB de I/O, OAM e a faixa de VRAM C/D atribuída como Work RAM. Para esta última, `NdsVramControl` valida enable+MST=2 e OFS=0/1 antes de tornar o alias visível ao ARM7; colisões de C/D no mesmo slot são rejeitadas de forma transacional. Palette RAM, Gamecard, Wi-Fi e endereços não modelados continuam fail-closed. O controlador ainda não implementa os destinos MST diferentes de LCDC e Work RAM C/D (BG/OBJ, motores A/B, texturas, extended palettes ou paletas), nem a renderização/PPU.

## Multiply ARM7TDMI

`runtime_mul_cycles(rs_value,signed_variant,extra)` calcula a espera dependente do operando do ARM7TDMI. A parte variável `m` é determinada pelos bytes altos do multiplicador:

| Condição em `rs_value` | `m` |
|---|---:|
| bits 31:8 são todos zero; ou signed e todos um | 1 |
| bits 31:16 são todos zero; ou signed e todos um | 2 |
| bits 31:24 são todos zero; ou signed e todos um | 3 |
| qualquer outro caso | 4 |

Para operações unsigned, somente a extensão com zeros reduz `m`. Para operações signed, a extensão com uns também conta. O resultado retornado é `m + extra`; o codegen fixado passa `extra=0` para MUL, `1` para MLA/UMULL/SMULL e `2` para UMLAL/SMLAL. O Thumb MUL passa `signed_variant=0`. O ARM9 retorna zero nesse helper porque sua contribuição interna é contabilizada estaticamente por `arm9_internal_cost` e `arm9_cycle_combine`.

## Limites deliberados

Permanecem fail-closed:
- fetch de código em regiões cacheadas e Gamecard;
- Gamecard/GBA slot, Wi-Fi, N32 ARM7 VRAM não sequencial e outras regiões dinâmicas não representadas no modelo atual.


O timing parcial não declara precisão temporal global, boot funcional ou compatibilidade jogável.

## Referências técnicas

- ARM946E-S Technical Reference Manual, branch/PC write e fetch pipeline: https://documentation-service.arm.com/static/5e8e3ee588295d1e18d353cf
- ARM7TDMI Technical Reference Manual, branch, branch-and-exchange e ciclos sequenciais: https://documentation-service.arm.com/static/5e8e1323fd977155116a3129
- GBATEK, Nintendo DS Memory Timings, clocks e NDS7/NDS9 CODE/DATA tables: https://problemkaputt.de/gbatek.htm
