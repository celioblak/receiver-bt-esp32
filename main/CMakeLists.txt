idf_component_register(
    SRCS "main.c" "storage.c" "logger.c" "es8388.c" "audio_codec.c" "bt_audio.c"
         "relay_control.c" "wifi_manager.c" "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer driver bt esp_wifi esp_netif esp_event mdns
             esp_http_server spiffs json
)
