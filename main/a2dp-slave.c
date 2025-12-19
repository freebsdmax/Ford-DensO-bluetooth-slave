/* Play music from Bluetooth device

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <inttypes.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_peripherals.h"
#include "periph_touch.h"
#include "periph_adc_button.h"
#include "periph_button.h"
#include "esp_bt_defs.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_a2dp_api.h"
#include "driver/uart.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"


#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"

#include "i2s_stream.h"
#include "board.h"
#include "bluetooth_service.h"
#include "filter_resample.h"
#include "raw_stream.h"

#if (CONFIG_ESP_LYRATD_MSC_V2_1_BOARD || CONFIG_ESP_LYRATD_MSC_V2_2_BOARD)
#include "filter_resample.h"
#endif
#include "bt_keycontrol.h"
#include "slave_version.h"
#include "slave_uart_version.h"
#include "slave_uart_version.h"

#include "audio_idf_version.h"
#include "uart_ota_protocol.h"
#include "uart_ota_slave.h"

extern void uart_ota_slave_save_version(void);
extern void uart_ota_crc_save_version(void);

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0))
#define HFP_RESAMPLE_RATE 16000
#else
#define HFP_RESAMPLE_RATE 8000
#endif

static const char *TAG = "BLUETOOTH_EXAMPLE";

/**
 * Зберегти версію A2DP Sink в NVS
 */
void a2dp_sink_save_version(void)
{
    static bool version_saved = false;
    if (version_saved) return;
    
    // Ініціалізувати NVS якщо потрібно
    static bool nvs_initialized = false;
    if (!nvs_initialized) {
        ESP_LOGI(TAG, "A2DP Sink: Initializing NVS");
        esp_err_t err_init = nvs_flash_init();
        if (err_init == ESP_ERR_NVS_NO_FREE_PAGES || err_init == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGI(TAG, "A2DP Sink: Erasing NVS");
            err_init = nvs_flash_erase();
            if (err_init != ESP_OK) {
                ESP_LOGW(TAG, "A2DP Sink: NVS erase failed: %s", esp_err_to_name(err_init));
            }
            err_init = nvs_flash_init();
        }
        if (err_init != ESP_OK) {
            ESP_LOGE(TAG, "A2DP Sink: Failed to init NVS: %s", esp_err_to_name(err_init));
            return;
        }
        nvs_initialized = true;
        ESP_LOGI(TAG, "A2DP Sink: NVS initialized");
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("a2dp_sink", NVS_READWRITE, &nvs_handle);
    ESP_LOGI(TAG, "A2DP Sink: Opening a2dp_sink NVS");
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "A2DP Sink: Setting date %s", __DATE__);
        nvs_set_str(nvs_handle, "date", __DATE__);
        ESP_LOGI(TAG, "A2DP Sink: Setting time %s", __TIME__);
        nvs_set_str(nvs_handle, "time", __TIME__);
        ESP_LOGI(TAG, "A2DP Sink: Committing");
        nvs_commit(nvs_handle);
        
        // Перевірити збережене значення
        ESP_LOGI(TAG, "A2DP Sink: Reading back saved values");
        char saved_date[12] = "";
        char saved_time[9] = "";
        size_t len = sizeof(saved_date);
        esp_err_t read_err = nvs_get_str(nvs_handle, "date", saved_date, &len);
        if (read_err == ESP_OK) {
            len = sizeof(saved_time);
            read_err = nvs_get_str(nvs_handle, "time", saved_time, &len);
            if (read_err == ESP_OK) {
                ESP_LOGI(TAG, "A2DP Sink NVS READ: DATE: %s TIME: %s", saved_date, saved_time);
            } else {
                ESP_LOGE(TAG, "A2DP Sink: Failed to read time from NVS: %s", esp_err_to_name(read_err));
            }
        } else {
            ESP_LOGE(TAG, "A2DP Sink: Failed to read date from NVS: %s", esp_err_to_name(read_err));
        }
        
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "💾 A2DP Sink version saved to NVS");
        ESP_LOGI(TAG, "A2DP Sink DATE: %s TIME: %s", __DATE__, __TIME__);
        
        version_saved = true;
    } else {
        ESP_LOGE(TAG, "❌ Failed to open a2dp_sink NVS: %s", esp_err_to_name(err));
    }
}

static const char *BT_HF_TAG = "BT_HF";

static audio_element_handle_t  raw_read, bt_stream_reader, i2s_stream_writer, i2s_stream_reader;
static audio_pipeline_handle_t pipeline_d, pipeline_e;
static bool is_get_hfp = true;

// UART configuration for commands
#define APP_UART_PORT       UART_NUM_1
#define APP_UART_TX_PIN     GPIO_NUM_23
#define APP_UART_RX_PIN     GPIO_NUM_22
#define APP_UART_BAUD       115200
#define UART_RX_BUF_LEN     256
#define UART_TX_BUF_LEN     256

// Sleep control configuration
#define SLEEP_WAKEUP_PIN    GPIO_NUM_34

// Forward declarations
static void enter_deep_sleep(gpio_num_t wakeup_pin);

// Commands from master
#define CMD_NEXT_TRACK  0x04
#define CMD_PREV_TRACK  0x05

// Auto-reconnect configuration
#define NVS_NAMESPACE "bt_storage"
#define NVS_KEY_BT_ADDR "last_bt_addr"
#define NVS_KEY_SLAVE_MAIN_DATE "slave_main_date"
#define NVS_KEY_SLAVE_MAIN_TIME "slave_main_time"
#define NVS_KEY_SLAVE_BT_DATE "slave_bt_date"
#define NVS_KEY_SLAVE_BT_TIME "slave_bt_time"
#define NVS_KEY_SLAVE_UART_DATE "slave_uart_date"
#define NVS_KEY_SLAVE_UART_TIME "slave_uart_time"
#define RECONNECT_TIMEOUT_MS 15000  // 15 seconds timeout for reconnection
#define DISCOVERABLE_TIMEOUT_MS 60000  // 60 seconds discoverable after failed reconnect

static bool auto_reconnect_enabled = true;
static bool is_connected = false;
static uint8_t last_connected_addr[6] = {0};
static bool has_saved_device = false;
//static QueueHandle_t i2c_cmd_queue = NULL;
static bool reconnect_task_running = false;
static esp_periph_handle_t bt_periph = NULL;

// Logging stream for I2C monitoring
#define LOG_STREAM_SIZE 2048
#define LOG_MAX_CHUNK   128
static StreamBufferHandle_t s_log_stream = NULL;
static vprintf_like_t s_prev_logger = NULL;

