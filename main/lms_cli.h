#pragma once

#include <stdbool.h>

/* Cliente do protocolo clássico de controle do LMS (Logitech Media Server),
 * porta 9090 -- confirmado (testado ao vivo) que o Music Assistant também
 * implementa esse protocolo, pra compatibilidade com clientes Squeezebox.
 *
 * Usado só pra ENVIAR comandos de transporte (play/pause/stop/next/
 * previous) -- não há polling de status aqui: play/pause/stop já são
 * conhecidos localmente via o próprio Slimproto (ver slimproto_get_status()
 * em slimproto.h), e o Music Assistant não preenche metadados de faixa
 * (título/artista/álbum) por este protocolo (testado: playlist_tracks
 * sempre 0, mesmo tocando) -- isso vem da API WebSocket própria do MA (ver
 * lms_metadata.c/.h), que é um protocolo separado.
 *
 * Cada chamada abre e fecha sua própria conexão TCP (comandos são raros,
 * disparados pelo usuário via /api/media). Uma tentativa de conexão
 * persistente compartilhada (2026-08-15) foi revertida no mesmo dia: causou
 * um PANIC real (reset reason confirmado em /api/logs) e depois a
 * reprodução travou, ambos após 11h+ de uptime estável sem esse código
 * rodar -- suspeito forte, mas sem coredump pra confirmar a causa exata.
 * Não retentar sem antes ativar CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH ou ter
 * uma captura serial ao vivo, senão vira mais um "conserto às cegas" como
 * o já documentado na memória do projeto sobre core-pinning. */

/* cmd: "play", "pause", "playpause", "stop", "next" ou "previous" (mesmo
 * vocabulário de bt_audio_media_control(), ver web_server.c). Usa o mesmo
 * host configurado em NVS_KEY_SLIM_HOST (ver config.h). Bloqueia por até
 * ~3s (timeout de socket), uma única tentativa -- chamar só a partir da
 * task do httpd, nunca de uma task de tempo real. Retorna false se o host
 * não está configurado, a conexão falhar, ou "cmd" for desconhecido.
 *
 * NOTA (2026-08-16): tanto uma conexão persistente compartilhada quanto um
 * retry com vTaskDelay foram tentados neste mesmo dia pra reduzir falhas
 * intermitentes de "next"/"previous" -- a primeira causou um PANIC
 * confirmado, a segunda um reset por WDT, ambos SEM evidência real na hora
 * (sem captura serial ligada ainda). Revertidos. Depois, com um monitor
 * serial ao vivo (`pio device monitor`) capturando backtraces reais, veio
 * a evidência de verdade: pelo menos 3 mecanismos de crash distintos e
 * confirmados durante a rajada de `strm` que um pulo de faixa dispara —
 * reuso de stack de task (corrigido em slimproto.c), escrita redundante na
 * NVS a cada `audg` do MA (corrigido em audio_codec.c), e um
 * use-after-free real dentro do próprio lwIP (`assert failed:
 * xQueueGenericSend ... (pxQueue)`, dentro de `lwip_netconn_do_write`).
 * Esse último não tem um fix direto no nosso código (é dentro do lwIP) --
 * mitigado dando uma folga curta (`slimproto_get_last_transition_time_us()`)
 * antes de abrir a conexão se uma transição de faixa acabou de acontecer,
 * reduzindo a sobreposição entre nossa conexão nova e a reconstrução da
 * conexão de dados do Slimproto. Reduz a chance, não elimina a causa raiz.
 * Se crashes persistirem mesmo com essa folga, o próximo passo é aumentar
 * `CONFIG_LWIP_MAX_SOCKETS` (hoje 16, apertado pra tudo que este firmware
 * mantém aberto) ou investigar mais fundo dentro do próprio lwIP. */
bool lms_cli_send_transport(const char *cmd);
