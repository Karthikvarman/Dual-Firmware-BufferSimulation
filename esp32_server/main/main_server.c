#include <ctype.h>
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

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "protocol.h"

#ifndef UART_SCLK_DEFAULT
#define UART_SCLK_DEFAULT UART_SCLK_APB
#endif

static const char *TAG = "SERVER_GATEWAY";

#define PC_UART_NUM UART_NUM_0
#define PC_UART_BAUD_RATE 115200
#define UART_RX_BUF_SIZE 1024
#define UART_TX_BUF_SIZE 1024
#define UART_QUEUE_DEPTH 20
#define CMD_LINE_MAX_LEN 64

static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                                          0xFF, 0xFF, 0xFF};

static QueueHandle_t s_uart_event_queue = NULL;
static QueueHandle_t s_wireless_rx_queue = NULL;
static uint16_t s_tx_seq = 0U;

#if defined(ESP_IDF_VERSION) &&                                                \
    (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
static void on_esp_now_recv(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data, int len) {
  (void)recv_info;
#else
static void on_esp_now_recv(const uint8_t *mac_addr, const uint8_t *data,
                            int len) {
  (void)mac_addr;
#endif
  wireless_packet_t pkt;
  if ((data == NULL) || (len <= 0) ||
      ((size_t)len > sizeof(wireless_packet_t))) {
    return;
  }

  memcpy(&pkt, data, (size_t)len);

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (s_wireless_rx_queue != NULL) {
    xQueueSendFromISR(s_wireless_rx_queue, &pkt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }
}

static esp_err_t server_send_command(uint8_t opcode, const void *payload,
                                     uint8_t payload_len) {
  wireless_packet_t pkt;
  size_t frame_bytes =
      proto_pack_frame(&pkt, ++s_tx_seq, opcode, payload, payload_len);

  return esp_now_send(s_broadcast_mac, (const uint8_t *)&pkt, frame_bytes);
}

static void print_help_menu(void) {
  const char *menu =
      "\r\n===================================================================="
      "\r\n"
      "           ESP32 TELEMETRY CIRCULAR BUFFER CONTROLLER              \r\n"
      "====================================================================\r\n"
      " Available Telecommands:\r\n"
      "   READ       (R) : Read oldest unread data; frees slot for "
      "rewriting\r\n"
      "   PEEK       (P) : Inspect oldest unread data without advancing "
      "tail\r\n"
      "   STATUS     (S) : Display buffer diagnostics, capacity, & pointers "
      "\r\n"
      "   PAUSE_WR       : Pause 0.1 Hz periodic write telemetry "
      "acquisition\r\n"
      "   RESUME_WR      : Resume periodic write; appends from last written\r\n"
      "   RESTART_RD     : Reset/resume read session\r\n"
      "   RESET_BUF      : Clear & reinitialize circular buffer memory\r\n"
      "   HELP       (?) : Show this telecommand command menu\r\n"
      "====================================================================\r\n"
      "CMD> ";
  uart_write_bytes(PC_UART_NUM, menu, strlen(menu));
}

static void display_telemetry_record(const char *label,
                                     const telemetry_record_t *rec) {
  char buf[256];
  int len = snprintf(
      buf, sizeof(buf),
      "\r\n--- [%s TELEMETRY RECORD] ---\r\n"
      "  Buffer Slot ID   : #%u\r\n"
      "  Sequence Number  : %lu\r\n"
      "  Client Uptime    : %lu ms (%lu.%03lu s)\r\n"
      "  Free Heap Memory : %lu bytes\r\n"
      "  Min Free Heap    : %lu bytes\r\n"
      "  Chip Temperature : %d C\r\n"
      "  Wi-Fi RSSI       : %d dBm\r\n"
      "  CPU Frequency    : %lu MHz\r\n"
      "------------------------------------\r\nCMD> ",
      label, (unsigned int)rec->buffer_slot_id, (unsigned long)rec->sequence_id,
      (unsigned long)rec->timestamp_ms,
      (unsigned long)(rec->timestamp_ms / 1000U),
      (unsigned long)(rec->timestamp_ms % 1000U),
      (unsigned long)rec->free_heap_bytes,
      (unsigned long)rec->min_free_heap_bytes, (int)rec->chip_temp_c,
      (int)rec->wifi_rssi_dbm, (unsigned long)rec->cpu_freq_mhz);

  if (len > 0) {
    uart_write_bytes(PC_UART_NUM, buf, (size_t)len);
  }
}

static void display_diagnostics(const proto_diagnostics_payload_t *diag) {
  char buf[384];
  int len = snprintf(
      buf, sizeof(buf),
      "\r\n--- [CIRCULAR BUFFER DIAGNOSTICS] ---\r\n"
      "  Total Capacity     : %lu slots\r\n"
      "  Active Stored Data : %lu slots (%lu%% utilized)\r\n"
      "  Free Slots Avail   : %lu slots\r\n"
      "  Next Write Address : Slot #%lu (Head)\r\n"
      "  Next Read Address  : Slot #%lu (Tail)\r\n"
      "  Total Writes Done  : %lu\r\n"
      "  Total Reads Done   : %lu\r\n"
      "  Memory Full Alarms : %lu\r\n"
      "  Buffer State       : %s%s\r\n"
      "  Write Acquisition  : %s\r\n"
      "-------------------------------------\r\nCMD> ",
      (unsigned long)diag->capacity, (unsigned long)diag->count,
      (unsigned long)(diag->capacity > 0 ? (diag->count * 100U / diag->capacity)
                                         : 0U),
      (unsigned long)(diag->capacity - diag->count),
      (unsigned long)diag->write_index, (unsigned long)diag->read_index,
      (unsigned long)diag->total_writes, (unsigned long)diag->total_reads,
      (unsigned long)diag->full_exceptions,
      diag->is_full ? "[FULL SATURATION] " : "",
      diag->is_empty ? "[EMPTY] " : "[ACTIVE DATA]",
      diag->write_enabled ? "ENABLED (0.1 Hz active)" : "PAUSED");

  if (len > 0) {
    uart_write_bytes(PC_UART_NUM, buf, (size_t)len);
  }
}

static void display_full_memory_alarm(const proto_alert_full_payload_t *alert) {
  char buf[350];
  int len = snprintf(
      buf, sizeof(buf),
      "\r\n"
      "========================================================================"
      "========\r\n"
      "*** [CRITICAL EXCEPTION: 0xEFULL_MEM] CLIENT CIRCULAR BUFFER OVERFLOW "
      "***\r\n"
      "  Status          : MEMORY FULL! 0.1 Hz periodic write blocked.\r\n"
      "  Buffer Slot     : Head reached #%lu (Capacity: %lu slots fully "
      "utilized).\r\n"
      "  Client Uptime   : %lu ms\r\n"
      "  Cumulative Hits : %lu full-memory exceptions raised.\r\n"
      "  Resolution      : Send 'READ' telecommand to extract oldest unread "
      "data\r\n"
      "                    and release memory address space for rewriting!\r\n"
      "========================================================================"
      "========\r\n"
      "CMD> ",
      (unsigned long)alert->blocked_write_idx,
      (unsigned long)alert->buffer_capacity, (unsigned long)alert->timestamp_ms,
      (unsigned long)alert->total_exceptions);

  if (len > 0) {
    uart_write_bytes(PC_UART_NUM, buf, (size_t)len);
  }
}

static void process_pc_command(const char *cmd) {
  char clean_cmd[CMD_LINE_MAX_LEN];
  size_t i = 0U;
  size_t j = 0U;

  while ((cmd[i] != '\0') && isspace((unsigned char)cmd[i])) {
    i++;
  }
  while ((cmd[i] != '\0') && (j < (CMD_LINE_MAX_LEN - 1U))) {
    clean_cmd[j++] = (char)toupper((unsigned char)cmd[i++]);
  }
  while ((j > 0U) && isspace((unsigned char)clean_cmd[j - 1U])) {
    j--;
  }
  clean_cmd[j] = '\0';

  if (strlen(clean_cmd) == 0U) {
    const char *prompt = "CMD> ";
    uart_write_bytes(PC_UART_NUM, prompt, strlen(prompt));
    return;
  }

  if ((strcmp(clean_cmd, "READ") == 0) || (strcmp(clean_cmd, "R") == 0)) {
    const char *msg = "Sending READ telecommand to Client...\r\n";
    uart_write_bytes(PC_UART_NUM, msg, strlen(msg));
    server_send_command(CMD_READ_REQ, NULL, 0U);
  } else if ((strcmp(clean_cmd, "PEEK") == 0) ||
             (strcmp(clean_cmd, "P") == 0)) {
    const char *msg = "Sending PEEK telecommand to Client...\r\n";
    uart_write_bytes(PC_UART_NUM, msg, strlen(msg));
    server_send_command(CMD_PEEK_REQ, NULL, 0U);
  } else if ((strcmp(clean_cmd, "STATUS") == 0) ||
             (strcmp(clean_cmd, "S") == 0)) {
    const char *msg = "Querying Client Buffer Diagnostics...\r\n";
    uart_write_bytes(PC_UART_NUM, msg, strlen(msg));
    server_send_command(CMD_STATUS_REQ, NULL, 0U);
  } else if (strcmp(clean_cmd, "PAUSE_WR") == 0) {
    const char *msg = "Commanding Client to PAUSE writing...\r\n";
    uart_write_bytes(PC_UART_NUM, msg, strlen(msg));
    server_send_command(CMD_PAUSE_WR_REQ, NULL, 0U);
  } else if (strcmp(clean_cmd, "RESUME_WR") == 0) {
    const char *msg = "Commanding Client to RESUME writing...\r\n";
    uart_write_bytes(PC_UART_NUM, msg, strlen(msg));
    server_send_command(CMD_RESUME_WR_REQ, NULL, 0U);
  } else if (strcmp(clean_cmd, "RESTART_RD") == 0) {
    const char *msg = "Commanding Client to RESTART reading session...\r\n";
    uart_write_bytes(PC_UART_NUM, msg, strlen(msg));
    server_send_command(CMD_RESTART_RD_REQ, NULL, 0U);
  } else if (strcmp(clean_cmd, "RESET_BUF") == 0) {
    const char *msg = "Commanding Client to RESET buffer memory...\r\n";
    uart_write_bytes(PC_UART_NUM, msg, strlen(msg));
    server_send_command(CMD_RESET_BUF_REQ, NULL, 0U);
  } else if ((strcmp(clean_cmd, "HELP") == 0) ||
             (strcmp(clean_cmd, "?") == 0)) {
    print_help_menu();
  } else {
    char err[96];
    snprintf(err, sizeof(err),
             "Unknown command: '%s'. Type 'HELP' for options.\r\nCMD> ",
             clean_cmd);
    uart_write_bytes(PC_UART_NUM, err, strlen(err));
  }
}

static void uart_rx_task(void *pvParameters) {
  uart_event_t event;
  uint8_t dtmp[128];
  char line_buffer[CMD_LINE_MAX_LEN];
  size_t line_pos = 0U;

  (void)pvParameters;

  while (1) {
    
    if (xQueueReceive(s_uart_event_queue, &event, portMAX_DELAY) == pdTRUE) {
      switch (event.type) {

      case UART_DATA: {
        int read_bytes = uart_read_bytes(PC_UART_NUM, dtmp,
                                         (uint32_t)event.size, portMAX_DELAY);
        for (int i = 0; i < read_bytes; ++i) {
          char c = (char)dtmp[i];

          uart_write_bytes(PC_UART_NUM, &c, 1U);

          if ((c == '\r') || (c == '\n')) {
            
            line_buffer[line_pos] = '\0';
            uart_write_bytes(PC_UART_NUM, "\r\n", 2U);
            process_pc_command(line_buffer);
            line_pos = 0U;
          } else if ((c == '\b') || (c == 0x7FU)) {
            
            if (line_pos > 0U) {
              line_pos--;
              uart_write_bytes(PC_UART_NUM, " \b", 2U);
            }
          } else {
            if (line_pos < (CMD_LINE_MAX_LEN - 1U)) {
              line_buffer[line_pos++] = c;
            }
          }
        }
        break;
      }

      case UART_FIFO_OVF:
      case UART_BUFFER_FULL:
        uart_flush_input(PC_UART_NUM);
        xQueueReset(s_uart_event_queue);
        break;

      default:
        break;
      }
    }
  }
}

static void wireless_rx_task(void *pvParameters) {
  wireless_packet_t pkt;

  (void)pvParameters;

  while (1) {
    if (xQueueReceive(s_wireless_rx_queue, &pkt, portMAX_DELAY) == pdTRUE) {

      switch (pkt.opcode) {

      case RESP_READ_DATA: {
        if (pkt.payload_len == sizeof(telemetry_record_t)) {
          telemetry_record_t rec;
          memcpy(&rec, pkt.payload, sizeof(rec));
          display_telemetry_record("OLDEST READ", &rec);
        }
        break;
      }

      case RESP_PEEK_DATA: {
        if (pkt.payload_len == sizeof(telemetry_record_t)) {
          telemetry_record_t rec;
          memcpy(&rec, pkt.payload, sizeof(rec));
          display_telemetry_record("PEEK (UNFREED)", &rec);
        }
        break;
      }

      case RESP_BUFFER_EMPTY: {
        const char *empty_msg = "\r\n[NOTICE] Circular buffer is EMPTY. No "
                                "unread telemetry available.\r\nCMD> ";
        uart_write_bytes(PC_UART_NUM, empty_msg, strlen(empty_msg));
        break;
      }

      case RESP_STATUS_DATA: {
        if (pkt.payload_len == sizeof(proto_diagnostics_payload_t)) {
          proto_diagnostics_payload_t diag;
          memcpy(&diag, pkt.payload, sizeof(diag));
          display_diagnostics(&diag);
        }
        break;
      }

      case MSG_ACK: {
        const char *ack_msg =
            "\r\n[SUCCESS] Client acknowledged command execution.\r\nCMD> ";
        uart_write_bytes(PC_UART_NUM, ack_msg, strlen(ack_msg));
        break;
      }

      case MSG_NACK: {
        const char *nack_msg =
            "\r\n[REJECTED] Client reported invalid command opcode.\r\nCMD> ";
        uart_write_bytes(PC_UART_NUM, nack_msg, strlen(nack_msg));
        break;
      }

      case ALERT_BUFFER_FULL: {
        
        if (pkt.payload_len == sizeof(proto_alert_full_payload_t)) {
          proto_alert_full_payload_t alert;
          memcpy(&alert, pkt.payload, sizeof(alert));
          display_full_memory_alarm(&alert);
        }
        break;
      }

      default:
        break;
      }
    }
  }
}

static void init_pc_uart_driver(void) {
  const uart_config_t uart_config = {
      .baud_rate = PC_UART_BAUD_RATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_ERROR_CHECK(uart_param_config(PC_UART_NUM, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(PC_UART_NUM, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));

  ESP_ERROR_CHECK(uart_driver_install(PC_UART_NUM, UART_RX_BUF_SIZE * 2,
                                      UART_TX_BUF_SIZE * 2, UART_QUEUE_DEPTH,
                                      &s_uart_event_queue, 0));
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
  ESP_LOGI(TAG, "=== ESP32 Telecommand Server Initializing ===");

  esp_err_t ret = nvs_flash_init();
  if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) ||
      (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  s_wireless_rx_queue = xQueueCreate(16, sizeof(wireless_packet_t));

  init_pc_uart_driver();

  init_wifi_espnow();

  xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 10, NULL);
  xTaskCreate(wireless_rx_task, "wireless_rx_task", 4096, NULL, 10, NULL);

  print_help_menu();
}
