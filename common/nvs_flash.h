#ifndef NVS_FLASH_H
#define NVS_FLASH_H

#include "esp_system.h"

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);

#endif /* NVS_FLASH_H */

