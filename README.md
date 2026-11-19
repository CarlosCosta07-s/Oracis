<!-- Oracis project page -->
<p align="center">
  <img src="assets/icon/oracis.svg" alt="Ícone do Oracis: estrela de platina e anel celeste" width="160">
</p>

<h1 align="center">Oracis</h1>

<p align="center">Um projeto independente de port nativo de Pokémon Platinum para plataformas modernas.</p>

<p align="center"><strong>Reconstruindo Sinnoh para além do hardware original.</strong></p>

> **Status:** a cópia local de Pokémon Platinum USA Rev 1 fornecida para desenvolvimento foi validada pelo extrator versionado em 19/11/2026. Os segmentos ARM9/ARM7 foram extraídos e verificados localmente (tamanho e SHA-1). A API `oracis_host_runtime_load_local_platinum_segments()` lê `arm9.bin` e `arm7.bin`, verifica tamanhos e SHA-1 fixos dos dois arquivos e só depois carrega as imagens na memória guest. O CMake também liga bancos estáticos locais por `ORACIS_LOCAL_BANK_DIR`, e a ponte C ABI consegue registrar/remover as tabelas. O host SDL3 desktop tenta carregar os segmentos locais durante a inicialização quando bancos estáticos estão ligados, mas não executa rounds: ainda faltam a emissão dos bancos reais, os serviços exigidos por eles e o provider arquitetural completo de IRQ. No Android, a seleção de arquivo e o carregamento local ainda não estão ligados. A revisão `a3032f1` passou na CI de 19/11/2026: desktop Linux (28/28 CTests e smoke de bancos sintéticos 1/1), tooling Python (11 testes), runtime ARM, Android ARM64 e compilador estático NDSRecomp.

## O projeto

Oracis busca executar Pokémon Platinum como código nativo em computadores modernos. O projeto utiliza recompilação estática dos códigos ARM7/ARM9 e um runtime independente para os serviços necessários do Nintendo DS. Não é um emulador com uma CPU guest interpretada como caminho principal.

## Direção técnica

- **NDSRecomp estático** para gerar bancos C ARM7/ARM9, em checkout externo e revisão fixada.
- **C99 + C++20** para bancos gerados e runtime independente.
- **CMake + Ninja** para o build.
- **SDL3** para janela, eventos, controles, toque e saída de áudio.
- **Python** para extração local e validação da cópia de jogo.
- **Rust** permanece apenas no gerador isolado do ícone.

Consulte [docs/TECH_STACK.md](docs/TECH_STACK.md), [docs/NDSRECOMP_INTEGRATION.md](docs/NDSRECOMP_INTEGRATION.md), [documentação do scheduler dual-core](docs/DUAL_CORE_SCHEDULER.md), o [mapa da ABI dos bancos](docs/RECOMP_ABI_MAP.md), o [contrato GuestMemory](docs/GUEST_MEMORY.md) e a [decisão ADR 0001](docs/adr/0001-ndsrecomp-static-compiler.md).

## Estado atual

A primeira revisão-alvo é **Pokémon Platinum USA Rev 1**, identificada localmente por SHA-1. ROM e arquivos derivados permanecem fora do Git, da CI e das releases.

O bootstrap do NDSRecomp está fixado no commit `14b7e15023e9affc9dc375e4e611ac703f5c76ab` e limita o checkout a `recompiler/`, `cmake/` e ao núcleo ARM MIT. O runtime/oracle/frontend upstream e os componentes derivados de melonDS não são incorporados.

