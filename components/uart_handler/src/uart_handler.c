#include <string.h>

#include "esp_log.h"
#include "driver/uart.h"

#include "freertos/semphr.h"

#include "uart_handler.h"

static const char *TAG = "uart_handler";
static SemaphoreHandle_t   s_tx_mutex;
static QueueHandle_t       s_evt_queue;
static QueueHandle_t       s_cmd_queue;

static char   s_cmd_buf[MAX_CMD_LEN];
static size_t s_cmd_idx = 0;
static bool   s_cmd_overflow = false;

static void uart_handler_echo_char(char c)
{
    // Echo printable characters and newline
    if ((c >= 32 && c <= 126) || c == '\n' || c == '\r') {
        uart_write_bytes(UART_NUM_0, &c, 1);
    }
}

static void uart_handler_input_evt_task(void *parameters)
{
    uart_event_t ev;
    uint8_t buf[128];
    for (;;) {
        xQueueReceive(s_evt_queue, &ev, portMAX_DELAY);
        if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            ESP_LOGW(TAG, "Overflow—flushing");
            uart_flush_input(UART_NUM_0);
            s_cmd_idx = 0;
            s_cmd_overflow = false;
            continue;
        }

        if (ev.type != UART_DATA) continue;

        uint32_t len = uart_read_bytes(UART_NUM_0, buf, ev.size, portMAX_DELAY);

        for (uint32_t i = 0; i < len; i++) {
            char c = buf[i];
            // Handle both \r and \n as command terminators (supports CR, LF, CRLF, LFCR)
            if (c == '\r' || c == '\n') {
                // Echo CRLF for proper terminal behavior (return to column 0 + new line)
                uart_handler_echo_char('\r');
                uart_handler_echo_char('\n');

                // Only send command if we have content and no overflow occurred
                if (s_cmd_idx > 0 && !s_cmd_overflow) {
                    s_cmd_buf[s_cmd_idx] = '\0';
                    command_t cmd = { .size = s_cmd_idx + 1 };
                    strncpy(cmd.str, s_cmd_buf, MAX_CMD_LEN);
                    xQueueSend(s_cmd_queue, &cmd, portMAX_DELAY);
                } else if (s_cmd_overflow) {
                    ESP_LOGW(TAG, "Command truncated at %d chars", MAX_CMD_LEN - 1);
                }
                
                s_cmd_idx = 0;
                s_cmd_overflow = false;
                continue;
            }

            // If in overflow state, echo but discard the char
            if (s_cmd_overflow) {
                uart_handler_echo_char(c);
                continue;
            }

            // If buffer has room, accept the character
            if (s_cmd_idx < MAX_CMD_LEN - 1) {
                s_cmd_buf[s_cmd_idx++] = c;
                uart_handler_echo_char(c);
                continue;
            }
            // Buffer is full (15 chars), enter overflow state
            s_cmd_overflow = true;
        }
    }
}

esp_err_t uart_handler_init()
{
    s_tx_mutex     = xSemaphoreCreateMutex();
    s_evt_queue    = xQueueCreate(EVT_QUEUE_SIZE, sizeof(uart_event_t));
    s_cmd_queue    = xQueueCreate(CMD_QUEUE_SIZE, sizeof(command_t));

    if (!s_tx_mutex || !s_evt_queue || !s_cmd_queue)
        return ESP_FAIL;

    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM_0, &cfg);
    uart_driver_install(UART_NUM_0, 1024, 512, EVT_QUEUE_SIZE,
                        &s_evt_queue, 0);
    xTaskCreate(uart_handler_input_evt_task, "uart_evt", 4096,
                NULL, 12, NULL);
    return ESP_OK;
}

QueueHandle_t uart_handler_get_queue(void)
{
    return s_cmd_queue;
}

esp_err_t uart_handler_send(const char* data, size_t len)
{
    if (xSemaphoreTake(s_tx_mutex, portMAX_DELAY) == pdTRUE) {
        uint32_t w = uart_write_bytes(UART_NUM_0, data, len);
        xSemaphoreGive(s_tx_mutex);
        return (w == len) ? ESP_OK : ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}
