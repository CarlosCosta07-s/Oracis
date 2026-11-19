# Base técnica do Oracis

> Arquitetura revisada em 19/11/2026 após a decisão de utilizar o compilador estático NDSRecomp e implementar um runtime próprio, sem componentes derivados de melonDS.

## Decisão resumida

| Camada | Tecnologia | Responsabilidade |
|---|---|---|
| Recompilação estática ARM7/ARM9 | **NDSRecomp** (revisão fixada) | Traduzir código guest ARMv4T/ARMv5TE em bancos de código nativo |
| Código emitido | C/C++ gerado pelo compilador | Unidades nativas locais; formato exato será validado no primeiro banco |
| Runtime nativo | **C++20** | Estado guest, dispatch, scheduler dual-CPU e serviços nativos |
| ABI dos bancos | Contrato C ABI próprio | Ponte estável entre bancos gerados e runtime Oracis |
| Dispatch estático | Registry C++20 + fachada C | Registro por CPU, lookup ARM/Thumb, validação de conteúdo e miss fail-closed |
| Memória guest/barramento | GuestMemory C++20 + mapa NDS retail + ABI C bus_* | Main RAM/mirrors e WRAM por CPU; MMIO/TCM/VRAM e timing pendentes |
| Ferramentas de extração/validação | **Python** | Ler header NDS, extrair segmentos locais e gerar TOML de entrada |
| Build | **CMake + Ninja** | Build do host e integração das ferramentas |
| Compilador host | Clang/GCC/MSVC com C++20 | x86-64 e, após adaptação, ARM64 |
| Janela, eventos e input | **SDL3** | Frontend, teclado, gamepad, toque e ciclo de eventos |
| Áudio host | **SDL3** | Saída de áudio do runtime/mixer próprio |
| Vídeo | Backend próprio inicialmente; API final a decidir | Compositor 2D/3D sem reutilizar dispositivos melonDS |
| Android | SDL3 + Activity Java/JNI mínimo | Integração posterior; não validada ainda |
| Ícone | Rust | Ferramenta isolada de geração do SVG |

## 1. NDSRecomp — compilador estático

O Oracis utilizará apenas o alvo upstream `nds_recompile`, fixado na revisão:

`14b7e15023e9affc9dc375e4e611ac703f5c76ab`

Esse snapshot declara CMake 3.20+ e C++20 no projeto do recompilador. O alvo consome o submódulo `external/arm-recomp-core` (MIT) e produz bancos nativos. O checkout é feito fora do repositório Oracis; não há submódulo ou cópia do runtime upstream.

O processo de integração está descrito em [NDSRECOMP_INTEGRATION.md](NDSRECOMP_INTEGRATION.md).

## 2. Runtime próprio — C++20

O Oracis não utilizará o runtime do NDSRecomp. Será criada uma implementação independente que consuma uma ABI C própria dos bancos recompilados.

Responsabilidades previstas:
- estado de registradores para ARM9 e ARM7;
- registro/lookup estático de bancos C por CPU e estado ARM/Thumb;
- verificação da identidade dos bytes do banco e suas dependências antes da execução;
- pilha de retorno guest por CPU para distinguir retorno nativo de salto guest;
- contadores de ciclos e controle cooperativo de fatias/yield por CPU;
- chamadas por slot ainda encaminhadas ao dispatcher; cache direto exige epoch e guardas de conteúdo;
- flags CPSR, banked registers e transições ARM/Thumb;
- helpers ABI de acesso ao User bank, aritmética/condições e CLZ;
- execução/dispatch de bancos estáticos por CPU;
- interface de memória guest com regiões configuráveis e aliases explícitos;
- interrupções, timers, DMA, IPC e sincronização;
- ponte de chamadas dos serviços de sistema;
- logs estruturados e diagnóstico de alvos não resolvidos.

