#ifndef BUZZER_H
#define BUZZER_H

/**
 * @brief Initialise the buzzer GPIO pin.
 * @return 0 on success, negative errno on failure.
 */
int buzzer_init(void);

/**
 * @brief Short beep — 500 ms (correct IR tap).
 */
void buzzer_beep_short(void);

/**
 * @brief Long beep — 1000 ms (wrong tap or timeout).
 */
void buzzer_beep_long(void);

#endif /* BUZZER_H */