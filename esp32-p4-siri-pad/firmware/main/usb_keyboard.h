#pragma once
#include <stdbool.h>
#include <stdint.h>
void usb_keyboard_start(void);
bool usb_keyboard_connected(void);
bool usb_keyboard_send(uint8_t modifiers, uint8_t key);
bool usb_keyboard_set(uint8_t modifiers, uint8_t key);
void usb_keyboard_release(void);
