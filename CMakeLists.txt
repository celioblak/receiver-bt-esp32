cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Componentes extras do ESP-ADF (definido via ADF_PATH em platformio.ini)
if(DEFINED ADF_PATH)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${ADF_PATH}/components"
    )
endif()

project(receiver-bt-esp32)
