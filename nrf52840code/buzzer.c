#include "buzzer.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_gpio.h>

#define BUZZER_PIN NRF_GPIO_PIN_MAP(0, 15)

#define BEEP_SHORT_MS    100   /* single beep ON duration  */
#define BEEP_GAP_MS       80   /* gap between double beep  */

int buzzer_init(void)
{
    printk("buzzer_init\n");
    nrf_gpio_cfg_output(BUZZER_PIN);
    nrf_gpio_pin_set(BUZZER_PIN);   /* HIGH = OFF (active LOW) */
    printk("buzzer_init done\n");
    return 0;
}

/* Single short beep — correct tap */
void buzzer_beep_short(void)
{
    printk("BEEP SHORT\n");
    nrf_gpio_pin_clear(BUZZER_PIN);   /* ON  */
    k_sleep(K_MSEC(BEEP_SHORT_MS));
    nrf_gpio_pin_set(BUZZER_PIN);     /* OFF */
}

/* Double short beep — wrong tap or timeout */
void buzzer_beep_long(void)
{
    printk("BEEP LONG (double)\n");
    nrf_gpio_pin_clear(BUZZER_PIN);   /* ON  */
    k_sleep(K_MSEC(BEEP_SHORT_MS));
    nrf_gpio_pin_set(BUZZER_PIN);     /* OFF */
    k_sleep(K_MSEC(BEEP_GAP_MS));
    nrf_gpio_pin_clear(BUZZER_PIN);   /* ON  */
    k_sleep(K_MSEC(BEEP_SHORT_MS));
    nrf_gpio_pin_set(BUZZER_PIN);     /* OFF */
}