cmake_minimum_required(VERSION 3.16)
project(esp32_circular_buffer_system C)

add_subdirectory(esp32_client)
add_subdirectory(esp32_server)

# Standalone native C verification executable
add_executable(test_circular_buffer
    tests/test_circular_buffer.c
    common/circular_buffer.c
)
target_include_directories(test_circular_buffer PRIVATE
    common
    mock_esp_idf/include
)
