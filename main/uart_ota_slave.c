#include "uart_ota_slave.h"
#include "uart_ota_protocol.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "UART_OTA_SLAVE";

/**
 * Зберегти версію UART OTA Slave в NVS
 */
void uart_ota_slave_save_version(void)
{
    static bool version_saved = false;
    if (version_saved) return;
    
    // Ініціалізувати NVS якщо потрібно
    static bool nvs_initialized = false;
    if (!nvs_initialized) {
        ESP_LOGI(TAG, "UART Slave: Initializing NVS");
        esp_err_t err_init = nvs_flash_init();
        if (err_init == ESP_ERR_NVS_NO_FREE_PAGES || err_init == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGI(TAG, "UART Slave: Erasing NVS");
            err_init = nvs_flash_erase();
            if (err_init != ESP_OK) {
                ESP_LOGW(TAG, "UART Slave: NVS erase failed: %s", esp_err_to_name(err_init));
            }
            err_init = nvs_flash_init();
        }
        if (err_init != ESP_OK) {
            ESP_LOGE(TAG, "UART Slave: Failed to init NVS: %s", esp_err_to_name(err_init));
            return;
        }
        nvs_initialized = true;
        ESP_LOGI(TAG, "UART Slave: NVS initialized");
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("uart_ota_slave", NVS_READWRITE, &nvs_handle);
    ESP_LOGI(TAG, "UART Slave: Opening uart_ota_slave NVS");
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UART Slave: Setting date %s", __DATE__);
        nvs_set_str(nvs_handle, "date", __DATE__);
        ESP_LOGI(TAG, "UART Slave: Setting time %s", __TIME__);
        nvs_set_str(nvs_handle, "time", __TIME__);
        ESP_LOGI(TAG, "UART Slave: Committing");
        nvs_commit(nvs_handle);
        
        // Перевірити збережене значення
        ESP_LOGI(TAG, "UART Slave: Reading back saved values");
        char saved_date[12] = "";
        char saved_time[9] = "";
        size_t len = sizeof(saved_date);
        esp_err_t read_err = nvs_get_str(nvs_handle, "date", saved_date, &len);
        if (read_err == ESP_OK) {
            len = sizeof(saved_time);
            read_err = nvs_get_str(nvs_handle, "time", saved_time, &len);
            if (read_err == ESP_OK) {
                ESP_LOGI(TAG, "UART Slave NVS READ: DATE: %s TIME: %s", saved_date, saved_time);
            } else {
                ESP_LOGE(TAG, "UART Slave: Failed to read time from NVS: %s", esp_err_to_name(read_err));
            }
        } else {
            ESP_LOGE(TAG, "UART Slave: Failed to read date from NVS: %s", esp_err_to_name(read_err));
        }
        
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "💾 UART OTA Slave version saved to NVS");
        ESP_LOGI(TAG, "UART Slave DATE: %s TIME: %s", __DATE__, __TIME__);
        
        version_saved = true;
    } else {
        ESP_LOGE(TAG, "❌ Failed to open uart_ota_slave NVS: %s", esp_err_to_name(err));
    }
}

// Глобальні змінні для OTA процесу
static bool ota_in_progress = false;
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;
static uint32_t expected_size = 0;
static uint32_t expected_crc32 = 0;
static uint32_t received_bytes = 0;
static uint16_t last_block_num = 0;

// UART конфігурація (має співпадати з master)
#define APP_UART_PORT UART_NUM_1

/**
 * Відправити відповідь на master
 */
static esp_err_t send_response(uint8_t response, uint16_t block_num)
{
    //ESP_LOGI(TAG, "📤 send_response: response=0x%02X, block_num=%d", response, block_num);
    
    ota_response_t resp = {
        .prefix = UART_CMD_PREFIX_OTA,
        .response = response,
        .block_num = block_num
    };
    
    //ESP_LOGI(TAG, "📤 Відправка відповіді: 0x%02X (block=%d, size=%d)", 
    //         response, block_num, sizeof(resp));
    
    int written = uart_write_bytes(APP_UART_PORT, (const char *)&resp, sizeof(resp));
    if (written != sizeof(resp)) {
        ESP_LOGE(TAG, "❌ Помилка відправки відповіді: %d/%d байт", written, sizeof(resp));
        return ESP_FAIL;
    }
    //ESP_LOGI(TAG, "📤 Written %d bytes, waiting for TX done...", written);
    uart_wait_tx_done(APP_UART_PORT, pdMS_TO_TICKS(100));
    //ESP_LOGI(TAG, "✅ TX done!");
    //ESP_LOGI(TAG, "✅ TX done!");

    return ESP_OK;
    return ESP_OK;
}

