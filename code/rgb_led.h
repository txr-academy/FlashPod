#ifndef RGB_LED_H
#define RGB_LED_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/drivers/led_strip.h>

/* ── Brightness ─────────────────────────────────────────── */
#define BRIGHTNESS_HALF  0x20
#define BRIGHTNESS_FULL  0xFF

/* ── Function declarations ──────────────────────────────── */
int  rgb_led_init(void);
void rgb_led_set_all(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);
void rgb_led_off(void);

#endif /* RGB_LED_H */