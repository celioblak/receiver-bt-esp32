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
 * disparados pelo usuário via /api/media -- não vale a complexidade de uma
 * conexão persistente compartilhada com filas/mutex). */

/* cmd: "play", "pause", "playpause", "stop", "next" ou "previous" (mesmo
 * vocabulário de bt_audio_media_control(), ver web_server.c). Usa o mesmo
 * host configurado em NVS_KEY_SLIM_HOST (ver config.h). Bloqueia por até
 * ~3s (timeout de socket) -- chamar só a partir da task do httpd, nunca de
 * uma task de tempo real. Retorna false se o host não está configurado, a
 * conexão falhar, ou "cmd" for desconhecido. */
bool lms_cli_send_transport(const char *cmd);
