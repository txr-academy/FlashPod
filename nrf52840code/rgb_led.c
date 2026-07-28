#include "rgb_led.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/device.h>
#include <string.h>

#define STRIP_NODE       DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_NUM_PIXELS];

int rgb_led_init(void)
{
    if (!device_is_ready(strip)) {
        return -1;
    }
    rgb_led_off();
    return 0;
}

void rgb_led_set_all(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    uint8_t sr = (uint8_t)((r * brightness) / 0xFF);
    uint8_t sg = (uint8_t)((g * brightness) / 0xFF);
    uint8_t sb = (uint8_t)((b * brightness) / 0xFF);

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i].r = sr;
        pixels[i].g = sg;
        pixels[i].b = sb;
    }
    led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
}

void rgb_led_off(void)
{
    memset(pixels, 0, sizeof(pixels));
    led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
}