static int log_uart_vprintf(const char *fmt, va_list args)
{
    int ret;
    va_list a1, a2;
    va_copy(a1, args);
    va_copy(a2, args);

    if (s_prev_logger) {
        ret = s_prev_logger(fmt, a1);
    } else {
        ret = vprintf(fmt, a1);
    }

    char buf[LOG_MAX_CHUNK];
    int n = vsnprintf(buf, sizeof(buf), fmt, a2);
    if (n > 0 && s_log_stream) {
        size_t to_send = (n < (int)sizeof(buf)) ? (size_t)n : (size_t)sizeof(buf);
        (void)xStreamBufferSend(s_log_stream, (const uint8_t *)buf, to_send, 0);
    }

    va_end(a1);
    va_end(a2);
    return ret;
}

// UART initialization for commands
static esp_err_t uart_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = APP_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)
        .source_clk = UART_SCLK_DEFAULT,
#endif
    };

    esp_err_t ret = uart_param_config(APP_UART_PORT, &uart_config);
    if (ret != ESP_OK) return ret;
    ret = uart_set_pin(APP_UART_PORT, APP_UART_TX_PIN, APP_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;
    ret = uart_driver_install(APP_UART_PORT, UART_RX_BUF_LEN, UART_TX_BUF_LEN, 0, NULL, 0);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "UART initialized (port %d, %d bps)", APP_UART_PORT, APP_UART_BAUD);
    }
    return ret;
}

// Буфер для OTA пакетів (максимальний розмір)
#define OTA_PACKET_BUF_SIZE 512
static uint8_t ota_packet_buffer[OTA_PACKET_BUF_SIZE];

