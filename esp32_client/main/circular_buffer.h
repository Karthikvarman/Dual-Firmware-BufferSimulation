#ifndef CIRCULAR_BUFFER_H
#define CIRCULAR_BUFFER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    CB_OK                      = 0x00, 
    CB_ERR_NULL_PTR            = 0x01, 
    CB_ERR_INVALID_PARAM       = 0x02, 
    CB_ERR_EMPTY               = 0x03, 
    CB_ERR_FULL                = 0xEF, 
    CB_ERR_WRITE_DISABLED      = 0x05, 
    CB_ERR_UNINITIALIZED       = 0x06  
} cb_status_t;

typedef struct {
    size_t   capacity;         
    size_t   count;            
    size_t   write_index;      
    size_t   read_index;       
    uint32_t total_writes;     
    uint32_t total_reads;      
    uint32_t full_exceptions;  
    bool     is_full;          
    bool     is_empty;         
    bool     write_enabled;    
} cb_diagnostics_t;

typedef struct {
    uint8_t *storage;          
    size_t   item_size;        
    size_t   capacity;         
    size_t   head;             
    size_t   tail;             
    size_t   count;            
    uint32_t total_writes;     
    uint32_t total_reads;      
    uint32_t full_exceptions;  
    bool     write_enabled;    
    bool     is_initialized;   
} circular_buffer_t;

cb_status_t cb_init(circular_buffer_t *cb,
                    uint8_t *storage,
                    size_t capacity,
                    size_t item_size);

cb_status_t cb_write(circular_buffer_t *cb,
                     const void *item_in,
                     size_t *written_idx);

cb_status_t cb_read(circular_buffer_t *cb,
                    void *item_out,
                    size_t *read_idx);

cb_status_t cb_peek(const circular_buffer_t *cb,
                    void *item_out,
                    size_t *peek_idx);

cb_status_t cb_restart_read(circular_buffer_t *cb);

cb_status_t cb_resume_read(circular_buffer_t *cb);

cb_status_t cb_pause_write(circular_buffer_t *cb);

cb_status_t cb_resume_write(circular_buffer_t *cb);

cb_status_t cb_reset(circular_buffer_t *cb);

cb_status_t cb_get_diagnostics(const circular_buffer_t *cb,
                               cb_diagnostics_t *diag);

size_t cb_format_full_exception(const circular_buffer_t *cb,
                                char *buf,
                                size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif
