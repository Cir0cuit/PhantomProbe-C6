# embedded_config.h is generated from ../ssid_list.conf, or from ../ssid_list.local.conf when
# that file exists. PlatformIO also runs the generator
# before every build (platformio.ini); this covers plain idf.py builds.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT PYTHON)
        set(PYTHON python)
    endif()
    execute_process(COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../scripts/generate_config.py"
                    RESULT_VARIABLE generate_config_result)
    if(NOT generate_config_result EQUAL 0)
        message(FATAL_ERROR "scripts/generate_config.py failed")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../ssid_list.conf")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../ssid_list.local.conf")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../ssid_list.local.conf")
    endif()
endif()

idf_component_register(SRCS "main.c" "beacon_generator.c" "config_parser.c" "ble_multi_adv.c" "rgb_led.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer esp_driver_rmt bt)
