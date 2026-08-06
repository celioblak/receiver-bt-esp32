#pragma once

#include "audio_hal.h"
#include "board_def.h"
#include "board_pins_config.h"

#ifdef __cplusplus
extern "C" {
#endif

struct audio_board_handle {
    audio_hal_handle_t audio_hal;
};

typedef struct audio_board_handle *audio_board_handle_t;

/* Inicializa a placa (atualmente: só o codec ES8388). Botões, LED e SD card
 * do kit não são usados por este projeto ainda — quando forem, entram aqui
 * seguindo o mesmo padrão do board.c das placas oficiais do ADF. */
audio_board_handle_t audio_board_init(void);

audio_hal_handle_t audio_board_codec_init(void);

audio_board_handle_t audio_board_get_handle(void);

esp_err_t audio_board_deinit(audio_board_handle_t audio_board);

#ifdef __cplusplus
}
#endif
