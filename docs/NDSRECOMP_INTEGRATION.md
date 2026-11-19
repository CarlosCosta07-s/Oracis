# Integração do NDSRecomp no Oracis

**Decisão:** utilizar exclusivamente o compilador estático do NDSRecomp como ferramenta externa de desenvolvimento. O runtime do Oracis será independente.

**Revisão upstream fixada:** `14b7e15023e9affc9dc375e4e611ac703f5c76ab`  
**Repositório:** [RetroPortingToolKit/ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp)  
**Referência de build:** `recompiler/CMakeLists.txt`, que declara CMake 3.20+ e C++20.

## 1. Escopo permitido

O Oracis usará o alvo `nds_recompile` para analisar binários ARM e emitir bancos de código nativo. O compilador dá suporte às famílias ARM7TDMI (ARMv4T) e ARM946E-S (ARMv5TE), usadas respectivamente pelos processadores ARM7 e ARM9 do Nintendo DS.

A integração será feita por um checkout de desenvolvimento externo, fora da árvore Git do Oracis. O script `tools/ndsrecomp/build_recompiler.py`:

1. baixa ou verifica o checkout na revisão fixada;
2. inicializa somente o submódulo `external/arm-recomp-core`;
3. configura somente o diretório `recompiler/`;
4. compila somente o alvo `nds_recompile`.

O script não configura nem compila o alvo `runner`, o oracle, o frontend ou os modelos de dispositivos upstream.

## 2. Limite absoluto: componentes derivados de melonDS

Os seguintes componentes não serão copiados, incluídos como submódulo, compilados, ligados ou distribuídos pelo Oracis:

- `runner/vendor/melonds/`;
- `runner/src/melonds_compute/`;
- `runner/src/frontend.cpp`;
- interfaces/shims derivados de melonDS;
- `oracle/` e seus patches contra melonDS;
- modelos de GPU, Wi-Fi, rede ou outros dispositivos obtidos de melonDS.

O uso do compilador estático não concede autorização para incorporar essas partes. A árvore upstream pode conter esses diretórios, mas o checkout ficará fora do repositório Oracis e o build automatizado selecionará apenas `nds_recompile`.

Não serão criados submódulos Git do NDSRecomp nem dependências do runner em CMake do Oracis.

## 3. Licenças verificadas na revisão fixada

A auditoria dos arquivos `LICENSE`, `PERMISSIONS.md`, `THIRD_PARTY_ATTRIBUTION.md` e `.gitmodules` indicou:

| Componente | Licença declarada | Tratamento no Oracis |
|---|---|---|
| Código próprio do NDSRecomp, incluindo o compilador e ferramentas | PolyForm Noncommercial 1.0.0, com permissões adicionais em `PERMISSIONS.md` | Ferramenta externa; preservar avisos e condições |
| `external/arm-recomp-core` | MIT | Dependência de build do compilador; licença e copyright preservados no checkout externo |
| `runner/vendor/melonds/` e adaptações derivadas | GPL-3.0-or-later | Excluídos |
| `runner/src/melonds_compute/` e interfaces derivadas | GPL-3.0-or-later | Excluídos |
| `oracle/` e patches contra melonDS | GPL-3.0-or-later / limites indicados pelo upstream | Excluídos |

As permissões adicionais do NDSRecomp permitem doações, patrocínios voluntários e publicidade quando não compram acesso ao software ou ao conteúdo; o software e os jogos derivados devem continuar acessíveis gratuitamente. Elas não transferem direitos sobre ROMs, BIOS, firmware, assets ou marcas.

A licença do compilador não cobre o conteúdo de Pokémon Platinum. A ROM continua sendo fornecida pelo usuário, validada localmente e nunca incluída em repositório, CI ou release.

## 4. Separação entre compilador e runtime

O compilador produz funções C/C++ e tabelas de dispatch para código ARM identificado. Ele não é, sozinho, um port jogável. O resultado precisa de um runtime que forneça, entre outros:

- estado de registradores ARM e flags CPSR;
- transições ARM/Thumb e chamadas/retornos;
- acesso à memória por endereço guest;
- dispatch entre bancos ARM9 e ARM7;
- interrupções, eventos e sincronização entre os dois processadores;
- acesso aos serviços de sistema e dispositivos necessários ao jogo.

O NDSRecomp publica uma interface C ABI em `recompiler/armv4t/runtime_arm.h`. Essa interface será estudada como referência de contrato, mas o Oracis escreverá sua própria ABI e suas próprias implementações. Não será copiado o runtime upstream nem o código de `runner/src/`.

O projeto adotará C99 onde a saída gerada exigir esse padrão e C++20 para o runtime, scheduler e serviços nativos. A linguagem exata de cada unidade gerada será confirmada com um banco de teste antes de congelar o pipeline.

