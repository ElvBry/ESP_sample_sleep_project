#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uart_handler.h"
#include "esp_err.h"
#include <string.h>

void app_main(void)
{
    ESP_ERROR_CHECK(uart_handler_init());
    QueueHandle_t q = uart_handler_get_queue();
    command_t cmd;
    while (xQueueReceive(q, &cmd, portMAX_DELAY)) {
        if (strcmp(cmd.str, "test") == 0) {
            const char* response = "Test Received!\r\n";
            ESP_ERROR_CHECK_WITHOUT_ABORT(uart_handler_send(response, strlen(response)));
        }
    }
}

