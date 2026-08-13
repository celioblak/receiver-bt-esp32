idf_component_register(
    SRCS "main.c" "storage.c" "logger.c" "es8388.c" "audio_codec.c" "audio_agc.c" "bt_audio.c"
         "relay_control.c" "wifi_manager.c" "web_server.c" "pairing.c" "mqtt_ha.c"
         "ota_manager.c" "dlna_renderer.c" "slimproto.c" "lms_cli.c" "lms_metadata.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer driver bt esp_wifi esp_netif esp_event mdns
             esp_http_server spiffs cjson mqtt app_update lwip esp_websocket_client
)
