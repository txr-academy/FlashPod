#include "ir_sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define IR_SENSOR_NODE   DT_ALIAS(ir_sensor)

static const struct gpio_dt_spec ir_gpio =
    GPIO_DT_SPEC_GET(IR_SENSOR_NODE, gpios);

int ir_sensor_init(void)
{
    if (!gpio_is_ready_dt(&ir_gpio)) {
        return -ENODEV;
    }
    return gpio_pin_configure_dt(&ir_gpio, GPIO_INPUT | GPIO_PULL_UP);
}

bool ir_sensor_detected(void)
{
    int val = gpio_pin_get_dt(&ir_gpio);
    if (val < 0) {
        return false;
    }
    return (val == 1);  /* GPIO_ACTIVE_LOW: physical LOW → logical 1 */
}