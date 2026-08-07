idf_component_register(
    SRCS "main.c" "storage.c" "logger.c" "es8388.c" "audio_codec.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer driver
)
