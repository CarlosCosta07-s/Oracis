<!-- Oracis project page -->
<p align="center">
  <img src="assets/icon/oracis.svg" alt="Ícone do Oracis: estrela de platina e anel celeste" width="160">
</p>

<h1 align="center">Oracis</h1>

<p align="center">Um projeto independente de port nativo de Pokémon Platinum para plataformas modernas.</p>

<p align="center"><strong>Reconstruindo Sinnoh para além do hardware original.</strong></p>

> **Status: runtime ARM, RAM guest e dispatch estático inicial implementados; integração com bancos reais pendente.** O NDSRecomp ainda não foi compilado/validado nesta revisão e nenhum banco do Pokémon Platinum foi emitido, ligado ou executado.

## O projeto

Oracis busca executar Pokémon Platinum como código nativo em computadores modernos. O projeto utiliza recompilação estática dos códigos ARM7/ARM9 e um runtime independente para os serviços necessários do Nintendo DS. Não é um emulador com uma CPU guest interpretada como caminho principal.

## Direção técnica

- **NDSRecomp estático** para gerar bancos C ARM7/ARM9, em checkout externo e revisão fixada.
- **C99 + C++20** para bancos gerados e runtime independente.
- **CMake + Ninja** para o build.
- **SDL3** para janela, eventos, controles, toque e saída de áudio.
- **Python** para extração local e validação da cópia de jogo.
- **Rust** permanece apenas no gerador isolado do ícone.

Consulte [docs/TECH_STACK.md](docs/TECH_STACK.md), [docs/NDSRECOMP_INTEGRATION.md](docs/NDSRECOMP_INTEGRATION.md), o [mapa da ABI dos bancos](docs/RECOMP_ABI_MAP.md), o [contrato GuestMemory](docs/GUEST_MEMORY.md) e a [decisão ADR 0001](docs/adr/0001-ndsrecomp-static-compiler.md).

## Estado atual

A primeira revisão-alvo é **Pokémon Platinum USA Rev 1**, identificada localmente por SHA-1. ROM e arquivos derivados permanecem fora do Git, da CI e das releases.

O bootstrap do NDSRecomp está fixado no commit `14b7e15023e9affc9dc375e4e611ac703f5c76ab` e limita o checkout a `recompiler/`, `cmake/` e ao núcleo ARM MIT. O runtime/oracle/frontend upstream e os componentes derivados de melonDS não são incorporados.

O Oracis já contém:
- estado ARM com registradores bancados e asserts de layout C99;
- GuestMemory: mapa inicial de RAM NDS retail, Main RAM compartilhada/mirrors, WRAM ARM7 privada e quatro modos WRAMCNT, incluindo o espelho da WRAM privada ARM7 no modo 0;
- wrappers C bus_read_*/bus_write_* que encaminham para RAM guest mapeada e falham com diagnóstico em endereços inválidos;
- operações CPSR/SPSR e entrada/retorno de exceções;
- avaliação de condições e atualização de flags NZCV;
- helpers de acesso aos registradores do User bank e instrução `CLZ` na ABI C;
- ponte ABI C para selecionar o estado ARM9 ou ARM7 via `g_cpu`, wrappers de flags/PSR e especialização `NDS_STATIC_CPU`;
- asserts do layout de `NdsStaticValidation`, `NdsDispatchEntry` e `NdsLinkSlot`;
- registro/remoção de bancos estáticos por ARM9/ARM7, lookup ARM/Thumb e `runtime_dispatch` com falha fechada;
- callback de identidade que compara bytes do banco e dependências contra `GuestMemory`, exigindo proveniência para RAM gravável;
- pilha de retorno guest separada por CPU, com chave PC+ARM/Thumb e desenrolamento de retornos não locais;
- contadores de ciclos e controle cooperativo de fatias ARM9/ARM7 por orçamento, yield e unwind;
- entradas `runtime_link_*` encaminhadas ao dispatcher normal; otimização de slots e folhas inline ainda desabilitada.

O CMake registra dezenove testes CTest, incluindo o mapa de RAM do NDS retail e os quatro modos WRAMCNT. **A execução dos testes para o estado atual ainda não foi confirmada**; a consulta aos status checks do último commit não retornou verificações. O mapa cobre RAM principal/WRAM; MMIO, CP15/TCM, BIOS, VRAM, OAM, temporização de memória, IRQ/SWI e serviços NitroSDK ainda não são suficientes para executar bancos reais.

A extração local da USA Rev 1 teve offsets, limites e hashes ARM9/ARM7 verificados. A execução integral do extrator versionado e a emissão de bancos continuam pendentes.

## Princípios

- Recompilação estática e execução nativa; sem interpretador ARM geral como fallback.
- Runtime, scheduler e dispositivos implementados independentemente.
- Não incorporar código ou assets de emuladores terceiros.
- Não incorporar código derivado de melonDS, direta ou indiretamente.
- ROM, BIOS, firmware, extrações, bancos gerados e saves ficam fora do repositório, CI e releases.
- Não presumir autorização a partir da disponibilidade pública de uma descompilação.
- Não declarar compatibilidade, desempenho ou conclusão sem testes reproduzíveis.

## Plataformas planejadas

Windows x64 e Linux x86-64 são os primeiros alvos. Android ARM64 será integrado depois de validar o runtime desktop.

## Aviso

Oracis é um projeto independente, sem afiliação ou endosso da Nintendo, The Pokémon Company ou Game Freak. Pokémon e seus elementos relacionados pertencem aos respectivos titulares. O projeto não distribui o jogo nem conteúdo proprietário.
