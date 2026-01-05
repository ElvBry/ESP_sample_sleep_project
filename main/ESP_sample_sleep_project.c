#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_partition.h"
#include "esp_timer.h"

#include "esp_err.h"
#include "esp_log.h"

#include "uart_handler.h"
#include "driver/temperature_sensor.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "main";

#define SETTINGS_MAGIC 0xDEADBEEF  // Change magic number to force re-initialization
#define LOG_START 4096

#define MIN_LOGGING_PERIOD_MS 5
#define DEFAULT_LOGGING_PERIOD_MS 5000
#define DATA_SPLICE_GAP_MS 60000        // Gap added to timestamp on boot to mark data splice from power surge (60s)

// States for settings.state
#define IDLE    0U
#define LOGGING 1U
#define ERROR   2U

static inline void send_msg(const char* msg) {
    uart_handler_send(msg, strlen(msg));
}


static uint32_t s_num_entries = 0;
static uint32_t s_initial_timestamp_ms = 0;

// Settings struct (stored at offset 0)
typedef struct {
    uint32_t magic;
    uint32_t logging_period_MS;
    uint8_t state;
    uint8_t log_level;
    uint8_t padding[2];
} __attribute__((packed)) settings_t;

// Log entry (stored after settings at LOG_START)
typedef struct {
    uint32_t timestamp;
    float temperature;
} __attribute__((packed)) log_entry_t;

// Return next available log slot in partition after reset or power cycle 
static uint32_t find_num_entries(const esp_partition_t* flash)
{
    log_entry_t entry;
    const uint32_t sector_size = 4096;
    const uint32_t entries_per_sector = sector_size / sizeof(log_entry_t);
    const uint32_t total_sectors = (flash->size - LOG_START) / sector_size;

    // Step 1: Find which sector has empty space
    uint32_t sector_count = 0;
    for (uint32_t i = 0; i < total_sectors; i++) {
        // Check last entry of this sector
        uint32_t sector_offset = LOG_START + (i * sector_size);
        uint32_t last_entry_offset = sector_offset + ((entries_per_sector - 1) * sizeof(log_entry_t));

        esp_partition_read(flash, last_entry_offset, &entry, sizeof(log_entry_t));

        // Check if sector has empty space
        if (entry.timestamp == 0xFFFFFFFF) {
            sector_count = i;
            break;
        }

        sector_count = i + 1;
    }

    // Check if flash is full
    if (sector_count >= total_sectors) {
        ESP_LOGW(TAG, "Flash full!");
        return total_sectors * entries_per_sector;
    }

    // Find first empty entry within this sector
    uint32_t sector_offset = LOG_START + (sector_count * sector_size);
    uint32_t entry_count = 0;

    for (uint32_t i = 0; i < entries_per_sector; i++) {
        uint32_t offset = sector_offset + (i * sizeof(log_entry_t));
        esp_partition_read(flash, offset, &entry, sizeof(log_entry_t));

        if (entry.timestamp == 0xFFFFFFFF) {
            entry_count = i;
            break;
        }
    }

    uint32_t total_entries = (sector_count * entries_per_sector) + entry_count;
    ESP_LOGI(TAG, "Found empty slot in sector %lu, entry %lu (total: %lu)",
             sector_count, entry_count, total_entries);

    return total_entries;
}

esp_err_t erase_and_initialize_partition(const esp_partition_t* flash, settings_t* settings)
{
    esp_err_t err = esp_partition_erase_range(flash, 0, flash->size);
    if (err != ESP_OK) {
        return err;
    }

    settings->magic = SETTINGS_MAGIC;
    settings->logging_period_MS = DEFAULT_LOGGING_PERIOD_MS;
    settings->state = IDLE;
    settings->log_level = ESP_LOG_INFO;

    err = esp_partition_write(flash, 0, settings, sizeof(settings_t));
    if (err != ESP_OK) {
        return err;
    }

    s_num_entries = 0;
    s_initial_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // Set log level
    esp_log_level_set(TAG, (esp_log_level_t)settings->log_level);

    return ESP_OK;
}

