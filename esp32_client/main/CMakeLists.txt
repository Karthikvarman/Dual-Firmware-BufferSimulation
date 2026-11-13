idf_component_register(SRCS "main_client.c" "../../common/circular_buffer.c"
                       INCLUDE_DIRS "." "../../common"
                       REQUIRES esp_timer esp_wifi esp_event nvs_flash driver)

