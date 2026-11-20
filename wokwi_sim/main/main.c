#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "circular_buffer.h"
#include "protocol.h"

#define BUFFER_CAPACITY 16U
#define TIMER_PERIOD_US 10000000ULL
#define COMMAND_BUFFER_SIZE 64

/* LED indicator GPIOs */
#define LED_HEAD_GPIO GPIO_NUM_25
#define LED_TAIL_GPIO GPIO_NUM_26
#define LED_FULL_GPIO GPIO_NUM_27

/* SSD1306 I2C Configuration */
#define I2C_SDA_GPIO GPIO_NUM_21
#define I2C_SCL_GPIO GPIO_NUM_22
#define SSD1306_I2C_ADDR 0x3C
#define OLED_WIDTH 128
#define OLED_HEIGHT 64

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "ESP32_SIM";

static telemetry_record_t s_storage[BUFFER_CAPACITY];
static circular_buffer_t s_buffer;
static SemaphoreHandle_t s_buffer_mutex;
static esp_timer_handle_t s_periodic_timer;
static uint32_t s_record_sequence;
static uint16_t s_tx_sequence;

/* Inter-task communication queues */
static QueueHandle_t s_server_to_client_q;
static QueueHandle_t s_client_to_server_q;

/* SSD1306 Device Handle & Framebuffer (128x64 / 8 = 1024 bytes) */
static i2c_master_dev_handle_t s_oled_handle = NULL;
static uint8_t s_oled_gram[1024];

/* Compact 5x8 ASCII Font for characters 32 to 90 */
static const uint8_t s_font5x8[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* 32: Space */
    {0x00, 0x00, 0x5f, 0x00, 0x00}, /* 33: ! */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* 34: " */
    {0x14, 0x7f, 0x14, 0x7f, 0x14}, /* 35: # */
    {0x24, 0x2a, 0x7f, 0x2a, 0x12}, /* 36: $ */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* 37: % */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* 38: & */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* 39: ' */
    {0x00, 0x1c, 0x22, 0x41, 0x00}, /* 40: ( */
    {0x00, 0x41, 0x22, 0x1c, 0x00}, /* 41: ) */
    {0x14, 0x08, 0x3e, 0x08, 0x14}, /* 42: * */
    {0x08, 0x08, 0x3e, 0x08, 0x08}, /* 43: + */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* 44: , */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* 45: - */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* 46: . */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* 47: / */
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, /* 48: 0 */
    {0x00, 0x42, 0x7f, 0x40, 0x00}, /* 49: 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 50: 2 */
    {0x21, 0x41, 0x45, 0x4b, 0x31}, /* 51: 3 */
    {0x18, 0x14, 0x12, 0x7f, 0x10}, /* 52: 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 53: 5 */
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, /* 54: 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 55: 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 56: 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1e}, /* 57: 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* 58: : */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* 59: ; */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* 60: < */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* 61: = */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* 62: > */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* 63: ? */
    {0x32, 0x49, 0x79, 0x41, 0x3e}, /* 64: @ */
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, /* 65: A */
    {0x7f, 0x49, 0x49, 0x49, 0x36}, /* 66: B */
    {0x3e, 0x41, 0x41, 0x41, 0x22}, /* 67: C */
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, /* 68: D */
    {0x7f, 0x49, 0x49, 0x49, 0x41}, /* 69: E */
    {0x7f, 0x09, 0x09, 0x09, 0x01}, /* 70: F */
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, /* 71: G */
    {0x7f, 0x08, 0x08, 0x08, 0x7f}, /* 72: H */
    {0x00, 0x41, 0x7f, 0x41, 0x00}, /* 73: I */
    {0x20, 0x40, 0x41, 0x3f, 0x01}, /* 74: J */
    {0x7f, 0x08, 0x14, 0x22, 0x41}, /* 75: K */
    {0x7f, 0x40, 0x40, 0x40, 0x40}, /* 76: L */
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, /* 77: M */
    {0x7f, 0x04, 0x08, 0x10, 0x7f}, /* 78: N */
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, /* 79: O */
    {0x7f, 0x09, 0x09, 0x09, 0x06}, /* 80: P */
    {0x3e, 0x41, 0x51, 0x21, 0x5e}, /* 81: Q */
    {0x7f, 0x09, 0x09, 0x19, 0x66}, /* 82: R */
    {0x26, 0x49, 0x49, 0x49, 0x32}, /* 83: S */
    {0x01, 0x01, 0x7f, 0x01, 0x01}, /* 84: T */
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, /* 85: U */
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, /* 86: V */
    {0x3f, 0x40, 0x38, 0x40, 0x3f}, /* 87: W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 88: X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 89: Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}  /* 90: Z */
};

