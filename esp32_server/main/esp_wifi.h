#ifndef ESP_WIFI_H
#define ESP_WIFI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_system.h"

typedef enum {
    WIFI_MODE_NULL = 0,
    WIFI_MODE_STA,
    WIFI_MODE_AP,
    WIFI_MODE_APSTA,
    WIFI_MODE_MAX
} wifi_mode_t;

typedef enum {
    WIFI_STORAGE_FLASH,
    WIFI_STORAGE_RAM
} wifi_storage_t;

typedef enum {
    WIFI_SECOND_CHAN_NONE = 0,
    WIFI_SECOND_CHAN_ABOVE,
    WIFI_SECOND_CHAN_BELOW,
} wifi_second_chan_t;

typedef struct {
    int dummy;
} wifi_init_config_t;

#define WIFI_INIT_CONFIG_DEFAULT() { 0 }

esp_err_t esp_netif_init(void);
esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_set_storage(wifi_storage_t storage);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_set_channel(uint8_t primary, wifi_second_chan_t second);

#endif /* ESP_WIFI_H */

