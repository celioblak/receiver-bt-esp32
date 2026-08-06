idf_component_register(
    SRCS "main.c" "storage.c" "logger.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer
)