static esp_err_t oled_cmd(uint8_t cmd) {
  if (!s_oled_handle) return ESP_FAIL;
  uint8_t buf[2] = {0x00, cmd};
  return i2c_master_transmit(s_oled_handle, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static void oled_clear(void) {
  memset(s_oled_gram, 0, sizeof(s_oled_gram));
}

static void oled_set_pixel(int x, int y, bool on) {
  if (x >= 0 && x < OLED_WIDTH && y >= 0 && y < OLED_HEIGHT) {
    if (on) {
      s_oled_gram[x + (y / 8) * OLED_WIDTH] |= (1 << (y % 8));
    } else {
      s_oled_gram[x + (y / 8) * OLED_WIDTH] &= ~(1 << (y % 8));
    }
  }
}

static void oled_draw_char(uint8_t x, uint8_t y, char c) {
  if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
  if (c < ' ' || c > 'Z') c = ' ';
  uint8_t idx = (uint8_t)(c - ' ');

  for (uint8_t col = 0; col < 5; col++) {
    uint8_t line = s_font5x8[idx][col];
    for (uint8_t row = 0; row < 8; row++) {
      if (line & (1 << row)) {
        oled_set_pixel(x + col, y + row, true);
      }
    }
  }
}

static void oled_draw_string(uint8_t x, uint8_t y, const char *str) {
  while (*str) {
    oled_draw_char(x, y, *str);
    x += 6;
    if (x + 5 >= OLED_WIDTH) break;
    str++;
  }
}

/* Draw a filled disc/node at (cx, cy) */
static void oled_draw_disc(int cx, int cy, int r, bool filled) {
  for (int dy = -r; dy <= r; dy++) {
    for (int dx = -r; dx <= r; dx++) {
      int dist2 = dx * dx + dy * dy;
      if (dist2 <= r * r) {
        if (filled || dist2 >= (r - 1) * (r - 1)) {
          oled_set_pixel(cx + dx, cy + dy, true);
        }
      }
    }
  }
}

static void oled_refresh(void) {
  if (!s_oled_handle) return;
  oled_cmd(0x21); oled_cmd(0); oled_cmd(OLED_WIDTH - 1);
  oled_cmd(0x22); oled_cmd(0); oled_cmd((OLED_HEIGHT / 8) - 1);

  uint8_t tx_buf[1025];
  tx_buf[0] = 0x40;
  memcpy(&tx_buf[1], s_oled_gram, 1024);
  i2c_master_transmit(s_oled_handle, tx_buf, sizeof(tx_buf), pdMS_TO_TICKS(500));
}

/* Renders a TRUE CIRCULAR RING BUFFER geometry on the 128x64 OLED screen */
static void update_oled_display(void) {
  char text[32];
  cb_diagnostics_t diag;

  if (!s_oled_handle) return;
  if (cb_get_diagnostics(&s_buffer, &diag) != CB_OK) return;

  oled_clear();

  /* Center of circle: (34, 32), Radius = 23 */
  const int cx = 34;
  const int cy = 32;
  const float radius = 23.0f;

  /* Draw 16 circular buffer nodes positioned in a circle */
  for (uint8_t i = 0; i < 16; i++) {
    /* Angle: 0 at top (-PI/2), moving clockwise */
    float angle = ((float)i / 16.0f) * 2.0f * (float)M_PI - ((float)M_PI / 2.0f);
    int node_x = cx + (int)roundf(radius * cosf(angle));
    int node_y = cy + (int)roundf(radius * sinf(angle));

    /* Check if slot is occupied */
    bool is_occupied = false;
    if (diag.count > 0 && !diag.is_empty) {
      if (diag.write_index > diag.read_index) {
        is_occupied = (i >= diag.read_index && i < diag.write_index);
      } else {
        is_occupied = (i >= diag.read_index || i < diag.write_index);
      }
    }

    /* Draw node: filled disc if occupied, hollow ring if free */
    oled_draw_disc(node_x, node_y, 3, is_occupied);
  }

  /* Center display inside the ring: shows Head and Tail pointers */
  char center_buf[8];
  snprintf(center_buf, sizeof(center_buf), "H%u", (unsigned int)diag.write_index);
  oled_draw_string(cx - 8, cy - 8, center_buf);
  snprintf(center_buf, sizeof(center_buf), "T%u", (unsigned int)diag.read_index);
  oled_draw_string(cx - 8, cy + 1, center_buf);

  /* Right column telemetry stats */
  oled_draw_string(68, 2, "RING BUF");
  
  snprintf(text, sizeof(text), "%u/16 SLOTS", (unsigned int)diag.count);
  oled_draw_string(68, 14, text);

  if (diag.is_full) {
    oled_draw_string(68, 26, "* FULL *");
    oled_draw_string(68, 38, "ERR:0xEF");
  } else if (diag.is_empty) {
    oled_draw_string(68, 26, "- EMPTY -");
    oled_draw_string(68, 38, "RD IDLE");
  } else {
    snprintf(text, sizeof(text), "%u%% USED", (unsigned int)((diag.count * 100) / 16));
    oled_draw_string(68, 26, text);
    oled_draw_string(68, 38, "ACTIVE");
  }

  snprintf(text, sizeof(text), "W:%lu", (unsigned long)diag.total_writes);
  oled_draw_string(68, 50, text);
  snprintf(text, sizeof(text), "R:%lu", (unsigned long)diag.total_reads);
  oled_draw_string(98, 50, text);

  oled_refresh();
}

static void init_oled_hardware(void) {
  const i2c_master_bus_config_t bus_cfg = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = I2C_SDA_GPIO,
      .scl_io_num = I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t bus_handle;
  if (i2c_new_master_bus(&bus_cfg, &bus_handle) != ESP_OK) {
    ESP_LOGE(TAG, "I2C bus init failed");
    return;
  }

  const i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = SSD1306_I2C_ADDR,
      .scl_speed_hz = 400000,
  };
  if (i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_oled_handle) != ESP_OK) {
    ESP_LOGE(TAG, "SSD1306 device add failed");
    return;
  }

  /* Send SSD1306 initialization sequence */
  const uint8_t init_cmds[] = {
      0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
      0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
      0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
  };
  for (size_t i = 0; i < sizeof(init_cmds); i++) {
    oled_cmd(init_cmds[i]);
  }

  update_oled_display();
}

