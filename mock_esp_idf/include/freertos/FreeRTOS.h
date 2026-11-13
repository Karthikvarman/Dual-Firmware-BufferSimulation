#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE              ((BaseType_t)1)
#define pdFALSE             ((BaseType_t)0)
#define pdPASS              (pdTRUE)
#define pdFAIL              (pdFALSE)

#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFUL)
#define portTICK_PERIOD_MS  ((TickType_t)1)

#define portYIELD_FROM_ISR() do {} while(0)

#endif /* FREERTOS_H */

