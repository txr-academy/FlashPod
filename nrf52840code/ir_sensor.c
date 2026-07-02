#include "ir_sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define IR_SENSOR_NODE   DT_ALIAS(ir_sensor)

static const struct gpio_dt_spec ir_gpio =
    GPIO_DT_SPEC_GET(IR_SENSOR_NODE, gpios);

static struct gpio_callback ir_cb_data;
static struct k_sem *ir_sem_ptr;

/* ── GPIO ISR ──────────────────────────────────────────── */
static void ir_gpio_isr(const struct device *dev,
                        struct gpio_callback *cb,
                        uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    if (ir_sem_ptr) {
        k_sem_give(ir_sem_ptr);
    }
}

/* ── Basic init (GPIO input, no interrupt) ─────────────── */
int ir_sensor_init(void)
{
    if (!gpio_is_ready_dt(&ir_gpio)) {
        return -ENODEV;
    }
    return gpio_pin_configure_dt(&ir_gpio, GPIO_INPUT | GPIO_PULL_UP);
}

/* ── Polling read (kept for compatibility) ─────────────── */
bool ir_sensor_detected(void)
{
    int val = gpio_pin_get_dt(&ir_gpio);
    if (val < 0) {
        return false;
    }
    return (val == 1);  /* GPIO_ACTIVE_LOW: physical LOW → logical 1 */
}

/* ── Interrupt init ────────────────────────────────────── */
int ir_sensor_init_interrupt(struct k_sem *sem)
{
    int ret;

    ir_sem_ptr = sem;

    /* Configure interrupt on active edge (object detected) */
    ret = gpio_pin_interrupt_configure_dt(&ir_gpio,
                                          GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        return ret;
    }

    gpio_init_callback(&ir_cb_data, ir_gpio_isr, BIT(ir_gpio.pin));
    ret = gpio_add_callback(ir_gpio.port, &ir_cb_data);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/* ── Enable / Disable interrupt ────────────────────────── */
void ir_sensor_int_enable(void)
{
    gpio_pin_interrupt_configure_dt(&ir_gpio, GPIO_INT_EDGE_TO_ACTIVE);
}

void ir_sensor_int_disable(void)
{
    gpio_pin_interrupt_configure_dt(&ir_gpio, GPIO_INT_DISABLE);
}