static void update_led_indicators(void) {
  cb_diagnostics_t diag;
  if (cb_get_diagnostics(&s_buffer, &diag) == CB_OK) {
    gpio_set_level(LED_FULL_GPIO, diag.is_full ? 1 : 0);
  }
  update_oled_display();
}

static void configure_leds(void) {
  const gpio_config_t led_cfg = {
      .pin_bit_mask = (1ULL << LED_HEAD_GPIO) | (1ULL << LED_TAIL_GPIO) |
                      (1ULL << LED_FULL_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&led_cfg));
  gpio_set_level(LED_HEAD_GPIO, 0);
  gpio_set_level(LED_TAIL_GPIO, 0);
  gpio_set_level(LED_FULL_GPIO, 0);
}

static void send_to_client(uint8_t opcode, const void *payload, uint8_t payload_len) {
  wireless_packet_t packet;
  proto_pack_frame(&packet, ++s_tx_sequence, opcode, payload, payload_len);
  xQueueSend(s_server_to_client_q, &packet, portMAX_DELAY);
}

static void send_to_server(uint8_t opcode, const void *payload, uint8_t payload_len) {
  wireless_packet_t packet;
  proto_pack_frame(&packet, ++s_tx_sequence, opcode, payload, payload_len);
  xQueueSend(s_client_to_server_q, &packet, portMAX_DELAY);
}

