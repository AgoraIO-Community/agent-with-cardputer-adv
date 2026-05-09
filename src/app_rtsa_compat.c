#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

/*
 * The prebuilt RTSA archive expects a few legacy platform symbols that are not
 * exported by ESP-IDF 6 with the current toolchain. Provide narrow shims here
 * rather than patching the binary SDK.
 */

uint32_t ahpl_bswap_32(uint32_t value)
{
    return __builtin_bswap32(value);
}

uint64_t ahpl_bswap_64(uint64_t value)
{
    return __builtin_bswap64(value);
}

BaseType_t xTaskCreateRestrictedPinnedToCore(const TaskParameters_t * const pxTaskDefinition,
                                             TaskHandle_t * const pxCreatedTask,
                                             const BaseType_t xCoreID)
{
    if (pxTaskDefinition == NULL) {
        return pdFAIL;
    }

    return xTaskCreatePinnedToCore(pxTaskDefinition->pvTaskCode,
                                   pxTaskDefinition->pcName,
                                   pxTaskDefinition->usStackDepth,
                                   pxTaskDefinition->pvParameters,
                                   pxTaskDefinition->uxPriority,
                                   pxCreatedTask,
                                   xCoreID);
}

/*
 * Some objects inside the RTSA archive were built against a libc variant that
 * exported `_ctype_`. Export a zeroed compatibility table under that exact
 * linker symbol name without colliding with the current libc headers.
 */
unsigned char app_rtsa_ctype_compat[257] __asm__("_ctype_");
