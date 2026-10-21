idf_component_register(SRCS "main.cpp" "pad.cpp" "ble.cpp" "usb.c" "ui.cpp" "onboard.cpp" "font_cn20.c" "font_port17.c" "app_icons.c"
 INCLUDE_DIRS "."
 REQUIRES esp-nimble-cpp espressif__tinyusb espressif__esp_audio_codec espressif__esp_codec_dev espressif__esp_lvgl_port espressif__esp_io_expander_tca9554 board_port nvs_flash esp_timer driver usb)
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}")
