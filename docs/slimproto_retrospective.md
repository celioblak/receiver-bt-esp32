# Retrospectiva: por que o Slimproto foi removido (2026-08-16)

Este documento existe porque o suporte a Music Assistant via protocolo Slimproto
(`main/slimproto.c`, `main/lms_cli.c`, `main/lms_metadata.c`) foi **removido** deste firmware
depois de uma sessão de depuração muito longa que encontrou crashes reais e recorrentes,
alguns com causa raiz dentro do próprio `lwIP` (a pilha TCP/IP do ESP-IDF), fora do alcance de
uma correção simples no nosso código. A decisão foi abandonar o Slimproto e voltar para DLNA,
desta vez no modelo **DMP (Digital Media Player)** em vez do modelo MediaRenderer usado antes.

Este arquivo resume o que foi tentado, o que funcionou, e por que, no fim, não valeu a pena
continuar por esse caminho — para que uma sessão futura não repita a mesma investigação do zero
se o Slimproto for reconsiderado algum dia.

## O que o Slimproto chegou a fazer, funcionando de verdade

- Conexão direta com o Music Assistant via protocolo Squeezebox/LMS (porta 3483), sem depender
  de descoberta multicast/SSDP — que nunca funcionou com o MA rodando em Docker (motivo original
  de abandonar o DLNA MediaRenderer da primeira vez).
- Decodificação nativa de FLAC no próprio firmware (`flac_stream_decoder.cpp`, componente
  `esphome/micro-flac`) — contornou um bug real do lado do Music Assistant que sempre mandava
  FLAC não importava a configuração.
- Status, controle (play/pause/next/previous/stop) e volume/mute reais, sincronizados nos dois
  sentidos com o Music Assistant.
- Metadados (título/artista/álbum) via API WebSocket própria do MA.
- Buffer de áudio (ring buffer em PSRAM) com backpressure de rede, cobrindo boa parte dos
  engasgos que a implementação inicial mais simples tinha.

## O que nunca ficou totalmente estável

Depois de muitas rodadas de correção ao longo de várias sessões (ver
`project_slimproto_music_assistant` e `project_audio_stutter_investigation` na memória do
projeto para o histórico completo, dia a dia), a troca rápida de faixa (pular várias vezes
seguidas, "next"/"previous" via `lms_cli.c`) continuava causando crashes reais e intermitentes:

1. **Reuso do stack estático da task de decodificação antes dela realmente sair** — corrigido
   com um semáforo real de sincronização (`s_data_task_exited_sem`) no lugar de um
   `vTaskDelay(50)` às cegas.
2. **`connect()` do socket de dados sem timeout de verdade** — podia bloquear por vários segundos
   sob disputa de rede, o que reabria a mesma corrida do item 1 mesmo com o semáforo, só que via
   um caminho diferente. Corrigido com o padrão non-blocking connect + `select()`.
3. **Escrita redundante na NVS a cada `audg`** (sincronização de volume que o MA manda sozinho,
   sem ação do usuário) — cada escrita desliga o cache da flash por um instante, o que podia
   coincidir com outra task acessando PSRAM ou no meio de uma operação de rede.
4. **Causa raiz final, nunca totalmente resolvida**: um `assert failed: xQueueGenericSend ...
   (pxQueue)` real, decodificado com backtrace ao vivo (captura serial + `addr2line`), apontando
   pra dentro do próprio `lwIP` (`tcpip_thread`, `lwip_netconn_do_connected`,
   `lwip_setsockopt_callback`) — um use-after-free quando uma conexão TCP é fechada enquanto uma
   operação assíncrona relacionada a ela (conectar, configurar socket) ainda está em andamento
   dentro da thread interna do lwIP. Aconteceu em pelo menos duas variantes diferentes
   (`connect()` e `setsockopt()`), sugerindo que não era um bug pontual e sim uma fragilidade
   mais ampla de como este firmware abre e fecha conexões TCP concorrentes (Slimproto de
   controle, Slimproto de dados, `lms_cli` a cada comando) sob rajadas rápidas de comandos.

Cada correção foi real e comprovada com evidência (backtrace decodificado, não suposição), mas
cada uma revelava outro mecanismo de crash logo em seguida. Depois de várias rodadas assim, a
decisão foi que persistir dentro do lwIP não valia mais o tempo — mais fácil migrar para uma
arquitetura que não dependa de abrir/fechar tantas conexões TCP concorrentes com tanta frequência.

## Por que DLNA modelo DMP, não MediaRenderer de novo

A tentativa anterior de DLNA (`main/dlna_renderer.c`, ainda no código) implementava o modelo
**MediaRenderer**: o dispositivo fica passivo, esperando o Music Assistant descobri-lo via
SSDP/multicast e mandar comandos SOAP. Isso nunca funcionou de forma confiável com o MA rodando
em Docker, porque multicast não atravessa a rede do container facilmente.

O modelo **DMP (Digital Media Player)** inverte isso: o dispositivo age como um **control point**
que se conecta diretamente ao Media Server do Music Assistant (se ele expuser um) e puxa o
conteúdo sozinho, em vez de esperar ser descoberto. Isso evita o problema de descoberta que
matou o MediaRenderer, e evita o padrão de múltiplas conexões TCP concorrentes de curta duração
que pareceu ser a raiz dos crashes do Slimproto.

## Arquivos removidos nesta migração

- `main/slimproto.c` / `main/slimproto.h`
- `main/lms_cli.c` / `main/lms_cli.h`
- `main/lms_metadata.c` / `main/lms_metadata.h`
- Referências em `main/main.c`, `main/web_server.c`, `main/mqtt_ha.c`, `main/CMakeLists.txt`

O decoder FLAC nativo (`main/flac_stream_decoder.cpp/.h`) foi mantido — é reutilizável pelo
caminho DLNA/DMP novo, já que o problema que ele resolve (Music Assistant sempre mandando FLAC)
não é específico do Slimproto.

Ver `slimproto-pre-dlna-dmp` (tag do git) para o snapshot completo do código antes desta remoção,
caso seja necessário consultar a implementação original no futuro.