bool handle_input_command(const command_t* cmd, const esp_partition_t* flash, settings_t* settings)
{
    if (strcmp(cmd->str, "help") == 0) {
        const char* help_msg =
            "Available commands:\r\n"
            "  help - Show this help message\r\n"
            "  start - Begin logging data\r\n"
            "  stop - Stop logging data\r\n"
            "  info - Show system information\r\n"
            "  set period <ms> - Set logging period in milliseconds\r\n"
            "  set level <0-5> - Set log level (0=none, 1=error, 2=warn, 3=info, 4=debug, 5=verbose)\r\n"
            "  dump <count> - Print last <count> entries in CSV format (omit for all)\r\n"
            "  clear <count> - Remove last <count> entries (omit for all)\r\n"
            "  reset - Erase all data and reset to initial state\r\n";
        send_msg(help_msg);
        return false;

    } else if (strcmp(cmd->str, "start") == 0) {
        if (settings->state == LOGGING) {
            send_msg("Already logging\r\n");
            return false;
        }
        settings->state = LOGGING;

        // Must erase sector before writing
        esp_partition_erase_range(flash, 0, 4096);
        esp_partition_write(flash, 0, settings, sizeof(settings_t));

        send_msg("Started logging\r\n");
        ESP_LOGI(TAG, "State changed to LOGGING");
        return true;

    } else if (strcmp(cmd->str, "stop") == 0) {
        if (settings->state == IDLE) {
            send_msg("Already stopped\r\n");
            return false;
        }
        settings->state = IDLE;

        esp_partition_erase_range(flash, 0, 4096);
        esp_partition_write(flash, 0, settings, sizeof(settings_t));

        send_msg("Stopped logging\r\n");
        ESP_LOGI(TAG, "State changed to IDLE");
        return true;

    } else if (strcmp(cmd->str, "info") == 0) {
        char info_msg[512];
        uint32_t max_entries = (flash->size - LOG_START) / sizeof(log_entry_t);
        uint32_t remaining = max_entries - s_num_entries;
        float percent_full = (float)s_num_entries / max_entries * 100.0f;

        const char* state_str = (settings->state == IDLE) ? "IDLE" :
                               (settings->state == LOGGING) ? "LOGGING" : "ERROR";
        const char* level_str = "";
        esp_log_level_t level = esp_log_level_get(TAG);
        switch(level) {
            case ESP_LOG_NONE: level_str = "NONE"; break;
            case ESP_LOG_ERROR: level_str = "ERROR"; break;
            case ESP_LOG_WARN: level_str = "WARN"; break;
            case ESP_LOG_INFO: level_str = "INFO"; break;
            case ESP_LOG_DEBUG: level_str = "DEBUG"; break;
            case ESP_LOG_VERBOSE: level_str = "VERBOSE"; break;
            default: level_str = "UNKNOWN"; break;
        }

        snprintf(info_msg, sizeof(info_msg),
            "\r\nSystem Information:\r\n"
            "  Project: ESP_sample_sleep_project\r\n"
            "  Logging period: %lu ms\r\n"
            "  Current state: %s\r\n"
            "  Entries logged: %lu / %lu\r\n"
            "  Remaining space: %lu entries (%.1f%% full)\r\n"
            "  Log level: %s\r\n\r\n",
            settings->logging_period_MS,
            state_str,
            s_num_entries, max_entries,
            remaining, percent_full,
            level_str);

        send_msg(info_msg);
        return false;

    } else if (strncmp(cmd->str, "set period ", 11) == 0) {
        uint32_t period = atoi(cmd->str + 11);
        if (period < MIN_LOGGING_PERIOD_MS) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Error: Period must be >= %u ms\r\n", MIN_LOGGING_PERIOD_MS);
            send_msg(msg);
            return false;
        }
        settings->logging_period_MS = period;

        esp_partition_erase_range(flash, 0, 4096);
        esp_partition_write(flash, 0, settings, sizeof(settings_t));

        char msg[64];
        snprintf(msg, sizeof(msg), "Period set to %lu ms\r\n", period);
        send_msg(msg);
        ESP_LOGI(TAG, "Period changed to %lu ms", period);
        return false;

    } else if (strncmp(cmd->str, "set level ", 10) == 0) {
        int level = atoi(cmd->str + 10);
        if (level < 0 || level > 5) {
            send_msg("Error: Level must be 0-5\r\n");
            return false;
        }
        settings->log_level = (uint8_t)level;
        esp_log_level_set(TAG, (esp_log_level_t)level);

        esp_partition_erase_range(flash, 0, 4096);
        esp_partition_write(flash, 0, settings, sizeof(settings_t));

        char msg[64];
        snprintf(msg, sizeof(msg), "Log level set to %d\r\n", level);
        send_msg(msg);
        return false;

    } else if (strncmp(cmd->str, "dump", 4) == 0) {
        uint32_t count = s_num_entries;
        if (strlen(cmd->str) > 5) {
            count = atoi(cmd->str + 5);
            if (count > s_num_entries) count = s_num_entries;
        }

        send_msg("timestamp_ms,temperature_C\r\n");

        uint32_t start_idx = (count >= s_num_entries) ? 0 : s_num_entries - count;
        for (uint32_t i = start_idx; i < s_num_entries; i++) {
            log_entry_t entry;
            uint32_t offset = LOG_START + i * sizeof(log_entry_t);
            esp_partition_read(flash, offset, &entry, sizeof(log_entry_t));

            char line[64];
            snprintf(line, sizeof(line), "%lu,%.2f\r\n", entry.timestamp, entry.temperature);
            send_msg(line);
        }

        char msg[64];
        snprintf(msg, sizeof(msg), "\r\nDumped %lu entries\r\n", count);
        send_msg(msg);
        return false;

    } else if (strncmp(cmd->str, "clear", 5) == 0) {
        uint32_t count;

        if (strlen(cmd->str) == 5) {
            // Clear all entries
            count = s_num_entries;
        } else {
            // Clear last N entries
            count = atoi(cmd->str + 6);
            if (count > s_num_entries) {
                count = s_num_entries;
            }
        }

        if (count == 0) {
            send_msg("No entries to clear\r\n");
            return false;
        }

        s_num_entries -= count;

        char msg[64];
        snprintf(msg, sizeof(msg), "Removed last %lu entries (now %lu total)\r\n",
                 count, s_num_entries);
        send_msg(msg);
        ESP_LOGI(TAG, "Logically removed %lu entries", count);
        return false;

    } else if (strcmp(cmd->str, "reset") == 0) {
        send_msg("Resetting and erasing all data...\r\n");
        esp_err_t err = erase_and_initialize_partition(flash, settings);
        if (err != ESP_OK) {
            send_msg("Error: Reset failed\r\n");
            ESP_LOGE(TAG, "Reset failed: %s", esp_err_to_name(err));
        } else {
            send_msg("Reset complete\r\n");
            ESP_LOGI(TAG, "System reset");
        }
        return false;

    } else {
        send_msg("Unknown command. Type 'help' for commands.\r\n");
        return false;
    }
}