/**
 * Обробити START команду
 */
static esp_err_t handle_start_command(const ota_start_packet_t *pkt)
{
    ESP_LOGI(TAG, "🔥 ENTER handle_start_command");
    
    // Зберегти версію при початку OTA
    uart_ota_slave_save_version();
    // Also save CRC version
    extern void uart_ota_crc_save_version(void);
    uart_ota_crc_save_version();
    
    if (ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress, aborting previous");
        if (ota_handle) {
            esp_ota_end(ota_handle);
            ota_handle = 0;
        }
    }
    
    ESP_LOGI(TAG, "📥 OTA START: size=%u, crc=0x%08X", pkt->total_size, pkt->crc32);
    
    // Перевірка розміру
    if (pkt->total_size == 0 || pkt->total_size > UART_OTA_MAX_SIZE) {
        ESP_LOGE(TAG, "Invalid firmware size: %u", pkt->total_size);
        send_response(OTA_RESP_ERROR, 0);
        return ESP_FAIL;
    }
    
    // Отримати partition для оновлення
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        send_response(OTA_RESP_ERROR, 0);
        return ESP_FAIL;
    }
    
    // Перевірка розміру partition
    if (pkt->total_size > update_partition->size) {
        ESP_LOGE(TAG, "Firmware too large for partition: %u > %u", 
                 pkt->total_size, update_partition->size);
        send_response(OTA_RESP_ERROR, 0);
        return ESP_FAIL;
    }
    
    // Початок OTA
    esp_err_t err = esp_ota_begin(update_partition, pkt->total_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        send_response(OTA_RESP_ERROR, 0);
        return err;
    }
    
    // Ініціалізація стану
    ota_in_progress = true;
    expected_size = pkt->total_size;
    expected_crc32 = pkt->crc32;
    received_bytes = 0;
    last_block_num = 0;
    
    ESP_LOGI(TAG, "✅ Ready for OTA, partition: %s", update_partition->label);
    ESP_LOGI(TAG, "📤 Sending READY response...");
    send_response(OTA_RESP_READY, 0);
    ESP_LOGI(TAG, "✅ READY response sent!");
    
    return ESP_OK;
}

/**
 * Обробити DATA_BLOCK команду
 */
static esp_err_t handle_data_block(const uint8_t *data, size_t len)
{
    if (!ota_in_progress || !ota_handle) {
        ESP_LOGE(TAG, "Data block received but OTA not started");
        send_response(OTA_RESP_ERROR, 0);
        return ESP_FAIL;
    }
    
    // Парсинг пакету
    if (len < 5) {  // prefix + cmd + block_num(2) + data_len(1)
        ESP_LOGE(TAG, "Data packet too short: %d", len);
        send_response(OTA_RESP_NACK, last_block_num);
        return ESP_FAIL;
    }
    
    uint16_t block_num = (data[3] << 8) | data[2];
    uint8_t data_len = data[4];
    const uint8_t *block_data = &data[5];
    
    // Перевірка розміру
    if (data_len > UART_OTA_BLOCK_SIZE || len < (5 + data_len + 2)) {
        ESP_LOGE(TAG, "Invalid data length: %d", data_len);
        send_response(OTA_RESP_NACK, block_num);
        return ESP_FAIL;
    }
    
    // Отримати CRC16 з кінця пакету
    uint16_t received_crc16 = (data[5 + data_len + 1] << 8) | data[5 + data_len];
    uint16_t calculated_crc16 = uart_ota_crc16(block_data, data_len);
    
    // Перевірка CRC16
    if (received_crc16 != calculated_crc16) {
        ESP_LOGE(TAG, "CRC16 mismatch for block %d: expected 0x%04X, got 0x%04X",
                 block_num, calculated_crc16, received_crc16);
        send_response(OTA_RESP_NACK, block_num);
        return ESP_FAIL;
    }
    
    // Перевірка порядку блоків
    if (block_num != last_block_num) {
        ESP_LOGW(TAG, "Block number mismatch: expected %d, got %d", last_block_num, block_num);
        send_response(OTA_RESP_NACK, last_block_num);
        return ESP_FAIL;
    }

    // Запис даних у flash
    esp_err_t err = esp_ota_write(ota_handle, block_data, data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA write failed for block %d: %s", block_num, esp_err_to_name(err));
        send_response(OTA_RESP_ERROR, block_num);
        return err;
    }

    received_bytes += data_len;
    last_block_num++;

    // Логування прогресу
    if ((block_num % 128 == 0) || (block_num == 1) || (received_bytes == expected_size)) {
        int percent = (received_bytes * 100) / expected_size;
        ESP_LOGI(TAG, "📊 OTA: %3d%% (%u/%u bytes, block %d)", percent, received_bytes, expected_size, block_num);
    }
    // Завжди відправляємо ACK після успішного запису
    send_response(OTA_RESP_ACK, block_num);
    return ESP_OK;
}

