idf_component_register(SRCS "main.c" "remote.c" "usb_keyboard.c" "font_cn28.c" INCLUDE_DIRS "." REQUIRES nvs_flash bt espressif__tinyusb esp_psram usb)
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
