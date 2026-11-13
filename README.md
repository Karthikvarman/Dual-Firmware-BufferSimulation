# Dual Firmware - Circular Buffer System Architecture

---

## 1. Executive Overview

This document provides a comprehensive, end-to-end explanation of the **Dual ESP32 Firmware and Circular Buffer Architecture**. It details how the system starts, how data is produced and consumed, what the 16 memory slots represent, and how the simulation works in the absence of physical microcontrollers.

```
       +-------------------------------------------------------------------+
       |                            OPERATOR PC                            |
       |                Web Simulator UI / Serial Monitor                  |
       +---------------------------------+---------------------------------+
                                         |
                            UART Serial (115200 Baud)
                                         |
                                         v
       +---------------------------------+---------------------------------+
       |                  NODE 2: ESP32 SERVER (GATEWAY)                   |
       |  Source: esp32_server/main/main_server.c                          |
       |                                                                   |
       |  1. UART interrupt service routine (ISR) captures telecommand     |
       |  2. Encapsulates command into wireless frame with CRC-16 check    |
       |  3. Transmits wireless frame via 2.4 GHz ESP-NOW protocol         |
       +---------------------------------+---------------------------------+
                                         |
                        Wireless Link (ESP-NOW 2.4 GHz)
                                         |
                                         v
       +---------------------------------+---------------------------------+
       |                  NODE 1: ESP32 CLIENT (SATELLITE)                 |
       |  Source: esp32_client/main/main_client.c                          |
       |                                                                   |
       |  [Timer 0.1 Hz] ---> Captures Telemetry (Temp, Heap, RSSI)        |
       |                           |                                       |
       |                           v                                       |
       |                 CIRCULAR BUFFER (16 SLOTS)                        |
       |                 Source: common/circular_buffer.c                  |
       |                           |                                       |
       |  [ESP-NOW RX] <-----------+                                       |
       |  Receives READ command  -> Pops OLDEST record (FIFO via Tail)     |
       |  Recycles memory slot   -> Downlinks record wirelessly to Server  |
       +-------------------------------------------------------------------+
```

---

## 2. System Architecture: How It Starts & Runs

### Step 1: Boot-Up and Initialization
1. **Client Node (`main_client.c`):**
   * Initializes non-volatile storage (NVS) and configures the 2.4 GHz Wi-Fi radio for peer-to-peer **ESP-NOW** communication.
   * Allocates a static, contiguous memory block of 16 slots (`circular_buffer_t`). No dynamic memory allocation (`malloc`/`free`) is used, ensuring zero heap fragmentation and compliance with aerospace **MISRA-C:2012** guidelines.
   * Initializes pointers: `head = 0` (write pointer) and `tail = 0` (read pointer), with `count = 0`.
   * Arms a hardware timer (`esp_timer`) set to trigger every **10,000,000 microseconds (10 seconds = 0.1 Hz)**.

2. **Server Node (`main_server.c`):**
   * Configures UART0 at 115200 baud (8 data bits, no parity, 1 stop bit) with an interrupt-driven FIFO queue.
   * Initializes the ESP-NOW transceiver to listen for telemetry broadcasts and full-memory alerts from the Client.

---

### Step 2: Periodic Data Acquisition (Writing to Buffer)
* Every **10 seconds**, the Client’s hardware timer ISR fires and dispatches a sampling event.
* A snapshot of system health is captured into a 24-byte record:
  * Sequence ID, system uptime, available heap RAM, CPU temperature, Wi-Fi signal strength, CPU frequency, and status flags.
* The Client invokes `circ_buf_push(&buf, &record)`:
  * The record is copied into memory slot `head`.
  * The write index increments circularly: `head = (head + 1) % 16`.
  * Occupancy `count` increments by 1.

---

### Step 3: Buffer Overflow Protection (`0xEFULL_MEM`)
* If all **16 slots** become saturated (`count == 16`) without being read:
  * The firmware **strictly forbids overwriting unread historical data**.
  * `circ_buf_push()` rejects the write and returns the designated error code:
    ```c
    #define CIRC_BUF_ERR_FULL 0xEFU  /* Distinct Full Memory Exception */
    ```
  * The Client immediately dispatches an urgent wireless alert packet (`OPCODE_FULL_ALARM`) to the Server.
  * The Server activates its critical warning indicators on the PC terminal:
    ```
    *** [CRITICAL EXCEPTION: 0xEFULL_MEM] CLIENT BUFFER OVERFLOW ***
    0.1 Hz periodic write blocked! Issue 'READ' command to extract oldest data and free memory.
    ```

---

### Step 4: Telecommand Downlink (Reading & Freeing Memory)
* The operator issues the `READ` telecommand via serial or UI button.
* Execution sequence:
  1. The Server packages `OPCODE_READ` with CRC-16 validation and sends it across ESP-NOW.
  2. The Client's wireless interrupt receives the frame and queues it for processing.
  3. The Client executes `circ_buf_pop(&buf, &extracted_record)`:
     * Extracts the record from index `tail` (the **oldest** unread record, enforcing strict FIFO ordering).
     * The slot at `tail` is marked free and made available for future writes.
     * The read pointer advances: `tail = (tail + 1) % 16`.
     * If the periodic write was blocked by the overflow state, it unblocks immediately.
  4. The record is wirelessly returned to the Server, displayed on the UART console, and appended to the Downlinked Telemetry Log.

