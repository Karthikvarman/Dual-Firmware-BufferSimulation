#ifndef PROTOCOL_H
#define PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if defined(_MSC_VER)
  #define PACKED_STRUCT_START __pragma(pack(push, 1))
  #define PACKED_STRUCT_END   __pragma(pack(pop))
  #define PACKED_ATTR
  #ifndef inline
    #define inline __inline
  #endif
#else
  #define PACKED_STRUCT_START
  #define PACKED_STRUCT_END
  #define PACKED_ATTR __attribute__((packed))
#endif

#define PROTOCOL_SYNC_BYTE_1       0xAAU
#define PROTOCOL_SYNC_BYTE_2       0x55U
#define PROTOCOL_MAX_PAYLOAD_SIZE  128U

PACKED_STRUCT_START
typedef struct PACKED_ATTR {
    uint32_t timestamp_ms;        
    uint32_t sequence_id;         
    uint32_t free_heap_bytes;     
    uint32_t min_free_heap_bytes; 
    int8_t   chip_temp_c;         
    int8_t   wifi_rssi_dbm;       
    uint16_t buffer_slot_id;      
    uint32_t cpu_freq_mhz;        
} telemetry_record_t;
PACKED_STRUCT_END

typedef enum {
    
    MSG_ACK                      = 0x01,
    MSG_NACK                     = 0x02,

    CMD_READ_REQ                 = 0x10,
    CMD_PEEK_REQ                 = 0x12,
    CMD_STATUS_REQ               = 0x20,
    CMD_PAUSE_WR_REQ             = 0x30,
    CMD_RESUME_WR_REQ            = 0x31,
    CMD_RESTART_RD_REQ           = 0x32,
    CMD_RESET_BUF_REQ            = 0x33,

    RESP_READ_DATA               = 0x11,
    RESP_PEEK_DATA               = 0x13,
    RESP_BUFFER_EMPTY            = 0x14,
    RESP_STATUS_DATA             = 0x21,

    ALERT_BUFFER_FULL            = 0xEF  
} msg_opcode_t;

PACKED_STRUCT_START
typedef struct PACKED_ATTR {
    uint32_t capacity;
    uint32_t count;
    uint32_t write_index;
    uint32_t read_index;
    uint32_t total_writes;
    uint32_t total_reads;
    uint32_t full_exceptions;
    uint8_t  is_full;
    uint8_t  is_empty;
    uint8_t  write_enabled;
    uint8_t  reserved;
} proto_diagnostics_payload_t;
PACKED_STRUCT_END

PACKED_STRUCT_START
typedef struct PACKED_ATTR {
    uint32_t blocked_write_idx;
    uint32_t buffer_capacity;
    uint32_t total_exceptions;
    uint32_t timestamp_ms;
} proto_alert_full_payload_t;
PACKED_STRUCT_END

PACKED_STRUCT_START
typedef struct PACKED_ATTR {
    uint8_t  sync1;               
    uint8_t  sync2;               
    uint16_t seq_num;             
    uint8_t  opcode;              
    uint8_t  payload_len;         
    uint8_t  payload[PROTOCOL_MAX_PAYLOAD_SIZE];
    uint16_t crc16;               
} wireless_packet_t;
PACKED_STRUCT_END

static inline uint16_t proto_compute_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    size_t i;
    uint8_t j;

    for (i = 0U; i < length; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8U);
        for (j = 0U; j < 8U; ++j) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1U);
            }
        }
    }
    return crc;
}

static inline size_t proto_pack_frame(wireless_packet_t *pkt,
                                      uint16_t seq,
                                      uint8_t opcode,
                                      const void *payload,
                                      uint8_t payload_len)
{
    size_t header_len;
    size_t crc_span_len;

    pkt->sync1 = PROTOCOL_SYNC_BYTE_1;
    pkt->sync2 = PROTOCOL_SYNC_BYTE_2;
    pkt->seq_num = seq;
    pkt->opcode = opcode;
    pkt->payload_len = payload_len;

    if ((payload != NULL) && (payload_len > 0U)) {
        size_t i;
        const uint8_t *src = (const uint8_t *)payload;
        for (i = 0U; i < (size_t)payload_len; ++i) {
            pkt->payload[i] = src[i];
        }
    }

    header_len = offsetof(wireless_packet_t, payload);
    crc_span_len = header_len + (size_t)payload_len;

    pkt->crc16 = proto_compute_crc16((const uint8_t *)pkt, crc_span_len);

    return crc_span_len + sizeof(uint16_t);
}

static inline bool proto_verify_frame(const wireless_packet_t *pkt, size_t received_len)
{
    size_t header_len;
    size_t expected_len;
    uint16_t calc_crc;

    if (received_len < (offsetof(wireless_packet_t, payload) + sizeof(uint16_t))) {
        return false;
    }

    if ((pkt->sync1 != PROTOCOL_SYNC_BYTE_1) || (pkt->sync2 != PROTOCOL_SYNC_BYTE_2)) {
        return false;
    }

    header_len = offsetof(wireless_packet_t, payload);
    expected_len = header_len + (size_t)pkt->payload_len + sizeof(uint16_t);

    if (received_len < expected_len) {
        return false;
    }

    calc_crc = proto_compute_crc16((const uint8_t *)pkt, header_len + (size_t)pkt->payload_len);
    return (calc_crc == pkt->crc16);
}

#ifdef __cplusplus
}
#endif

#endif
