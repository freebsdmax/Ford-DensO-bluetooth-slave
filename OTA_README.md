# I2C Slave Device with OTA Support

This ESP32 Bluetooth device acts as an I2C slave (address `0x42`) and can be updated via OTA through I2C protocol from a master device (e.g., `pipeline_play_sdcard_music`).

## I2C Configuration

- **Address**: `0x42`
- **SDA**: GPIO 22
- **SCL**: GPIO 23
- **Pull-ups**: Internal enabled

## OTA Protocol

The device implements the following I2C OTA protocol:

### 1. OTA Start (0xF0)
Master sends: `[0xF0, size_MSB, size_2, size_3, size_LSB]`  
Slave responds: `0xAA` (success) or `0xFF` (error)

### 2. OTA Data Block (0xF1)
Master sends: `[0xF1, block_hi, block_lo, len, data[64]]`  
- `block_hi`, `block_lo`: 16-bit block number
- `len`: actual data length (max 64)
- `data[]`: firmware bytes

Slave responds: `0xBB` (success) or `0xFF` (error)

### 3. OTA End (0xF2)
Master sends: `[0xF2]`  
Slave responds: `0xCC` (success) or `0xFF` (error)  
Device will reboot automatically after 2 seconds.

### 4. OTA Abort (0xF3)
Master sends: `[0xF3]`  
Slave responds: `0xDD` (aborted)

## Regular I2C Commands

- `0x01` - Volume Up
- `0x02` - Volume Down
- `0x03` - Play/Pause
- `0x04` - Next Track
- `0x05` - Previous Track
- `0x10` - Get Status (returns `0x01` if connected, `0x00` otherwise)

## Building and Flashing

1. Build the project:
   ```bat
   cd d:\emulators\esp32-bluetooth
   idf.py build
   ```

2. Flash to ESP32:
   ```bat
   idf.py -p COMx flash monitor
   ```

3. Or use OTA from master device via web interface at `http://192.168.4.1/`

## OTA Update from Master Device

1. Connect to ESP32-S3 (pipeline_play_sdcard_music) Wi-Fi AP: `ESP32-CAN-Sniffer` / `12345678`
2. Open `http://192.168.4.1/` in browser
3. Select **I2C Slave (0x42)** radio button
4. Choose `app.bin` from `esp32-bluetooth/build/` folder
5. Click Upload & Update
6. Wait for completion (~30 seconds for 1.5MB firmware)
7. I2C slave device will reboot automatically

## Partitions

The device uses OTA partitions:
- `ota_0`: 1536KB at 0x20000
- `ota_1`: 1536KB at 0x1A0000
- `otadata`: 8KB at 0xF000

## Notes

- Firmware blocks are sent in 64-byte chunks
- Total transfer time depends on I2C speed (100kHz) and firmware size
- Device reboots automatically after successful OTA
- Master device must implement proper timing (20-50ms delays between blocks)
- If OTA fails mid-transfer, device stays on previous working partition
