#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/queue.h"

// Helper component in order to safely input and output char strings to serial, accepts both CR (\r) and LF (\n) as command terminators

#define EVT_QUEUE_SIZE 8
#define CMD_QUEUE_SIZE 8


#define MAX_CMD_LEN 64 // including null terminator, feel free to adjust as needed

typedef struct {
    char    str[MAX_CMD_LEN];
    uint16_t size;
} command_t;

/**
 * @brief Initialize UART component with character-by-character input handling
 *
 * Initializes UART0 at 115200 baud with the following behavior:
 * - Characters are echoed immediately for user feedback
 * - Commands are limited to MAX_CMD_LEN characters counting null terminator
 * - Characters beyond 15 are echoed but discarded until line ending
 * - Accepts both CR (\r) and LF (\n) as command terminators
 * - Commands are sent to queue when CR or LF is received
 *
 * @return ESP_OK on success, ESP_FAIL on failure
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
