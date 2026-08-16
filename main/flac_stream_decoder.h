#pragma once

#include <stddef.h>
#include <stdint.h>

/* Ponte C para o decoder micro-flac (API em C++) -- ver flac_stream_decoder.cpp.
 * Isola o C++ pra quem consome isso poder continuar em C puro. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flac_stream_decoder flac_stream_decoder_t;

typedef enum {
    FLAC_STREAM_OK = 0,             /* um bloco de amostras foi decodificado */
    FLAC_STREAM_HEADER_READY = 1,   /* STREAMINFO disponivel, chame os getters abaixo */
    FLAC_STREAM_END_OF_STREAM = 2,
    FLAC_STREAM_NEED_MORE_DATA = 3, /* precisa de mais bytes de entrada antes do proximo passo */
    FLAC_STREAM_ERROR = -1,
} flac_stream_result_t;

flac_stream_decoder_t *flac_stream_decoder_create(void);
void flac_stream_decoder_destroy(flac_stream_decoder_t *dec);

/* output recebe amostras int32_t alinhadas a esquerda (left-justified) --
 * independente da profundidade de bits real do FLAC (16/24-bit), os 16 bits
 * mais significativos de cada amostra ja ficam prontos pra um shift simples
 * (>>16) na conversao pra 16-bit, igual ao truncamento ja usado no caminho
 * PCM/WAV existente. */
flac_stream_result_t flac_stream_decoder_feed(flac_stream_decoder_t *dec, const uint8_t *input,
                                               size_t input_len, int32_t *output,
                                               size_t output_capacity_samples,
                                               size_t *bytes_consumed, size_t *samples_decoded);

/* Validos somente apos FLAC_STREAM_HEADER_READY */
uint32_t flac_stream_decoder_sample_rate(const flac_stream_decoder_t *dec);
uint32_t flac_stream_decoder_channels(const flac_stream_decoder_t *dec);
uint32_t flac_stream_decoder_bits_per_sample(const flac_stream_decoder_t *dec);
uint32_t flac_stream_decoder_output_buffer_samples(const flac_stream_decoder_t *dec);

#ifdef __cplusplus
}
#endif