---

## 3. The 16 Memory Slots: Mechanism & Contents

### Why 16 Slots?
In spaceborne payloads, storage capacity must be bounded and deterministic. 16 slots at 0.1 Hz provide a rolling buffer capacity of **160 seconds** of telemetry before ground intervention or downlink is required.

```
                  [ Slot 0 ]
             [15]            [ 1 ]
          [14]                  [ 2 ]
        [13]                      [ 3 ]
        [12]      16 SLOTS        [ 4 ]
        [11]    CIRCULAR BUFFER   [ 5 ]
          [10]                  [ 6 ]
             [ 9 ]            [ 7 ]
                  [ Slot 8 ]
```

### Exact Structure of Each Slot
Each slot holds a 24-byte packed binary structure (`telemetry_record_t`) defined in `common/protocol.h`:

```c
typedef struct {
    uint32_t sequence_id;      /* Monotonically increasing record counter (1, 2, 3...) */
    uint32_t timestamp_ms;     /* Client system uptime in milliseconds */
    uint32_t free_heap_bytes;  /* Available internal SRAM heap (memory leak monitoring) */
    uint32_t min_free_heap;    /* Lowest historic free memory watermark */
    int8_t   chip_temp_c;      /* Core processor temperature in degrees Celsius */
    int8_t   wifi_rssi_dbm;    /* ESP-NOW radio receiver power in dBm */
    uint16_t cpu_freq_mhz;     /* Xtensa CPU core clock frequency (240 MHz) */
    uint32_t flags;            /* Status bitflags and subsystem health diagnostics */
} telemetry_record_t;
```

### Detailed Field Descriptions

| Field | Example Value | Description & Aerospace Significance |
| :--- | :--- | :--- |
| **`sequence_id`** | `#1, #2, #3...` | Verifies data continuity; any gap indicates dropped wireless frames. |
| **`timestamp_ms`** | `10000 ms (10.0 s)` | Absolute elapsed operational time since node power-on. |
| **`free_heap_bytes`**| `245,696 Bytes` | Monitored to detect memory depletion or runtime instability. |
| **`min_free_heap`** | `210,000 Bytes` | Worst-case memory margin reached during operation. |
| **`chip_temp_c`** | `38.6 °C` | Thermal profile of the onboard microprocessor. |
| **`wifi_rssi_dbm`** | `-46 dBm` | Radio signal strength between payload and ground station. |
| **`cpu_freq_mhz`** | `240 MHz` | Clock speed verification for power consumption auditing. |

---

## 4. Hardware vs. Simulation: How Telemetry is Generated

Because physical ESP32 microcontrollers are not attached to the system, the project utilizes a dual-path engineering approach:

### Path A: Physical ESP32 Hardware (Production Firmware)
When flashed to real ESP32 boards via ESP-IDF, the code in `esp32_client/main/main_client.c` reads real silicon registers:
* **Temperature:** `temperature_sensor_get_celsius()` (internal CPU junction thermal sensor).
* **Heap:** `esp_get_free_heap_size()` (internal SRAM controller).
* **Uptime:** `esp_timer_get_time()` (internal 64-bit microsecond counter).
* **Radio:** `esp_wifi_sta_get_ap_info()` (2.4 GHz RF baseband).

### Path B: Virtual Simulator (Software Simulation)
In the browser simulator (`web_simulator/index.html`), the exact same states, timing, and pointer logic are executed in software:
* **Timer Loop:** Generates realistic 10-second intervals (`0.1 Hz`) or 2-second fast testing cycles (`1.0 Hz`).
* **Synthetic Silicon Modeling:** Generates genuine ESP32 operational values (~245 KB heap, 38.2 °C–39.8 °C junction heat, -45 dBm to -51 dBm wireless attenuation).
* **Authentic Algorithm:** The circular buffer logic, FIFO extraction, pointer handling, and `0xEF` overflow locking match the C implementation line-for-line.

---

## 5. Telecommand Interface Reference

| Command | Shorthand | Operational Function | Effect on Buffer |
| :--- | :---: | :--- | :--- |
| **`READ`** | `R` | Downlinks the oldest record in the buffer. | Advances `tail`, **frees slot** for new writes. |
| **`PEEK`** | `P` | Non-destructive telemetry inspection. | Leaves `tail` unchanged; slot is **not** freed. |
| **`STATUS`**| `S` | Reports capacity, head, tail, and write/read counts. | Read-only diagnostic query. |
| **`PAUSE_WR`**| — | Suspends periodic 0.1 Hz data generation. | Preserves current buffer state. |
| **`RESUME_WR`**| — | Restarts periodic writing from current head index. | Normal acquisition resumes. |
| **`RESTART_RD`**| — | Restarts/resumes downlink session. | Keeps current read pointer position. |
| **`RESET_BUF`**| — | Clears all memory slots and resets all counters. | Sets `head=0`, `tail=0`, `count=0`. |
| **`HELP`** | `?` | Displays telecommand reference manual. | Read-only assistance. |