Não será incluído um interpretador ARM geral como fallback. Código executável dinâmico, overlays e blocos copiados para RAM terão de ser identificados e recompilados localmente, com validação de conteúdo e geração. Alvos ainda não cobertos devem falhar de forma explícita.

## 3. Memória guest e barramento

`oracis_memory` fornece `GuestMemory`, os wrappers C bus_read_*/bus_write_* e `NdsRetailRamMap`. O mapa inicial cobre 4MB de Main RAM compartilhada em 0x02000000 e os três espelhos retail, 64KB de WRAM privada ARM7 em 0x03800000 e os quatro modos Shared WRAM derivados de WRAMCNT. As regiões têm visibilidade explícita por CPU e as janelas Shared WRAM repetem seus bancos físicos de 16KB/32KB; quando o modo 0 desaloja Shared WRAM do ARM7, sua janela espelha a WRAM privada de 64KB.

O modo WRAM inicial é fornecido explicitamente pelo host, e a API `set_shared_wram_mode()` pode remapear os quatro modos preservando os bancos; o mapa ainda não reage a escritas guest em WRAMCNT/WRAMSTAT. MMIO WRAMCNT/WRAMSTAT, DMA/IPC, CP15/TCM, BIOS, VRAM, OAM e custos de ciclos por região continuam pendentes. O registry ainda não tem carregador de bancos reais emitidos para Platinum.

Referência do contrato: [GUEST_MEMORY.md](GUEST_MEMORY.md).

## 4. Licenças e fronteira do NDSRecomp

A auditoria do snapshot upstream identificou PolyForm Noncommercial 1.0.0 para o código próprio do compilador, permissões adicionais em `PERMISSIONS.md` e MIT para `external/arm-recomp-core`.

O Oracis não incorpora:
- `runner/vendor/melonds/`;
- `runner/src/melonds_compute/`;
- `runner/src/frontend.cpp`;
- shims/interfaces derivados de melonDS;
- `oracle/` ou patches contra melonDS.

O runtime, os dispositivos e os serviços do Oracis serão escritos separadamente. Nenhum direito sobre o jogo, ROM, firmware, BIOS ou marcas decorre da licença do recompilador.

A reutilização de código de `pret/pokeplatinum` continua bloqueada até que os direitos sejam esclarecidos; o caminho de recompilação estática usa a cópia local do usuário e não exige incorporar essa descompilação.

## 4. SDL3 — frontend

SDL3 permanece como camada de plataforma para janela, input, gamepad, toque e saída de áudio. O host de teste existente continua útil como prova inicial de janela, mas ainda não está conectado ao runtime recompilado.

O projeto Android exige Activity/integração JNI e testes de ciclo de vida; o cross-build atual é somente uma etapa preparatória e não comprova um APK funcional.

## 5. CMake, Ninja e padrão da linguagem

O projeto usa CMake + Ninja. C99 fica disponível para unidades geradas que precisem desse padrão; C++20 é a base do runtime próprio e do compilador estático.

O CMake raiz declara as linguagens C e C++ para que as duas partes possam coexistir.

## 6. Dados locais

- A primeira revisão-alvo é Pokémon Platinum USA Rev 1, SHA-1 `0862ec35b24de5c7e2dcb88c9eea0873110d755c`.
- O arquivo NDS, segmentos ARM extraídos, TOMLs gerados, bancos nativos e caches ficam em diretórios locais ignorados pelo Git.
- ROM, BIOS, firmware, saves, recursos extraídos e bancos derivados não entram no repositório, CI ou releases.
- O Oracis valida identidade e limites do arquivo antes de extrair qualquer segmento.

## 7. Plataformas

1. Windows x64 e Linux x86-64 para desenvolver e validar o toolchain.
2. Android ARM64 após o runtime desktop e o frontend estabilizarem.
3. macOS/iOS como alvos posteriores da camada multiplataforma.

A prioridade é demonstrar recompilação e execução nativa da primeira revisão no desktop antes de comprometer a integração mobile.
