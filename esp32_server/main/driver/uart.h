#ifndef DRIVER_UART_H
#define DRIVER_UART_H

#include <stdint.h>
#include <stddef.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    UART_NUM_0 = 0,
    UART_NUM_1,
    UART_NUM_2,
    UART_NUM_MAX
} uart_port_t;

typedef enum {
    UART_DATA_8_BITS = 0,
    UART_DATA_MAX
} uart_word_length_t;

typedef enum {
    UART_PARITY_DISABLE = 0,
    UART_PARITY_MAX
} uart_parity_t;

typedef enum {
    UART_STOP_BITS_1 = 0,
    UART_STOP_BITS_MAX
} uart_stop_bits_t;

typedef enum {
    UART_HW_FLOWCTRL_DISABLE = 0,
    UART_HW_FLOWCTRL_MAX
} uart_hw_flowcontrol_t;

typedef enum {
    UART_SCLK_DEFAULT = 0,
    UART_SCLK_APB,
    UART_SCLK_RTC
} uart_sclk_t;

typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    uart_sclk_t source_clk;
} uart_config_t;

typedef enum {
    UART_DATA,
    UART_BUFFER_FULL,
    UART_FIFO_OVF,
    UART_FRAME_ERR,
    UART_PARITY_ERR,
    UART_DATA_BREAK,
    UART_EVENT_MAX
} uart_event_type_t;

typedef struct {
    uart_event_type_t type;
    size_t size;
    bool timeout_flag;
} uart_event_t;

#define UART_PIN_NO_CHANGE (-1)

esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config);
esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num);
esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size, int queue_size, QueueHandle_t *uart_queue, int intr_alloc_flags);
int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, TickType_t ticks_to_wait);
int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);
esp_err_t uart_flush_input(uart_port_t uart_num);

#endif /* DRIVER_UART_H */

