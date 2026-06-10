#ifndef IR_SENSOR_H
#define IR_SENSOR_H

#include <stdbool.h>

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

#endif /* IR_SENSOR_H */