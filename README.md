<!-- Oracis project page -->
<p align="center">
  <img src="assets/icon/oracis.svg" alt="Ícone do Oracis: estrela de platina e anel celeste" width="160">
</p>

<h1 align="center">Oracis</h1>

<p align="center">Um projeto independente de port nativo de Pokémon Platinum para plataformas modernas.</p>

<p align="center"><strong>Reconstruindo Sinnoh para além do hardware original.</strong></p>

> **Status: Fase 1 — Integração do NDSRecomp em andamento.** A revisão USA Rev 1 foi identificada e o bootstrap do compilador estático foi adicionado. Ainda não há banco recompilado integrado nem runtime jogável.

## O projeto

Oracis busca executar Pokémon Platinum como código nativo em computadores modernos. O projeto utilizará recompilação estática dos códigos ARM7/ARM9 e um runtime independente para os serviços necessários do Nintendo DS. Não será um emulador com uma CPU guest interpretada como caminho principal.

## Direção técnica

- **NDSRecomp estático** para emissão de bancos nativos ARM7/ARM9, em checkout externo e revisão fixada.
- **C99 + C++20** para unidades geradas e runtime independente.
- **CMake + Ninja** para o build.
- **SDL3** para janela, eventos, controles, toque e saída de áudio.
- **Python** para extração local, validação de ROM e configuração dos bancos.
- **Rust** permanece apenas no gerador isolado do ícone.

Consulte [docs/TECH_STACK.md](docs/TECH_STACK.md), [docs/NDSRECOMP_INTEGRATION.md](docs/NDSRECOMP_INTEGRATION.md) e a [decisão de arquitetura ADR 0001](docs/adr/0001-ndsrecomp-static-compiler.md).

## Estado atual

A primeira revisão-alvo é **Pokémon Platinum USA Rev 1**. A cópia fornecida para identificação teve SHA-1 calculado localmente e correspondente ao hash Rev 1 documentado. A ROM e quaisquer arquivos derivados permanecem fora do Git, da CI e das releases.

Foi adicionado [um script para preparar o NDSRecomp](tools/ndsrecomp/build_recompiler.py), fixado no commit `14b7e15023e9affc9dc375e4e611ac703f5c76ab`. Ele configura e compila apenas o alvo `nds_recompile`; não compila o runner upstream. Também foi criado um [extrator ARM9/ARM7](tools/ndsrecomp/extract_nds_programs.py), executado com sucesso contra a cópia local USA Rev 1; os binários resultantes permanecem fora do Git.

O runtime do Oracis será escrito separadamente. Não serão incorporados runner, oracle, frontend nem componentes derivados de melonDS. A interface C ABI do recompilador será estudada para criar um contrato independente.

A auditoria da descompilação [pret/pokeplatinum](https://github.com/pret/pokeplatinum) permanece documentada, mas nenhum código ou recurso dela foi copiado. O licenciamento da descompilação continua pendente.

O host SDL3 existente é apenas um frontend experimental; ainda não foi conectado aos bancos recompilados. Consulte o [registro da Fase 1](docs/PHASE_1_SKELETON.md) e o [roteiro](docs/DEVELOPMENT_ROADMAP.md).

## Princípios

- Recompilação estática e execução nativa; sem interpretador ARM geral como fallback.
- Runtime, scheduler e dispositivos implementados independentemente.
- Não incorporar código ou assets de emuladores terceiros.
- Não incorporar código derivado de melonDS, direta ou indiretamente.
- ROM, BIOS, firmware, dados extraídos, bancos recompilados e saves ficam fora do repositório, CI e releases.
- Não presumir autorização a partir da disponibilidade pública de uma descompilação.
- Não declarar compatibilidade, desempenho ou conclusão sem testes reproduzíveis.

## Plataformas planejadas

Windows x64 e Linux x86-64 são os primeiros alvos. Android ARM64 será considerado após a validação do runtime desktop.

## Identidade visual

O símbolo Oracis é um sigilo celeste original: uma estrela facetada em platina atravessada por um anel orbital dourado. O desenho é vetorial e gerado por código. Consulte [docs/VISUAL_IDENTITY.md](docs/VISUAL_IDENTITY.md).

## Aviso

Oracis é um projeto independente, sem afiliação ou endosso da Nintendo, The Pokémon Company ou Game Freak. Pokémon e seus elementos relacionados pertencem aos respectivos titulares. O projeto não distribui o jogo nem conteúdo proprietário.
