
"""
===============================================================================
Interactive Software Simulator: ESP32 Client-Server Wireless Circular Buffer
===============================================================================
Simulates the entire multi-node hardware/software system on PC:
  - ESP32 Client Node:
      * 0.1 Hz telemetry acquisition (every 10 seconds; configurable)
      * Static circular buffer memory management
      * Full-memory exception broadcasting (0xEFULL_MEM)
  - ESP32 Server Node:
      * PC Serial Terminal UI (Non-blocking UART emulation)
      * ESP-NOW packet framing with CRC-16 CCITT
      * Telecommand dispatch and response formatting
===============================================================================
"""

import sys
import time
import threading
import queue
import struct
from test_circular_buffer import (
    CircularBuffer, TelemetryRecord,
    CB_OK, CB_ERR_EMPTY, CB_ERR_FULL, CB_ERR_WRITE_DISABLED
)

CMD_READ_REQ       = 0x10
RESP_READ_DATA     = 0x11
CMD_PEEK_REQ       = 0x12
RESP_PEEK_DATA     = 0x13
RESP_BUFFER_EMPTY  = 0x14
CMD_STATUS_REQ     = 0x20
RESP_STATUS_DATA   = 0x21
CMD_PAUSE_WR_REQ   = 0x30
CMD_RESUME_WR_REQ  = 0x31
CMD_RESTART_RD_REQ = 0x32
CMD_RESET_BUF_REQ  = 0x33
MSG_ACK            = 0x01
MSG_NACK           = 0x02
ALERT_BUFFER_FULL  = 0xEF

SYNC1 = 0xAA
SYNC2 = 0x55

def compute_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc

def pack_frame(seq: int, opcode: int, payload: bytes = b"") -> bytes:
    header = struct.pack("<BBHBB", SYNC1, SYNC2, seq, opcode, len(payload))
    data_to_crc = header + payload
    crc = compute_crc16(data_to_crc)
    return data_to_crc + struct.pack("<H", crc)

def unpack_frame(raw: bytes):
    if len(raw) < 7:
        return None
    sync1, sync2, seq, opcode, payload_len = struct.unpack("<BBHBB", raw[:6])
    if sync1 != SYNC1 or sync2 != SYNC2:
        return None
    payload = raw[6:6 + payload_len]
    recv_crc, = struct.unpack("<H", raw[6 + payload_len: 8 + payload_len])
    calc_crc = compute_crc16(raw[:6 + payload_len])
    if recv_crc != calc_crc:
        return None
    return seq, opcode, payload


