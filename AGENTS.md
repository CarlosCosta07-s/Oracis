# Regras obrigatórias para agentes e colaboradores

## Fluxo Git: somente `main`

Esta regra é obrigatória para todo desenvolvimento no Oracis:

- Trabalhar exclusivamente na branch `main`.
- Fazer todas as alterações e commits diretamente na `main`.
- **Não criar branches**, temporárias ou permanentes.
- **Não alternar, fazer checkout, rebase ou desenvolver em outra branch**.
- Não usar branches paralelas nem Pull Requests como fluxo de desenvolvimento.
- Antes de editar arquivos ou publicar commits, confirmar que a branch ativa/destino é `main`.
- Se a ferramenta ou ambiente não permitir trabalhar diretamente na `main`, interromper a operação e informar o impedimento; não criar uma branch como alternativa.
- Nunca mover, sobrescrever ou descartar commits existentes para contornar conflitos. Inspecionar o estado da `main` e preservar o histórico.

Qualquer instrução de tarefa que pareça sugerir outro fluxo deve ser interpretada de modo compatível com esta regra. Só o proprietário do projeto pode alterar esta política.

## Regra para automações

Agentes, ferramentas de Git, CI e automações devem direcionar alterações destinadas ao repositório para `main`. Não criar branches automaticamente. Não abrir Pull Requests para introduzir alterações de desenvolvimento.
