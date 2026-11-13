#ifndef ESP_SYSTEM_H
#define ESP_SYSTEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef int esp_err_t;

#define ESP_OK                      0
#define ESP_FAIL                   -1
#define ESP_ERR_NO_MEM              0x101
#define ESP_ERR_INVALID_ARG         0x102
#define ESP_ERR_INVALID_STATE       0x103
#define ESP_ERR_INVALID_SIZE        0x104
#define ESP_ERR_NOT_FOUND           0x105
#define ESP_ERR_NOT_SUPPORTED       0x106
#define ESP_ERR_TIMEOUT             0x107
#define ESP_ERR_NVS_NO_FREE_PAGES   0x1100
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1101

#define ESP_ERROR_CHECK(x) do { esp_err_t __err = (x); (void)__err; } while(0)

uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);

#endif /* ESP_SYSTEM_H */