O Oracis já contém:
- estado ARM com registradores bancados e asserts de layout C99;
- GuestMemory: mapa de RAM NDS retail, Main RAM compartilhada/mirrors, WRAM ARM7 privada, quatro modos WRAMCNT e leitura ARM7 de WRAMSTAT; as sete janelas Shared WRAM são pré-mapeadas e suas máscaras de visibilidade por CPU são atualizadas de forma transacional, com o mapa validado antes da aplicação, sem desalocação nem perda dos bytes físicos;
- wrappers C bus_read_*/bus_write_* que encaminham para RAM guest mapeada e falham com diagnóstico em endereços inválidos;
- controlador MMIO `NdsInterruptControl` com IME/IE/IF independentes por CPU, IRQ requests latcheados, IF W1C e `try_enter_irq` condicionado a `CPSR.I`; `run_dual_core_round` consulta e entrega IRQs nas fronteiras; o callback host fornece o LR ARM/Thumb e a máquina seleciona a base vetorial pelo ARM9 CP15 Control.V;
- `NdsTimerControl` com Timer 0–3 para cada CPU, reload, prescalers, cascata e pedidos de IRQ; `NdsSystemClock` converte os contadores cumulativos ARM9/ARM7 na razão 2:1, preserva meio ciclo e chama `advance_bus_cycles(n)` uma única vez por intervalo de barramento completo;
- carregador local de programas que lê os dois binários extraídos, exige tamanhos e SHA-1 conhecidos de ARM9/ARM7 e só entrega as imagens à máquina depois de validar ambas; módulo SHA-1 próprio coberto por vetores conhecidos;
- `NdsMachine` como contexto proprietário dos dois estados ARM e dos dispositivos base; instalação do mapa retail, vínculo C ABI em ordem de dependência, rollback/desvínculo reverso, início/fim de fatias com relatório automático dos contadores cumulativos, contabilização de ciclos ociosos com precisão de core e rounds dual-core sincronizados em 2:1; HALT/wake básico por CPU conectado ao BIOS SWI `0x06`, SWI ARM7 `0x1F` (parâmetro em R2) e CP15 WFI do ARM9; ARM9 CP15 Control.V (bit 13) seleciona vetores baixos/altos e as demais operações CP15/MPU/cache/TCM continuam pendentes;
- operações CPSR/SPSR e entrada/retorno de exceções;
- avaliação de condições e atualização de flags NZCV;
- helpers de acesso aos registradores do User bank e instrução `CLZ` na ABI C;
- ponte ABI C para selecionar o estado ARM9 ou ARM7 via `g_cpu`, wrappers de flags/PSR e especialização `NDS_STATIC_CPU`;
- asserts do layout de `NdsStaticValidation`, `NdsDispatchEntry` e `NdsLinkSlot`;
- registro/remoção de bancos estáticos por ARM9/ARM7, lookup ARM/Thumb e `runtime_dispatch` com falha fechada;
- callback de identidade que compara bytes do banco e dependências contra `GuestMemory`, exigindo proveniência para RAM gravável;
- pilha de retorno guest separada por CPU, com chave PC+ARM/Thumb e desenrolamento de retornos não locais;
- contadores de ciclos e controle cooperativo de fatias ARM9/ARM7 por orçamento, yield e unwind automático durante o retorno dos frames C gerados;
- `NdsKeypadControl` com KEYINPUT compartilhado, KEYCNT independente por CPU, condições OR/AND e IRQ de teclado; o host SDL3 mapeia X/Z, Enter/Backspace, setas e Q/W, limpa entradas ao perder foco e encaminha estado pela ponte C;
- entradas `runtime_link_*` encaminhadas ao dispatcher normal; otimização de slots e folhas inline ainda desabilitada.

A validação automatizada atual registra **28 CTests** no build desktop, todos aprovados na revisão `a3032f1`. O mesmo job compilou e executou o smoke test opcional de bancos locais (1/1), usando fixtures sintéticas. Os demais jobs da execução também passaram: runtime ARM sem dependência SDL3, Android ARM64 e build do alvo estático `nds_recompile`. O tooling Python executou 11 testes, incluindo a validação estrita do emissor e da identidade `nds-codegen-v3`. [Execução da CI](https://github.com/CarlosCosta07-s/Oracis/actions/runs/44604378710).

O extrator e o emissor local validam a identidade dos segmentos, os metadados TOML, os hashes ARM9/ARM7 e a identidade do gerador. A emissão dos dois bancos é transacional. O novo carregador local verifica o tamanho e o SHA-1 fixos dos dois arquivos antes de modificar a memória guest; os testes rejeitam imagens ausentes, truncadas ou adulteradas, e os vetores SHA-1 conhecidos passam.

**Ainda não foi concluído:** construir o NDSRecomp fixado, emitir e compilar os bancos reais a partir dos segmentos extraídos e auditar seus símbolos/serviços. A API do carregador local está testada com arquivos inválidos sintéticos. Quando os bancos estáticos estão ligados, o host desktop tenta carregar os segmentos no início; se os arquivos não existirem, permanece em modo de protótipo, e se estiverem inválidos encerra com diagnóstico. A seleção de arquivo e a localização dos dados no Android ainda não estão integradas. O host não executa rounds: o provider arquitetural completo de IRQ e os serviços necessários à execução continuam pendentes. Nenhum teste atual comprova boot ou compatibilidade jogável.

## Princípios

- Recompilação estática e execução nativa; sem interpretador ARM geral como fallback.
- Runtime, scheduler e dispositivos implementados independentemente.
- Não incorporar código ou assets de emuladores terceiros.
- Não incorporar código derivado de melonDS, direta ou indiretamente.
- ROM, BIOS, firmware, extrações, bancos gerados e saves ficam fora do repositório, CI e releases.
- Não presumir autorização a partir da disponibilidade pública de uma descompilação.
- Não declarar compatibilidade, desempenho ou conclusão sem testes reproduzíveis.

## Plataformas planejadas

Linux x86-64 e Android ARM64 já passam pelos builds da CI; isso comprova compilação, não execução jogável. Windows x64 continua planejado e requer validação própria.

## Aviso

Oracis é um projeto independente, sem afiliação ou endosso da Nintendo, The Pokémon Company ou Game Freak. Pokémon e seus elementos relacionados pertencem aos respectivos titulares. O projeto não distribui o jogo nem conteúdo proprietário.
