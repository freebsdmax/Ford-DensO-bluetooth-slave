# Ford Player — Slave (ESP32 A2DP)

Bluetooth-модуль системи: приймає звук з телефона (A2DP) і віддає його Master-платі
по I2S, а керування та оновлення прошивки отримує по UART.

Працює на **ESP-ADF** (Espressif Audio Development Framework).

---

## Роль у системі

```
 телефон ──Bluetooth(A2DP/AVRCP)──► SLAVE ──I2S──► MASTER ──► підсилювач
                                     ▲
                                     └──UART (керування + OTA)── MASTER
```

- **Аудіо**: телефон → A2DP → Slave декодує → I2S у Master.
- **Керування**: Master шле по UART `next/prev/play/pause` → Slave передає їх
  телефону як **AVRCP** команди.
- **Оновлення**: Master передає образ прошивки по UART, Slave пише його в OTA-слот.

## Залізо

| Параметр | Значення |
|---|---|
| Чіп | **ESP32** (класичний, з Bluetooth Classic) |
| Flash | 4 МБ |
| Framework | **ESP-IDF 5.5.1** + **ESP-ADF** |
| Роль BT | A2DP **Sink** + AVRCP (CT) |

## Підключення

### UART до Master

| Slave | Master | Примітка |
|---|---|---|
| TX **GPIO 23** | RX **GPIO 9** | дані Slave → Master |
| RX **GPIO 22** | TX **GPIO 8** | дані Master → Slave |
| GND | GND | обов'язково спільна земля |

**Baud: 115200**, 8N1, без flow control. Швидкість має збігатися на обох платах —
інакше Slave читає сміття й може випадково надіслати телефону `next/pause`.

### I2S до Master

| Сигнал | Пін на Master |
|---|---|
| BCLK | GPIO **41** |
| WS (LRCK) | GPIO **39** |
| DIN (дані) | GPIO **40** |

Slave тактує лінію (I2S master), Master працює як I2S **slave** і приймає потік.

## Збірка

```powershell
C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1
$env:ADF_PATH = "C:\Espressif\esp-adf"    # обов'язково

cd D:\emulators\Ford-DensO-bluetooth-slave
idf.py set-target esp32
idf.py build
```

Результат: `build/Ford-DensO-bluetooth-slave.bin`.

## Прошивка

```powershell
idf.py -p COMx flash monitor
```

Розділи (4 МБ):

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 КБ |
| otadata | data/ota | 0xF000 | 8 КБ |
| phy_init | data/phy | 0x11000 | 4 КБ |
| ota_0 | app | 0x20000 | 1536 КБ |
| ota_1 | app | 0x1A0000 | 1536 КБ |

## UART-протокол

Байт-орієнтований, без довжини кадру для команд: перший байт — префікс.

### Звичайні команди (Master → Slave)

| Префікс | Команда | Дія |
|---|---|---|
| `0xAA` | `0x04` NEXT | AVRCP next |
| `0xAA` | `0x05` PREV | AVRCP previous |
| `0xAA` | `0x06` GET_VERSION | відповідь 65 Б (дати збірок) |
| `0xAA` | `0x07` PLAY | AVRCP play |
| `0xAA` | `0x08` PAUSE | AVRCP pause |

### OTA (Master → Slave)

| Префікс | Команда | Формат |
|---|---|---|
| `0x55` | `0x01` START | `size:4`, `crc32:4` → відповідь **READY 0x08** |
| `0x55` | `0x02` DATA | `block:2`, `len:1`, `data[240]`, `crc16:2` → **ACK 0x06** / **NACK 0x07** |
| `0x55` | `0x03` END | `crc32:4` → **COMPLETE 0x09** |
| `0x55` | `0x05` ABORT | скасувати |

Відповідь: `[0x55][resp][block:2]` (4 байти), таймаут на блок — 1 с, 3 спроби.

## Автовідтворення

Після підключення телефона Slave:
1. чекає, поки піднімуться і **A2DP**, і **AVRCP**;
2. надсилає AVRCP `play` (до 5 спроб по 2 с);
3. зупиняється, коли аудіо реально пішло (`a2dp_audio_started`).

У логах:
```
Sending AVRCP play command (attempt 1/5)
A2DP audio is already playing
```
Якщо AVRCP не піднявся:
```
AVRCP did not connect; skipping automatic play command
```

## OTA від Master

Master передає образ по UART блоками по 240 Б з підтвердженням кожного блоку.
Швидкість ~115200 → **~25–30 КБ/с**, образ 1.2 МБ ≈ 40–60 с.

> ⚠️ Під час OTA **ніщо інше** не має писати в UART. Master це гарантує м'ютексом;
> якщо прошивка стара — опитування версії може зірвати передачу.

## Релізи

GitHub Actions (`.github/workflows/release.yml`) збирає прошивку і публікує реліз
при пуші тега:

```bash
git add .
git commit -m "release: v1.1.0"
git push
git tag v1.1.0
git push origin v1.1.0
```

Артефакти релізу:
- `Ford-DensO-bluetooth-slave.bin` — образ для OTA з Master;
- `Ford-DensO-bluetooth-slave-full.bin` — повний образ: `esptool write_flash 0x0`;
- `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`.

Збірка: контейнер `espressif/idf:v5.5.1` + ESP-ADF (`fcce23e3`), ціль `esp32`.


## Типові проблеми

| Симптом | Причина / рішення |
|---|---|
| Немає звуку в Master | перевір I2S (BCLK 41 / WS 39 / DIN 40) і спільну землю |
| Музика не стартує сама | AVRCP не підключився, або телефон не має активної медіасесії |
| Треки не перемикаються з веб-UI | Master не в external mode, або UART не 115200 |
| OTA зависає на першому блоці | не збігається baud; див. `UART_OTA_MASTER` у логах Master |
| Slave перезавантажується сам | просідає живлення (піковий струм підсилювача) |
| Випадкові `next/pause` | різний baud на платах → Slave парсить шум як команди |

## Файли

| Файл | Призначення |
|---|---|
| `main/a2dp-slave.c` | A2DP sink, AVRCP, UART-таск, автоплей |
| `main/uart_ota_slave.c` | прийом і запис OTA-образу |
| `main/uart_ota_protocol.h` | формат пакетів (має збігатися з Master!) |
| `main/slave_version.h`, `slave_uart_version.h` | версії для телеметрії |

## Ліцензія

Код проєкту — для особистого використання. ESP-IDF та ESP-ADF — під ліцензіями Espressif.
