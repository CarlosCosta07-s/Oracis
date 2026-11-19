# Timing parcial de busca de instruções ARM9

**Estado:** implementado apenas para fetch uncached de um subconjunto de regiões. Não é o modelo de timing completo do Nintendo DS e não permite executar o jogo inteiro.

## Contrato da ABI

O codegen chama `runtime_code_cycles(pc)` no caminho de busca de instruções. O helper usa a CPU ativa e, no ARM9, CPSR.T para distinguir ARM e Thumb. Os valores são expressos em ciclos do core ARM9 (aproximadamente 66 MHz), enquanto as tabelas de acesso ao barramento usam ciclos de aproximadamente 33 MHz.

O helper retorna zero para ARM7 porque o custo de fetch do ARM7 já está incluído no custo-base das instruções geradas para esse core. Isso não significa que fetch ARM7 tenha custo físico zero.

## Regiões cobertas

| Região guest | Fetch ARM | Fetch Thumb (primeira metade) | Segunda metade Thumb |
|---|---:|---:|---:|
| ITCM ativa (janela CP15 publicada) | 1 | 1 | 0 |
| Main RAM uncached (`0x02000000–0x02FFFFFF`) | 18 | 18 | 0 |
| WRAM (`0x03000000–0x03FFFFFF`) | 8 | 8 | 0 |
| I/O (`0x04000000–0x04FFFFFF`) | 8 | 8 | 0 |
| OAM (`0x07000000–0x07003FFF`) | 8 | 8 | 0 |
| BIOS ARM9 (`0xFFFF0000–0xFFFF7FFF`) | 8 | 8 | 0 |

A janela ITCM publicada pelo controlador CP15 pode variar de 4 KB a 32 MB; quando habilitada, o timing reconhece o espelho programado, incluindo a janela de 32 MB que contém `0x01FF8000`. A configuração é atualizada ao escrever Control/c9 e limpa no shutdown da máquina.

A tabela usa a razão de clock de 2:1 do ARM9 em relação ao barramento: Main RAM N32=9 bus cycles torna-se 18 core cycles; WRAM/BIOS/I/O/OAM N32=4 torna-se 8; uma hit de ITCM de meio ciclo do barramento equivale a 1 core cycle ARM9. Em Thumb, duas instruções de 16 bits partilham um fetch de 32 bits: o custo N32 completo é atribuído à primeira halfword e a segunda recebe zero adicional. Isso segue o contrato de `NDS_ARM9_CODE_K(pc, thumb)` do compilador fixado: a halfword com `pc & 2` retorna o sentinel zero, enquanto a primeira mantém a classe de timing integral. ITCM é a exceção de granularidade: o fetch custa 1 ciclo ARM9 e a segunda metade continua sem custo adicional.

## Limites deliberados

`runtime_code_cycles` falha explicitamente fora das regiões acima. Ainda não cobre código executado a partir de VRAM/paleta, Gamecard, regiões cacheadas, influência do I-cache ou outras configurações de MPU/cache.

Também continuam fail-closed:
- `runtime_mem_cycles`: custos de leitura/escrita de dados e waits de cada região;
- `runtime_mul_cycles`: tempos de multiplicação e early termination;
- `arm9_refill_cycles` / `arm7_refill_cycles`: pipeline refill em branches e mudanças de PC;
- `arm7_cycle_combine`: combinação dos custos de dados/instrução no ARM7.

O fetch ARM9 parcial não declara boot funcional, precisão global de timing ou compatibilidade jogável.

## Referências técnicas

- GBATEK, Nintendo DS memory map e Memory Timings: https://problemkaputt.de/gbatek.htm
- ARM946E-S Technical Reference Manual: https://documentation-service.arm.com/static/5e8e3ee588295d1e18d3aa82
