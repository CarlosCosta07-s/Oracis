# Mapa de desenvolvimento do Oracis

Este roteiro segue o guia de desenvolvimento fornecido para o projeto. Cada etapa deve produzir uma entrega pequena, compilável, executável e verificável. Nenhuma fase será considerada concluída apenas porque stubs ou interfaces foram criados.

## Regras permanentes

1. Passos pequenos; cada entrega deve compilar e rodar.
2. Logs completos devem orientar a investigação de falhas.
3. Desktop e Android fazem parte da CI desde o esqueleto inicial.
4. ROM, firmware, recursos extraídos, saves e patches binários não entram no repositório, CI ou releases.
5. Mudanças de comportamento devem ser comparadas com um emulador de referência, sem copiar seu código.
6. Antes de usar código ou ferramentas externas, verificar commit, licença e instruções atuais.
7. Não presumir direitos de reutilização a partir da disponibilidade pública de um repositório.

## Fases

| Fase | Área | Entrega principal | Estado |
|---:|---|---|---|
| 0 | Baseline | Estado, revisão/região, build e direitos de reutilização identificados | Revisão USA Rev 1 identificada; licença/build upstream ainda pendentes |
| 1 | Esqueleto multiplataforma | Janela SDL3 com duas telas de teste 256 × 192 no desktop e Android | Em implementação; código e CI iniciais adicionados, validação pendente |
| 2 | Compilação nativa do jogo | Código C compilando e linkando para x86-64 e ARM64, com stubs explícitos para serviços ainda ausentes | Bloqueada até esclarecer reutilização/licença |
| 3 | NitroSDK / HLE | Serviços OS, MI/DMA/MATH, FS/NitroFS, CARD, PAD/TP/RTC e stubs offline | Não iniciada |
| 4 | Boot visível | Primeiro logo/aviso e, depois, tela de título | Não iniciada |
| 5 | Vídeo 2D | VRAM, paletas, OAM, BGs, sprites, janelas, blending e efeitos por scanline | Não iniciada |
| 6 | Vídeo 3D | Geometry Engine, transformações, iluminação, atributos, texturas, transparência, fog e rasterização | Não iniciada |
| 7 | Áudio | SDAT, SSEQ/SBNK/SWAR/STRM, canais e mixagem | Não iniciada |
| 8 | Entrada e UX | Teclado, gamepad, toque, controles virtuais e configurações | Não iniciada |
| 9 | Testes automatizados | Input movies, comparações de frames, testes unitários e sanitizadores | Não iniciada |
| 10 | Performance mobile | Profiling, batching, cache e meta de 60 FPS em aparelho intermediário | Não iniciada |
| 11 | Empacotamento | Windows, Linux, macOS, Android e posteriormente iOS | Não iniciada |

## Ordem interna da camada NitroSDK

1. OS: heaps/arenas, tick, eventos de VBlank, sincronização e escalonamento cooperativo.
2. MI/DMA/MATH: cópias, DMA e operações matemáticas com semântica compatível.
3. FS/NitroFS: leitura sob demanda a partir da cópia fornecida pelo usuário.
4. CARD: saves locais com importação/exportação compatível.
5. PAD/TP/RTC: controles, toque e relógio do sistema.
6. WM/Wi-Fi/PXI: stubs offline enquanto funções de rede não forem necessárias.

## Armadilhas a testar explicitamente

- Conversões entre ponteiros de 32 e 64 bits.
- Packing e alinhamento de estruturas lidas de arquivos.
- Ponto fixo, overflow, shifts, divisão por zero e sinal.
- Rotinas ainda em assembly: converter para C ou isolar um lifter apenas quando necessário.
- Overlays e endereços de registradores de hardware.
- Ordem dos eventos de VBlank e determinismo do loop.
- Efeitos por scanline em batalhas e transições.
- Comportamento indefinido revelado por Clang/UBSan.

## Critério de “inicializa nativamente”

- Build de referência e revisão identificada.
- Executável SDL3 abre no desktop e no Android.
- Código do jogo compila e linka para x86-64 e ARM64.
- Serviços OS/FS/CARD/PAD básicos funcionam.
- Loop principal permanece ativo por 10 minutos sem crash.
- Vídeo 2D apresenta a tela de título.
- Menu, novo jogo, mapa 3D e áudio funcionam.
- Save/load funciona.
- Input movie passa na comparação definida com o emulador de referência.

## Critério de qualidade

Não anunciar compatibilidade, desempenho, áudio ou conclusão de subsistemas sem testes reproduzíveis e logs associados. Cada milestone deve registrar limitações conhecidas e diferenças observadas.
