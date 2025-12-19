UART Control & OTA Link

Overview
- This firmware switches the control/OTA link from I2C slave to UART.
- Default pins: TX=GPIO17, RX=GPIO16, port=UART1, baud=115200.
- Pins/baud can be overridden at compile time via macros: CONFIG_APP_UART_PORT, CONFIG_APP_UART_TX_PIN, CONFIG_APP_UART_RX_PIN, CONFIG_APP_UART_BAUD (optional; defaults are used if not defined).

Wiring
- ESP32 TX (GPIO17) -> Master RX
- ESP32 RX (GPIO16) -> Master TX
- Common GND between devices

Protocol
- Byte-oriented, no start-of-frame. Commands are executed as soon as required bytes arrive.
- Keep write calls atomic per command to avoid interleaving from the master side.

Async Events (device -> master)
- 0x90: BT_CONNECTED
- 0x91: BT_DISCONNECTED

Commands (master -> ESP32)
- 0x04: NEXT_TRACK (no payload)
- 0x05: PREV_TRACK (no payload)
- 0x03: PLAY_PAUSE (no payload)
- 0x01: VOL_UP (no payload)
- 0x02: VOL_DOWN (no payload)
- 0x10: GET_STATUS (no payload) -> returns 1 byte: 0x01 connected, 0x00 not connected
- 0x21: LOG_AVAIL (no payload) -> returns 2 bytes: <avail_low><avail_high>
- 0x20 <len>: LOG_READ -> returns up to <len> bytes of log

OTA (master -> ESP32)
- 0xEF: ENTER_OTA_MODE -> returns 0xEE
- 0xF0 <size3><size2><size1><size0>: OTA_START (big endian size) -> ACK 0xAA or 0xFF <err>
- 0xF1 <block_hi><block_lo><len><data...>: OTA_DATA -> ACK 0xBB or 0xFF <err>
- 0xF2: OTA_END -> ACK 0xCC or 0xFF <err> (device reboots on success)
- 0xF3: OTA_ABORT -> ACK 0xDD

OTA Error codes (after 0xFF)
- 0x00: none
- 0x01: not started
- 0x02: block mismatch
- 0x03: write failed
- 0x04: partition error
- 0x05: length invalid
- 0x06: busy

Notes
- During OTA, the firmware minimizes extra UART chatter by disabling mirrored log prints (silent mode), but OTA acks still transmit.
- For reliability, send OTA_DATA in moderate chunk sizes (<= 128 bytes as implemented). The <len> byte is the data block size.

Examples (hex, spaces for readability)
- GET_STATUS:    send "10" -> recv "01" or "00"
- LOG_AVAIL:     send "21" -> recv "<L> <H>"
- LOG_READ(64):  send "20 40" -> recv up to 64 bytes
- OTA_START(0x00123456): send "F0 00 12 34 56" -> recv "AA" on success
- OTA_DATA blk 0 with 16 bytes: send "F1 00 00 10 <16 bytes>" -> recv "BB"
- OTA_END:       send "F2" -> recv "CC" then device reboots

Troubleshooting
- If you see no responses: verify wiring, baud=115200, TX/RX not swapped, common ground.
- If OTA fails early: ensure enough power and that the update partition is present.
- If parsing desync occurs (due to line noise), resend the command; the parser drops unknown bytes to resync.
