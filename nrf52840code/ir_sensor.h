#ifndef IR_SENSOR_H
#define IR_SENSOR_H

#include <stdbool.h>
#include <zephyr/kernel.h>

/**
 * @brief Initialise the IR sensor GPIO pin.
 * @return 0 on success, negative errno on failure.
 */
int ir_sensor_init(void);

/**
 * @brief Read the IR sensor.
 * @return true  – object detected (hand close)
 *         false – nothing detected
 */
bool ir_sensor_detected(void);

/**
 * @brief Configure a GPIO interrupt on the IR sensor pin.
 *
 * When the sensor triggers (active edge), the provided semaphore
 * is given from ISR context so a waiting thread can wake up
 * immediately.
 *
 * @param sem  Pointer to the semaphore to give on detection.
 * @return 0 on success, negative errno on failure.
 */
int ir_sensor_init_interrupt(struct k_sem *sem);

/**
 * @brief Enable the IR sensor GPIO interrupt.
 */
void ir_sensor_int_enable(void);

/**
 * @brief Disable the IR sensor GPIO interrupt.
 */
void ir_sensor_int_disable(void);

#endif /* IR_SENSOR_H */