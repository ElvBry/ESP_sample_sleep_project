#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/queue.h"

// This is a helper component in order to safely input and output char strings to serial

#define EVT_QUEUE_SIZE 8
#define CMD_QUEUE_SIZE 8


#define MAX_CMD_LEN 16
#define INPUT_END_CH '\n' // Character indicating end of input

#define TIMEOUT_DURATION_MS 10

typedef struct {
    char    str[MAX_CMD_LEN];
    uint16_t size;
} command_t;

/**
 * @brief Initialize UART component
 * @return ESP_OK on success, ESP_FAIL on failure
 * @param uxPriority Priority of the uart input task for freeRTOS
 */
esp_err_t uart_handler_init();

// blocks until command string is ready
QueueHandle_t uart_handler_get_queue(void);

/**
 * @brief Send raw bytes (thread-safe)
 * @param data Pointer to the char string
 * @param len Length of char string
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t uart_handler_send(const char* data, size_t len);