/**
 * Обробити END команду
 */
static esp_err_t handle_end_command(const ota_end_packet_t *pkt)
{
    if (!ota_in_progress || !ota_handle) {
        ESP_LOGE(TAG, "END received but OTA not started");
        send_response(OTA_RESP_ERROR, 0);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "📥 OTA END: final_crc=0x%08X", pkt->final_crc32);
    
    // Перевірка розміру
    if (received_bytes != expected_size) {
        ESP_LOGE(TAG, "Size mismatch: received %u, expected %u", 
                 received_bytes, expected_size);
        esp_ota_end(ota_handle);
        ota_handle = 0;
        ota_in_progress = false;
        send_response(OTA_RESP_ERROR, 0);
        return ESP_FAIL;
    }
    
    // Перевірка CRC32
    if (pkt->final_crc32 != expected_crc32) {
        ESP_LOGE(TAG, "CRC32 mismatch: received 0x%08X, expected 0x%08X",
                 pkt->final_crc32, expected_crc32);
        esp_ota_end(ota_handle);
        ota_handle = 0;
        ota_in_progress = false;
        send_response(OTA_RESP_ERROR, 0);
        return ESP_FAIL;
    }
    
    // Завершення OTA
    esp_err_t err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        ota_handle = 0;
        ota_in_progress = false;
        send_response(OTA_RESP_ERROR, 0);
        return err;
    }
    
    // Встановлення нової boot partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        ota_handle = 0;
        ota_in_progress = false;
        send_response(OTA_RESP_ERROR, 0);
        return err;
    }
    
    ota_handle = 0;
    ota_in_progress = false;
    
    ESP_LOGI(TAG, "✅ OTA completed successfully!");
    send_response(OTA_RESP_COMPLETE, 0);
    
    // Перезавантаження через 2 секунди
    ESP_LOGI(TAG, "🔄 Rebooting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    
    return ESP_OK;
}

/**
 * Обробити ABORT команду
 */
static esp_err_t handle_abort_command(void)
{
    ESP_LOGW(TAG, "⚠️ OTA ABORT received");
    
    if (ota_in_progress && ota_handle) {
        esp_ota_end(ota_handle);
        ota_handle = 0;
    }
    
    ota_in_progress = false;
    received_bytes = 0;
    last_block_num = 0;
    
    return ESP_OK;
}

/**
 * Основна функція обробки UART OTA команд
 * Викликається з UART handler коли отримано OTA префікс
 */
esp_err_t uart_ota_process_command(const uint8_t *data, size_t len)
{
    if (len < 2) {
        ESP_LOGE(TAG, "Command too short: %d", len);
        return ESP_FAIL;
    }
    
    // Перевірка префіксу
    if (data[0] != UART_CMD_PREFIX_OTA) {
        ESP_LOGE(TAG, "Invalid OTA prefix: 0x%02X", data[0]);
        return ESP_FAIL;
    }
    
    uint8_t cmd = data[1];
    
    switch (cmd) {
        case OTA_CMD_START:
            ESP_LOGI(TAG, "🚀 Processing OTA_CMD_START...");
            if (len >= sizeof(ota_start_packet_t)) {
                ESP_LOGI(TAG, "📦 START packet size OK: %d >= %d", len, sizeof(ota_start_packet_t));
                return handle_start_command((const ota_start_packet_t *)data);
            }
            ESP_LOGE(TAG, "❌ START packet too short: %d < %d", len, sizeof(ota_start_packet_t));
            return ESP_FAIL;
            
        case OTA_CMD_DATA_BLOCK:
            return handle_data_block(data, len);
            
        case OTA_CMD_END:
            if (len >= sizeof(ota_end_packet_t)) {
                return handle_end_command((const ota_end_packet_t *)data);
            }
            ESP_LOGE(TAG, "END packet too short");
            return ESP_FAIL;
            
        case OTA_CMD_ABORT:
            return handle_abort_command();
            
        default:
            ESP_LOGW(TAG, "Unknown OTA command: 0x%02X", cmd);
            return ESP_FAIL;
    }
}

/**
 * Перевірити чи OTA в процесі
 */
bool uart_ota_is_in_progress(void)
{
    return ota_in_progress;
}
