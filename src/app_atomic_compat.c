#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_atomic_lock = portMUX_INITIALIZER_UNLOCKED;

uint8_t __atomic_fetch_add_1(volatile void *ptr, uint8_t value, int memorder)
{
    volatile uint8_t *target = (volatile uint8_t *)ptr;
    uint8_t previous;

    (void)memorder;
    portENTER_CRITICAL(&s_atomic_lock);
    previous = *target;
    *target = (uint8_t)(previous + value);
    portEXIT_CRITICAL(&s_atomic_lock);
    return previous;
}

uint8_t __atomic_fetch_sub_1(volatile void *ptr, uint8_t value, int memorder)
{
    volatile uint8_t *target = (volatile uint8_t *)ptr;
    uint8_t previous;

    (void)memorder;
    portENTER_CRITICAL(&s_atomic_lock);
    previous = *target;
    *target = (uint8_t)(previous - value);
    portEXIT_CRITICAL(&s_atomic_lock);
    return previous;
}
