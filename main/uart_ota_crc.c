#include "uart_ota_protocol.h"
#include "esp_crc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

/**
 * Зберегти версію UART OTA CRC в NVS
 */
void uart_ota_crc_save_version(void)
{
    static bool version_saved = false;
    if (version_saved) return;
    
    static const char *TAG = "UART_OTA_CRC";
    
    // Ініціалізувати NVS якщо потрібно
    static bool nvs_initialized = false;
    if (!nvs_initialized) {
        ESP_LOGI(TAG, "UART OTA CRC: Initializing NVS");
        esp_err_t err_init = nvs_flash_init();
        if (err_init == ESP_ERR_NVS_NO_FREE_PAGES || err_init == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGI(TAG, "UART OTA CRC: Erasing NVS");
            err_init = nvs_flash_erase();
            if (err_init != ESP_OK) {
                ESP_LOGW(TAG, "UART OTA CRC: NVS erase failed: %s", esp_err_to_name(err_init));
            }
            err_init = nvs_flash_init();
        }
        if (err_init != ESP_OK) {
            ESP_LOGE(TAG, "UART OTA CRC: Failed to init NVS: %s", esp_err_to_name(err_init));
            return;
        }
        nvs_initialized = true;
        ESP_LOGI(TAG, "UART OTA CRC: NVS initialized");
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("uart_ota_crc", NVS_READWRITE, &nvs_handle);
    ESP_LOGI(TAG, "UART OTA CRC: Opening uart_ota_crc NVS");
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UART OTA CRC: Setting date %s", __DATE__);
        nvs_set_str(nvs_handle, "date", __DATE__);
        ESP_LOGI(TAG, "UART OTA CRC: Setting time %s", __TIME__);
        nvs_set_str(nvs_handle, "time", __TIME__);
        ESP_LOGI(TAG, "UART OTA CRC: Committing");
        nvs_commit(nvs_handle);
        
        // Перевірити збережене значення
        ESP_LOGI(TAG, "UART OTA CRC: Reading back saved values");
        char saved_date[12] = "";
        char saved_time[9] = "";
        size_t len = sizeof(saved_date);
        esp_err_t read_err = nvs_get_str(nvs_handle, "date", saved_date, &len);
        if (read_err == ESP_OK) {
            len = sizeof(saved_time);
            read_err = nvs_get_str(nvs_handle, "time", saved_time, &len);
            if (read_err == ESP_OK) {
                ESP_LOGI(TAG, "UART OTA CRC NVS READ: DATE: %s TIME: %s", saved_date, saved_time);
            } else {
                ESP_LOGE(TAG, "UART OTA CRC: Failed to read time from NVS: %s", esp_err_to_name(read_err));
            }
        } else {
            ESP_LOGE(TAG, "UART OTA CRC: Failed to read date from NVS: %s", esp_err_to_name(read_err));
        }
        
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "💾 UART OTA CRC version saved to NVS");
        ESP_LOGI(TAG, "UART OTA CRC DATE: %s TIME: %s", __DATE__, __TIME__);
        
        version_saved = true;
    } else {
        ESP_LOGE(TAG, "❌ Failed to open uart_ota_crc NVS: %s", esp_err_to_name(err));
    }
}

/**
 * Обчислити CRC16 для даних (MODBUS)
 */
uint16_t uart_ota_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/**
 * Обчислити CRC32 для даних
 */
uint32_t uart_ota_crc32(const uint8_t *data, size_t len)
{
    return esp_crc32_le(0, data, len);
}
