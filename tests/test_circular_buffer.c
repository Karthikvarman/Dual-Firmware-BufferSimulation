#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../common/circular_buffer.h"
#include "../common/protocol.h"

#define TEST_CAPACITY 8U

static telemetry_record_t s_test_storage[TEST_CAPACITY];
static circular_buffer_t  s_test_cb;

static void populate_dummy_telemetry(telemetry_record_t *rec, uint32_t seq)
{
    rec->timestamp_ms        = seq * 10000U; 
    rec->sequence_id         = seq;
    rec->free_heap_bytes     = 240000U - (seq * 128U);
    rec->min_free_heap_bytes = 210000U;
    rec->chip_temp_c         = (int8_t)(38 + (seq % 5U));
    rec->wifi_rssi_dbm       = (int8_t)(-45 - (seq % 10U));
    rec->buffer_slot_id      = 0U;
    rec->cpu_freq_mhz        = 240U;
}

int main(void)
{
    cb_status_t status;
    char exception_banner[200];
    uint32_t seq;
    size_t slot;
    telemetry_record_t in_rec;
    telemetry_record_t out_rec;
    cb_diagnostics_t diag;

    printf("====================================================================\n");
    printf("   INSPECITY ASSIGNMENT 1b: NATIVE C CIRCULAR BUFFER TESTBENCH     \n");
    printf("====================================================================\n");

    printf("\n[TEST 1] Initializing buffer in pre-allocated static RAM...\n");
    status = cb_init(&s_test_cb, (uint8_t *)s_test_storage, TEST_CAPACITY, sizeof(telemetry_record_t));
    assert(status == CB_OK);
    assert(s_test_cb.capacity == TEST_CAPACITY);
    assert(s_test_cb.count == 0U);
    printf("  PASS: Buffer initialized successfully. Allocated slots: %u (%u bytes total).\n",
           (unsigned int)TEST_CAPACITY, (unsigned int)(TEST_CAPACITY * sizeof(telemetry_record_t)));

    printf("\n[TEST 2] Testing sequential writes & Head pointer progression...\n");
    for (seq = 1U; seq <= TEST_CAPACITY; ++seq) {
        populate_dummy_telemetry(&in_rec, seq);
        status = cb_write(&s_test_cb, &in_rec, &slot);
        assert(status == CB_OK);
        assert(slot == (seq - 1U));
        printf("  Wrote Record #%02u -> Slot #%02u | Head moved to %u | Count: %u\n",
               (unsigned int)seq, (unsigned int)slot, (unsigned int)s_test_cb.head, (unsigned int)s_test_cb.count);
    }
    assert(s_test_cb.count == TEST_CAPACITY);
    assert(s_test_cb.head == 0U); 
    printf("  PASS: Buffer successfully saturated to maximum capacity (%u slots).\n", (unsigned int)TEST_CAPACITY);

    printf("\n[TEST 3] Testing Full-Memory Exception on overflow attempt...\n");
    populate_dummy_telemetry(&in_rec, 999U);
    status = cb_write(&s_test_cb, &in_rec, &slot);
    assert(status == CB_ERR_FULL);
    assert(status == 0xEF); 
    assert(s_test_cb.full_exceptions == 1U);
    cb_format_full_exception(&s_test_cb, exception_banner, sizeof(exception_banner));
    printf("  Distinguishable Exception Emitted:\n    %s\n", exception_banner);
    printf("  PASS: Full-memory exception raised safely without corrupting stored data.\n");

    printf("\n[TEST 4] Testing Peek Operation (Inspect without advancing Tail)...\n");
    status = cb_peek(&s_test_cb, &out_rec, &slot);
    assert(status == CB_OK);
    assert(out_rec.sequence_id == 1U);
    assert(slot == 0U);
    assert(s_test_cb.count == TEST_CAPACITY); 
    printf("  Peeked Slot #%u -> Sequence #%u (Count remains %u).\n",
           (unsigned int)slot, (unsigned int)out_rec.sequence_id, (unsigned int)s_test_cb.count);
    printf("  PASS: Peek verified non-destructive.\n");

    printf("\n[TEST 5] Reading 3 oldest records & verifying slot recycling...\n");
    for (seq = 1U; seq <= 3U; ++seq) {
        status = cb_read(&s_test_cb, &out_rec, &slot);
        assert(status == CB_OK);
        assert(out_rec.sequence_id == seq);
        assert(slot == (seq - 1U));
        printf("  Read Oldest: Seq #%02u from Slot #%02u | Tail moved to %u | Count: %u\n",
               (unsigned int)out_rec.sequence_id, (unsigned int)slot,
               (unsigned int)s_test_cb.tail, (unsigned int)s_test_cb.count);
    }
    assert(s_test_cb.count == (TEST_CAPACITY - 3U));
    printf("  Slots 0, 1, and 2 are now released and available for rewriting.\n");

    for (seq = 101U; seq <= 103U; ++seq) {
        populate_dummy_telemetry(&in_rec, seq);
        status = cb_write(&s_test_cb, &in_rec, &slot);
        assert(status == CB_OK);
        printf("  Rewrote into Reclaimed Slot #%02u -> New Seq #%03u\n",
               (unsigned int)slot, (unsigned int)seq);
    }
    assert(s_test_cb.count == TEST_CAPACITY);
    printf("  PASS: Memory slots successfully recycled and rewritten.\n");

    printf("\n[TEST 6] Resuming reading from last read session...\n");
    uint32_t expected_seqs[] = { 4U, 5U, 6U, 7U, 8U, 101U, 102U, 103U };
    for (int i = 0; i < 8; ++i) {
        status = cb_read(&s_test_cb, &out_rec, &slot);
        assert(status == CB_OK);
        assert(out_rec.sequence_id == expected_seqs[i]);
        printf("  Resumed Read [%d/8]: Extracted Seq #%03u from Slot #%02u (Tail: %u)\n",
               i + 1, (unsigned int)out_rec.sequence_id, (unsigned int)slot, (unsigned int)s_test_cb.tail);
    }
    assert(s_test_cb.count == 0U);
    printf("  PASS: Chronological FIFO ordering preserved across wrap-around sessions.\n");

    printf("\n[TEST 7] Testing empty buffer underflow and diagnostics query...\n");
    status = cb_read(&s_test_cb, &out_rec, &slot);
    assert(status == CB_ERR_EMPTY);

    status = cb_get_diagnostics(&s_test_cb, &diag);
    assert(status == CB_OK);
    assert(diag.is_empty == true);
    assert(diag.is_full == false);
    assert(diag.total_writes == 11U);
    assert(diag.total_reads == 11U);
    assert(diag.full_exceptions == 1U);
    printf("  Diagnostics Snapshot:\n");
    printf("    Capacity: %u | Count: %u | Total Writes: %u | Total Reads: %u | Exceptions: %u\n",
           (unsigned int)diag.capacity, (unsigned int)diag.count,
           (unsigned int)diag.total_writes, (unsigned int)diag.total_reads,
           (unsigned int)diag.full_exceptions);
    printf("  PASS: Underflow prevented, diagnostics 100%% consistent.\n");

    printf("\n[TEST 8] Testing User Control Telecommands (PAUSE_WR, RESUME_WR, RESET)...\n");
    cb_pause_write(&s_test_cb);
    status = cb_write(&s_test_cb, &in_rec, &slot);
    assert(status == CB_ERR_WRITE_DISABLED);
    printf("  PASS: Pause writing successfully blocked writes.\n");

    cb_resume_write(&s_test_cb);
    status = cb_write(&s_test_cb, &in_rec, &slot);
    assert(status == CB_OK);
    printf("  PASS: Resume writing successfully accepted write.\n");

    cb_reset(&s_test_cb);
    assert(s_test_cb.count == 0U && s_test_cb.head == 0U && s_test_cb.tail == 0U);
    printf("  PASS: Reset cleared all pointers and restored initial state.\n");

    printf("\n====================================================================\n");
    printf("  ALL 8 C VERIFICATION TESTS PASSED SUCCESSFULLY! (100%% COMPLIANT)   \n");
    printf("====================================================================\n");

    return 0;
}