## 5. Código dinâmico, overlays e limites de execução

O NDSRecomp upstream possui mecanismos adicionais para código copiado para RAM, incluindo caminhos interpretados/JIT no runtime. Esses mecanismos não serão importados automaticamente.

O Oracis manterá o requisito de port nativo: código ARM executável deverá ter um banco recompilado e validado. Overlays e regiões de código materializadas em RAM precisam ser identificados, extraídos localmente, recompilados e associados à identidade/hash correta. Uma região não reconhecida deverá falhar de forma explícita, com endereço, CPU e contexto de execução; não será executada por um interpretador geral ARM como fallback.

A estratégia para code-copies, overlays, dispatch indireto e invalidation será especificada em um ADR separado após o primeiro banco real ser emitido. Não declarar compatibilidade até demonstrar que todos os alvos executáveis necessários estão cobertos.

## 6. Identidade da primeira revisão

A primeira revisão-alvo é **Pokémon Platinum USA Rev 1**, identificada localmente pelo SHA-1:

`0862ec35b24de5c7e2dcb88c9eea0873110d755c`

O identificador pode ser usado na validação local. O conteúdo da ROM, os binários ARM extraídos, os bancos recompilados e os arquivos de configuração gerados a partir dela devem permanecer em diretórios ignorados pelo Git.

### Segmentos verificados localmente

O extrator Oracis foi executado contra a cópia local USA Rev 1. O SHA-1 completo conferiu com o identificador esperado, os offsets/tamanhos dos segmentos estavam dentro do arquivo e ARM9/ARM7 não se sobrepunham.

| Segmento | Offset na ROM | Load address | Entry PC | Tamanho | SHA-1 do segmento |
|---|---:|---:|---:|---:|---|
| ARM9 | `0x00004000` | `0x02000000` | `0x02000800` (ARM) | `0x001023F8` | `7ec963e737f0f61efcbbf5c7a3d33cbf378143f8` |
| ARM7 | `0x00409800` | `0x02380000` | `0x02380000` (ARM) | `0x000277FC` | `082bb349ae7d057edbbd91a07316b1402b18b803` |

Os hashes são dos segmentos extraídos, não de bancos recompilados. Os binários e TOMLs foram gerados apenas no ambiente local e não foram adicionados ao Git.

## 7. Como preparar o compilador

Na raiz do repositório Oracis, com Python 3, Git, CMake 3.20+, Ninja e um compilador C++20 instalados:

```sh
python tools/ndsrecomp/build_recompiler.py
```

Por padrão, o checkout externo será criado em `../ndsrecomp` e o build em `.oracis/ndsrecomp-build`.

Para usar outro local:

```sh
python tools/ndsrecomp/build_recompiler.py \
  --source /caminho/externo/ndsrecomp \
  --build-dir .oracis/ndsrecomp-build
```

O script recusa mover ou sobrescrever um checkout existente que não esteja exatamente na revisão fixada. Ele também não inicializa o submódulo FreeBIOS.

## 8. Extrair os segmentos da cópia local

Com a ROM fornecida pelo próprio usuário:

```sh
python tools/ndsrecomp/extract_nds_programs.py "/caminho/Pokemon - Platinum Version (USA) (Rev 1).nds"
```

O extrator aceita somente o SHA-1 USA Rev 1 esperado, valida os limites dos dois segmentos, calcula o SHA-1 de cada binário, confere os dados gravados e gera `arm9.toml`, `arm7.toml` e `manifest.json` em `.oracis/local/pokemon-platinum-usa-rev1`.

Requer Python 3.10 ou mais recente. Todos os arquivos de saída são locais e ignorados pelo Git.

## 9. Validação ainda pendente

O extrator foi executado com sucesso contra a cópia local e os TOMLs foram analisados como TOML válido. **Ainda não validado:** a compilação do executável `nds_recompile`, sua execução contra os dois segmentos, a emissão dos bancos e a compilação desses bancos com uma ABI Oracis.

## 8. Próximos marcos

1. Executar o bootstrap e registrar a versão/hash real do executável `nds_recompile`.
2. Criar um extrator local de cabeçalho e segmentos ARM9/ARM7 do arquivo NDS, com verificação do SHA-1 USA Rev 1 e limites de arquivo.
3. Gerar configurações TOML próprias para cada segmento e emitir os primeiros bancos.
4. Compilar um banco de teste para comprovar o contrato C ABI independente.
5. Implementar o runtime ARM9/ARM7 próprio, sem importar o runtime upstream.
6. Só então ligar serviços de memória, interrupções, timers, DMA, vídeo, áudio e save.

A etapa 1 não estará concluída apenas porque o executável do recompilador foi compilado: o primeiro banco real precisa ser emitido e compilado com sucesso.
