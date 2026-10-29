# Ford Player — Slave (ESP32 Bluetooth) · довідка

Окремий пристрій, який приймає звук із телефона по Bluetooth і віддає його на
Master по I2S, а командами/метаданими обмінюється по UART.

## Три репозиторії (пов'язані між собою)

| Компонент | Репозиторій | Роль |
|---|---|---|
| **Master** | https://github.com/freebsdmax/Ford-DensO-mp3-master | ESP32-S3: MP3, BLE, Web UI, OTA, CAN |
| **Slave** (цей) | https://github.com/freebsdmax/Ford-DensO-bluetooth-slave | ESP32: A2DP-приймач, AVRCP-метадані, UART |
| **App** | https://github.com/freebsdmax/Esp32_Android_app | Android (BLE-only) |

Повна довідка по системі: **`HELP.md` у репозиторії Master**
(https://github.com/freebsdmax/Ford-DensO-mp3-master/blob/debug/HELP.md).

## Залізо

| Параметр | Значення |
|---|---|
| MCU | ESP32 (Classic BT + BLE), 4 МБ flash |
| Роль BT | **A2DP sink** (приймає звук), AVRCP **CT** |
| UART до Master | UART1, **TX 23, RX 22**, 115200 |
| I2S вихід | як налаштовано у board (BCLK/WS/DOUT) → на Master (вхід 41/39/40) |
| Пробудження | GPIO 34 |

## Що вміє

- приймає A2DP-потік (SBC) і віддає PCM по I2S у Master;
- AVRCP: збирає **назву, виконавця, тривалість, позицію, play/pause** і віддає
  їх Master по UART (команда `0x0A`);
- виконує команди Master: `0x04` next, `0x05` prev, `0x07` play, `0x08` pause,
  `0x0B` seek (наближений), `0x09` версія прошивки;
- приймає власну прошивку по UART (`uart_ota_slave`), версія = git-тег.

## Збірка і прошивка

```powershell
cd D:\emulators\Ford-DensO-bluetooth-slave
& "C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1"
idf.py build
idf.py -p COMx flash monitor
```

Злитий образ (ESP32, 4 МБ):

```powershell
python -m esptool merge_bin --flash_mode dio --flash_freq 40m --flash_size 4MB `
  0x1000 build\bootloader\bootloader.bin `
  0x8000 build\partition_table\partition-table.bin `
  0x10000 build\Ford-DensO-bluetooth-slave.bin
```

## Оновлення

1. **З застосунку** (основне): OTA → «Прошивка Slave» → Master приймає .bin по BLE,
   кладе у `/storage/slave.bin`, прошиває ним Slave по UART (115200) і видаляє
   файл після успіху. ~2 хвилини, прогрес у шторці.
2. **З Web UI Master**: `POST /upload_slave`.
3. **По USB** (якщо Slave старіший за `v1.0.3` — він ще не знає команд `0x09`/`0x0A`).

## UART протокол

| Код | Напрям | Значення |
|---|---|---|
| `0x04` | M→S | наступний трек |
| `0x05` | M→S | попередній трек |
| `0x06` | M→S | версія збірки з NVS |
| `0x07` | M→S | play |
| `0x08` | M→S | pause |
| `0x09` | M→S | версія прошивки → `[0xAA][0x09][len][chars]` |
| `0x0C` | M→S | дай стан медіа |
| `0x0A` | S→M | `flags,pos:4,dur:4,tlen,title,alen,artist` |
| `0x0B` | M→S | seek `[delta_ms:int32 LE]` |

OTA-кадри Master→Slave: `[0x55][0x01..0x05]` (start/data/end/verify/abort).

## Обмеження

- **Абсолютної перемотки немає** (AVRCP): seek = до 3 натискань ⏩/⏪ по ~10 с.
- Метадані залежать від додатка: YouTube дає заголовок відео + канал, альбом
  порожній, у live-стрімів тривалість 0.
- Якщо телефон не публікує MediaSession — метаданих не буде взагалі.

## Версії

| Версія | Що додано |
|---|---|
| v1.0.3 | відповідь на `0x09` (версія прошивки) |
| v1.0.4 | виправлено обробник `0x09` |
| v1.0.5 | AVRCP-метадані (`0x0A`), seek (`0x0B`), запит стану (`0x0C`) |
| v1.0.6 | менші кроки seek (макс. 3 натискання, ігнорування < 5 с) |

## Посилання

- Master: https://github.com/freebsdmax/Ford-DensO-mp3-master
- Slave: https://github.com/freebsdmax/Ford-DensO-bluetooth-slave
- App: https://github.com/freebsdmax/Esp32_Android_app
