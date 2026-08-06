# Componente "audio_board" customizado para o ESP32 Audio Kit V2.2 (ESP32-A1S).
#
# O ESP-ADF oficial não tem suporte a essa placa (só LyraT, Korvo, Box,
# Kaluga etc.). Vários componentes centrais do ADF (audio_hal, audio_stream,
# esp_peripherals, display_service) dependem, pelo NOME, de um componente
# chamado exatamente "audio_board" que forneça board.h / board_pins_config.h.
# Este componente substitui o do ADF (excluído via EXCLUDE_COMPONENTS no
# CMakeLists.txt raiz) com a pinagem real deste kit.

idf_component_register(
    SRCS "board.c" "board_pins_config.c"
    INCLUDE_DIRS "." "include"
    PRIV_REQUIRES audio_hal audio_sal
)
