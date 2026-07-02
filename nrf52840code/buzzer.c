#include "buzzer.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_gpio.h>

#define BUZZER_PIN NRF_GPIO_PIN_MAP(0, 15)

#define BEEP_SHORT_MS   100
#define BEEP_LONG_MS   500

int buzzer_init(void)
{
    printk("buzzer_init\n");

    nrf_gpio_cfg_output(BUZZER_PIN);

    /* Active LOW buzzer:
     * HIGH = OFF
     * LOW  = ON
     */
    nrf_gpio_pin_set(BUZZER_PIN);

    printk("buzzer_init done\n");
    return 0;
}

void buzzer_beep_short(void)
{
    printk("BEEP SHORT\n");

    nrf_gpio_pin_clear(BUZZER_PIN);  // ON
    k_sleep(K_MSEC(BEEP_SHORT_MS));
    nrf_gpio_pin_set(BUZZER_PIN);    // OFF
}

void buzzer_beep_long(void)
{
    printk("BEEP LONG\n");

    nrf_gpio_pin_clear(BUZZER_PIN);  // ON
    k_sleep(K_MSEC(BEEP_LONG_MS));
    nrf_gpio_pin_set(BUZZER_PIN);    // OFF
}