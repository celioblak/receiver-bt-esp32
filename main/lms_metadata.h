#pragma once

#include <stdbool.h>

/* Metadados de "tocando agora" (título/artista/álbum) via API própria do
 * Music Assistant (WebSocket, porta 8095, path /ws) -- NÃO vem pelo
 * protocolo Slimproto/LMS clássico (porta 9090, ver lms_cli.h): testado ao
 * vivo contra o MA, o campo playlist_tracks fica 0 por ali mesmo tocando.
 * A API própria exige um token JWT de longa duração (gerado no perfil do
 * usuário dentro do MA -- não existe token fixo configurável do lado do
 * servidor nesta versão) guardado em NVS_KEY_MA_TOKEN (ver config.h,
 * aceito via POST /api/config, nunca devolvido por GET). Sem token
 * configurado, o recurso fica desativado em silêncio (mesmo padrão do MQTT
 * sem broker configurado).
 *
 * Usa o mesmo host de NVS_KEY_SLIM_HOST (Slimproto e a API WebSocket rodam
 * no mesmo host do Music Assistant). Consulta "player_queues/get" a cada
 * poucos segundos via polling (não assina eventos push -- mais simples e
 * suficiente pra exibir status, não precisa ser instantâneo). */

void lms_metadata_init(void);

typedef struct {
    bool valid;    /* true assim que a primeira resposta chegou */
    bool playing;  /* queue state == "playing" */
    char title[64];
    char artist[64];
    char album[64];
} lms_metadata_t;

void lms_metadata_get(lms_metadata_t *out);

/* true assim que lms_metadata_init() encontrou um token configurado (não
 * diz se ele é válido -- só que o recurso não está desativado por falta de
 * configuração). Usado pra distinguir "não configurado" de "configurado
 * mas rejeitado" na UI/Home Assistant. */
bool lms_metadata_is_configured(void);

/* true se a última tentativa de autenticação com o token configurado foi
 * rejeitada pelo servidor (token inválido/expirado) -- token JWT de longa
 * duração do Music Assistant expira (normalmente depois de 1 ano); sem
 * isso visível em algum lugar, título/artista/álbum somem em silêncio e
 * ninguém percebe o motivo. false também enquanto ainda não tentou
 * autenticar nenhuma vez (não confundir com "falhou"). */
bool lms_metadata_auth_failed(void);
