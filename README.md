# ESP32 Bluetooth A2DP/HFP Slave

Українська версія | [中文版本](./README_CN.md)

## 📋 Огляд проекту

**Slave ESP32** - це Bluetooth-приймач, який працює разом з Master ESP32 для створення повноцінної Bluetooth аудіо системи. Проект базується на **ESP-ADF (Espressif Audio Development Framework)** для обробки аудіо та Bluetooth стеків. Цей пристрій приймає аудіо через Bluetooth A2DP та передає його до Master ESP32 через I2S для подальшого відтворення, а також передає команди керування через UART.

## 🔌 Підключення апаратного забезпечення

```
ESP32 (Slave)                Підключення
=============                ===========
GPIO 23  ────────────────── UART TX ── Master ESP32 GPIO 9 (керування)
GPIO 22  ────────────────── UART RX ── Master ESP32 GPIO 8 (керування)
GPIO 26  ────────────────── I2S BCLK ─ Master ESP32 GPIO 41 (аудіо)
GPIO 33  ────────────────── I2S WS ─── Master ESP32 GPIO 39 (аудіо)
GPIO 25  ────────────────── I2S DOUT ─ Master ESP32 GPIO 40 (аудіо)
GND      ────────────────── GND ────── Master ESP32 GND
3.3V     ────────────────── 3.3V ───── Master ESP32 3.3V

Bluetooth: Вбудований модуль ESP32
Speaker: Підключений до аудіо кодека (опціонально)
```

### UART конфігурація (керування)
- **Швидкість**: 115200 bps
- **Дані**: 8 біт
- **Парність**: Відсутня
- **Стоп-біт**: 1
- **Потік**: Без контролю
- **Призначення**: Команди керування, OTA оновлення

### I2S конфігурація (аудіо)
- **Швидкість**: 44.1 kHz (або 48 kHz)
- **Розрядність**: 16 біт
- **Канали**: Stereo
- **Призначення**: Передача аудіо до Master ESP32

## 🎵 Підтримувані Bluetooth профілі

- **A2DP** (Advanced Audio Distribution Profile): Передача високоякісного аудіо
- **AVRCP** (Audio/Video Remote Control Profile): Керування відтворенням
- **HFP** (Hands-Free Profile): Голосові дзвінки через Bluetooth

## 🚀 Швидкий старт

### 1. Збірка та прошивка
```bash
cd d:\emulators\esp32-bluetooth
idf.py build
idf.py -p COM_SLAVE flash
```

### 2. Підключення до Master ESP32
1. Підключіть UART лінії між Slave та Master ESP32 (керування)
2. Підключіть I2S лінії між Slave та Master ESP32 (аудіо):
   - Slave GPIO 26 (BCLK) ↔ Master GPIO 41
   - Slave GPIO 33 (WS) ↔ Master GPIO 39  
   - Slave GPIO 25 (DOUT) ↔ Master GPIO 40
3. Переконайтеся, що обидва пристрої живляться від 3.3V
3. Master ESP32 створить Wi-Fi AP для керування

### 3. Bluetooth підключення
1. Увімкніть Bluetooth на вашому телефоні/комп'ютері
2. Знайдіть пристрій "ESP32-A2DP-SINK"
3. Підключіться та почніть відтворення музики

## 📋 Архітектура системи

```
[Bluetooth Device] ── A2DP ──► [Slave ESP32] ── I2S ──► [Master ESP32] ── I2S ──► [Audio Codec] ──► [Speakers]
                                    │                        │
                                    ▼                        ▼
                            [UART керування]        [USB Flash Music]
                                                     [Web Interface]
```

### Функції Slave ESP32:
- **Bluetooth прийом**: A2DP аудіо стрімінг
- **I2S передача**: Відправка аудіо до Master через I2S
- **UART керування**: Отримання команд від Master
- **OTA оновлення**: Безпровідне оновлення через Master ESP32

## ⚙️ Конфігурація

### menuconfig налаштування:
```bash
menuconfig > Audio HAL > ESP32-Lyrat-Mini V1.1 (або інша плата)
```

### Bluetooth налаштування:
- **Device Name**: ESP32-A2DP-SINK (можна змінити у коді)
- **PIN Code**: 1234 (якщо потрібно)

## 🔧 Розширена конфігурація

### Зміна Bluetooth імені:
У `main/a2dp_sink_and_hfp_example.c`:
```c
#define BLUETOOTH_DEVICE_NAME "My ESP32 Audio"
```

### Налаштування UART:
```c
#define APP_UART_TX_PIN GPIO_NUM_23
#define APP_UART_RX_PIN GPIO_NUM_22
#define APP_UART_BAUD 115200
```

## 🐛 Вирішення проблем

### Slave ESP32 не підключається по UART:
- Перевірте GPIO підключення (TX-RX, RX-TX)
- Перевірте швидкість UART (має співпадати з Master)
- Перевірте живлення (3.3V стабільне)

### I2S аудіо не передається:
- Перевірте I2S підключення (GPIO 25,26,33)
- Перевірте частоту дискретизації (має співпадати з Master)
- Перевірте рівні сигналів

### Bluetooth не працює:
- Перевірте чи увімкнено Bluetooth на джерелі
- Перевірте відстань (максимум 10м)
- Спробуйте перезавантажити Slave ESP32

### Низька якість аудіо:
- Перевірте Bluetooth версію джерела (бажано 4.0+)
- Зменшіть відстань між пристроями
- Перевірте наявність перешкод

## 📊 Системні вимоги

- **ESP32** мікроконтролер
- **Bluetooth джерело** (телефон, комп'ютер з Bluetooth 4.0+)
- **3.3V джерело живлення**
- **UART з'єднання** з Master ESP32 (GPIO 22,23)
- **I2S з'єднання** з Master ESP32 (GPIO 25,26,33)

## 🔄 Версії компонентів

- **ESP-IDF**: v5.0+
- **ESP-ADF**: Audio Development Framework
- **Bluetooth Stack**: Classic Bluetooth

## 📝 Ліцензія

Unlicense OR CC0-1.0

## 🔗 Дивіться також

- **Master ESP32**: [pipeline_play_sdcard_music](../pipeline_play_sdcard_music/)
- **Повна документація**: ESP-IDF та ESP-ADF