// UART command handler task з підтримкою OTA
static void uart_cmd_task(void *arg)
{
    uint8_t prefix;
    
    ESP_LOGI(TAG, "✅ UART command task started");
    
    // Ініціалізація NVS для збереження версій
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to init NVS: %s", esp_err_to_name(nvs_err));
    } else {
        // Зберігаємо версії slave в NVS
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            nvs_set_str(nvs_handle, NVS_KEY_SLAVE_MAIN_DATE, SLAVE_BUILD_DATE);
            nvs_set_str(nvs_handle, NVS_KEY_SLAVE_MAIN_TIME, SLAVE_BUILD_TIME);
            nvs_set_str(nvs_handle, NVS_KEY_SLAVE_BT_DATE, SLAVE_BUILD_DATE);  // для BT використовуємо ті ж
            nvs_set_str(nvs_handle, NVS_KEY_SLAVE_BT_TIME, SLAVE_BUILD_TIME);
            nvs_set_str(nvs_handle, NVS_KEY_SLAVE_UART_DATE, SLAVE_UART_BUILD_DATE);
            nvs_set_str(nvs_handle, NVS_KEY_SLAVE_UART_TIME, SLAVE_UART_BUILD_TIME);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "💾 Slave versions saved to NVS");
        } else {
            ESP_LOGE(TAG, "❌ Failed to save slave versions to NVS: %s", esp_err_to_name(err));
        }
    }
    
    while (1) {
        // Читаємо перший байт (prefix або команду)
        int len = uart_read_bytes(APP_UART_PORT, &prefix, 1, pdMS_TO_TICKS(100));
        
        if (len != 1) {
            continue;
        }
        
        ESP_LOGD(TAG, "📥 Received prefix/cmd: 0x%02X", prefix);
        
        // Розпізнаємо тип команди по префіксу
        if (prefix == UART_CMD_PREFIX_OTA) {
            //ESP_LOGI(TAG, "🔄 OTA prefix detected: 0x%02X", prefix);
            // НЕ очищуємо буфер - команда може вже бути в буфері!
            // uart_flush(APP_UART_PORT);
            
            // Читаємо команду OTA
            uint8_t ota_cmd;
            len = uart_read_bytes(APP_UART_PORT, &ota_cmd, 1, pdMS_TO_TICKS(500));
            if (len != 1) {
                ESP_LOGW(TAG, "⚠️ Failed to read OTA command");
                continue;
            }
            
            //ESP_LOGI(TAG, "🔄 OTA command received: 0x%02X", ota_cmd);
            
            // Складаємо пакет в буфері
            ota_packet_buffer[0] = prefix;
            ota_packet_buffer[1] = ota_cmd;
            size_t packet_len = 2;
            
            // Читаємо решту пакету в залежності від команди
            size_t expected_size = 0;
            
            switch (ota_cmd) {
                case OTA_CMD_START:
                    ESP_LOGI(TAG, "📥 OTA_CMD_START received - processing...");
                    expected_size = sizeof(ota_start_packet_t) - 2;  // Мінус prefix і cmd
                    break;
                    
                case OTA_CMD_DATA_BLOCK: {
                    // Читаємо block_num (2 bytes) та data_len (1 byte)
                    uint8_t header[3];
                    len = uart_read_bytes(APP_UART_PORT, header, 3, pdMS_TO_TICKS(500));
                    if (len != 3) {
                        ESP_LOGW(TAG, "Failed to read data block header: %d bytes", len);
                        continue;
                    }
                    memcpy(&ota_packet_buffer[2], header, 3);
                    packet_len += 3;
                    uint8_t data_len = header[2];  // data_len is 3rd byte
                    expected_size = data_len + 2;  // data + crc16
                    break;
                }
                    
                case OTA_CMD_END:
                    expected_size = sizeof(ota_end_packet_t) - 2;
                    break;
                    
                case OTA_CMD_ABORT:
                    expected_size = 0;  // Немає додаткових даних
                    break;
                    
                default:
                    ESP_LOGW(TAG, "Unknown OTA command: 0x%02X", ota_cmd);
                    continue;
            }
            
            // Читаємо решту даних якщо потрібно
            if (expected_size > 0) {
                if (packet_len + expected_size > OTA_PACKET_BUF_SIZE) {
                    ESP_LOGE(TAG, "OTA packet too large: %d", (int)(packet_len + expected_size));
                    continue;
                }
                
                len = uart_read_bytes(APP_UART_PORT, &ota_packet_buffer[packet_len], 
                                     expected_size, pdMS_TO_TICKS(2000));
                if (len != expected_size) {
                    ESP_LOGW(TAG, "Failed to read OTA data: %d/%d", len, (int)expected_size);
                    continue;
                }
                packet_len += expected_size;
            }
            
            // Обробити OTA команду
            uart_ota_process_command(ota_packet_buffer, packet_len);
            
        } else if (prefix == UART_CMD_PREFIX_NORMAL) {
            // ====== ЗВИЧАЙНА КОМАНДА З ПРЕФІКСОМ ======
            uint8_t cmd;
            len = uart_read_bytes(APP_UART_PORT, &cmd, 1, pdMS_TO_TICKS(100));
            if (len != 1) {
                continue;
            }
            
            switch (cmd) {
                case UART_CMD_NEXT_TRACK:
                    ESP_LOGI(TAG, "⏭️ CMD: Next track ");
                    if (bt_periph) {
                        periph_bluetooth_next(bt_periph);
                    }
                    break;
                    
                case UART_CMD_PREV_TRACK:
                    ESP_LOGI(TAG, "⏮️ CMD: Previous track ");
                    if (bt_periph) {
                        periph_bluetooth_prev(bt_periph);
                    }
                    break;
                    
                case UART_CMD_GET_VERSION:
                    ESP_LOGI(TAG, "🔨 CMD: Get version");
                    {
                        version_response_t resp = {
                            .prefix = UART_CMD_PREFIX_NORMAL,
                            .cmd = UART_CMD_GET_VERSION
                        };
                        
                        // Читаємо версії з NVS окремих просторів
                        // Основний додаток (з a2dp_sink)
                        nvs_handle_t nvs_handle_main;
                        esp_err_t nvs_err_main = nvs_open("a2dp_sink", NVS_READONLY, &nvs_handle_main);
                        if (nvs_err_main == ESP_OK) {
                            size_t len;
                            len = sizeof(resp.main_build_date);
                            nvs_get_str(nvs_handle_main, "date", resp.main_build_date, &len);
                            len = sizeof(resp.main_build_time);
                            nvs_get_str(nvs_handle_main, "time", resp.main_build_time, &len);
                            nvs_close(nvs_handle_main);
                        } else {
                            ESP_LOGW(TAG, "NVS a2dp_sink недоступний для main");
                        }
                        
                        // CRC (з uart_ota_crc)
                        nvs_handle_t nvs_handle_crc;
                        esp_err_t nvs_err_crc = nvs_open("uart_ota_crc", NVS_READONLY, &nvs_handle_crc);
                        if (nvs_err_crc == ESP_OK) {
                            size_t len;
                            len = sizeof(resp.crc_build_date);
                            nvs_get_str(nvs_handle_crc, "date", resp.crc_build_date, &len);
                            len = sizeof(resp.crc_build_time);
                            nvs_get_str(nvs_handle_crc, "time", resp.crc_build_time, &len);
                            nvs_close(nvs_handle_crc);
                        } else {
                            ESP_LOGW(TAG, "NVS uart_ota_crc недоступний для crc");
                        }
                        
                        // UART OTA компонент
                        nvs_handle_t nvs_handle_uart;
                        esp_err_t nvs_err_uart = nvs_open("uart_ota_slave", NVS_READONLY, &nvs_handle_uart);
                        if (nvs_err_uart == ESP_OK) {
                            size_t len;
                            len = sizeof(resp.uart_build_date);
                            nvs_get_str(nvs_handle_uart, "date", resp.uart_build_date, &len);
                            len = sizeof(resp.uart_build_time);
                            nvs_get_str(nvs_handle_uart, "time", resp.uart_build_time, &len);
                            nvs_close(nvs_handle_uart);
                        } else {
                            ESP_LOGW(TAG, "NVS uart_ota_slave недоступний для uart");
                        }
                        
                        // Fallback до compile-time версій якщо NVS недоступний
                        if (nvs_err_main != ESP_OK) {
                            strncpy(resp.main_build_date, SLAVE_BUILD_DATE, sizeof(resp.main_build_date) - 1);
                            strncpy(resp.main_build_time, SLAVE_BUILD_TIME, sizeof(resp.main_build_time) - 1);
                        }
                        if (nvs_err_crc != ESP_OK) {
                            strncpy(resp.crc_build_date, SLAVE_BUILD_DATE, sizeof(resp.crc_build_date) - 1);
                            strncpy(resp.crc_build_time, SLAVE_BUILD_TIME, sizeof(resp.crc_build_time) - 1);
                        }
                        
                        ESP_LOGI(TAG, "📤 Відправка версій slave:");
                        ESP_LOGI(TAG, "  🎵 Main: %s %s", resp.main_build_date, resp.main_build_time);
                        ESP_LOGI(TAG, "  📡 CRC: %s %s", resp.crc_build_date, resp.crc_build_time);
                        ESP_LOGI(TAG, "  📡 UART: %s %s", resp.uart_build_date, resp.uart_build_time);
                        
                        int written = uart_write_bytes(APP_UART_PORT, (const char *)&resp, sizeof(resp));
                        ESP_LOGI(TAG, "📤 Написано байт: %d з %d", written, sizeof(resp));
                        
                        esp_err_t wait_result = uart_wait_tx_done(APP_UART_PORT, pdMS_TO_TICKS(100));
                        ESP_LOGI(TAG, "📤 Очікування передачі: %s", esp_err_to_name(wait_result));
                    }
                    break;
                    
                default:
                    ESP_LOGW(TAG, "Unknown normal command: 0x%02X", cmd);
                    break;
            }
            
        } else if (prefix == CMD_NEXT_TRACK || prefix == CMD_PREV_TRACK) {
            // ====== СТАРА КОМАНДА БЕЗ ПРЕФІКСУ (для сумісності) ======
            switch (prefix) {
                case CMD_NEXT_TRACK:
                    ESP_LOGI(TAG, "⏭️ CMD: Next track (legacy)");
                    if (bt_periph) {
                        periph_bluetooth_next(bt_periph);
                    }
                    break;
                    
                case CMD_PREV_TRACK:
                    ESP_LOGI(TAG, "⏮️ CMD: Previous track (legacy)");
                    if (bt_periph) {
                        periph_bluetooth_prev(bt_periph);
                    }
                    break;
            }
        }
        // Інші байти ігноруємо
    }
}



static void i2c_sleep_on_pin(gpio_num_t pin)
{
    // Configure GPIO for deep sleep wakeup
    rtc_gpio_init(pin);
    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(pin);
    rtc_gpio_pulldown_dis(pin);
    esp_sleep_enable_ext0_wakeup(pin, 0); // Wake on LOW
    ESP_LOGI(TAG, "Sleep wakeup configured on GPIO%d", pin);
}

// Forward declarations
static esp_err_t save_last_bt_device(uint8_t *addr);
static esp_err_t load_last_bt_device(uint8_t *addr);

