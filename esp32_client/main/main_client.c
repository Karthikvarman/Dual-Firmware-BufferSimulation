#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_idf_version.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "circular_buffer.h"
#include "protocol.h"

static const char *TAG = "CLIENT_NODE";

#define CIRCULAR_BUFFER_CAPACITY 16U
#define TIMER_PERIOD_US 10000000ULL 
#define EVENT_QUEUE_DEPTH 16U

static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                                          0xFF, 0xFF, 0xFF};

static telemetry_record_t s_telemetry_storage[CIRCULAR_BUFFER_CAPACITY];
static circular_buffer_t s_circ_buf;
static SemaphoreHandle_t s_buf_mutex = NULL;

typedef enum {
  CLIENT_EVT_TIMER_TICK,
  CLIENT_EVT_PACKET_RX
} client_event_type_t;

typedef struct {
  client_event_type_t type;
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  wireless_packet_t packet;
  size_t packet_len;
} client_event_t;

static QueueHandle_t s_client_event_queue = NULL;
static esp_timer_handle_t s_periodic_timer = NULL;
static uint32_t s_record_sequence = 0U;
static uint16_t s_tx_sequence = 0U;

static void IRAM_ATTR on_periodic_timer_tick(void *arg) {
  client_event_t evt;
  evt.type = CLIENT_EVT_TIMER_TICK;
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  (void)arg;
  if (s_client_event_queue != NULL) {
    xQueueSendFromISR(s_client_event_queue, &evt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }
}

#if defined(ESP_IDF_VERSION) &&                                                \
    (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
static void on_esp_now_recv(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data, int len)
#else
static void on_esp_now_recv(const uint8_t *mac_addr, const uint8_t *data,
                            int len)
#endif
{
  if ((data == NULL) || (len <= 0) ||
      ((size_t)len > sizeof(wireless_packet_t))) {
    return;
  }

  client_event_t evt;
  evt.type = CLIENT_EVT_PACKET_RX;
#if defined(ESP_IDF_VERSION) &&                                                \
    (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
  if (recv_info != NULL) {
    memcpy(evt.mac_addr, recv_info->src_addr, ESP_NOW_ETH_ALEN);
  }
#else
  if (mac_addr != NULL) {
    memcpy(evt.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  }
#endif
  memcpy(&evt.packet, data, (size_t)len);
  evt.packet_len = (size_t)len;

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (s_client_event_queue != NULL) {
    xQueueSendFromISR(s_client_event_queue, &evt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }
}

static esp_err_t client_send_packet(const uint8_t *dest_mac, uint8_t opcode,
                                    const void *payload, uint8_t payload_len) {
  wireless_packet_t tx_frame;
  size_t frame_bytes = proto_pack_frame(&tx_frame, ++s_tx_sequence, opcode,
                                        payload, payload_len);

  return esp_now_send(dest_mac, (const uint8_t *)&tx_frame, frame_bytes);
}

static void capture_system_telemetry(telemetry_record_t *rec,
                                     uint16_t slot_id) {
  rec->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
  rec->sequence_id = ++s_record_sequence;
  rec->free_heap_bytes = (uint32_t)esp_get_free_heap_size();
  rec->min_free_heap_bytes = (uint32_t)esp_get_minimum_free_heap_size();
  rec->chip_temp_c = 38; 
  rec->wifi_rssi_dbm = -45;
  rec->buffer_slot_id = slot_id;
  rec->cpu_freq_mhz = 240U;
}

static void client_worker_task(void *pvParameters) {
  client_event_t evt;
  char exception_str[160];

  (void)pvParameters;

  ESP_LOGI(TAG, "Client Worker Task initialized. Buffer capacity: %u records.",
           CIRCULAR_BUFFER_CAPACITY);

  while (1) {
    
    if (xQueueReceive(s_client_event_queue, &evt, portMAX_DELAY) == pdTRUE) {

      switch (evt.type) {

      case CLIENT_EVT_TIMER_TICK: {
        
        telemetry_record_t record;
        size_t written_slot = 0U;
        cb_status_t status;

        capture_system_telemetry(&record, 0U);

        xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
        status = cb_write(&s_circ_buf, &record, &written_slot);
        if (status == CB_OK) {
          
          record.buffer_slot_id = (uint16_t)written_slot;
          ESP_LOGI(TAG,
                   "[0.1 Hz WRITE] Slot #%u: Seq=%lu, Uptime=%lums, "
                   "FreeHeap=%lu bytes",
                   (unsigned int)written_slot,
                   (unsigned long)record.sequence_id,
                   (unsigned long)record.timestamp_ms,
                   (unsigned long)record.free_heap_bytes);
        } else if (status == CB_ERR_FULL) {
          
          cb_format_full_exception(&s_circ_buf, exception_str,
                                   sizeof(exception_str));
          ESP_LOGE(TAG, "%s", exception_str);

          proto_alert_full_payload_t alert;
          alert.blocked_write_idx = (uint32_t)s_circ_buf.head;
          alert.buffer_capacity = (uint32_t)s_circ_buf.capacity;
          alert.total_exceptions = s_circ_buf.full_exceptions;
          alert.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

          (void)client_send_packet(s_broadcast_mac, ALERT_BUFFER_FULL, &alert,
                                   sizeof(alert));
        } else if (status == CB_ERR_WRITE_DISABLED) {
          ESP_LOGW(TAG, "[0.1 Hz WRITE] Writing is currently PAUSED by user "
                        "telecommand.");
        }
        xSemaphoreGive(s_buf_mutex);
        break;
      }

      case CLIENT_EVT_PACKET_RX: {
        
        if (!proto_verify_frame(&evt.packet, evt.packet_len)) {
          ESP_LOGW(TAG, "Corrupt wireless packet received. CRC mismatch.");
          break;
        }

        switch (evt.packet.opcode) {

        case CMD_READ_REQ: {
          
          telemetry_record_t record;
          size_t read_slot = 0U;
          cb_status_t status;

          xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
          status = cb_read(&s_circ_buf, &record, &read_slot);
          xSemaphoreGive(s_buf_mutex);

          if (status == CB_OK) {
            ESP_LOGI(TAG,
                     "[TELECOMMAND READ] Extracted oldest slot #%u (Seq=%lu). "
                     "Slot freed.",
                     (unsigned int)read_slot,
                     (unsigned long)record.sequence_id);
            (void)client_send_packet(evt.mac_addr, RESP_READ_DATA, &record,
                                     sizeof(record));
          } else if (status == CB_ERR_EMPTY) {
            ESP_LOGW(TAG, "[TELECOMMAND READ] Buffer is EMPTY.");
            (void)client_send_packet(evt.mac_addr, RESP_BUFFER_EMPTY, NULL, 0U);
          }
          break;
        }

        case CMD_PEEK_REQ: {
          
          telemetry_record_t record;
          size_t peek_slot = 0U;
          cb_status_t status;

          xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
          status = cb_peek(&s_circ_buf, &record, &peek_slot);
          xSemaphoreGive(s_buf_mutex);

          if (status == CB_OK) {
            (void)client_send_packet(evt.mac_addr, RESP_PEEK_DATA, &record,
                                     sizeof(record));
          } else {
            (void)client_send_packet(evt.mac_addr, RESP_BUFFER_EMPTY, NULL, 0U);
          }
          break;
        }

        case CMD_STATUS_REQ: {
          
          cb_diagnostics_t diag;
          proto_diagnostics_payload_t payload;

          xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
          (void)cb_get_diagnostics(&s_circ_buf, &diag);
          xSemaphoreGive(s_buf_mutex);

          payload.capacity = (uint32_t)diag.capacity;
          payload.count = (uint32_t)diag.count;
          payload.write_index = (uint32_t)diag.write_index;
          payload.read_index = (uint32_t)diag.read_index;
          payload.total_writes = diag.total_writes;
          payload.total_reads = diag.total_reads;
          payload.full_exceptions = diag.full_exceptions;
          payload.is_full = diag.is_full ? 1U : 0U;
          payload.is_empty = diag.is_empty ? 1U : 0U;
          payload.write_enabled = diag.write_enabled ? 1U : 0U;
          payload.reserved = 0U;

          (void)client_send_packet(evt.mac_addr, RESP_STATUS_DATA, &payload,
                                   sizeof(payload));
          break;
        }

        case CMD_PAUSE_WR_REQ: {
          xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
          (void)cb_pause_write(&s_circ_buf);
          xSemaphoreGive(s_buf_mutex);
          ESP_LOGI(TAG, "Telecommand PAUSE WRITE executed.");
          (void)client_send_packet(evt.mac_addr, MSG_ACK, NULL, 0U);
          break;
        }

        case CMD_RESUME_WR_REQ: {
          xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
          (void)cb_resume_write(&s_circ_buf);
          xSemaphoreGive(s_buf_mutex);
          ESP_LOGI(TAG, "Telecommand RESUME WRITE executed.");
          (void)client_send_packet(evt.mac_addr, MSG_ACK, NULL, 0U);
          break;
        }

        case CMD_RESTART_RD_REQ: {
          xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
          (void)cb_restart_read(&s_circ_buf);
          xSemaphoreGive(s_buf_mutex);
          ESP_LOGI(TAG, "Telecommand RESTART READ executed.");
          (void)client_send_packet(evt.mac_addr, MSG_ACK, NULL, 0U);
          break;
        }

        case CMD_RESET_BUF_REQ: {
          xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
          (void)cb_reset(&s_circ_buf);
          xSemaphoreGive(s_buf_mutex);
          ESP_LOGI(TAG, "Telecommand RESET BUFFER executed.");
          (void)client_send_packet(evt.mac_addr, MSG_ACK, NULL, 0U);
          break;
        }

        default:
          ESP_LOGW(TAG, "Unknown opcode 0x%02X", evt.packet.opcode);
          (void)client_send_packet(evt.mac_addr, MSG_NACK, NULL, 0U);
          break;
        }
        break;
      }

      default:
        break;
      }
    }
  }
}

static void init_wifi_espnow(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_recv_cb(on_esp_now_recv));

  esp_now_peer_info_t peer_info;
  memset(&peer_info, 0, sizeof(peer_info));
  memcpy(peer_info.peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
  peer_info.channel = 1;
  peer_info.encrypt = false;
  ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));
}

void app_main(void) {
  ESP_LOGI(TAG, "=== ESP32 Circular Buffer Client Initializing ===");

  esp_err_t ret = nvs_flash_init();
  if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) ||
      (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  cb_status_t cb_ret =
      cb_init(&s_circ_buf, (uint8_t *)s_telemetry_storage,
              CIRCULAR_BUFFER_CAPACITY, sizeof(telemetry_record_t));
  if (cb_ret != CB_OK) {
    ESP_LOGE(TAG,
             "Fatal: Circular buffer initialization failed with code 0x%02X",
             cb_ret);
    return;
  }

  s_buf_mutex = xSemaphoreCreateMutex();
  s_client_event_queue =
      xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(client_event_t));

  init_wifi_espnow();

  xTaskCreate(client_worker_task, "client_worker", 4096, NULL, 5, NULL);

  const esp_timer_create_args_t timer_args = {.callback =
                                                  &on_periodic_timer_tick,
                                              .arg = NULL,
                                              .dispatch_method = ESP_TIMER_TASK,
                                              .name = "cb_0_1hz_timer"};
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_periodic_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(s_periodic_timer, TIMER_PERIOD_US));

  ESP_LOGI(TAG, "0.1 Hz Periodic Telemetry Timer Started (Period: 10.0s). "
                "Ready for telecommands.");
}
