# Wokwi dual-node simulation

This ESP-IDF project runs the client and server roles on two ESP32 boards in
Wokwi. Both boards use the same firmware image; GPIO 4 selects the role:

- Server board: GPIO 4 is connected to 3V3.
- Client board: GPIO 4 is connected to GND.

The client uses the repository's `common/circular_buffer.c` and `protocol.h`.
The 16-slot buffer writes telemetry every 10 seconds, as in the firmware.
The server console accepts `READ`, `PEEK`, `STATUS`, `PAUSE_WR`, `RESUME_WR`,
`RESTART_RD`, `RESET_BUF`, and `HELP`.

Wokwi does not provide an ESP-NOW link between these simulated boards in this
setup. UART1 (GPIO 17 TX and GPIO 16 RX) carries the same CRC-framed commands
and responses instead. The buffer, command opcodes, packet format, and timer
period remain in C; this setup does not validate ESP-NOW radio behavior.

## Run it

Open `wokwi_sim` as the VS Code workspace folder with ESP-IDF and the Wokwi
extension installed. In its terminal, build the ESP32 image:

```powershell
idf.py set-target esp32
idf.py build
```

Then run **Wokwi: Start Simulator** from the VS Code Command Palette. Use the
server's serial monitor to enter commands. The first telemetry write occurs
after 10 seconds; the full-buffer alarm occurs on the first write after all 16
slots are occupied.
