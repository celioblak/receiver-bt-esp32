cmake_minimum_required(VERSION 3.16)

# O ESP32 Audio Kit V2.2 (ESP32-A1S) não tem placa oficial no ESP-ADF.
# audio_hal, audio_stream, esp_peripherals e display_service dependem, pelo
# NOME, de um componente "audio_board". Excluímos o do ADF e usamos o nosso
# (components/audio_board), com a pinagem real deste kit. Ver
# components/audio_board/CMakeLists.txt para o racional completo.
set(EXCLUDE_COMPONENTS "audio_board")

# Componentes extras do ESP-ADF (definido via ADF_PATH em platformio.ini)
if(DEFINED ADF_PATH)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${ADF_PATH}/components"
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(receiver-bt-esp32)
