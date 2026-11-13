
"""
===============================================================================
Interactive Terminal REPL: Dual ESP32 Firmware Simulator (No Hardware Needed)
===============================================================================
Simulates both ESP32 Client and Server nodes in real-time on your PC.
- Client collects system telemetry at 0.1 Hz (every 10s) and writes to Circular Buffer.
- Server exposes interactive PC Serial Terminal (UART 115200 baud emulation).
- When buffer saturates, Client broadcasts 0xEFULL_MEM exception.
- Type commands in real-time: READ, PEEK, STATUS, PAUSE_WR, RESUME_WR, RESET_BUF, HELP.
===============================================================================
"""

import sys
import os
import time
import threading
import queue
import struct

sys.path.insert(0, os.path.dirname(__file__))

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


class VirtualClientNode(threading.Thread):
    def __init__(self, air_c: queue.Queue, air_s: queue.Queue, capacity: int = 8, period_sec: float = 10.0):
        super().__init__(daemon=True)
        self.air_c = air_c
        self.air_s = air_s
        self.period_sec = period_sec
        self.cb = CircularBuffer(capacity, struct.calcsize("<IIIIbbHI"))
        self.seq_id = 0
        self.tx_seq = 0
        self.running = True
        self.start_time = time.time()

    def run(self):
        last_tick = time.time()
        while self.running:
            try:
                while True:
                    raw_pkt = self.air_c.get_nowait()
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
        uptime_ms = int((time.time() - self.start_time) * 1000) & 0xFFFFFFFF
        rec = TelemetryRecord(
            timestamp_ms=uptime_ms,
            sequence_id=self.seq_id,
            free_heap=245760 - (self.seq_id * 64),
            min_free_heap=210000,
            chip_temp=38 + (self.seq_id % 4),
            wifi_rssi=-48 - (self.seq_id % 6),
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
            self.air_s.put(pkt)

    def handle_command(self, opcode: int):
        self.tx_seq += 1
        if opcode == CMD_READ_REQ:
            status, data, slot = self.cb.read()
            if status == CB_OK:
                pkt = pack_frame(self.tx_seq, RESP_READ_DATA, data)
            else:
                pkt = pack_frame(self.tx_seq, RESP_BUFFER_EMPTY, b"")
            self.air_s.put(pkt)

        elif opcode == CMD_PEEK_REQ:
            status, data, slot = self.cb.peek()
            if status == CB_OK:
                pkt = pack_frame(self.tx_seq, RESP_PEEK_DATA, data)
            else:
                pkt = pack_frame(self.tx_seq, RESP_BUFFER_EMPTY, b"")
            self.air_s.put(pkt)

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
            self.air_s.put(pkt)

        elif opcode == CMD_PAUSE_WR_REQ:
            self.cb.pause_write()
            self.air_s.put(pack_frame(self.tx_seq, MSG_ACK, b""))

        elif opcode == CMD_RESUME_WR_REQ:
            self.cb.resume_write()
            self.air_s.put(pack_frame(self.tx_seq, MSG_ACK, b""))

        elif opcode == CMD_RESTART_RD_REQ:
            self.cb.restart_read()
            self.air_s.put(pack_frame(self.tx_seq, MSG_ACK, b""))

        elif opcode == CMD_RESET_BUF_REQ:
            self.cb.reset()
            self.air_s.put(pack_frame(self.tx_seq, MSG_ACK, b""))


def print_menu():
    print("""
====================================================================
      Dual Firmware (ESP32) - Circular Buffer Simulation            
====================================================================
 Available Telecommands:
   READ       (R) : Read oldest unread data; frees slot for rewriting
   PEEK       (P) : Inspect oldest unread data without advancing tail
   STATUS     (S) : Display buffer diagnostics, capacity, & pointers 
   PAUSE_WR       : Pause 0.1 Hz periodic write telemetry acquisition
   RESUME_WR      : Resume periodic write; appends from last written
   RESTART_RD     : Reset/resume read session
   RESET_BUF      : Clear & reinitialize circular buffer memory
   FAST           : Toggle 0.1 Hz (10s) vs 1 Hz (1s) acquisition speed
   HELP       (?) : Show this telecommand menu
   QUIT       (Q) : Exit simulator
====================================================================""")


def main():
    air_to_client = queue.Queue()
    air_to_server = queue.Queue()

    client = VirtualClientNode(air_to_client, air_to_server, capacity=8, period_sec=10.0)
    client.start()

    print_menu()
    print("\n[CLIENT INITIALIZED] Buffer capacity: 8 slots. 0.1 Hz periodic timer started (10.0s).")
    print("Ready for serial telecommands. Type 'STATUS' or 'READ' below:\n")

    tx_seq = 0

    def check_incoming_packets():
        while client.running:
            try:
                raw_pkt = air_to_server.get(timeout=0.1)
                parsed = unpack_frame(raw_pkt)
                if not parsed:
                    continue
                _, opcode, payload = parsed

                if opcode == RESP_READ_DATA or opcode == RESP_PEEK_DATA:
                    rec = TelemetryRecord.deserialize(payload)
                    lbl = "READ (SLOT FREED)" if opcode == RESP_READ_DATA else "PEEK (UNFREED)"
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
                    print(f"  Write Acquisition  : {'ENABLED (0.1 Hz active)' if wr_en else 'PAUSED'}")
                    print("-------------------------------------\nCMD> ", end="", flush=True)

                elif opcode == ALERT_BUFFER_FULL:
                    slot, cap, ex_cnt, uptime = struct.unpack("<IIII", payload)
                    print("\n" + "=" * 80)
                    print("*** [CRITICAL EXCEPTION: 0xEFULL_MEM] CLIENT CIRCULAR BUFFER OVERFLOW ***")
                    print(f"  Status          : MEMORY FULL! 0.1 Hz periodic write blocked.")
                    print(f"  Buffer Slot     : Head reached #{slot} (Capacity: {cap} slots fully utilized).")
                    print(f"  Client Uptime   : {uptime} ms")
                    print(f"  Cumulative Hits : {ex_cnt} full-memory exceptions raised.")
                    print(f"  Resolution      : Send 'READ' telecommand to extract oldest unread data")
                    print(f"                    and release memory address space for rewriting!")
                    print("=" * 80 + "\nCMD> ", end="", flush=True)

                elif opcode == MSG_ACK:
                    print("\n[SUCCESS] Client acknowledged command execution.\nCMD> ", end="", flush=True)

            except queue.Empty:
                pass

    listener_thread = threading.Thread(target=check_incoming_packets, daemon=True)
    listener_thread.start()

    fast_mode = False

    try:
        while True:
            cmd = input("CMD> ").strip().upper()
            if not cmd:
                continue

            tx_seq += 1

            if cmd in ("QUIT", "Q", "EXIT"):
                print("Exiting simulator. Goodbye!")
                client.running = False
                break
            elif cmd in ("READ", "R"):
                air_to_client.put(pack_frame(tx_seq, CMD_READ_REQ))
            elif cmd in ("PEEK", "P"):
                air_to_client.put(pack_frame(tx_seq, CMD_PEEK_REQ))
            elif cmd in ("STATUS", "S"):
                air_to_client.put(pack_frame(tx_seq, CMD_STATUS_REQ))
            elif cmd == "PAUSE_WR":
                air_to_client.put(pack_frame(tx_seq, CMD_PAUSE_WR_REQ))
            elif cmd == "RESUME_WR":
                air_to_client.put(pack_frame(tx_seq, CMD_RESUME_WR_REQ))
            elif cmd == "RESTART_RD":
                air_to_client.put(pack_frame(tx_seq, CMD_RESTART_RD_REQ))
            elif cmd == "RESET_BUF":
                air_to_client.put(pack_frame(tx_seq, CMD_RESET_BUF_REQ))
            elif cmd == "FAST":
                fast_mode = not fast_mode
                client.period_sec = 1.0 if fast_mode else 10.0
                print(f"[SETTING] Acquisition speed: {'1.0s (Fast)' if fast_mode else '10.0s (0.1 Hz Standard)'}")
            elif cmd in ("HELP", "?"):
                print_menu()
            else:
                print(f"Unknown command: '{cmd}'. Type 'HELP' for valid telecommands.")
            time.sleep(0.15)
    except (KeyboardInterrupt, EOFError):
        print("\nExiting simulator.")
        client.running = False

if __name__ == "__main__":
    main()