const char *c_hf_evt_str[] = {
    "CONNECTION_STATE_EVT",              /*!< connection state changed event */
    "AUDIO_STATE_EVT",                   /*!< audio connection state change event */
    "VR_STATE_CHANGE_EVT",                /*!< voice recognition state changed */
    "CALL_IND_EVT",                      /*!< call indication event */
    "CALL_SETUP_IND_EVT",                /*!< call setup indication event */
    "CALL_HELD_IND_EVT",                 /*!< call held indicator event */
    "NETWORK_STATE_EVT",                 /*!< network state change event */
    "SIGNAL_STRENGTH_IND_EVT",           /*!< signal strength indication event */
    "ROAMING_STATUS_IND_EVT",            /*!< roaming status indication event */
    "BATTERY_LEVEL_IND_EVT",             /*!< battery level indication event */
    "CURRENT_OPERATOR_EVT",              /*!< current operator name event */
    "RESP_AND_HOLD_EVT",                 /*!< response and hold event */
    "CLIP_EVT",                          /*!< Calling Line Identification notification event */
    "CALL_WAITING_EVT",                  /*!< call waiting notification */
    "CLCC_EVT",                          /*!< listing current calls event */
    "VOLUME_CONTROL_EVT",                /*!< audio volume control event */
    "AT_RESPONSE",                       /*!< audio volume control event */
    "SUBSCRIBER_INFO_EVT",               /*!< subscriber information event */
    "INBAND_RING_TONE_EVT",              /*!< in-band ring tone settings */
    "LAST_VOICE_TAG_NUMBER_EVT",         /*!< requested number from AG event */
    "RING_IND_EVT",                      /*!< ring indication event */
};

// esp_hf_client_connection_state_t
const char *c_connection_state_str[] = {
    "disconnected",
    "connecting",
    "connected",
    "slc_connected",
    "disconnecting",
};

// esp_hf_client_audio_state_t
const char *c_audio_state_str[] = {
    "disconnected",
    "connecting",
    "connected",
    "connected_msbc",
};

/// esp_hf_vr_state_t
const char *c_vr_state_str[] = {
    "disabled",
    "enabled",
};

// esp_hf_service_availability_status_t
const char *c_service_availability_status_str[] = {
    "unavailable",
    "available",
};

// esp_hf_roaming_status_t
const char *c_roaming_status_str[] = {
    "inactive",
    "active",
};

// esp_hf_client_call_state_t
const char *c_call_str[] = {
    "NO call in progress",
    "call in progress",
};

// esp_hf_client_callsetup_t
const char *c_call_setup_str[] = {
    "NONE",
    "INCOMING",
    "OUTGOING_DIALING",
    "OUTGOING_ALERTING"
};

// esp_hf_client_callheld_t
const char *c_call_held_str[] = {
    "NONE held",
    "Held and Active",
    "Held",
};

// esp_hf_response_and_hold_status_t
const char *c_resp_and_hold_str[] = {
    "HELD",
    "HELD ACCEPTED",
    "HELD REJECTED",
};

// esp_hf_client_call_direction_t
const char *c_call_dir_str[] = {
    "outgoing",
    "incoming",
};

// esp_hf_client_call_state_t
const char *c_call_state_str[] = {
    "active",
    "held",
    "dialing",
    "alerting",
    "incoming",
    "waiting",
    "held_by_resp_hold",
};

// esp_hf_current_call_mpty_type_t
const char *c_call_mpty_type_str[] = {
    "single",
    "multi",
};

// esp_hf_volume_control_target_t
const char *c_volume_control_target_str[] = {
    "SPEAKER",
    "MICROPHONE"
};

// esp_hf_at_response_code_t
const char *c_at_response_code_str[] = {
    "OK",
    "ERROR"
    "ERR_NO_CARRIER",
    "ERR_BUSY",
    "ERR_NO_ANSWER",
    "ERR_DELAYED",
    "ERR_BLACKLILSTED",
    "ERR_CME",
};

// esp_hf_subscriber_service_type_t
const char *c_subscriber_service_type_str[] = {
    "unknown",
    "voice",
    "fax",
};

// esp_hf_client_in_band_ring_state_t
const char *c_inband_ring_state_str[] = {
    "NOT provided",
    "Provided",
};

static void bt_app_hf_client_audio_open(void)
{
    ESP_LOGE(BT_HF_TAG, "bt_app_hf_client_audio_open");
    int sample_rate = HFP_RESAMPLE_RATE;
    audio_element_info_t bt_info = {0};
    audio_element_getinfo(bt_stream_reader, &bt_info);
    bt_info.sample_rates = sample_rate;
    bt_info.channels = 1;
    bt_info.bits = 16;
    audio_element_setinfo(bt_stream_reader, &bt_info);
    audio_element_report_info(bt_stream_reader);
}

static void bt_app_hf_client_audio_close(void)
{
    ESP_LOGE(BT_HF_TAG, "bt_app_hf_client_audio_close");
    int sample_rate = periph_bluetooth_get_a2dp_sample_rate();
    audio_element_info_t bt_info = {0};
    audio_element_getinfo(bt_stream_reader, &bt_info);
    bt_info.sample_rates = sample_rate;
    bt_info.channels = 2;
    bt_info.bits = 16;
    audio_element_setinfo(bt_stream_reader, &bt_info);
    audio_element_report_info(bt_stream_reader);
}

static uint32_t bt_app_hf_client_outgoing_cb(uint8_t *p_buf, uint32_t sz)
{
    int out_len_bytes = 0;
    char *enc_buffer = (char *)audio_malloc(sz);
    AUDIO_MEM_CHECK(BT_HF_TAG, enc_buffer, return 0);
    if (is_get_hfp) {
        out_len_bytes = raw_stream_read(raw_read, enc_buffer, sz);
    }

    if (out_len_bytes == sz) {
        is_get_hfp = false;
        memcpy(p_buf, enc_buffer, out_len_bytes);
        free(enc_buffer);
        return sz;
    } else {
        is_get_hfp = true;
        free(enc_buffer);
        return 0;
    }
}

static void bt_app_hf_client_incoming_cb(const uint8_t *buf, uint32_t sz)
{
    if (bt_stream_reader) {
        if (audio_element_get_state(bt_stream_reader) == AEL_STATE_RUNNING) {
            audio_element_output(bt_stream_reader, (char *)buf, sz);
            esp_hf_client_outgoing_data_ready();
        }
    }
}
/* callback for GAP */
static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    ESP_LOGI(TAG, "GAP callback event: %d", event);
    
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Authentication success: %s", param->auth_cmpl.device_name);
            ESP_LOGI(TAG, "Device address: %02x:%02x:%02x:%02x:%02x:%02x",
                     param->auth_cmpl.bda[0], param->auth_cmpl.bda[1],
                     param->auth_cmpl.bda[2], param->auth_cmpl.bda[3],
                     param->auth_cmpl.bda[4], param->auth_cmpl.bda[5]);
            
            // Save connected device address
            memcpy(last_connected_addr, param->auth_cmpl.bda, 6);
            save_last_bt_device(last_connected_addr);
            has_saved_device = true;
        } else {
            ESP_LOGE(TAG, "Authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %"PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%"PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_MODE_CHG_EVT mode:%d", param->mode_chg.mode);
        // Save address when mode changes (device connected)
        if (param->mode_chg.mode == ESP_BT_PM_MD_ACTIVE) {
            ESP_LOGI(TAG, "Device became active: %02x:%02x:%02x:%02x:%02x:%02x",
                     param->mode_chg.bda[0], param->mode_chg.bda[1],
                     param->mode_chg.bda[2], param->mode_chg.bda[3],
                     param->mode_chg.bda[4], param->mode_chg.bda[5]);
            memcpy(last_connected_addr, param->mode_chg.bda, 6);
            save_last_bt_device(last_connected_addr);
            has_saved_device = true;
        }
        break;
    default:
        ESP_LOGI(TAG, "GAP event: %d", event);
        break;
    }
}