class SimulatedClient(threading.Thread):
    def __init__(self, air_to_client: queue.Queue, air_to_server: queue.Queue,
                 capacity: int = 6, period_sec: float = 3.0):
        super().__init__(daemon=True)
        self.air_to_client = air_to_client
        self.air_to_server = air_to_server
        self.period_sec = period_sec
        self.cb = CircularBuffer(capacity, struct.calcsize("<IIIIbbHI"))
        self.seq_id = 0
        self.tx_seq = 0
        self.running = True

    def run(self):
        last_tick = time.time()
        while self.running:
            try:
                raw_pkt = self.air_to_client.get_nowait()
                parsed = unpack_frame(raw_pkt)
                if parsed:
                    _, opcode, _ = parsed
                    self.handle_command(opcode)
            except queue.Empty:
                pass

            now = time.time()
            if now - last_tick >= self.period_sec:
                last_tick = now
                self.record_telemetry()

            time.sleep(0.05)

    def record_telemetry(self):
        self.seq_id += 1
        uptime_ms = int(time.time() * 1000) & 0xFFFFFFFF
        rec = TelemetryRecord(
            timestamp_ms=uptime_ms,
            sequence_id=self.seq_id,
            free_heap=245120 - (self.seq_id * 64),
            min_free_heap=210400,
            chip_temp=38,
            wifi_rssi=-48,
            buffer_slot_id=self.cb.head
        )
        status, slot = self.cb.write(rec.serialize())
        if status == CB_OK:
            pass
        elif status == CB_ERR_FULL:
            self.tx_seq += 1
            payload = struct.pack("<IIII", self.cb.head, self.cb.capacity,
                                  self.cb.full_exceptions, uptime_ms)
            pkt = pack_frame(self.tx_seq, ALERT_BUFFER_FULL, payload)
            self.air_to_server.put(pkt)

    def handle_command(self, opcode: int):
        self.tx_seq += 1
        if opcode == CMD_READ_REQ:
            status, data, slot = self.cb.read()
            if status == CB_OK:
                pkt = pack_frame(self.tx_seq, RESP_READ_DATA, data)
            else:
                pkt = pack_frame(self.tx_seq, RESP_BUFFER_EMPTY, b"")
            self.air_to_server.put(pkt)

        elif opcode == CMD_PEEK_REQ:
            status, data, slot = self.cb.peek()
            if status == CB_OK:
                pkt = pack_frame(self.tx_seq, RESP_PEEK_DATA, data)
            else:
                pkt = pack_frame(self.tx_seq, RESP_BUFFER_EMPTY, b"")
            self.air_to_server.put(pkt)

        elif opcode == CMD_STATUS_REQ:
            payload = struct.pack("<IIIIIIIBBBx",
                                  self.cb.capacity,
                                  self.cb.count,
                                  self.cb.head,
                                  self.cb.tail,
                                  self.cb.total_writes,
                                  self.cb.total_reads,
                                  self.cb.full_exceptions,
                                  1 if self.cb.count >= self.cb.capacity else 0,
                                  1 if self.cb.count == 0 else 0,
                                  1 if self.cb.write_enabled else 0)
            pkt = pack_frame(self.tx_seq, RESP_STATUS_DATA, payload)
            self.air_to_server.put(pkt)

        elif opcode == CMD_PAUSE_WR_REQ:
            self.cb.pause_write()
            pkt = pack_frame(self.tx_seq, MSG_ACK, b"")
            self.air_to_server.put(pkt)

        elif opcode == CMD_RESUME_WR_REQ:
            self.cb.resume_write()
            pkt = pack_frame(self.tx_seq, MSG_ACK, b"")
            self.air_to_server.put(pkt)

        elif opcode == CMD_RESTART_RD_REQ:
            self.cb.restart_read()
            pkt = pack_frame(self.tx_seq, MSG_ACK, b"")
            self.air_to_server.put(pkt)

        elif opcode == CMD_RESET_BUF_REQ:
            self.cb.reset()
            pkt = pack_frame(self.tx_seq, MSG_ACK, b"")
            self.air_to_server.put(pkt)