esp_err_t log_data_entry(const esp_partition_t* flash, log_entry_t* entry)
{
    esp_err_t err = ESP_OK;
    uint32_t entry_offset = LOG_START + s_num_entries * sizeof(log_entry_t);

    // Erase sector if this is the first entry in it
    if (entry_offset % 4096 == 0) {
        ESP_LOGI(TAG, "Erasing sector at offset %lu", entry_offset);
        err = esp_partition_erase_range(flash, entry_offset, 4096);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase sector: %s", esp_err_to_name(err));
            return err;
        }
    }

    // Write entry
    err = esp_partition_write(flash, entry_offset, entry, sizeof(log_entry_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write entry: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Wrote entry at offset %lu", entry_offset);

    s_num_entries++;
    return ESP_OK;
}


void app_main(void)
{
    // Find flash partition (subtype 0x40 from partitions.csv)
    const esp_partition_t* flash = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        0x40,
        "storage"
    );

    if (!flash) {
        ESP_LOGE(TAG, "Flash partition not found!");
        return;
    }

    ESP_LOGI(TAG, "Flash: address=0x%lx, size=%lu bytes", flash->address, flash->size);

    settings_t settings;
    esp_partition_read(flash, 0, &settings, sizeof(settings_t));

    // Check magic number to detect first boot
    if (settings.magic != SETTINGS_MAGIC) {
        ESP_LOGI(TAG, "First boot - erasing partition and initializing");

        if (erase_and_initialize_partition(flash, &settings) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize partition");
            return;
        }
        
    } else {
        ESP_LOGI(TAG, "Continuing from previous session (period=%lu, state=%u)",
                 settings.logging_period_MS, settings.state);

        // Restore log level from flash
        esp_log_level_set(TAG, (esp_log_level_t)settings.log_level);

        s_num_entries = find_num_entries(flash);
        ESP_LOGI(TAG, "Current number of entries: %lu", s_num_entries);

        // Read last timestamp and set initial timestamp ahead to mark data splice
        if (s_num_entries > 0) {
            log_entry_t last_entry;
            uint32_t last_offset = LOG_START + (s_num_entries - 1) * sizeof(log_entry_t);
            esp_partition_read(flash, last_offset, &last_entry, sizeof(log_entry_t));

            // Set initial timestamp ahead of last entry to mark splice
            s_initial_timestamp_ms = last_entry.timestamp + DATA_SPLICE_GAP_MS;
            ESP_LOGI(TAG, "Last timestamp: %lu ms, new initial: %lu ms",
                     last_entry.timestamp, s_initial_timestamp_ms);
        } else {
            s_initial_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
    }

    // Initialize UART
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_handler_init());
    QueueHandle_t q = uart_handler_get_queue();
    command_t cmd;

    // Initialize temperature sensor
    temperature_sensor_handle_t temp_sensor = NULL;
    temperature_sensor_config_t temp_config = {
        .range_min = -10,
        .range_max = 80,
        .clk_src = 0
    };

    esp_err_t err = temperature_sensor_install(&temp_config, &temp_sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install temp sensor: %s", esp_err_to_name(err));
        send_msg("Error: Temperature sensor init failed\r\n");
        return;
    }

    err = temperature_sensor_enable(temp_sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable temp sensor: %s", esp_err_to_name(err));
        send_msg("Error: Temperature sensor enable failed\r\n");
        return;
    }

    ESP_LOGI(TAG, "Temperature sensor initialized");

    uint32_t start_time_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (;;) {
        switch (settings.state) {
        case IDLE:
            send_msg("\r\nIDLE - Type 'help' for commands\r\n");
            ESP_LOGI(TAG, "State: IDLE, waiting for commands");
            while (xQueueReceive(q, &cmd, portMAX_DELAY)) {
                if (handle_input_command(&cmd, flash, &settings)) break;
            }
            // Reset start time when leaving IDLE
            start_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
            break;

        case LOGGING:
            // Check for commands (non-blocking)
            if (xQueueReceive(q, &cmd, 0)) {
                handle_input_command(&cmd, flash, &settings);
            }

            // Read temperature
            float temperature;
            err = temperature_sensor_get_celsius(temp_sensor, &temperature);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read temperature: %s", esp_err_to_name(err));
                temperature = 99.9f;  // Use 99.9f on error
            }

            // Calculate relative timestamp
            uint32_t relative_ms = (uint32_t)(esp_timer_get_time() / 1000) - start_time_ms;
            uint32_t timestamp = s_initial_timestamp_ms + relative_ms;

            // Log entry
            log_data_entry(flash, &(log_entry_t){
                .timestamp = timestamp,
                .temperature = temperature
            });

            vTaskDelay(pdMS_TO_TICKS(settings.logging_period_MS));
            break;

        case ERROR:
            // Minimal error handling - wait for user to reset
            send_msg("\r\nERROR state - Type 'reset' to recover\r\n");
            ESP_LOGE(TAG, "In ERROR state");
            while (xQueueReceive(q, &cmd, portMAX_DELAY)) {
                if (strcmp(cmd.str, "reset") == 0) {
                    handle_input_command(&cmd, flash, &settings);
                    break;
                }
            }
            break;

        default:
            ESP_LOGE(TAG, "Unknown state %u, resetting to IDLE", settings.state);
            settings.state = IDLE;
            esp_partition_write(flash, 0, &settings, sizeof(settings_t));
            break;
        }
    }
}
