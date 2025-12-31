#include <string.h>

#include "esp_log.h"
#include "driver/uart.h"

#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "uart_handler.h"

static const char *TAG = "uart_handler";
static SemaphoreHandle_t   s_tx_mutex;
static QueueHandle_t       s_evt_queue;
static QueueHandle_t       s_cmd_queue;
static TimerHandle_t       s_timeout_timer;

static char   s_cmd_buf[MAX_CMD_LEN];
static size_t s_cmd_idx = 0;

// If input does not end with INPUT_END_CH, reading of input buffer will end in TIMEOUT_DURATION_MS
static void IRAM_ATTR uart_handler_timeout_cb(TimerHandle_t t)
{
    #if INPUT_END_CH == '\n'
        ESP_LOGI(TAG, "command must end with \\n\n");
    #else
        ESP_LOGI(TAG, "command must end with %c", INPUT_END_CH);
    #endif
    uart_flush_input(UART_NUM_0);
    s_cmd_idx = 0;
}

static void uart_handler_input_evt_task(void *parameters)
{
    uart_event_t ev;
    uint8_t buf[128];
    for (;;) {
        if (xQueueReceive(s_evt_queue, &ev, portMAX_DELAY)) {
            if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
                ESP_LOGW(TAG, "Overflow—flushing");
                uart_flush_input(UART_NUM_0);
                s_cmd_idx = 0;
                xTimerStop(s_timeout_timer, 0);
                continue;
            }

            if (ev.type != UART_DATA) continue;

            uint32_t len = uart_read_bytes(UART_NUM_0, buf, ev.size, portMAX_DELAY);
            for (uint32_t i = 0; i < len; i++) {
                char c = buf[i];
                xTimerReset(s_timeout_timer, 0);
                if (c == INPUT_END_CH) {
                    xTimerStop(s_timeout_timer, 0);
                    if (s_cmd_idx == 0) {
                        uart_flush_input(UART_NUM_0);
                        continue;
                    }

                    s_cmd_buf[s_cmd_idx] = '\0';
                    command_t cmd = { .size = s_cmd_idx + 1 };
                    strncpy(cmd.str, s_cmd_buf, MAX_CMD_LEN);
                    xQueueSend(s_cmd_queue, &cmd, portMAX_DELAY);
                    s_cmd_idx = 0;
                    break;
                }

                if (s_cmd_idx < MAX_CMD_LEN - 1) {
                    s_cmd_buf[s_cmd_idx++] = c;
                    continue;
                }

                xTimerStop(s_timeout_timer, 0);
                ESP_LOGW(TAG, "Cmd too long—dropping");
                uart_flush_input(UART_NUM_0);
                s_cmd_idx = 0;
                break;
            }
        }
    }
}

esp_err_t uart_handler_init()
{
    s_tx_mutex     = xSemaphoreCreateMutex();
    s_evt_queue    = xQueueCreate(EVT_QUEUE_SIZE, sizeof(uart_event_t));
    s_cmd_queue    = xQueueCreate(CMD_QUEUE_SIZE, sizeof(command_t));
    s_timeout_timer = xTimerCreate("to", pdMS_TO_TICKS(TIMEOUT_DURATION_MS), pdFALSE,
                                   NULL, uart_handler_timeout_cb);

    if (!s_tx_mutex || !s_evt_queue || !s_cmd_queue || !s_timeout_timer)
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
