idf_component_register(
    SRCS "ESP_sample_sleep_project.c"
    INCLUDE_DIRS "."
    REQUIRES esp_partition uart_handler esp_driver_tsens esp_timer
)