class SimulatedServer:
    def __init__(self, air_to_client: queue.Queue, air_to_server: queue.Queue):
        self.air_to_client = air_to_client
        self.air_to_server = air_to_server
        self.tx_seq = 0

    def send_cmd(self, opcode: int):
        self.tx_seq += 1
        pkt = pack_frame(self.tx_seq, opcode)
        self.air_to_client.put(pkt)

    def check_incoming(self):
        try:
            while True:
                raw_pkt = self.air_to_server.get_nowait()
                parsed = unpack_frame(raw_pkt)
                if not parsed:
                    continue
                _, opcode, payload = parsed
                self.render_packet(opcode, payload)
        except queue.Empty:
            pass

    def render_packet(self, opcode: int, payload: bytes):
        if opcode == RESP_READ_DATA or opcode == RESP_PEEK_DATA:
            rec = TelemetryRecord.deserialize(payload)
            lbl = "READ (FREED)" if opcode == RESP_READ_DATA else "PEEK (UNFREED)"
            print(f"\n--- [{lbl} TELEMETRY RECORD] ---")
            print(f"  Buffer Slot ID   : #{rec.buffer_slot_id}")
            print(f"  Sequence Number  : {rec.sequence_id}")
            print(f"  Client Uptime    : {rec.timestamp_ms} ms ({rec.timestamp_ms / 1000.0:.2f} s)")
            print(f"  Free Heap Memory : {rec.free_heap} bytes")
            print(f"  Min Free Heap    : {rec.min_free_heap} bytes")
            print(f"  Chip Temperature : {rec.chip_temp} C")
            print(f"  Wi-Fi RSSI       : {rec.wifi_rssi} dBm")
            print(f"  CPU Frequency    : {rec.cpu_freq_mhz} MHz")
            print("------------------------------------\nCMD> ", end="", flush=True)

        elif opcode == RESP_BUFFER_EMPTY:
            print("\n[NOTICE] Circular buffer is EMPTY. No unread telemetry.\nCMD> ", end="", flush=True)

        elif opcode == RESP_STATUS_DATA:
            cap, cnt, wr, rd, tot_wr, tot_rd, full_ex, is_f, is_e, wr_en = struct.unpack("<IIIIIIIBBBx", payload)
            print(f"\n--- [CIRCULAR BUFFER DIAGNOSTICS] ---")
            print(f"  Total Capacity     : {cap} slots")
            print(f"  Active Stored Data : {cnt} slots ({cnt * 100 // cap}% utilized)")
            print(f"  Free Slots Avail   : {cap - cnt} slots")
            print(f"  Next Write Address : Slot #{wr} (Head)")
            print(f"  Next Read Address  : Slot #{rd} (Tail)")
            print(f"  Total Writes Done  : {tot_wr}")
            print(f"  Total Reads Done   : {tot_rd}")
            print(f"  Memory Full Alarms : {full_ex}")
            print(f"  Buffer State       : {'[FULL SATURATION] ' if is_f else ''}{'[EMPTY]' if is_e else '[ACTIVE DATA]'}")
            print(f"  Write Acquisition  : {'ENABLED' if wr_en else 'PAUSED'}")
            print("-------------------------------------\nCMD> ", end="", flush=True)

        elif opcode == ALERT_BUFFER_FULL:
            slot, cap, ex_cnt, uptime = struct.unpack("<IIII", payload)
            print("\n" + "=" * 80)
            print("*** [CRITICAL EXCEPTION: 0xEFULL_MEM] CLIENT CIRCULAR BUFFER OVERFLOW ***")
            print(f"  Status          : MEMORY FULL! Periodic write blocked.")
            print(f"  Buffer Slot     : Head reached #{slot} (Capacity: {cap} slots fully utilized).")
            print(f"  Client Uptime   : {uptime} ms")
            print(f"  Cumulative Hits : {ex_cnt} full-memory exceptions raised.")
            print(f"  Resolution      : Send 'READ' telecommand to extract oldest unread data")
            print(f"                    and release memory address space for rewriting!")
            print("=" * 80 + "\nCMD> ", end="", flush=True)

        elif opcode == MSG_ACK:
            print("\n[SUCCESS] Client acknowledged command execution.\nCMD> ", end="", flush=True)


def main():
    print("====================================================================")
    print("  ESP32 CLIENT-SERVER WIRELESS CIRCULAR BUFFER SIMULATION")
    print("====================================================================")
    print("Running automated demonstration sequence...")

    air_c = queue.Queue()
    air_s = queue.Queue()

    client = SimulatedClient(air_c, air_s, capacity=4, period_sec=0.5)
    server = SimulatedServer(air_c, air_s)
    client.start()

    print("\n1. Allowing client to write telemetry and trigger memory full exception...")
    for _ in range(12):
        server.check_incoming()
        time.sleep(0.2)

    print("\n2. Sending 'STATUS' telecommand...")
    server.send_cmd(CMD_STATUS_REQ)
    time.sleep(0.2)
    server.check_incoming()

    print("\n3. Sending 'READ' telecommand (1st read)...")
    server.send_cmd(CMD_READ_REQ)
    time.sleep(0.2)
    server.check_incoming()

    print("\n4. Sending 'READ' telecommand (2nd read)...")
    server.send_cmd(CMD_READ_REQ)
    time.sleep(0.2)
    server.check_incoming()

    print("\n5. Checking STATUS after reading 2 records...")
    server.send_cmd(CMD_STATUS_REQ)
    time.sleep(0.2)
    server.check_incoming()

    print("\n6. Sending 'PAUSE_WR'...")
    server.send_cmd(CMD_PAUSE_WR_REQ)
    time.sleep(0.2)
    server.check_incoming()

    print("\n7. Sending 'RESUME_WR'...")
    server.send_cmd(CMD_RESUME_WR_REQ)
    time.sleep(0.2)
    server.check_incoming()

    client.running = False
    print("\n====================================================================")
    print("  SIMULATION DEMO COMPLETED SUCCESSFULLY!")
    print("====================================================================")

if __name__ == "__main__":
    main()