/* callback for HF_CLIENT */
void bt_hf_client_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    if (event <= ESP_HF_CLIENT_RING_IND_EVT) {
        ESP_LOGE(BT_HF_TAG, "APP HFP event: %s", c_hf_evt_str[event]);
    } else {
        ESP_LOGE(BT_HF_TAG, "APP HFP invalid event %d", event);
    }

    switch (event) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        ESP_LOGE(BT_HF_TAG, "--connection state %s, peer feats 0x%" PRIx32 ", chld_feats 0x%" PRIx32,
                 c_connection_state_str[param->conn_stat.state],
                 param->conn_stat.peer_feat,
                 param->conn_stat.chld_feat);
        
        // Track connection state for auto-reconnect
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED ||
            param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            is_connected = true;
            ESP_LOGI(BT_HF_TAG, "Device connected via HFP");
        } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            is_connected = false;
            ESP_LOGI(BT_HF_TAG, "Device disconnected");
        }
        break;
    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        ESP_LOGE(BT_HF_TAG, "--audio state %s",
                 c_audio_state_str[param->audio_stat.state]);
#if CONFIG_HFP_AUDIO_DATA_PATH_HCI
        if ((param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED)
            || (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC)) {
            bt_app_hf_client_audio_open();
            esp_hf_client_register_data_callback(bt_app_hf_client_incoming_cb,
                                                 bt_app_hf_client_outgoing_cb);
        } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            bt_app_hf_client_audio_close();
        }
#endif /* #if CONFIG_HFP_AUDIO_DATA_PATH_HCI */
        break;
    case ESP_HF_CLIENT_BVRA_EVT:
        ESP_LOGE(BT_HF_TAG, "--VR state %s",
                 c_vr_state_str[param->bvra.value]);
        break;
    case ESP_HF_CLIENT_CIND_SERVICE_AVAILABILITY_EVT:
        ESP_LOGE(BT_HF_TAG, "--NETWORK STATE %s",
                 c_service_availability_status_str[param->service_availability.status]);
        break;
    case ESP_HF_CLIENT_CIND_ROAMING_STATUS_EVT:
        ESP_LOGE(BT_HF_TAG, "--ROAMING: %s",
                 c_roaming_status_str[param->roaming.status]);
        break;
    case ESP_HF_CLIENT_CIND_SIGNAL_STRENGTH_EVT:
        ESP_LOGE(BT_HF_TAG, "-- signal strength: %d",
                 param->signal_strength.value);
        break;
    case ESP_HF_CLIENT_CIND_BATTERY_LEVEL_EVT:
        ESP_LOGE(BT_HF_TAG, "--battery level %d",
                 param->battery_level.value);
        break;
    case ESP_HF_CLIENT_COPS_CURRENT_OPERATOR_EVT:
        ESP_LOGE(BT_HF_TAG, "--operator name: %s",
                 param->cops.name);
        break;
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        ESP_LOGE(BT_HF_TAG, "--Call indicator %s",
                 c_call_str[param->call.status]);
        break;
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        ESP_LOGE(BT_HF_TAG, "--Call setup indicator %s",
                 c_call_setup_str[param->call_setup.status]);
        break;
    case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
        ESP_LOGE(BT_HF_TAG, "--Call held indicator %s",
                 c_call_held_str[param->call_held.status]);
        break;
    case ESP_HF_CLIENT_BTRH_EVT:
        ESP_LOGE(BT_HF_TAG, "--response and hold %s",
                 c_resp_and_hold_str[param->btrh.status]);
        break;
    case ESP_HF_CLIENT_CLIP_EVT:
        ESP_LOGE(BT_HF_TAG, "--clip number %s",
                 (param->clip.number == NULL) ? "NULL" : (param->clip.number));
        break;
    case ESP_HF_CLIENT_CCWA_EVT:
        ESP_LOGE(BT_HF_TAG, "--call_waiting %s",
                 (param->ccwa.number == NULL) ? "NULL" : (param->ccwa.number));
        break;
    case ESP_HF_CLIENT_CLCC_EVT:
        ESP_LOGE(BT_HF_TAG, "--Current call: idx %d, dir %s, state %s, mpty %s, number %s",
                 param->clcc.idx,
                 c_call_dir_str[param->clcc.dir],
                 c_call_state_str[param->clcc.status],
                 c_call_mpty_type_str[param->clcc.mpty],
                 (param->clcc.number == NULL) ? "NULL" : (param->clcc.number));
        break;
    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
        ESP_LOGE(BT_HF_TAG, "--volume_target: %s, volume %d",
                 c_volume_control_target_str[param->volume_control.type],
                 param->volume_control.volume);
        break;
    case ESP_HF_CLIENT_AT_RESPONSE_EVT:
        ESP_LOGE(BT_HF_TAG, "--AT response event, code %d, cme %d",
                 param->at_response.code, param->at_response.cme);
        break;
    case ESP_HF_CLIENT_CNUM_EVT:
        ESP_LOGE(BT_HF_TAG, "--subscriber type %s, number %s",
                 c_subscriber_service_type_str[param->cnum.type],
                 (param->cnum.number == NULL) ? "NULL" : param->cnum.number);
        break;
    case ESP_HF_CLIENT_BSIR_EVT:
        ESP_LOGE(BT_HF_TAG, "--inband ring state %s",
                 c_inband_ring_state_str[param->bsir.state]);
        break;
    case ESP_HF_CLIENT_BINP_EVT:
        ESP_LOGE(BT_HF_TAG, "--last voice tag number: %s",
                 (param->binp.number == NULL) ? "NULL" : param->binp.number);
        break;
    default:
        ESP_LOGE(BT_HF_TAG, "HF_CLIENT EVT: %d", event);
        break;
    }
}

// Auto-reconnect functions
static esp_err_t save_last_bt_device(uint8_t *addr)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_set_blob(nvs_handle, NVS_KEY_BT_ADDR, addr, 6);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save BT address: %s", esp_err_to_name(err));
    } else {
        err = nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Saved BT device: %02x:%02x:%02x:%02x:%02x:%02x",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    }
    
    nvs_close(nvs_handle);
    return err;
}

