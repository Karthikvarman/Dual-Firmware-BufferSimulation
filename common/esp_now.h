#ifndef ESP_NOW_H
#define ESP_NOW_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_system.h"
#include "esp_idf_version.h"

#define ESP_NOW_ETH_ALEN 6

typedef enum {
    ESP_NOW_SEND_SUCCESS = 0,
    ESP_NOW_SEND_FAIL
} esp_now_send_status_t;

typedef struct {
    uint8_t peer_addr[ESP_NOW_ETH_ALEN];
    uint8_t lmk[16];
    uint8_t channel;
    uint8_t ifidx;
    bool encrypt;
    void *priv;
} esp_now_peer_info_t;

typedef struct {
    uint8_t src_addr[ESP_NOW_ETH_ALEN];
    uint8_t des_addr[ESP_NOW_ETH_ALEN];
    void *rx_ctrl;
} esp_now_recv_info_t;

#if defined(ESP_IDF_VERSION) && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
typedef void (*esp_now_recv_cb_t)(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len);
#else
typedef void (*esp_now_recv_cb_t)(const uint8_t *mac_addr, const uint8_t *data, int data_len);
#endif

typedef void (*esp_now_send_cb_t)(const uint8_t *mac_addr, esp_now_send_status_t status);

esp_err_t esp_now_init(void);
esp_err_t esp_now_deinit(void);
esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb);
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer);
esp_err_t esp_now_send(const uint8_t *peer_addr, const uint8_t *data, size_t len);

#endif /* ESP_NOW_H */
