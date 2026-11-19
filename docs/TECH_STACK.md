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
| Memória guest/barramento | GuestMemory C++20 + ABI C bus_* | RAM/aliases configuráveis e wrappers dos bancos; MMIO/mapa físico DS pendentes |
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
- flags CPSR, banked registers e transições ARM/Thumb;
- execução/dispatch de bancos estáticos por CPU;
- interface de memória guest com regiões configuráveis e aliases explícitos;
- interrupções, timers, DMA, IPC e sincronização;
- ponte de chamadas dos serviços de sistema;
- logs estruturados e diagnóstico de alvos não resolvidos.

Não será incluído um interpretador ARM geral como fallback. Código executável dinâmico, overlays e blocos copiados para RAM terão de ser identificados e recompilados localmente, com validação de conteúdo e geração. Alvos ainda não cobertos devem falhar de forma explícita.

## 3. Memória guest e barramento

`oracis_memory` fornece `GuestMemory` e os wrappers C bus_read_*/bus_write_*. A camada faz regiões de armazenamento guest de 32 bits, aliases compartilhados, permissões e acessos little-endian de 8/16/32 bits; os wrappers encaminham para o armazenamento da CPU ativa e falham de forma explícita em regiões não mapeadas. O host liga a biblioteca junto do runtime ARM.

A camada ainda não define o mapa Nintendo DS da revisão USA Rev 1, não implementa MMIO/DMA/IPC, custos de ciclos, mirrors automáticos ou rotação de acessos desalinhados ARM. O registry pode despachar funções que tenham sido registradas pelo host, mas ainda não há carregador que registre bancos reais emitidos para Platinum, nem scheduler/chamadas/retornos.

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