static void capture_telemetry(telemetry_record_t *record, uint16_t slot) {
  record->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
  record->sequence_id = ++s_record_sequence;
  record->free_heap_bytes = (uint32_t)esp_get_free_heap_size();
  record->min_free_heap_bytes = (uint32_t)esp_get_minimum_free_heap_size();
  record->chip_temp_c = 38;
  record->wifi_rssi_dbm = -45;
  record->buffer_slot_id = slot;
  record->cpu_freq_mhz = 240U;
}

static void on_periodic_timer(void *arg) {
  telemetry_record_t record;
  size_t written_slot = 0U;
  cb_status_t status;
  char exception_text[160];

  (void)arg;

  if (xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  capture_telemetry(&record, (uint16_t)s_buffer.head);
  status = cb_write(&s_buffer, &record, &written_slot);

  if (status == CB_OK) {
    record.buffer_slot_id = (uint16_t)written_slot;
    update_led_indicators();
    gpio_set_level(LED_HEAD_GPIO, 1);
    printf("\r\n[0.1 Hz CLIENT WRITE] Stored at Slot #%u: Seq=%lu, FreeHeap=%lu bytes\r\nCMD> ",
           (unsigned int)written_slot, (unsigned long)record.sequence_id,
           (unsigned long)record.free_heap_bytes);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEAD_GPIO, 0);
  } else if (status == CB_ERR_FULL) {
    proto_alert_full_payload_t alert = {
        .blocked_write_idx = (uint32_t)s_buffer.head,
        .buffer_capacity = (uint32_t)s_buffer.capacity,
        .total_exceptions = s_buffer.full_exceptions,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };

    (void)cb_format_full_exception(&s_buffer, exception_text, sizeof(exception_text));
    update_led_indicators();
    send_to_server(ALERT_BUFFER_FULL, &alert, sizeof(alert));
  } else if (status == CB_ERR_WRITE_DISABLED) {
    ESP_LOGW(TAG, "Periodic telemetry writing is paused");
  }

  xSemaphoreGive(s_buffer_mutex);
}

static void print_help(void) {
  printf("\r\n=======================================================\r\n"
         "  DUAL ESP32 FIRMWARE REALTIME SIMULATOR (LIVE WOKWI)\r\n"
         "=======================================================\r\n"
         " Commands:\r\n"
         "   READ       (R) : Read & pop oldest telemetry record\r\n"
         "   PEEK       (P) : Inspect oldest telemetry record (unfreed)\r\n"
         "   STATUS     (S) : Display buffer capacity & utilization\r\n"
         "   PAUSE_WR       : Pause 0.1 Hz telemetry writes\r\n"
         "   RESUME_WR      : Resume 0.1 Hz telemetry writes\r\n"
         "   RESTART_RD     : Reset read pointer back to oldest record\r\n"
         "   RESET_BUF      : Flush & reinitialize circular buffer\r\n"
         "   HELP       (?) : Show this help message\r\n"
         "=======================================================\r\n\r\n");
}

static void display_record(const char *label, const telemetry_record_t *record) {
  printf("\r\n--- [%s TELEMETRY RECORD] ---\r\n"
         "  Buffer Slot ID   : #%u\r\n"
         "  Sequence Number  : %lu\r\n"
         "  Client Uptime    : %lu ms\r\n"
         "  Free Heap Memory : %lu bytes\r\n"
         "  Min Free Heap    : %lu bytes\r\n"
         "  Chip Temperature : %d C\r\n"
         "  Wi-Fi RSSI       : %d dBm\r\n"
         "  CPU Frequency    : %lu MHz\r\n",
         label, (unsigned int)record->buffer_slot_id,
         (unsigned long)record->sequence_id,
         (unsigned long)record->timestamp_ms,
         (unsigned long)record->free_heap_bytes,
         (unsigned long)record->min_free_heap_bytes,
         (int)record->chip_temp_c, (int)record->wifi_rssi_dbm,
         (unsigned long)record->cpu_freq_mhz);
}

