idf_component_register(SRCS "main.c" "beacon_generator.c" "config_parser.c" "ble_multi_adv.c" "rgb_led.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event nvs_flash esp_timer driver esp_driver_rmt bt)
