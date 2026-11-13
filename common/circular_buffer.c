#include "circular_buffer.h"
#include <string.h>
#include <stdio.h>

static void cb_memcpy_bounded(uint8_t *dest, const uint8_t *src, size_t n)
{
    size_t i;
    for (i = 0U; i < n; ++i) {
        dest[i] = src[i];
    }
}

cb_status_t cb_init(circular_buffer_t *cb,
                    uint8_t *storage,
                    size_t capacity,
                    size_t item_size)
{
    if ((cb == NULL) || (storage == NULL)) {
        return CB_ERR_NULL_PTR;
    }

    if ((capacity == 0U) || (item_size == 0U)) {
        return CB_ERR_INVALID_PARAM;
    }

    cb->storage         = storage;
    cb->item_size       = item_size;
    cb->capacity        = capacity;
    cb->head            = 0U;
    cb->tail            = 0U;
    cb->count           = 0U;
    cb->total_writes    = 0U;
    cb->total_reads     = 0U;
    cb->full_exceptions = 0U;
    cb->write_enabled   = true;
    cb->is_initialized  = true;

    return CB_OK;
}

cb_status_t cb_write(circular_buffer_t *cb,
                     const void *item_in,
                     size_t *written_idx)
{
    uint8_t *slot_ptr;

    if ((cb == NULL) || (item_in == NULL)) {
        return CB_ERR_NULL_PTR;
    }

    if (!cb->is_initialized) {
        return CB_ERR_UNINITIALIZED;
    }

    if (!cb->write_enabled) {
        return CB_ERR_WRITE_DISABLED;
    }

    if (cb->count >= cb->capacity) {
        cb->full_exceptions++;
        return CB_ERR_FULL;
    }

    slot_ptr = &cb->storage[cb->head * cb->item_size];

    cb_memcpy_bounded(slot_ptr, (const uint8_t *)item_in, cb->item_size);

    if (written_idx != NULL) {
        *written_idx = cb->head;
    }

    cb->head = (cb->head + 1U) % cb->capacity;
    cb->count++;
    cb->total_writes++;

    return CB_OK;
}

cb_status_t cb_read(circular_buffer_t *cb,
                    void *item_out,
                    size_t *read_idx)
{
    const uint8_t *slot_ptr;

    if ((cb == NULL) || (item_out == NULL)) {
        return CB_ERR_NULL_PTR;
    }

    if (!cb->is_initialized) {
        return CB_ERR_UNINITIALIZED;
    }

    if (cb->count == 0U) {
        return CB_ERR_EMPTY;
    }

    slot_ptr = &cb->storage[cb->tail * cb->item_size];

    cb_memcpy_bounded((uint8_t *)item_out, slot_ptr, cb->item_size);

    if (read_idx != NULL) {
        *read_idx = cb->tail;
    }

    cb->tail = (cb->tail + 1U) % cb->capacity;
    cb->count--;
    cb->total_reads++;

    return CB_OK;
}

cb_status_t cb_peek(const circular_buffer_t *cb,
                    void *item_out,
                    size_t *peek_idx)
{
    const uint8_t *slot_ptr;

    if ((cb == NULL) || (item_out == NULL)) {
        return CB_ERR_NULL_PTR;
    }

    if (!cb->is_initialized) {
        return CB_ERR_UNINITIALIZED;
    }

    if (cb->count == 0U) {
        return CB_ERR_EMPTY;
    }

    slot_ptr = &cb->storage[cb->tail * cb->item_size];
    cb_memcpy_bounded((uint8_t *)item_out, slot_ptr, cb->item_size);

    if (peek_idx != NULL) {
        *peek_idx = cb->tail;
    }

    return CB_OK;
}

cb_status_t cb_restart_read(circular_buffer_t *cb)
{
    if (cb == NULL) {
        return CB_ERR_NULL_PTR;
    }

    if (!cb->is_initialized) {
        return CB_ERR_UNINITIALIZED;
    }

    return CB_OK;
}

cb_status_t cb_resume_read(circular_buffer_t *cb)
{
    if (cb == NULL) {
        return CB_ERR_NULL_PTR;
    }

    if (!cb->is_initialized) {
        return CB_ERR_UNINITIALIZED;
    }

    return CB_OK;
}

cb_status_t cb_pause_write(circular_buffer_t *cb)
{
    if (cb == NULL) {
        return CB_ERR_NULL_PTR;
    }

    if (!cb->is_initialized) {
        return CB_ERR_UNINITIALIZED;
    }

    cb->write_enabled = false;
    return CB_OK;
}

cb_status_t cb_resume_write(circular_buffer_t *cb)
{
    if (cb == NULL) {
        return CB_ERR_NULL_PTR;
    }

    if (!cb->is_initialized) {
        return CB_ERR_UNINITIALIZED;
    }

    cb->write_enabled = true;
    return CB_OK;
}

cb_status_t cb_reset(circular_buffer_t *cb)
{
    if (cb == NULL) {
        return CB_ERR_NULL_PTR;
    }

    if (!cb->is_initialized) {
        return CB_ERR_UNINITIALIZED;
    }

    cb->head            = 0U;
    cb->tail            = 0U;
    cb->count           = 0U;
    cb->total_writes    = 0U;
    cb->total_reads     = 0U;
    cb->full_exceptions = 0U;
    cb->write_enabled   = true;

    return CB_OK;
}

cb_status_t cb_get_diagnostics(const circular_buffer_t *cb,
                               cb_diagnostics_t *diag)
{
    if ((cb == NULL) || (diag == NULL)) {
        return CB_ERR_NULL_PTR;
    }

    if (!cb->is_initialized) {
        return CB_ERR_UNINITIALIZED;
    }

    diag->capacity        = cb->capacity;
    diag->count           = cb->count;
    diag->write_index     = cb->head;
    diag->read_index      = cb->tail;
    diag->total_writes    = cb->total_writes;
    diag->total_reads     = cb->total_reads;
    diag->full_exceptions = cb->full_exceptions;
    diag->is_full         = (cb->count >= cb->capacity);
    diag->is_empty        = (cb->count == 0U);
    diag->write_enabled   = cb->write_enabled;

    return CB_OK;
}

size_t cb_format_full_exception(const circular_buffer_t *cb,
                                char *buf,
                                size_t buf_len)
{
    int written;

    if ((cb == NULL) || (buf == NULL) || (buf_len == 0U)) {
        return 0U;
    }

    written = snprintf(buf, buf_len,
        "*** [EXCEPTION: 0xEFULL_MEM] CIRCULAR BUFFER FULL - Write blocked at slot %u/%u! Read required to free memory slot. Total exceptions: %u ***",
        (unsigned int)cb->head,
        (unsigned int)cb->capacity,
        (unsigned int)cb->full_exceptions);

    if (written < 0) {
        return 0U;
    }

    if ((size_t)written >= buf_len) {
        return buf_len - 1U;
    }

    return (size_t)written;
}