static void display_diagnostics(const proto_diagnostics_payload_t *diag) {
  const uint32_t utilization =
      diag->capacity > 0U ? diag->count * 100U / diag->capacity : 0U;

  printf("\r\n--- [CIRCULAR BUFFER DIAGNOSTICS] ---\r\n"
         "  Total Capacity     : %lu slots\r\n"
         "  Active Stored Data : %lu slots (%lu%% utilized)\r\n"
         "  Free Slots Avail   : %lu slots\r\n"
         "  Next Write Address : Slot #%lu (Head)\r\n"
         "  Next Read Address  : Slot #%lu (Tail)\r\n"
         "  Total Writes Done  : %lu\r\n"
         "  Total Reads Done   : %lu\r\n"
         "  Memory Full Alarms : %lu\r\n"
         "  Buffer State       : %s\r\n"
         "  Write Acquisition  : %s\r\n",
         (unsigned long)diag->capacity, (unsigned long)diag->count,
         (unsigned long)utilization,
         (unsigned long)(diag->capacity - diag->count),
         (unsigned long)diag->write_index, (unsigned long)diag->read_index,
         (unsigned long)diag->total_writes, (unsigned long)diag->total_reads,
         (unsigned long)diag->full_exceptions,
         diag->is_full ? "FULL" : (diag->is_empty ? "EMPTY" : "ACTIVE DATA"),
         diag->write_enabled ? "ENABLED" : "PAUSED");
}

static void handle_server_packet(const wireless_packet_t *packet) {
  if (packet->opcode == RESP_READ_DATA &&
      packet->payload_len == sizeof(telemetry_record_t)) {
    telemetry_record_t record;
    memcpy(&record, packet->payload, sizeof(record));
    display_record("OLDEST READ", &record);
    gpio_set_level(LED_TAIL_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_set_level(LED_TAIL_GPIO, 0);
  } else if (packet->opcode == RESP_PEEK_DATA &&
             packet->payload_len == sizeof(telemetry_record_t)) {
    telemetry_record_t record;
    memcpy(&record, packet->payload, sizeof(record));
    display_record("PEEK (UNFREED)", &record);
  } else if (packet->opcode == RESP_BUFFER_EMPTY) {
    printf("\r\n[NOTICE] Circular buffer is EMPTY. No records to read.\r\n");
  } else if (packet->opcode == RESP_STATUS_DATA &&
             packet->payload_len == sizeof(proto_diagnostics_payload_t)) {
    proto_diagnostics_payload_t diagnostics;
    memcpy(&diagnostics, packet->payload, sizeof(diagnostics));
    display_diagnostics(&diagnostics);
  } else if (packet->opcode == MSG_ACK) {
    printf("\r\n[SUCCESS] Client acknowledged command execution.\r\n");
  } else if (packet->opcode == MSG_NACK) {
    printf("\r\n[REJECTED] Client rejected the command.\r\n");
  } else if (packet->opcode == ALERT_BUFFER_FULL &&
             packet->payload_len == sizeof(proto_alert_full_payload_t)) {
    proto_alert_full_payload_t alert;
    memcpy(&alert, packet->payload, sizeof(alert));
    printf("\r\n*******************************************************\r\n"
           "*** [CRITICAL EXCEPTION: 0xEFULL_MEM] BUFFER FULL   ***\r\n"
           "  Blocked write slot : #%lu\r\n"
           "  Buffer capacity    : %lu\r\n"
           "  Total exceptions   : %lu\r\n"
           "  Client uptime      : %lu ms\r\n"
           "  ACTION: Type 'READ' or 'R' to pop oldest record.\r\n"
           "*******************************************************\r\n",
           (unsigned long)alert.blocked_write_idx,
           (unsigned long)alert.buffer_capacity,
           (unsigned long)alert.total_exceptions,
           (unsigned long)alert.timestamp_ms);
  }
}

static void process_console_command(char *line) {
  size_t start = 0U;
  size_t end = strlen(line);

  while (start < end && isspace((unsigned char)line[start])) {
    start++;
  }
  while (end > start && isspace((unsigned char)line[end - 1U])) {
    end--;
  }
  line[end] = '\0';

  for (size_t i = start; line[i] != '\0'; i++) {
    line[i] = (char)toupper((unsigned char)line[i]);
  }

  const char *command = line + start;
  if (strlen(command) == 0) {
    return;
  }

  if (strcmp(command, "READ") == 0 || strcmp(command, "R") == 0) {
    send_to_client(CMD_READ_REQ, NULL, 0U);
  } else if (strcmp(command, "PEEK") == 0 || strcmp(command, "P") == 0) {
    send_to_client(CMD_PEEK_REQ, NULL, 0U);
  } else if (strcmp(command, "STATUS") == 0 || strcmp(command, "S") == 0) {
    send_to_client(CMD_STATUS_REQ, NULL, 0U);
  } else if (strcmp(command, "PAUSE_WR") == 0) {
    send_to_client(CMD_PAUSE_WR_REQ, NULL, 0U);
  } else if (strcmp(command, "RESUME_WR") == 0) {
    send_to_client(CMD_RESUME_WR_REQ, NULL, 0U);
  } else if (strcmp(command, "RESTART_RD") == 0) {
    send_to_client(CMD_RESTART_RD_REQ, NULL, 0U);
  } else if (strcmp(command, "RESET_BUF") == 0) {
    send_to_client(CMD_RESET_BUF_REQ, NULL, 0U);
  } else if (strcmp(command, "HELP") == 0 || strcmp(command, "?") == 0) {
    print_help();
  } else {
    printf("\r\n[ERROR] Unknown command: '%s'. Type 'HELP' or '?' for list.\r\n", command);
  }
}

static void server_task(void *arg) {
  char line[COMMAND_BUFFER_SIZE];
  size_t idx = 0;
  (void)arg;

  vTaskDelay(pdMS_TO_TICKS(500));
  print_help();
  printf("Server console ready. Type a command (e.g. STATUS, READ):\r\nCMD> ");
  fflush(stdout);

  while (1) {
    int c = getchar();
    if (c != EOF && c != 0xFF) {
      if (c == '\r' || c == '\n') {
        printf("\r\n");
        fflush(stdout);
        line[idx] = '\0';
        if (idx > 0) {
          process_console_command(line);
          idx = 0;
        }
        /* Drain any responses returned by Client */
        wireless_packet_t resp;
        while (xQueueReceive(s_client_to_server_q, &resp, pdMS_TO_TICKS(50)) == pdTRUE) {
          handle_server_packet(&resp);
        }
        printf("CMD> ");
        fflush(stdout);
      } else if (c == '\b' || c == 0x7F) {
        if (idx > 0) {
          idx--;
          printf("\b \b");
          fflush(stdout);
        }
      } else if (idx < sizeof(line) - 1) {
        line[idx++] = (char)c;
        putchar(c);  /* Echo typed character back so user sees what they type */
        fflush(stdout);
      }
    } else {
      /* Drain background client packets like full buffer alert */
      wireless_packet_t resp;
      if (xQueueReceive(s_client_to_server_q, &resp, pdMS_TO_TICKS(20)) == pdTRUE) {
        handle_server_packet(&resp);
        printf("CMD> %.*s", (int)idx, line);
        fflush(stdout);
      }
    }
  }

  vTaskDelete(NULL);
}

static void client_task(void *arg) {
  wireless_packet_t packet;
  (void)arg;

  while (xQueueReceive(s_server_to_client_q, &packet, portMAX_DELAY) == pdTRUE) {
    telemetry_record_t record;
    size_t slot = 0U;
    cb_status_t status;

    switch (packet.opcode) {
    case CMD_READ_REQ:
      xSemaphoreTake(s_buffer_mutex, portMAX_DELAY);
      status = cb_read(&s_buffer, &record, &slot);
      update_led_indicators();
      xSemaphoreGive(s_buffer_mutex);
      if (status == CB_OK) {
        send_to_server(RESP_READ_DATA, &record, sizeof(record));
      } else {
        send_to_server(RESP_BUFFER_EMPTY, NULL, 0U);
      }
      break;

    case CMD_PEEK_REQ:
      xSemaphoreTake(s_buffer_mutex, portMAX_DELAY);
      status = cb_peek(&s_buffer, &record, &slot);
      xSemaphoreGive(s_buffer_mutex);
      if (status == CB_OK) {
        send_to_server(RESP_PEEK_DATA, &record, sizeof(record));
      } else {
        send_to_server(RESP_BUFFER_EMPTY, NULL, 0U);
      }
      break;

    case CMD_STATUS_REQ: {
      proto_diagnostics_payload_t payload = {0};
      cb_diagnostics_t diag;
      xSemaphoreTake(s_buffer_mutex, portMAX_DELAY);
      if (cb_get_diagnostics(&s_buffer, &diag) == CB_OK) {
        payload.capacity = (uint32_t)diag.capacity;
        payload.count = (uint32_t)diag.count;
        payload.write_index = (uint32_t)diag.write_index;
        payload.read_index = (uint32_t)diag.read_index;
        payload.total_writes = diag.total_writes;
        payload.total_reads = diag.total_reads;
        payload.full_exceptions = diag.full_exceptions;
        payload.is_full = diag.is_full ? 1 : 0;
        payload.is_empty = diag.is_empty ? 1 : 0;
        payload.write_enabled = diag.write_enabled ? 1 : 0;
      }
      xSemaphoreGive(s_buffer_mutex);
      send_to_server(RESP_STATUS_DATA, &payload, sizeof(payload));
      break;
    }

    case CMD_PAUSE_WR_REQ:
      xSemaphoreTake(s_buffer_mutex, portMAX_DELAY);
      status = cb_pause_write(&s_buffer);
      xSemaphoreGive(s_buffer_mutex);
      send_to_server(status == CB_OK ? MSG_ACK : MSG_NACK, NULL, 0U);
      break;

    case CMD_RESUME_WR_REQ:
      xSemaphoreTake(s_buffer_mutex, portMAX_DELAY);
      status = cb_resume_write(&s_buffer);
      xSemaphoreGive(s_buffer_mutex);
      send_to_server(status == CB_OK ? MSG_ACK : MSG_NACK, NULL, 0U);
      break;

    case CMD_RESTART_RD_REQ:
      xSemaphoreTake(s_buffer_mutex, portMAX_DELAY);
      status = cb_restart_read(&s_buffer);
      xSemaphoreGive(s_buffer_mutex);
      send_to_server(status == CB_OK ? MSG_ACK : MSG_NACK, NULL, 0U);
      break;

    case CMD_RESET_BUF_REQ:
      xSemaphoreTake(s_buffer_mutex, portMAX_DELAY);
      status = cb_reset(&s_buffer);
      update_led_indicators();
      xSemaphoreGive(s_buffer_mutex);
      send_to_server(status == CB_OK ? MSG_ACK : MSG_NACK, NULL, 0U);
      break;

    default:
      send_to_server(MSG_NACK, NULL, 0U);
      break;
    }
  }
}

void app_main(void) {
  configure_leds();
  init_oled_hardware();

  s_buffer_mutex = xSemaphoreCreateMutex();
  s_server_to_client_q = xQueueCreate(16, sizeof(wireless_packet_t));
  s_client_to_server_q = xQueueCreate(16, sizeof(wireless_packet_t));

  const cb_status_t buffer_status =
      cb_init(&s_buffer, (uint8_t *)s_storage, BUFFER_CAPACITY,
              sizeof(telemetry_record_t));
  if (buffer_status != CB_OK) {
    ESP_LOGE(TAG, "Circular buffer initialization failed: 0x%02X",
             (unsigned int)buffer_status);
    return;
  }

  /* Start 0.1 Hz Telemetry Timer (10s period) */
  ESP_ERROR_CHECK(esp_timer_create(
      &(const esp_timer_create_args_t){
          .callback = on_periodic_timer,
          .arg = NULL,
          .dispatch_method = ESP_TIMER_TASK,
          .name = "cb_0_1hz_timer",
      },
      &s_periodic_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(s_periodic_timer, TIMER_PERIOD_US));

  /* Create Client Worker Task on Core 1 */
  xTaskCreatePinnedToCore(client_task, "client_task", 4096, NULL, 5, NULL, 1);

  /* Create Server Console Task on Core 0 */
  xTaskCreatePinnedToCore(server_task, "server_console", 4096, NULL, 5, NULL, 0);
}