static esp_err_t load_last_bt_device(uint8_t *addr)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved BT device found");
        return err;
    }
    
    size_t required_size = 6;
    err = nvs_get_blob(nvs_handle, NVS_KEY_BT_ADDR, addr, &required_size);
    if (err == ESP_OK && required_size == 6) {
        ESP_LOGI(TAG, "Loaded last BT device: %02x:%02x:%02x:%02x:%02x:%02x",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        has_saved_device = true;
    } else {
        ESP_LOGW(TAG, "Failed to load BT address: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    return err;
}

static void bt_reconnect_task(void *arg)
{
    ESP_LOGI(TAG, "Starting reconnection task...");
    reconnect_task_running = true;
    
    if (!has_saved_device) {
        ESP_LOGI(TAG, "No saved device, enabling discoverable mode");
        if (bt_periph) {
            periph_bluetooth_discover(bt_periph);
        }
        reconnect_task_running = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Wait a bit for BT stack to be ready
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "Attempting to reconnect to last device: %02x:%02x:%02x:%02x:%02x:%02x",
             last_connected_addr[0], last_connected_addr[1], last_connected_addr[2],
             last_connected_addr[3], last_connected_addr[4], last_connected_addr[5]);
    
    // Initiate connection to saved device
    esp_err_t err = esp_a2d_sink_connect(last_connected_addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initiate A2DP connection: %s", esp_err_to_name(err));
        // Fall back to discoverable mode
        if (bt_periph) {
            ESP_LOGI(TAG, "Enabling discoverable mode for new connections");
            periph_bluetooth_discover(bt_periph);
        }
        reconnect_task_running = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Wait to see if connection succeeds
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(RECONNECT_TIMEOUT_MS);
    
    while ((xTaskGetTickCount() - start_time) < timeout_ticks) {
        if (is_connected) {
            ESP_LOGI(TAG, "Successfully reconnected!");
            reconnect_task_running = false;
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Reconnection failed, enable discoverable mode
    ESP_LOGW(TAG, "Reconnection timeout. Enabling discoverable mode for new devices");
    if (bt_periph) {
        periph_bluetooth_discover(bt_periph);
    }
    
    reconnect_task_running = false;
    vTaskDelete(NULL);
}

// Sleep control functions
static void enter_deep_sleep(gpio_num_t wakeup_pin)
{
    ESP_LOGI(TAG, "Entering deep sleep, wakeup on GPIO%d HIGH", wakeup_pin);
    
    // Configure wakeup source
    esp_sleep_enable_ext0_wakeup(wakeup_pin, 1); // Wake on HIGH
    
    // Enter deep sleep
    esp_deep_sleep_start();
}

static void i2c_sleep_monitor_task(void *arg)
{
    gpio_num_t pin = (gpio_num_t)(int)arg;
    
    ESP_LOGI(TAG, "Sleep monitor task started, monitoring GPIO%d", pin);
    
    while (1) {
        int level = gpio_get_level(pin);
        
        if (level == 0) {
            // Pin is LOW, enter deep sleep
            ESP_LOGI(TAG, "GPIO%d is LOW, entering deep sleep", pin);
            vTaskDelay(100 / portTICK_PERIOD_MS); // Small delay for log output
            enter_deep_sleep(pin);
        }
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    // Зберегти версію A2DP Sink
    a2dp_sink_save_version();
    // Зберегти версію UART OTA Slave
    uart_ota_slave_save_version();
    // Зберегти версію UART OTA CRC
    uart_ota_crc_save_version();
    
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    esp_log_level_set("UART_OTA_SLAVE", ESP_LOG_INFO);

    ESP_LOGI(TAG, "🔨 Build: %s %s", __DATE__, __TIME__);

    // 💾 Зберігаємо версії slave при першому запуску або при зміні
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        // Перевіряємо поточну збережену версію
        char saved_date[12] = "";
        char saved_time[9] = "";
        size_t len = sizeof(saved_date);
        nvs_get_str(nvs_handle, NVS_KEY_SLAVE_MAIN_DATE, saved_date, &len);
        len = sizeof(saved_time);
        nvs_get_str(nvs_handle, NVS_KEY_SLAVE_MAIN_TIME, saved_time, &len);
        
        // Оновлюємо тільки якщо версія змінилась
        bool version_changed = (strcmp(saved_date, __DATE__) != 0) || (strcmp(saved_time, __TIME__) != 0);
        
        if (version_changed || strlen(saved_date) == 0) {
            // Зберігаємо поточну версію збірки main додатку
            err = nvs_set_str(nvs_handle, NVS_KEY_SLAVE_MAIN_DATE, __DATE__);
            if (err == ESP_OK) {
                err = nvs_set_str(nvs_handle, NVS_KEY_SLAVE_MAIN_TIME, __TIME__);
            }
            // Зберігаємо поточну версію збірки BT компонента
            if (err == ESP_OK) {
                err = nvs_set_str(nvs_handle, NVS_KEY_SLAVE_BT_DATE, __DATE__);
            }
            if (err == ESP_OK) {
                err = nvs_set_str(nvs_handle, NVS_KEY_SLAVE_BT_TIME, __TIME__);
            }
            // Зберігаємо поточну версію збірки UART компонента
            if (err == ESP_OK) {
                err = nvs_set_str(nvs_handle, NVS_KEY_SLAVE_UART_DATE, __DATE__);
            }
            if (err == ESP_OK) {
                err = nvs_set_str(nvs_handle, NVS_KEY_SLAVE_UART_TIME, __TIME__);
            }
            if (err == ESP_OK) {
                nvs_commit(nvs_handle);
                ESP_LOGI(TAG, "💾 Оновлено версії slave: Main/BT/UART %s %s", __DATE__, __TIME__);
            }
        } else {
            ESP_LOGI(TAG, "📋 Версії slave не змінились: Main %s %s", saved_date, saved_time);
        }
        nvs_close(nvs_handle);
    }

    // Initialize I2C log stream and hook ESP_LOG to mirror logs into it
    s_log_stream = xStreamBufferCreate(LOG_STREAM_SIZE, 1);
    if (s_log_stream) {
        s_prev_logger = esp_log_set_vprintf(log_uart_vprintf);
        ESP_LOGI(TAG, "UART log stream initialized (%d bytes)", LOG_STREAM_SIZE);
    } else {
        ESP_LOGW(TAG, "Failed to create UART log stream");
    }

    ESP_LOGI(TAG, "[ 1 ] Create Bluetooth service");
    
    // Load last connected device
    load_last_bt_device(last_connected_addr);
    
    bluetooth_service_cfg_t bt_cfg = {
        .device_name = "ForD DensO",
        .mode = BLUETOOTH_A2DP_SINK,
    };
    bluetooth_service_start(&bt_cfg);
    
    // Register GAP callback to track connections
    esp_bt_gap_register_callback(bt_gap_cb);
    
    // Set default parameters for Legacy Pairing (Just Works)
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
    
    ESP_LOGI(TAG, "Bluetooth GAP callback registered and security params set");
    
    esp_hf_client_register_callback(bt_hf_client_cb);
    esp_hf_client_init();

    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    // Initialize UART for commands
    if (uart_init() == ESP_OK) {
        xTaskCreate(uart_cmd_task, "uart_cmd", 4096, NULL, 5, NULL);
        ESP_LOGI(TAG, "UART command handler started");
    }

    ESP_LOGI(TAG, "[ 3 ] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_d = audio_pipeline_init(&pipeline_cfg);
    pipeline_e = audio_pipeline_init(&pipeline_cfg);

    ESP_LOGI(TAG, "[3.1] Create i2s stream to write data to codec chip and read data from codec chip");
    i2s_stream_cfg_t i2s_cfg1 = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg1.type = AUDIO_STREAM_WRITER;
    // Explicit I2S pin configuration
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    i2s_cfg1.chan_cfg.id = I2S_NUM_0;
    i2s_cfg1.std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    i2s_cfg1.std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    i2s_cfg1.std_cfg.clk_cfg.sample_rate_hz = 48000;
    i2s_cfg1.std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    i2s_cfg1.std_cfg.gpio_cfg.bclk = GPIO_NUM_26;
    i2s_cfg1.std_cfg.gpio_cfg.ws = GPIO_NUM_33;
    i2s_cfg1.std_cfg.gpio_cfg.dout = GPIO_NUM_25;
    i2s_cfg1.std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
#else
    i2s_cfg1.i2s_port = I2S_NUM_0;
    i2s_cfg1.i2s_config.sample_rate = 48000;
    i2s_cfg1.i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_cfg1.i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_cfg1.i2s_pin.mck_io_num = -1;
    i2s_cfg1.i2s_pin.bck_io_num = GPIO_NUM_26;
    i2s_cfg1.i2s_pin.ws_io_num = GPIO_NUM_33;
    i2s_cfg1.i2s_pin.data_out_num = GPIO_NUM_25;
    i2s_cfg1.i2s_pin.data_in_num = -1;
#endif
    i2s_stream_writer = i2s_stream_init(&i2s_cfg1);

    i2s_stream_cfg_t i2s_cfg2 = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg2.type = AUDIO_STREAM_READER;
    // Explicit I2S pin configuration
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    i2s_cfg2.chan_cfg.id = I2S_NUM_0;
    i2s_cfg2.std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    i2s_cfg2.std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    i2s_cfg2.std_cfg.clk_cfg.sample_rate_hz = 48000;
    i2s_cfg2.std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    i2s_cfg2.std_cfg.gpio_cfg.bclk = GPIO_NUM_26;
    i2s_cfg2.std_cfg.gpio_cfg.ws = GPIO_NUM_33;
    i2s_cfg2.std_cfg.gpio_cfg.dout = GPIO_NUM_25;
    i2s_cfg2.std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
#else
    i2s_cfg2.i2s_port = I2S_NUM_0;
    i2s_cfg2.i2s_config.sample_rate = 48000;
    i2s_cfg2.i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_cfg2.i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_cfg2.i2s_pin.mck_io_num = -1;
    i2s_cfg2.i2s_pin.bck_io_num = GPIO_NUM_26;
    i2s_cfg2.i2s_pin.ws_io_num = GPIO_NUM_33;
    i2s_cfg2.i2s_pin.data_out_num = GPIO_NUM_25;
    i2s_cfg2.i2s_pin.data_in_num = -1;
#endif
#if defined CONFIG_ESP_LYRAT_MINI_V1_1_BOARD
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    i2s_cfg2.chan_cfg.id = I2S_NUM_1;
#else
    i2s_cfg2.i2s_port = I2S_NUM_1;
    i2s_cfg2.i2s_config.use_apll = false;
#endif  /* ESP_IDF_VERSION <= ESP_IDF_VERSION_VAL(5, 0, 0) */
#endif  /* CONFIG_ESP_LYRAT_MINI_V1_1_BOARD */
    i2s_stream_reader = i2s_stream_init(&i2s_cfg2);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_read = raw_stream_init(&raw_cfg);

    ESP_LOGI(TAG, "[3.2] Create Bluetooth stream");
    bt_stream_reader = bluetooth_service_create_stream();

#if (CONFIG_ESP_LYRATD_MSC_V2_1_BOARD || CONFIG_ESP_LYRATD_MSC_V2_2_BOARD)
    rsp_filter_cfg_t rsp_d_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    audio_element_handle_t filter_d = rsp_filter_init(&rsp_d_cfg);
    audio_pipeline_register(pipeline_d, filter_d, "filter_d");

    rsp_filter_cfg_t rsp_e_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_e_cfg.src_rate = 48000;
    rsp_e_cfg.src_ch = 2;
    rsp_e_cfg.dest_rate = HFP_RESAMPLE_RATE;
    rsp_e_cfg.dest_ch = 1;
    audio_element_handle_t filter_e = rsp_filter_init(&rsp_e_cfg);
    audio_pipeline_register(pipeline_e, filter_e, "filter_e");
#endif

    ESP_LOGI(TAG, "[3.3] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline_d, bt_stream_reader, "bt");
    audio_pipeline_register(pipeline_d, i2s_stream_writer, "i2s_w");

    audio_pipeline_register(pipeline_e, i2s_stream_reader, "i2s_r");
    audio_pipeline_register(pipeline_e, raw_read, "raw");

    ESP_LOGI(TAG, "[3.4] Link it together [Bluetooth]-->bt_stream_reader-->i2s_stream_writer-->[codec_chip]");
#if (CONFIG_ESP_LYRATD_MSC_V2_1_BOARD || CONFIG_ESP_LYRATD_MSC_V2_2_BOARD)
    const char *link_d[3] = {"bt", "filter_d", "i2s_w"};
    audio_pipeline_link(pipeline_d, &link_d[0], 3);

    const char *link_e[3] = {"i2s_r", "filter_e", "raw"};
    audio_pipeline_link(pipeline_e, &link_e[0], 3);
#else
    const char *link_d[2] = {"bt", "i2s_w"};
    audio_pipeline_link(pipeline_d, &link_d[0], 2);

    const char *link_e[2] = {"i2s_r", "raw"};
    audio_pipeline_link(pipeline_e, &link_e[0], 2);
#endif

    ESP_LOGI(TAG, "[ 4 ] Initialize peripherals");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    ESP_LOGI(TAG, "[4.1] Initialize Touch peripheral");
    audio_board_key_init(set);

    ESP_LOGI(TAG, "[4.2] Create Bluetooth peripheral");
    bt_periph = bluetooth_service_create_periph();

    ESP_LOGI(TAG, "[4.2] Start all peripherals");
    esp_periph_start(set, bt_periph);

    ESP_LOGI(TAG, "[4.2.1] Start auto-reconnect if needed");
    // Start auto-reconnect task if we have a saved device
    if (has_saved_device && auto_reconnect_enabled) {
        ESP_LOGI(TAG, "Auto-reconnect enabled, will try to reconnect to last device");
        xTaskCreate(bt_reconnect_task, "bt_reconnect", 3072, NULL, 4, NULL);
    } else {
        ESP_LOGI(TAG, "No saved device or auto-reconnect disabled, staying discoverable");
    }

    ESP_LOGI(TAG, "[4.3] Initialize Sleep Control");
    // Check wakeup reason
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_EXT0:
            ESP_LOGI(TAG, "Wakeup caused by external signal using RTC_IO (EXT0)");
            break;
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            ESP_LOGI(TAG, "Wakeup was not caused by deep sleep: %d", wakeup_reason);
            break;
    }
    // Initialize sleep control on GPIO34
    i2c_sleep_on_pin(SLEEP_WAKEUP_PIN);
    
    // Start sleep monitor task
    xTaskCreate(i2c_sleep_monitor_task, "sleep_mon", 2048, (void*)(int)SLEEP_WAKEUP_PIN, 4, NULL);
    ESP_LOGI(TAG, "Sleep monitor task started on GPIO%d", SLEEP_WAKEUP_PIN);

    ESP_LOGI(TAG, "[ 5 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[5.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline_d, evt);

    ESP_LOGI(TAG, "[5.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 6 ] Start audio_pipeline");
    audio_pipeline_run(pipeline_d);
    audio_pipeline_run(pipeline_e);

    ESP_LOGI(TAG, "[ 7 ] Listen for all pipeline events");
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)bt_stream_reader
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(bt_stream_reader, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from Bluetooth, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);
            
            // Mark as connected
            if (!is_connected) {
                is_connected = true;
                ESP_LOGI(TAG, "Bluetooth A2DP connection established");
            }
#if (CONFIG_ESP_LYRATD_MSC_V2_1_BOARD || CONFIG_ESP_LYRATD_MSC_V2_2_BOARD)
            rsp_filter_set_src_info(filter_d, music_info.sample_rates, music_info.channels);
            i2s_stream_set_clk(i2s_stream_writer, 48000, 16, 2);
#else
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
#endif

#if defined CONFIG_ESP_LYRAT_MINI_V1_1_BOARD
            i2s_stream_set_clk(i2s_stream_reader, music_info.sample_rates, music_info.bits, music_info.channels);
#endif

            continue;
        }
        if ((msg.source_type == PERIPH_ID_TOUCH || msg.source_type == PERIPH_ID_BUTTON || msg.source_type == PERIPH_ID_ADC_BTN)
            && (msg.cmd == PERIPH_TOUCH_TAP || msg.cmd == PERIPH_BUTTON_PRESSED || msg.cmd == PERIPH_ADC_BUTTON_PRESSED)) {

            if ((int)msg.data == get_input_play_id()) {
                ESP_LOGI(TAG, "[ * ] [Play] touch tap event");
                periph_bluetooth_play(bt_periph);
            } else if ((int)msg.data == get_input_set_id()) {
                ESP_LOGI(TAG, "[ * ] [Set] touch tap event");
                periph_bluetooth_pause(bt_periph);
            } else if ((int)msg.data == get_input_volup_id()) {
                ESP_LOGI(TAG, "[ * ] [Vol+] touch tap event");
                periph_bluetooth_next(bt_periph);
            } else if ((int)msg.data == get_input_voldown_id()) {
                ESP_LOGI(TAG, "[ * ] [Vol-] touch tap event");
                periph_bluetooth_prev(bt_periph);
            }
        }

        /* Stop when the Bluetooth is disconnected or suspended */
        if (msg.source_type == PERIPH_ID_BLUETOOTH
            && msg.source == (void *)bt_periph) {
            if (msg.cmd == PERIPH_BLUETOOTH_DISCONNECTED) {
                ESP_LOGW(TAG, "[ * ] Bluetooth disconnected");
                is_connected = false;
                
                // Try to reconnect if auto-reconnect is enabled and no task is already running
                if (auto_reconnect_enabled && has_saved_device && !reconnect_task_running) {
                    ESP_LOGI(TAG, "Starting auto-reconnect task...");
                    xTaskCreate(bt_reconnect_task, "bt_reconnect", 3072, NULL, 4, NULL);
                    continue; // Don't break, continue listening
                } else if (!auto_reconnect_enabled || !has_saved_device) {
                    // No auto-reconnect, enable discoverable mode immediately
                    ESP_LOGI(TAG, "Enabling discoverable mode for new connections");
                    if (bt_periph) {
                        periph_bluetooth_discover(bt_periph);
                    }
                    continue; // Don't break, continue listening
                }
                continue; // Continue listening in all cases
            }
        }
        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (int)msg.data == AEL_STATUS_STATE_STOPPED) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
    }

    ESP_LOGI(TAG, "[ 8 ] Stop audio_pipeline");
    audio_pipeline_stop(pipeline_d);
    audio_pipeline_wait_for_stop(pipeline_d);
    audio_pipeline_terminate(pipeline_d);
    audio_pipeline_stop(pipeline_e);
    audio_pipeline_wait_for_stop(pipeline_e);
    audio_pipeline_terminate(pipeline_e);

    audio_pipeline_unregister(pipeline_d, bt_stream_reader);
    audio_pipeline_unregister(pipeline_d, i2s_stream_writer);

    audio_pipeline_unregister(pipeline_e, i2s_stream_reader);
    audio_pipeline_unregister(pipeline_e, raw_read);

#if (CONFIG_ESP_LYRATD_MSC_V2_1_BOARD || CONFIG_ESP_LYRATD_MSC_V2_2_BOARD)
    audio_pipeline_unregister(pipeline_d, filter_d);
    audio_pipeline_unregister(pipeline_e, filter_e);
#endif
    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline_d);

    /* Stop all peripherals before removing the listener */
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline_d);
    audio_element_deinit(bt_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(i2s_stream_reader);
    audio_element_deinit(raw_read);
#if (CONFIG_ESP_LYRATD_MSC_V2_1_BOARD || CONFIG_ESP_LYRATD_MSC_V2_2_BOARD)
    audio_element_deinit(filter_d);
    audio_element_deinit(filter_e);
#endif
    esp_periph_set_destroy(set);
    bluetooth_service_destroy();
}
