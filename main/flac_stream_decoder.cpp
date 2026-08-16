#include "flac_stream_decoder.h"

#include "micro_flac/flac_decoder.h"

#include "esp_heap_caps.h"

struct flac_stream_decoder {
    micro_flac::FLACDecoder decoder;
};

extern "C" flac_stream_decoder_t *flac_stream_decoder_create(void)
{
    /* So dados (nao e stack de execucao de nenhuma task) -- seguro e
     * desejavel em PSRAM. Mesmo padrao de heap_caps_malloc_prefer usado
     * internamente pela propria esphome/micro-flac (ver alloc.h,
     * FLAC_MALLOC): tenta PSRAM primeiro, cai pra RAM interna so se a PSRAM
     * falhar (nao deveria, sobram ~4MB). */
    flac_stream_decoder_t *dec = (flac_stream_decoder_t *)heap_caps_malloc_prefer(
        sizeof(flac_stream_decoder_t), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (dec == nullptr) {
        return nullptr;
    }
    new (dec) flac_stream_decoder_t();

    /* Nao usamos metadados de tags/capa -- so o STREAMINFO (obrigatorio,
     * sempre analisado) importa aqui. Zerar o limite dos outros tipos evita
     * que um VORBIS_COMMENT ou PICTURE grande vindo no cabeçalho do stream
     * force uma alocacao desnecessaria. */
    dec->decoder.set_max_metadata_size(micro_flac::FLAC_METADATA_TYPE_VORBIS_COMMENT, 0);
    dec->decoder.set_max_metadata_size(micro_flac::FLAC_METADATA_TYPE_PICTURE, 0);
    dec->decoder.set_max_metadata_size(micro_flac::FLAC_METADATA_TYPE_APPLICATION, 0);
    dec->decoder.set_max_metadata_size(micro_flac::FLAC_METADATA_TYPE_CUESHEET, 0);
    dec->decoder.set_max_metadata_size(micro_flac::FLAC_METADATA_TYPE_SEEKTABLE, 0);

    return dec;
}

extern "C" void flac_stream_decoder_destroy(flac_stream_decoder_t *dec)
{
    if (dec == nullptr) {
        return;
    }
    dec->~flac_stream_decoder();
    heap_caps_free(dec);
}

extern "C" flac_stream_result_t flac_stream_decoder_feed(flac_stream_decoder_t *dec,
                                                           const uint8_t *input, size_t input_len,
                                                           int32_t *output,
                                                           size_t output_capacity_samples,
                                                           size_t *bytes_consumed,
                                                           size_t *samples_decoded)
{
    if (dec == nullptr) {
        return FLAC_STREAM_ERROR;
    }
    size_t consumed = 0;
    size_t decoded = 0;
    micro_flac::FLACDecoderResult r =
        dec->decoder.decode(input, input_len, output, output_capacity_samples, consumed, decoded);
    if (bytes_consumed != nullptr) {
        *bytes_consumed = consumed;
    }
    if (samples_decoded != nullptr) {
        *samples_decoded = decoded;
    }

    switch (r) {
        case micro_flac::FLAC_DECODER_SUCCESS: return FLAC_STREAM_OK;
        case micro_flac::FLAC_DECODER_HEADER_READY: return FLAC_STREAM_HEADER_READY;
        case micro_flac::FLAC_DECODER_END_OF_STREAM: return FLAC_STREAM_END_OF_STREAM;
        case micro_flac::FLAC_DECODER_NEED_MORE_DATA: return FLAC_STREAM_NEED_MORE_DATA;
        default: return FLAC_STREAM_ERROR;
    }
}

extern "C" uint32_t flac_stream_decoder_sample_rate(const flac_stream_decoder_t *dec)
{
    return dec != nullptr ? dec->decoder.get_stream_info().sample_rate() : 0;
}

extern "C" uint32_t flac_stream_decoder_channels(const flac_stream_decoder_t *dec)
{
    return dec != nullptr ? dec->decoder.get_stream_info().num_channels() : 0;
}

extern "C" uint32_t flac_stream_decoder_bits_per_sample(const flac_stream_decoder_t *dec)
{
    return dec != nullptr ? dec->decoder.get_stream_info().bits_per_sample() : 0;
}

extern "C" uint32_t flac_stream_decoder_output_buffer_samples(const flac_stream_decoder_t *dec)
{
    return dec != nullptr ? dec->decoder.get_output_buffer_size_samples() : 0;
}
