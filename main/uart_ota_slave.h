#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Обробити OTA команду отриману через UART
 * 
 * @param data Дані пакету (включаючи префікс)
 * @param len Довжина даних
 * @return ESP_OK якщо успішно
 */
esp_err_t uart_ota_process_command(const uint8_t *data, size_t len);

/**
 * Перевірити чи OTA процес активний
 * 
 * @return true якщо OTA в процесі
 */
bool uart_ota_is_in_progress(void);
